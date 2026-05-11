/*
 * LED Class Core
 *
 * Copyright 2005-2006 Openedhand Ltd.
 *
 * Author: Richard Purdie <rpurdie@openedhand.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <dt-bindings/leds/common.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/rwsem.h>
#include <uapi/linux/uleds.h>
#include "leds.h"

DECLARE_RWSEM(leds_list_lock);
EXPORT_SYMBOL_GPL(leds_list_lock);

LIST_HEAD(leds_list);
EXPORT_SYMBOL_GPL(leds_list);

static const char * const led_colors[LED_COLOR_ID_MAX] = {
	[LED_COLOR_ID_WHITE] = "white",
	[LED_COLOR_ID_RED] = "red",
	[LED_COLOR_ID_GREEN] = "green",
	[LED_COLOR_ID_BLUE] = "blue",
	[LED_COLOR_ID_AMBER] = "amber",
	[LED_COLOR_ID_VIOLET] = "violet",
	[LED_COLOR_ID_YELLOW] = "yellow",
	[LED_COLOR_ID_IR] = "ir",
	[LED_COLOR_ID_MULTI] = "multicolor",
	[LED_COLOR_ID_RGB] = "rgb",
	[LED_COLOR_ID_PURPLE] = "purple",
	[LED_COLOR_ID_ORANGE] = "orange",
	[LED_COLOR_ID_PINK] = "pink",
	[LED_COLOR_ID_CYAN] = "cyan",
	[LED_COLOR_ID_LIME] = "lime",
};

static void led_timer_function(unsigned long data)
{
	struct led_classdev *led_cdev = (void *)data;
	unsigned long brightness;
	unsigned long delay;

	if (!led_cdev->blink_delay_on || !led_cdev->blink_delay_off) {
		led_set_brightness_async(led_cdev, LED_OFF);
		return;
	}

	if (led_cdev->flags & LED_BLINK_ONESHOT_STOP) {
		led_cdev->flags &= ~LED_BLINK_ONESHOT_STOP;
		return;
	}

	brightness = led_get_brightness(led_cdev);
	if (!brightness) {
		/* Time to switch the LED on. */
		if (led_cdev->delayed_set_value) {
			led_cdev->blink_brightness =
					led_cdev->delayed_set_value;
			led_cdev->delayed_set_value = 0;
		}
		brightness = led_cdev->blink_brightness;
		delay = led_cdev->blink_delay_on;
	} else {
		/* Store the current brightness value to be able
		 * to restore it when the delay_off period is over.
		 */
		led_cdev->blink_brightness = brightness;
		brightness = LED_OFF;
		delay = led_cdev->blink_delay_off;
	}

	led_set_brightness_async(led_cdev, brightness);

	/* Return in next iteration if led is in one-shot mode and we are in
	 * the final blink state so that the led is toggled each delay_on +
	 * delay_off milliseconds in worst case.
	 */
	if (led_cdev->flags & LED_BLINK_ONESHOT) {
		if (led_cdev->flags & LED_BLINK_INVERT) {
			if (brightness)
				led_cdev->flags |= LED_BLINK_ONESHOT_STOP;
		} else {
			if (!brightness)
				led_cdev->flags |= LED_BLINK_ONESHOT_STOP;
		}
	}

	mod_timer(&led_cdev->blink_timer, jiffies + msecs_to_jiffies(delay));
}

static void set_brightness_delayed(struct work_struct *ws)
{
	struct led_classdev *led_cdev =
		container_of(ws, struct led_classdev, set_brightness_work);

	led_stop_software_blink(led_cdev);

	led_set_brightness_async(led_cdev, led_cdev->delayed_set_value);
}

static void led_set_software_blink(struct led_classdev *led_cdev,
				   unsigned long delay_on,
				   unsigned long delay_off)
{
	int current_brightness;

	current_brightness = led_get_brightness(led_cdev);
	if (current_brightness)
		led_cdev->blink_brightness = current_brightness;
	if (!led_cdev->blink_brightness)
		led_cdev->blink_brightness = led_cdev->max_brightness;

	led_cdev->blink_delay_on = delay_on;
	led_cdev->blink_delay_off = delay_off;

	/* never on - just set to off */
	if (!delay_on) {
		led_set_brightness_async(led_cdev, LED_OFF);
		return;
	}

	/* never off - just set to brightness */
	if (!delay_off) {
		led_set_brightness_async(led_cdev, led_cdev->blink_brightness);
		return;
	}

	mod_timer(&led_cdev->blink_timer, jiffies + 1);
}


static void led_blink_setup(struct led_classdev *led_cdev,
		     unsigned long *delay_on,
		     unsigned long *delay_off)
{
	if (!(led_cdev->flags & LED_BLINK_ONESHOT) &&
	    led_cdev->blink_set &&
	    !led_cdev->blink_set(led_cdev, delay_on, delay_off))
		return;

	/* blink with 1 Hz as default if nothing specified */
	if (!*delay_on && !*delay_off)
		*delay_on = *delay_off = 500;

	led_set_software_blink(led_cdev, *delay_on, *delay_off);
}

void led_init_core(struct led_classdev *led_cdev)
{
	INIT_WORK(&led_cdev->set_brightness_work, set_brightness_delayed);

	setup_timer(&led_cdev->blink_timer, led_timer_function,
		    (unsigned long)led_cdev);
}
EXPORT_SYMBOL_GPL(led_init_core);

void led_blink_set(struct led_classdev *led_cdev,
		   unsigned long *delay_on,
		   unsigned long *delay_off)
{
	del_timer_sync(&led_cdev->blink_timer);

	led_cdev->flags &= ~LED_BLINK_ONESHOT;
	led_cdev->flags &= ~LED_BLINK_ONESHOT_STOP;

	led_blink_setup(led_cdev, delay_on, delay_off);
}
EXPORT_SYMBOL(led_blink_set);

void led_blink_set_oneshot(struct led_classdev *led_cdev,
			   unsigned long *delay_on,
			   unsigned long *delay_off,
			   int invert)
{
	if ((led_cdev->flags & LED_BLINK_ONESHOT) &&
	     timer_pending(&led_cdev->blink_timer))
		return;

	led_cdev->flags |= LED_BLINK_ONESHOT;
	led_cdev->flags &= ~LED_BLINK_ONESHOT_STOP;

	if (invert)
		led_cdev->flags |= LED_BLINK_INVERT;
	else
		led_cdev->flags &= ~LED_BLINK_INVERT;

	led_blink_setup(led_cdev, delay_on, delay_off);
}
EXPORT_SYMBOL(led_blink_set_oneshot);

void led_stop_software_blink(struct led_classdev *led_cdev)
{
	del_timer_sync(&led_cdev->blink_timer);
	led_cdev->blink_delay_on = 0;
	led_cdev->blink_delay_off = 0;
}
EXPORT_SYMBOL_GPL(led_stop_software_blink);

void led_set_brightness(struct led_classdev *led_cdev,
			enum led_brightness brightness)
{
	int ret = 0;

	/* delay brightness if soft-blink is active */
	if (led_cdev->blink_delay_on || led_cdev->blink_delay_off) {
		led_cdev->delayed_set_value = brightness;
		if (brightness == LED_OFF)
			schedule_work(&led_cdev->set_brightness_work);
		return;
	}

	if (led_cdev->flags & SET_BRIGHTNESS_ASYNC) {
		led_set_brightness_async(led_cdev, brightness);
		return;
	} else if (led_cdev->flags & SET_BRIGHTNESS_SYNC)
		ret = led_set_brightness_sync(led_cdev, brightness);
	else
		ret = -EINVAL;

	if (ret < 0)
		dev_dbg(led_cdev->dev, "Setting LED brightness failed (%d)\n",
			ret);
}
EXPORT_SYMBOL(led_set_brightness);

int led_update_brightness(struct led_classdev *led_cdev)
{
	int ret = 0;

	if (led_cdev->brightness_get) {
		ret = led_cdev->brightness_get(led_cdev);
		if (ret >= 0) {
			led_cdev->brightness = ret;
			return 0;
		}
	}

	return ret;
}
EXPORT_SYMBOL(led_update_brightness);

/* Caller must ensure led_cdev->led_access held */
void led_sysfs_disable(struct led_classdev *led_cdev)
{
	lockdep_assert_held(&led_cdev->led_access);

	led_cdev->flags |= LED_SYSFS_DISABLE;
}
EXPORT_SYMBOL_GPL(led_sysfs_disable);

/* Caller must ensure led_cdev->led_access held */
void led_sysfs_enable(struct led_classdev *led_cdev)
{
	lockdep_assert_held(&led_cdev->led_access);

	led_cdev->flags &= ~LED_SYSFS_DISABLE;
}
EXPORT_SYMBOL_GPL(led_sysfs_enable);

static void led_parse_fwnode_props(struct device *dev,
				   struct fwnode_handle *fwnode,
				   struct led_properties *props)
{
	int ret;

	if (!fwnode)
		return;

	if (fwnode_property_present(fwnode, "label")) {
		ret = fwnode_property_read_string(fwnode, "label", &props->label);
		if (ret)
			dev_err(dev, "Error parsing 'label' property (%d)\n", ret);
		return;
	}

	if (fwnode_property_present(fwnode, "color")) {
		ret = fwnode_property_read_u32(fwnode, "color", &props->color);
		if (ret)
			dev_err(dev, "Error parsing 'color' property (%d)\n", ret);
		else if (props->color >= LED_COLOR_ID_MAX)
			dev_err(dev, "LED color identifier out of range\n");
		else
			props->color_present = true;
	}

	if (!fwnode_property_present(fwnode, "function"))
		return;

	ret = fwnode_property_read_string(fwnode, "function", &props->function);
	if (ret) {
		dev_err(dev,
			"Error parsing 'function' property (%d)\n",
			ret);
	}

	if (!fwnode_property_present(fwnode, "function-enumerator"))
		return;

	ret = fwnode_property_read_u32(fwnode, "function-enumerator",
				       &props->func_enum);
	if (ret) {
		dev_err(dev,
			"Error parsing 'function-enumerator' property (%d)\n",
			ret);
	} else {
		props->func_enum_present = true;
	}
}

int led_compose_name(struct device *dev, struct led_init_data *init_data,
		     char *led_classdev_name)
{
	struct led_properties props = {};
	struct fwnode_handle *fwnode = init_data->fwnode;
	const char *devicename = init_data->devicename;
	int n;

	if (!led_classdev_name)
		return -EINVAL;

	led_parse_fwnode_props(dev, fwnode, &props);

	if (props.label) {
		/*
		 * If init_data.devicename is NULL, then it indicates that
		 * DT label should be used as-is for LED class device name.
		 * Otherwise the label is prepended with devicename to compose
		 * the final LED class device name.
		 */
		if (devicename) {
			n = snprintf(led_classdev_name, LED_MAX_NAME_SIZE, "%s:%s",
				     devicename, props.label);
		} else {
			n = snprintf(led_classdev_name, LED_MAX_NAME_SIZE, "%s", props.label);
		}
	} else if (props.function || props.color_present) {
		char tmp_buf[LED_MAX_NAME_SIZE];

		if (props.func_enum_present) {
			n = snprintf(tmp_buf, LED_MAX_NAME_SIZE, "%s:%s-%d",
				     props.color_present ? led_colors[props.color] : "",
				     props.function ?: "", props.func_enum);
		} else {
			n = snprintf(tmp_buf, LED_MAX_NAME_SIZE, "%s:%s",
				     props.color_present ? led_colors[props.color] : "",
				     props.function ?: "");
		}
		if (n >= LED_MAX_NAME_SIZE)
			return -E2BIG;

		if (init_data->devname_mandatory) {
			n = snprintf(led_classdev_name, LED_MAX_NAME_SIZE, "%s:%s",
				     devicename, tmp_buf);
		} else {
			n = snprintf(led_classdev_name, LED_MAX_NAME_SIZE, "%s", tmp_buf);
		}
	} else if (init_data->default_label) {
		if (!devicename) {
			dev_err(dev, "Legacy LED naming requires devicename segment");
			return -EINVAL;
		}
		n = snprintf(led_classdev_name, LED_MAX_NAME_SIZE, "%s:%s",
			     devicename, init_data->default_label);
	} else if (is_of_node(fwnode)) {
		n = snprintf(led_classdev_name, LED_MAX_NAME_SIZE, "%s",
			     to_of_node(fwnode)->name);
	} else {
		return -EINVAL;
	}

	if (n >= LED_MAX_NAME_SIZE)
		return -E2BIG;

	return 0;
}
EXPORT_SYMBOL_GPL(led_compose_name);

const char *led_get_color_name(u8 color_id)
{
	if (color_id >= ARRAY_SIZE(led_colors))
		return NULL;

	return led_colors[color_id];
}
EXPORT_SYMBOL_GPL(led_get_color_name);
