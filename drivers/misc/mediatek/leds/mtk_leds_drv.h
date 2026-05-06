/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef MTK_LEDS_DRV_H
#define MTK_LEDS_DRV_H

#include <linux/leds.h>
#include <mtk_leds_hal.h>

/**
 * led device node structure with mtk extentions
 * cdev: common led device structure
 * bdev: used by the backlight class driver (backlight.c)
 * cust: customization data from device tree
 * work: workqueue for specialfied led device
 * level: brightness level
 * delay_on: on time if led is blinking
 * delay_off: off time if led is blinking
 */
struct mt65xx_led_data {
	struct led_classdev cdev;
	struct backlight_device *bdev;
	struct cust_mt65xx_led cust;
	struct work_struct work;
	int level;
	int delay_on;
	int delay_off;
};

/****************************************************************************
 * LED DRV functions
 ***************************************************************************/

#ifdef CONTROL_BL_TEMPERATURE
int setMaxbrightness(int max_level, int enable);
#endif

extern int mt65xx_leds_brightness_set(enum mt65xx_led_type type, enum led_brightness level);
#ifdef CONFIG_MTK_LEDS
extern int backlight_brightness_set(int level);
#else
#define backlight_brightness_set(level) do { } while (0)
#endif
extern int disp_bls_set_max_backlight(unsigned int level);

#endif
