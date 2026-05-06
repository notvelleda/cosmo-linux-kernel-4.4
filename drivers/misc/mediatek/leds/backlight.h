#ifndef _MTK_LED_BACKLIGHT_H
#define _MTK_LED_BACKLIGHT_H

#include <linux/device.h>
#include <mtk_leds_drv.h>

int mtk_backlight_register(struct device *dev, struct mt65xx_led_data *led_data);
void mtk_backlight_unregister(struct device *dev,
			      struct mt65xx_led_data *led_data);

#endif
