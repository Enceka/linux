// SPDX-License-Identifier: GPL-2.0-only
/*
 * UMS9621 camera power domain. Based on CharlesGool/e5-sleep-kernel
 * e5-sleep2 (52ad641), with the DCAM/ISP/top sequence verified against
 * E5's stock sprd_camsys_pw_domain.ko, qogirn6l operations.
 * Registers and masks come from the original device tree.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>

#define CAMSYS_POLL_MAX 30
enum camsys_bank_id { CAMSYS_TOP, CAMSYS_DCAM, CAMSYS_ISP, CAMSYS_BANKS };
struct camsys_reg { struct regmap *map; u32 reg, mask; };
struct camsys_bank {
	struct camsys_reg force, automatic, state;
};
struct camsys_pd {
	struct generic_pm_domain gpd;
	struct device *dev;
	struct camsys_bank banks[CAMSYS_BANKS];
	struct clk *mm_eb;
	bool clock_on;
};

static int camsys_reg_get(struct device *dev, const char *prop,
			  struct camsys_reg *out)
{
	u32 args[2];

	out->map = syscon_regmap_lookup_by_phandle_args(dev->of_node, prop, 2, args);
	if (IS_ERR(out->map))
		return dev_err_probe(dev, PTR_ERR(out->map), "no %s syscon\n", prop);
	out->reg = args[0];
	out->mask = args[1];
	return out->mask ? 0 : -EINVAL;
}

static int camsys_switch(struct camsys_bank *bank, bool on)
{
	unsigned int value[3], expected;
	int ret, i, j;

	expected = on ? 0 : 7U << __ffs(bank->state.mask);
	if (expected & ~bank->state.mask)
		return -EINVAL;
	ret = regmap_update_bits(bank->automatic.map, bank->automatic.reg,
				 bank->automatic.mask, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(bank->force.map, bank->force.reg,
				 bank->force.mask, on ? 0 : bank->force.mask);
	if (ret)
		return ret;
	/* Stock qogirn6l: 300-350 us and three equal state reads, up to 30 tries. */
	for (i = 0; i < CAMSYS_POLL_MAX; i++) {
		usleep_range(300, 350);
		for (j = 0; j < 3; j++) {
			ret = regmap_read(bank->state.map, bank->state.reg, &value[j]);
			if (ret)
				return ret;
			value[j] &= bank->state.mask;
		}
		if (value[0] == expected && value[1] == expected && value[2] == expected)
			return 0;
	}
	return -ETIMEDOUT;
}

static int camsys_all_off(struct camsys_pd *pd)
{
	static const int order[] = { CAMSYS_DCAM, CAMSYS_ISP, CAMSYS_TOP };
	int ret;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		ret = camsys_switch(&pd->banks[order[i]], false);
		if (ret)
			return ret;
	}
	return 0;
}

static int camsys_all_on(struct camsys_pd *pd)
{
	static const int order[] = { CAMSYS_TOP, CAMSYS_DCAM, CAMSYS_ISP };
	int ret;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		ret = camsys_switch(&pd->banks[order[i]], true);
		if (ret)
			return ret;
	}
	return 0;
}

static void camsys_clock_off(void *data)
{
	struct camsys_pd *pd = data;

	if (pd->clock_on) {
		clk_disable_unprepare(pd->mm_eb);
		pd->clock_on = false;
	}
}

static int camsys_pd_power_on(struct generic_pm_domain *gpd)
{
	struct camsys_pd *pd = container_of(gpd, struct camsys_pd, gpd);
	int ret, cleanup;

	if (!pd->clock_on) {
		ret = clk_prepare_enable(pd->mm_eb);
		if (ret)
			return ret;
		pd->clock_on = true;
	}
	ret = camsys_all_on(pd);
	if (!ret)
		return 0;
	cleanup = camsys_all_off(pd);
	if (!cleanup)
		camsys_clock_off(pd);
	else
		dev_err(pd->dev, "camera power-on rollback failed: %d\n", cleanup);
	return ret;
}

static int camsys_pd_power_off(struct generic_pm_domain *gpd)
{
	struct camsys_pd *pd = container_of(gpd, struct camsys_pd, gpd);
	int ret, cleanup;

	ret = camsys_all_off(pd);
	if (!ret) {
		camsys_clock_off(pd);
		return 0;
	}
	/* genpd keeps the domain logically on when power_off fails. */
	cleanup = camsys_all_on(pd);
	if (cleanup)
		dev_err(pd->dev, "camera power-off rollback failed: %d\n", cleanup);
	return ret;
}

static int camsys_pd_probe(struct platform_device *pdev)
{
	static const char * const props[CAMSYS_BANKS][3] = {
		{ "force_shutdown", "shutdown_en", "camera_power_state" },
		{ "dcam_force_shutdown", "dcam_shutdown_en", "dcam_power_state" },
		{ "isp_force_shutdown", "isp_shutdown_en", "isp_power_state" },
	};
	struct device *dev = &pdev->dev;
	struct camsys_pd *pd;
	int ret, i;

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	pd->dev = dev;
	for (i = 0; i < CAMSYS_BANKS; i++) {
		ret = camsys_reg_get(dev, props[i][0], &pd->banks[i].force);
		if (ret)
			return ret;
		ret = camsys_reg_get(dev, props[i][1], &pd->banks[i].automatic);
		if (ret)
			return ret;
		ret = camsys_reg_get(dev, props[i][2], &pd->banks[i].state);
		if (ret)
			return ret;
	}
	pd->mm_eb = devm_clk_get(dev, "clk_mm_eb");
	if (IS_ERR(pd->mm_eb))
		return dev_err_probe(dev, PTR_ERR(pd->mm_eb), "no clk_mm_eb\n");
	ret = devm_add_action_or_reset(dev, camsys_clock_off, pd);
	if (ret)
		return ret;
	/* Own the bootloader's AON gate reference before closing it. */
	ret = clk_prepare_enable(pd->mm_eb);
	if (ret)
		return ret;
	pd->clock_on = true;
	ret = camsys_all_off(pd);
	if (ret)
		return dev_err_probe(dev, ret, "camera domain did not shut down\n");
	camsys_clock_off(pd);
	pd->gpd.name = dev_name(dev);
	pd->gpd.power_on = camsys_pd_power_on;
	pd->gpd.power_off = camsys_pd_power_off;
	ret = pm_genpd_init(&pd->gpd, NULL, true);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(dev->of_node, &pd->gpd);
	if (ret) {
		pm_genpd_remove(&pd->gpd);
		return ret;
	}
	dev_info(dev, "camera/DCAM/ISP domains off; power managed by genpd\n");
	return 0;
}

static const struct of_device_id camsys_pd_of_match[] = {
	{ .compatible = "sprd,qogirl6l-camsys-domain" },
	{ }
};
static struct platform_driver camsys_pd_driver = {
	.probe = camsys_pd_probe,
	.driver = {
		.name = "sprd-camsys-pd",
		.of_match_table = camsys_pd_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(camsys_pd_driver);
