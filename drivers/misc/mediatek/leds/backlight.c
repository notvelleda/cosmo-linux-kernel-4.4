/*
 * Backlight driver for the MediaTek LED subsystem.
 *
 * This is needed since software isn't typically designed to look for
 * /sys/class/leds/lcd-backlight when setting the LCD backlight
 */

#include <linux/backlight.h>
#include <linux/device.h>
#include <mtk_leds_drv.h>

static int mtk_backlight_update_status(struct backlight_device *bl)
{
	struct mt65xx_led_data *led_data = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	led_data->cdev.brightness_set(&led_data->cdev, brightness);
	return 0;
}

static int mtk_backlight_get_brightness(struct backlight_device *bl)
{
	struct mt65xx_led_data *led_data = bl_get_data(bl);
	return led_data->level;
}

static const struct backlight_ops mtk_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = mtk_backlight_update_status,
	.get_brightness = mtk_backlight_get_brightness
};

int mtk_backlight_register(struct device *dev,
			   struct mt65xx_led_data *led_data)
{
	struct backlight_device *bl;
	struct backlight_properties props;
	const char *name = "mtk_backlight";

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = 255;

	bl = devm_backlight_device_register(dev, name, dev, led_data,
					    &mtk_backlight_ops, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	led_data->level = bl->props.brightness = 255;
	led_data->bdev = bl;

	return 0;
}

void mtk_backlight_unregister(struct device *dev,
			      struct mt65xx_led_data *led_data)
{
	if (led_data->bdev != NULL)
		devm_backlight_device_unregister(dev, led_data->bdev);
}
