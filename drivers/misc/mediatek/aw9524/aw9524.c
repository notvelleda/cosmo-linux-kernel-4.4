/*
 * AWINIC AW9524 LED driver, rewritten to use the backported multicolor LED 
 * class framework
 *
 * Copyright (C) 2016 liweilei@awinic.com.cn
 * Copyright (C) 2026 June Carlson <notvelleda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "dt-bindings/leds/common.h"
#include "linux/device.h"
#include "linux/kernel.h"
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <mt-plat/mtk_pwm.h>

#define P0_INPUT_AW9524		0x00	//P0口引脚当前逻辑状态。0-低电平;1-高电平
#define P1_INPUT_AW9524		0x01	//P1口引脚当前逻辑状态。0-低电平;1-高电平
#define P0_OUTPUT_AW9524	0x02	//设置P0口引脚输出值。0-输出低电平;1-输出高电平
#define P1_OUTPUT_AW9524	0x03	//设置P1口引脚输出值。0-输出低电平;1-输出高电平
#define P0_CONFIG_AW9524	0x04	//P0口输入/输出模式选择。0-输出模式;1-输入模式
#define P1_CONFIG_AW9524	0x05	//P1口输入/输出模式选择。0-输出模式;1-输入模式
#define P0_INT_AW9524		0x06	//P0口中断使能。0-中断使能;1-中断不使能
#define P1_INT_AW9524		0x07	//P1口中断使能。0-中断使能;1-中断不使能
#define ID_REG_AW9524		0x10	//ID寄存器,只读,读出值为23H
#define CTL_REG_AW9524		0x11	//设置P0口驱动模式。若D[4]=0,P0口为Open-Drain模式;若D[4]=1,P0口为Push-Pull模式
#define P0_LED_MODE_AW9524	0x12	//配置P0_7~P0_0为LED或GPIO模式。 1:GPIO模式 0:LED模式
#define P1_LED_MODE_AW9524	0x13	//配置P1_7~P1_0为LED或GPIO模式。 1:GPIO模式 0:LED模式
#define P1_0_DIM0_AW9524	0x20
#define P1_1_DIM0_AW9524	0x21
#define P1_2_DIM0_AW9524	0x22
#define P1_3_DIM0_AW9524	0x23
#define P0_0_DIM0_AW9524	0x24
#define P0_1_DIM0_AW9524	0x25
#define P0_2_DIM0_AW9524	0x26
#define P0_3_DIM0_AW9524	0x27
#define P0_4_DIM0_AW9524	0x28
#define P0_5_DIM0_AW9524	0x29
#define P0_6_DIM0_AW9524	0x2A
#define P0_7_DIM0_AW9524	0x2B
#define P1_4_DIM0_AW9524	0x2C
#define P1_5_DIM0_AW9524	0x2D
#define P1_6_DIM0_AW9524	0x2E
#define P1_7_DIM0_AW9524	0x2F
#define SW_RSTN_AW9524		0x7F

extern int aeon_gpio_set(const char *name);

#ifdef CONFIG_OF
static const struct of_device_id aw9524_of_match[] = {
	{.compatible = "mediatek,aw9524_key"},
	{},
};
#endif

/* stored as (address, data) */
static u8 reset_commands[18][2] = {
	{SW_RSTN_AW9524, 0x00},	// Software Reset

	{P0_LED_MODE_AW9524, 0x00},	// P0: 0~7 led
	{P0_CONFIG_AW9524, 0x00},	// P0: output Mode
	{CTL_REG_AW9524, 0x02},	// P0: 1/4
	{P0_0_DIM0_AW9524, 0x00},	//设置电流等级64
	{P0_1_DIM0_AW9524, 0x00},
	{P0_2_DIM0_AW9524, 0x40},
	{P0_3_DIM0_AW9524, 0x00},
	{P0_4_DIM0_AW9524, 0x00},
	{P0_5_DIM0_AW9524, 0x00},

	{P1_LED_MODE_AW9524, 0x00},// P1: 0~7 led
	{P1_CONFIG_AW9524, 0x00},	// P1: output Mode
	{P1_0_DIM0_AW9524, 0x00},	//设置电流等级64
	{P1_1_DIM0_AW9524, 0x00},
	{P1_2_DIM0_AW9524, 0x00},
	{P1_3_DIM0_AW9524, 0x00},
	{P1_4_DIM0_AW9524, 0x00},
	{P1_5_DIM0_AW9524, 0x00}
};

struct aw9524_i2c_data {
	struct i2c_client *client;
	struct led_classdev_mc mc_cdev;
};

static const char *channel_names[4] = {
	"cosmo_internal",
	"cosmo_camera",
	"cosmo_rocker_right",
	"cosmo_rocker_left"
};

static u8 channel_to_register[4][3] = {
	{P0_0_DIM0_AW9524, P0_1_DIM0_AW9524, P0_2_DIM0_AW9524},
	{P0_3_DIM0_AW9524, P0_4_DIM0_AW9524, P0_5_DIM0_AW9524},
	{P1_0_DIM0_AW9524, P1_1_DIM0_AW9524, P1_2_DIM0_AW9524},
	{P1_3_DIM0_AW9524, P1_4_DIM0_AW9524, P1_5_DIM0_AW9524}
};

/* set in aw9524_probe */
static struct pinctrl *aw9524_pin;
static struct pinctrl_state *aw9524_shdn_high;
static struct pinctrl_state *aw9524_shdn_low;

static int aw9524_write_reg(struct i2c_client *client, u8 addr, u8 reg_data)
{
	int ret;
	u8 wdbuf[512] = {0};

	struct i2c_msg msgs[1] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 2,
			.buf = wdbuf,
		}
	};

	wdbuf[0] = addr;
	wdbuf[1] = reg_data;

	ret = i2c_transfer(client->adapter, msgs, 1);

	if (ret < 0)
		dev_err(&client->dev,
		        "Failed to write to device (return value %d)\n", ret);
	return ret;
}

static int aw9524_read_reg(struct i2c_client *client, u8 addr)
{
	int ret;
	u8 rdbuf[512] = {0};

	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = rdbuf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = rdbuf,
		},
	};

	rdbuf[0] = addr;
	
	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		dev_err(&client->dev,
		    "Failed to read from device (return value %d)\n", ret);

	return rdbuf[0];
}

static void led_brightness_set(struct led_classdev *led_cdev,
                               enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct aw9524_i2c_data *data = container_of(mc_cdev,
	                                            struct aw9524_i2c_data,
                                                    mc_cdev);
	unsigned int channel = mc_cdev->subled_info[0].channel;

	led_mc_calc_color_components(mc_cdev, brightness);

	aw9524_write_reg(data->client, channel_to_register[channel][0],
	                 mc_cdev->subled_info[0].brightness * 4);
	aw9524_write_reg(data->client, channel_to_register[channel][1],
	                 mc_cdev->subled_info[1].brightness * 5);
	aw9524_write_reg(data->client, channel_to_register[channel][2],
	                 mc_cdev->subled_info[2].brightness * 5);
}

static int aw9524_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	u8 reg_value;
	int i, ret;
	struct aw9524_i2c_data *data;
	struct device *dev = &client->dev;
	struct mc_subled *all_subleds;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		return -ENODEV;
	}

	/* hardware reset, this pointer access is guaranteed to be safe since
	 * if they can't be set then the i2c driver will never be initialized
	 */
	pinctrl_select_state(aw9524_pin, aw9524_shdn_low);
	msleep(5);
	pinctrl_select_state(aw9524_pin, aw9524_shdn_high);
	msleep(5);

	/* not sure what this does, is this needed? */
	for (reg_value = 0, i = 5; i > 0 && reg_value != 0x23; i--) {
		reg_value = aw9524_read_reg(client, 0x10);
		msleep(10);
	}

	if (i == 0)
		return -ENODEV;

	data = kzalloc(sizeof(struct aw9524_i2c_data) * 4, GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	all_subleds = devm_kcalloc(dev, 4 * 3, sizeof(struct mc_subled),
	                           GFP_KERNEL);
	if (all_subleds == NULL)
		return -ENOMEM;

	i2c_set_clientdata(client, data);

	/* software reset */
	for (i = 0; i < 18; i++)
		aw9524_write_reg(client, reset_commands[i][0],
		                 reset_commands[i][1]);

	for (i = 0; i < 4; i++) {
		struct led_classdev_mc *mc_led_cdev = &data[i].mc_cdev;
		struct mc_subled *subleds = &all_subleds[i * 3];

		data[i].client = client;
		mc_led_cdev->led_cdev.name = channel_names[i];
		mc_led_cdev->led_cdev.max_brightness = 18;
		mc_led_cdev->led_cdev.brightness_set = led_brightness_set;
		mc_led_cdev->num_colors = 3;
		mc_led_cdev->subled_info = subleds;

		subleds[0].color_index = LED_COLOR_ID_RED;
		subleds[0].channel = i;
		subleds[1].color_index = LED_COLOR_ID_GREEN;
		subleds[1].channel = i;
		subleds[2].color_index = LED_COLOR_ID_BLUE;
		subleds[2].channel = i;

		ret = led_classdev_multicolor_register(dev, mc_led_cdev);

		if (ret) {
			dev_err(dev, "Couldn't allocate LED %d (%s)\n",
			        i, channel_names[i]);
			kzfree(data);
			kzfree(all_subleds);
			i2c_set_clientdata(client, NULL);
			return ret;
		}
	}

	dev_info(dev, "Initialized AW9524 I2C devices\n");
	return 0;
}

static int aw9524_i2c_remove(struct i2c_client *client)
{
	struct aw9524_i2c_data *data = i2c_get_clientdata(client);
	kzfree(data);
	i2c_set_clientdata(client, NULL);
	return 0;
}

static const struct i2c_device_id aw9524_i2c_id[] = {
	{ "aw9524", 0 },
	{}
};

static struct i2c_driver aw9524_i2c_driver = {
	.probe          = aw9524_i2c_probe,
	.remove         = aw9524_i2c_remove,
	.id_table       = aw9524_i2c_id,
	.driver = {
		.name   = "aw9524",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = aw9524_of_match,
#endif
	}
};

/* i'm not sure if this backlight code actually uses the aw9524, but it's here
 * since the original driver has it here
 */
u32 kbd_pwm_lut[6][2] = {
	{0x00000000, 0x00000000},
	{0x00001fff, 0x00000000},
	{0x07ffffff, 0x00000000},
	{0xffffffff, 0x0000003f},
	{0xffffffff, 0x000fffff},
	{0xffffffff, 0xffffffff}
};

extern unsigned int hdmi_det_gpio;
unsigned int keyboardlight_flag = 0;

static void kbd_brightness_set(struct led_classdev *led_cdev,
                               enum led_brightness brightness)
{
	struct pwm_spec_config spec_config;
	(void) led_cdev;

	spec_config.pwm_no = PWM1;
	spec_config.mode = PWM_MODE_FIFO;
	spec_config.clk_div = CLK_DIV8;
	spec_config.clk_src = PWM_CLK_NEW_MODE_BLOCK;
	spec_config.PWM_MODE_FIFO_REGS.IDLE_VALUE = false;
	spec_config.PWM_MODE_FIFO_REGS.GUARD_VALUE = false;
	spec_config.PWM_MODE_FIFO_REGS.STOP_BITPOS_VALUE = 63;
	spec_config.PWM_MODE_FIFO_REGS.HDURATION = 1;
	spec_config.PWM_MODE_FIFO_REGS.LDURATION = 1;
	spec_config.PWM_MODE_FIFO_REGS.GDURATION = 0;
	spec_config.PWM_MODE_FIFO_REGS.WAVE_NUM  = 0;
	spec_config.PWM_MODE_FIFO_REGS.SEND_DATA0 = kbd_pwm_lut[brightness][0];
	spec_config.PWM_MODE_FIFO_REGS.SEND_DATA1 = kbd_pwm_lut[brightness][1];

	if (spec_config.PWM_MODE_FIFO_REGS.SEND_DATA0 == 0) {
		keyboardlight_flag = 0;

		if (!gpio_get_value(hdmi_det_gpio))
			aeon_gpio_set("sil9022_hdmi_hplg0"); // GPIO178
	} else {
		keyboardlight_flag = 1;
		aeon_gpio_set("sil9022_hdmi_hplg1"); // GPIO178
	}

	pwm_set_spec_config(&spec_config);
}

static int aw9524_probe(struct platform_device *pdev)
{
	static struct led_classdev *kbd_backlight_cdev;
	struct device *dev = &pdev->dev;
	int ret = 0;

	aw9524_pin = devm_pinctrl_get(dev);

	if (IS_ERR(aw9524_pin)) {
		dev_err(dev, "Couldn't get aw9524 pinctrl\n");
		return PTR_ERR(aw9524_pin);
	}

	aw9524_shdn_high = pinctrl_lookup_state(aw9524_pin, "aw9524_shdn_high");
	if (IS_ERR(aw9524_shdn_high)) {
		dev_err(dev, "Couldn't get pin state aw9524_shdn_high\n");
		return PTR_ERR(aw9524_shdn_high);
	}

	aw9524_shdn_low = pinctrl_lookup_state(aw9524_pin, "aw9524_shdn_low");
	if (IS_ERR(aw9524_shdn_low)) {
		dev_err(dev, "Couldn't get pin state aw9524_shdn_low\n");
		return PTR_ERR(aw9524_shdn_low);
	}

	ret = i2c_add_driver(&aw9524_i2c_driver);

	if (ret) {
		dev_err(dev, "Error registering I2C driver\n");
		return ret;
	}

	kbd_backlight_cdev = kzalloc(sizeof(struct led_classdev), GFP_KERNEL);
	if (kbd_backlight_cdev == NULL) {
		dev_err(dev, "Failed to allocate memory for keyboard backlight"
		        "device\n");
		return -ENOMEM;
	}

	kbd_backlight_cdev->max_brightness = 5;
	kbd_backlight_cdev->brightness_set = kbd_brightness_set;
	kbd_backlight_cdev->name = "kbd_backlight";

	ret = devm_led_classdev_register(dev, kbd_backlight_cdev);

	if (ret)
		dev_err(dev, "Error registering keyboard backlight device\n");
	else
		dev_info(dev, "Initialized AW9524 platform device\n");

	return ret;
}

static int aw9524_remove(struct platform_device *pdev)
{
	(void) pdev;
	i2c_del_driver(&aw9524_i2c_driver);
	return 0;
}

static struct platform_driver aw9524_driver = {
	.probe = aw9524_probe,
	.remove = aw9524_remove,
	.driver = {
		.name = "aw9524",
#ifdef CONFIG_OF
		.of_match_table = aw9524_of_match,
#endif
	}
};

static int __init aw9524_init(void) {
	int ret = platform_driver_register(&aw9524_driver);

	if (ret)
		pr_err("Failed to register AW9524 driver\n");
	return ret;
}

static void __exit aw9524_exit(void) {
	platform_driver_unregister(&aw9524_driver);
}

module_init(aw9524_init);
module_exit(aw9524_exit);

MODULE_AUTHOR("June Carlson <notvelleda@gmail.com>");
MODULE_DESCRIPTION("AWINIC AW9524 LED driver");
MODULE_LICENSE("GPL");