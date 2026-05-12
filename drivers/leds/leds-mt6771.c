/*
 * Driver for built in LEDs and the LCD backlight for MediaTek MT6771 based
 * devices
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2026 June Carlson <notvelleda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <mt-plat/mtk_pwm.h>
#include "../misc/mediatek/video/include/ddp_gamma.h"
#include "../misc/mediatek/video/include/ddp_pwm.h"
#include "../misc/mediatek/video/include/mtkfb.h"
#include "linux/kernel.h"

enum led_type {
	LED_TYPE_RED,
	LED_TYPE_GREEN,
	LED_TYPE_BLUE,
	LED_TYPE_JOGBALL,
	LED_TYPE_KEYBOARD,
	LED_TYPE_BUTTON,
	LED_TYPE_LCD,
	LED_TYPE_TOTAL
};

const char *led_of_names[] = {
	"mediatek,red",
	"mediatek,green",
	"mediatek,blue",
	"mediatek,jogball-backlight",
	"mediatek,keyboard-backlight",
	"mediatek,button-backlight",
	"mediatek,lcd-backlight"
};

const char *led_device_names[] = {
	"red",
	"green",
	"blue",
	"jogball_backlight",
	"kbd_backlight",
	"button_backlight",
	"lcd_backlight"
};

enum led_mode {
	LED_MODE_NONE,
	LED_MODE_PWM,
	LED_MODE_GPIO,
	LED_MODE_PMIC,
	LED_MODE_LCM,
	LED_MODE_BLS_PWM
};

struct led_data {
	enum led_type led_type;
	enum led_mode led_mode;

	union {
		u32 data;
		u32 pwm_number;
	};

	int clock_source;
	int clock_div;
	int low_duration;
	int high_duration;
	u8 pmic_pad;

	union {
		struct led_classdev cdev;
		struct backlight_device *bl;
	};
};

extern int aeon_gpio_set(const char *name);

extern unsigned int hdmi_det_gpio;
unsigned int keyboardlight_flag = 0;

static void keyboard_special_handling(struct pwm_spec_config *spec_config)
{
	if (spec_config->PWM_MODE_FIFO_REGS.SEND_DATA0 == 0) {
		keyboardlight_flag = 0;

		if (!gpio_get_value(hdmi_det_gpio))
			aeon_gpio_set("sil9022_hdmi_hplg0"); /* GPIO 178 */
	} else {
		keyboardlight_flag = 1;
		aeon_gpio_set("sil9022_hdmi_hplg1"); /* GPIO 178 */
	}
}

static void leds_mt6771_brightness_set(struct led_classdev *led_cdev,
                                       enum led_brightness brightness)
{
	struct led_data *led = container_of(led_cdev, struct led_data, cdev);
	struct pwm_spec_config spec_config;

	if (led->led_type == LED_TYPE_LCD) {
		int brightness_10bit = brightness;

		/* PWM mode brightness levels are only 6 bits, so they need to
		 * be expanded if in use
		 */
		if (led->led_mode == LED_MODE_PWM)
			brightness_10bit = (1023 * brightness + 31) / 63;

		disp_pq_notify_backlight_changed(brightness_10bit);
	}

	switch (led->led_mode) {
	case LED_MODE_PWM:
		spec_config.pwm_no = led->pwm_number;
		spec_config.mode = PWM_MODE_FIFO;
		spec_config.clk_div = led->clock_div;
		spec_config.clk_src = led->clock_source;
		spec_config.pmic_pad = led->pmic_pad;
		spec_config.PWM_MODE_FIFO_REGS.IDLE_VALUE = 0;
		spec_config.PWM_MODE_FIFO_REGS.GUARD_VALUE = brightness > 32;
		spec_config.PWM_MODE_FIFO_REGS.STOP_BITPOS_VALUE = 31;
		spec_config.PWM_MODE_FIFO_REGS.HDURATION = led->high_duration;
		spec_config.PWM_MODE_FIFO_REGS.LDURATION = led->low_duration;
		spec_config.PWM_MODE_FIFO_REGS.GDURATION =
			(led->high_duration + 1) * 32 - 1;
		spec_config.PWM_MODE_FIFO_REGS.WAVE_NUM = 0;
		spec_config.PWM_MODE_FIFO_REGS.SEND_DATA0 =
			(1 << (brightness & 31)) - 1;

		if (led->led_type == LED_TYPE_KEYBOARD)
			keyboard_special_handling(&spec_config);

		pwm_set_spec_config(&spec_config);
		break;
	case LED_MODE_LCM:
		mtkfb_set_backlight_level(brightness);
		break;
	case LED_MODE_BLS_PWM:
		disp_bls_set_backlight(brightness);
		break;
	default:
		break;
	}
}

static int leds_mt6771_backlight_update_status(struct backlight_device *bl)
{
	struct led_data *led_data = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	/* this is hacky, led_cdev can't be used at all in that function except
	 * for getting the LED data because of this
	 */
	leds_mt6771_brightness_set(&led_data->cdev, brightness);
	return 0;
}

static int leds_mt6771_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

static const struct backlight_ops backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = leds_mt6771_backlight_update_status,
	.get_brightness = leds_mt6771_backlight_get_brightness
};

/* finds matching nodes in the device tree and reads their properties to
 * construct the LED data array
 */
static struct led_data *find_device_entries(struct platform_device *pdev)
{
	int i;
	struct led_data *led_data = devm_kcalloc(&pdev->dev, LED_TYPE_TOTAL,
	                                         sizeof(struct led_data),
	                                         GFP_KERNEL);

	if (led_data == NULL)
		return NULL;

	for (i = 0; i < LED_TYPE_TOTAL; i++) {
		struct device_node *led_node;
		struct led_data *led = &led_data[i];
		int pwm_config[5];

		led->led_type = i;
		led_node = of_find_compatible_node(NULL, NULL, led_of_names[i]);

		if (led_node == NULL) {
			dev_warn(&pdev->dev,
			         "Couldn't find a device node matching %s\n",
			         led_of_names[i]);
			led->led_mode = LED_MODE_NONE;
			continue;
		}

		if (of_property_read_u32(led_node, "led_mode",
		                         &led->led_mode) < 0)
			led->led_mode = LED_MODE_NONE;

		if (of_property_read_u32(led_node, "data", &led->data) < 0)
			led->data = -1;

		if (of_property_read_u32_array(led_node, "pwm_config",
		                               pwm_config, 5) < 0)
			memset(pwm_config, 0, sizeof(int) * 5);

		led->clock_source = pwm_config[0];
		led->clock_div = pwm_config[1];
		led->low_duration = pwm_config[2];
		led->high_duration = pwm_config[3];
		led->pmic_pad = pwm_config[4];
	}

	return led_data;
}

static int leds_mt6771_probe(struct platform_device *pdev)
{
	int i;
	struct led_data *led_data = find_device_entries(pdev);

	if (led_data == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, led_data);

#ifdef CONFIG_LEDS_MT6771_DT_BUG
	/* workaround for a device tree bug, the keyboard backlight LED entry
	 * is set to be ignored on the Cosmo Communicator and changing the
	 * source file has no effect
	 */
	led_data[LED_TYPE_KEYBOARD] = (struct led_data) {
		.led_type = LED_TYPE_KEYBOARD,
		.led_mode = LED_MODE_PWM,
		.pwm_number = PWM1,
		.clock_source = PWM_CLK_NEW_MODE_BLOCK,
		.clock_div = CLK_DIV8,
		.low_duration = 1,
		.high_duration = 1,
		.pmic_pad = 0
	};
#endif

	for (i = 0; i < LED_TYPE_TOTAL; i++) {
		struct led_data *led = &led_data[i];
		const char *led_name = led_device_names[led->led_type];
		int ret;
		int max_brightness = led->led_mode == LED_MODE_PWM ? 63 : 1023;

		if (led->led_mode == LED_MODE_PMIC) {
			dev_err(&pdev->dev,"PMIC mode isn't supported, "
			        "ignoring %s LED", led_name);
			led->led_mode = LED_MODE_NONE;
			continue;
		} else if (led->led_mode == LED_MODE_GPIO) {
			dev_err(&pdev->dev,"GPIO mode isn't supported, "
			        "ignoring %s LED", led_name);
			led->led_mode = LED_MODE_NONE;
			continue;
		} else if (led->led_mode == LED_MODE_NONE) {
			dev_dbg(&pdev->dev, "LED %s mode is None, ignoring\n",
			         led_name);
			continue;
		}

		if (led->led_type == LED_TYPE_LCD) {
			struct backlight_properties props;

			memset(&props, 0, sizeof(props));
			props.type = BACKLIGHT_PLATFORM;
			props.max_brightness = max_brightness;

			led->bl = devm_backlight_device_register(&pdev->dev,
			                                         led_name,
			                                         &pdev->dev,
			                                         led,
			                                         &backlight_ops,
			                                         &props);

			if (IS_ERR(led->bl))
				ret = PTR_ERR(led->bl);
			else {
				ret = 0;
				led->bl->props.brightness = max_brightness;
				leds_mt6771_brightness_set(&led->cdev,
				                           max_brightness);
			}
		} else {
			led->cdev.name = led_name;
			led->cdev.max_brightness = max_brightness;
			led->cdev.brightness = 0;
			led->cdev.brightness_set = leds_mt6771_brightness_set;
			ret = devm_led_classdev_register(&pdev->dev,
			                                 &led->cdev);
		}

		if (ret < 0) {
			dev_err(&pdev->dev,
			        "Failed to register device for %s\n",
			        led_name);
			return ret;
		} else
			dev_dbg(&pdev->dev, "Registered device for %s\n",
			         led_name);
	}

	dev_info(&pdev->dev, "Successfully initialized driver\n");
	return 0;
}

static int leds_mt6771_remove(struct platform_device *pdev)
{
	struct led_data *led_data = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < LED_TYPE_TOTAL; i++) {
		struct led_data *led = &led_data[i];

		if (led->led_mode == LED_MODE_NONE)
			continue;

		if (led->led_type == LED_TYPE_LCD)
			devm_backlight_device_unregister(&pdev->dev, led->bl);
		else
			devm_led_classdev_unregister(&pdev->dev, &led->cdev);
	}

	return 0;
}

static void leds_mt6771_shutdown(struct platform_device *pdev)
{
	struct led_data *led_data = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < LED_TYPE_TOTAL; i++) {
		struct led_data *led = &led_data[i];

		switch (led->led_mode) {
		case LED_MODE_PWM:
			/* this might not be the correct behavior but it seems
			 * to work?
			 */
			mt_pwm_disable(led->pwm_number, led->pmic_pad);
			break;
		case LED_MODE_LCM:
			mtkfb_set_backlight_level(0);
			break;
		case LED_MODE_BLS_PWM:
			disp_bls_set_backlight(0);
			break;
		default:
			break;
		}
	}
}

static struct platform_device leds_mt6771_device = {
	.name = "leds-mt6771",
	.id = -1
};

static struct platform_driver leds_mt6771_driver = {
	.probe = leds_mt6771_probe,
	.remove = leds_mt6771_remove,
	.shutdown = leds_mt6771_shutdown,
	.driver = {
		.name = "leds-mt6771",
		.owner = THIS_MODULE
	}
};

static int __init leds_mt6771_init(void)
{
	int ret = platform_device_register(&leds_mt6771_device);
	if (ret < 0) {
		pr_err("Failed to register MT6771 LED platform device\n");
		return ret;
	}

	ret = platform_driver_register(&leds_mt6771_driver);

	if (ret < 0)
		pr_err("Failed to register MT6771 LED platform driver\n");
	return ret;
}

static void __exit leds_mt6771_exit(void)
{
	platform_driver_unregister(&leds_mt6771_driver);
	platform_device_unregister(&leds_mt6771_device);
}

module_init(leds_mt6771_init);
module_exit(leds_mt6771_exit);

MODULE_AUTHOR("June Carlson <notvelleda@gmail.com>");
MODULE_DESCRIPTION("MT6771 built in LEDs and LCD backlight driver");
MODULE_LICENSE("GPL");