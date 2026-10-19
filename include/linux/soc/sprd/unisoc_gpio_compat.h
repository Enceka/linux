/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The legacy GPIO calls Unisoc's 5.15 drivers make that 6.x no longer has, in terms of what it has: the
 * drivers keep their calls and get this header (upstream/tools/port-api.py adds it).
 */
#ifndef __LINUX_SOC_SPRD_UNISOC_GPIO_COMPAT_H
#define __LINUX_SOC_SPRD_UNISOC_GPIO_COMPAT_H

#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>
#include <dt-bindings/gpio/gpio.h>

/* devm_gpio_request(): gpio_request(), freed with the device; the direction is left alone, as it was */
static inline void unisoc_devm_gpio_free(void *gpio)
{
	gpio_free((unsigned long)gpio);
}

static inline int unisoc_devm_gpio_request(struct device *dev, unsigned int gpio, const char *label)
{
	int ret = gpio_request(gpio, label);

	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, unisoc_devm_gpio_free, (void *)(unsigned long)gpio);
}
#define devm_gpio_request(dev, gpio, label)	unisoc_devm_gpio_request(dev, gpio, label)

/*
 * of_get_named_gpio_flags(): the GPIO number, and OF_GPIO_ACTIVE_LOW when the specifier says so.
 *
 * 7.2 has no include/linux/of_gpio.h, so the phandle is resolved by hand the way the old
 * drivers/gpio/of_gpio.c did: find the controller the phandle points at, translate the specifier
 * to one of its lines and hand back the legacy global number the callers still pass to gpio_request().
 */
enum of_gpio_flags {
	OF_GPIO_ACTIVE_LOW = 0x1,
};

static inline int unisoc_of_get_named_gpio_flags(const struct device_node *np, const char *list_name,
						 int index, enum of_gpio_flags *flags)
{
	struct of_phandle_args args;
	struct gpio_device *gdev;
	struct gpio_desc *desc;
	int ret;

	if (flags)
		*flags = 0;
	ret = of_parse_phandle_with_args(np, list_name, "#gpio-cells", index, &args);
	if (ret)
		return ret;
	if (flags && args.args_count > 1 && (args.args[1] & GPIO_ACTIVE_LOW))
		*flags = OF_GPIO_ACTIVE_LOW;

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(args.np));
	if (!gdev) {
		of_node_put(args.np);
		return -EPROBE_DEFER;
	}

	desc = gpio_device_get_desc(gdev, args.args[0]);
	ret = IS_ERR(desc) ? PTR_ERR(desc) : desc_to_gpio(desc);

	gpio_device_put(gdev);
	of_node_put(args.np);
	return ret;
}
#define of_get_named_gpio_flags(np, name, index, flags) \
	unisoc_of_get_named_gpio_flags(np, name, index, flags)
#define of_get_named_gpio(np, name, index) \
	unisoc_of_get_named_gpio_flags(np, name, index, NULL)
#define of_get_gpio_flags(np, index, flags) \
	unisoc_of_get_named_gpio_flags(np, "gpios", index, flags)

#endif
