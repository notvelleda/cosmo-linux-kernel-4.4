/*
 * Driver for controlling the STM32 microcontroller in the Cosmo Communicator at
 * a low level
 *
 * Copyright (C) 2026 June Carlson <notvelleda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define STM32_NAME "cosmo-stm32"

int aeon_gpio_set(const char *name);

static const struct of_device_id stm32_of_match[] = {
	{.compatible = "mediatek, STM32_KEY-eint"},
	{},
};

static unsigned int stm32_wake_irqnr;
static struct workqueue_struct *stm32_wake_queue;
static struct work_struct stm32_wake_work;

static struct input_dev *stm32_input_dev;

static void kpd_stm32_wake_work_handler(struct work_struct *work)
{
	input_report_key(stm32_input_dev, KEY_STM32_WAKE_MTK, 1);
	input_sync(stm32_input_dev);
	mdelay(1);
	input_report_key(stm32_input_dev, KEY_STM32_WAKE_MTK, 0);
	input_sync(stm32_input_dev);

	enable_irq(stm32_wake_irqnr);
}

static irqreturn_t kpd_stm32_wake_eint_handler(int irq, void *dev_id)
{
	disable_irq_nosync(stm32_wake_irqnr);
	queue_work(stm32_wake_queue, &stm32_wake_work);

	return IRQ_HANDLED;
}

static int stm32_pdrv_probe(struct platform_device *pdev)
{
	int ret;

	stm32_wake_queue = create_singlethread_workqueue("stm32_key");
	INIT_WORK(&stm32_wake_work, kpd_stm32_wake_work_handler);

	stm32_wake_irqnr = irq_of_parse_and_map(pdev->dev.of_node, 0);
	ret = devm_request_irq(&pdev->dev, stm32_wake_irqnr,
	                       (irq_handler_t) kpd_stm32_wake_eint_handler,
	                       IRQ_TYPE_EDGE_RISING, "stm32_wake_eint", NULL);

	if (ret < 0) {
		dev_err(&pdev->dev, "Couldn't request an IRQ for the"
		         " STM32 wake input\n");
		return ret;
	}

	stm32_input_dev = devm_input_allocate_device(&pdev->dev);

	if (IS_ERR(stm32_input_dev)) {
		dev_err(&pdev->dev, "Failed to allocate an input device\n");
		return PTR_ERR(stm32_input_dev);
	}

	stm32_input_dev->name = STM32_NAME;
	stm32_input_dev->id.bustype = BUS_HOST;
	stm32_input_dev->dev.parent = &pdev->dev;

	input_set_capability(stm32_input_dev, EV_KEY, KEY_STM32_WAKE_MTK);

	ret = input_register_device(stm32_input_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register input device\n");
		return ret;
	}

	enable_irq_wake(stm32_wake_irqnr);
	enable_irq(stm32_wake_irqnr);

	/* reset STM32 */
	ret = aeon_gpio_set("aeon_reset_stm32_low");
	if (ret < 0)
		goto gpio_set_failed;

	mdelay(1);
	aeon_gpio_set("aeon_reset_stm32_high");
	if (ret < 0)
		goto gpio_set_failed;

	mdelay(20);
	aeon_gpio_set("aeon_reset_stm32_low");
	if (ret < 0)
gpio_set_failed:
		dev_warn(&pdev->dev, "Failed to reset STM32\n");

	dev_info(&pdev->dev, "Successfully initialized driver\n");
	return 0;
}

static int stm32_pdrv_remove(struct platform_device *pdev)
{
	disable_irq_wake(stm32_wake_irqnr);
	disable_irq(stm32_wake_irqnr);
	cancel_work_sync(&stm32_wake_work);

	input_unregister_device(stm32_input_dev);

	return 0;
}

static ssize_t download_fw_store(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count)
{
	long value;
	const char *state_name;
	int ret = kstrtol(buf, 10, &value);

	if (ret < 0)
		return ret;

	if (value > 0)
		state_name = "aeon_stm32_download_fw_high";
	else
		state_name = "aeon_stm32_download_fw_low";

	ret = aeon_gpio_set(state_name);

	if (ret < 0)
		return ret;
	else
		return count;
}

static DEVICE_ATTR_WO(download_fw);

static ssize_t wake_store(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count)
{
	long value;
	int ret = kstrtol(buf, 10, &value);

	if (ret < 0)
		return ret;

	if (value > 0) {
		ret = aeon_gpio_set("aeon_wake_stm32_low");
		if (ret < 0)
			return ret;

		mdelay(1);

		ret = aeon_gpio_set("aeon_wake_stm32_high");
		if (ret < 0)
			return ret;

		mdelay(1);

		ret = aeon_gpio_set("aeon_wake_stm32_low");
	} else
		ret = aeon_gpio_set("aeon_wake_stm32_low");

	if (ret < 0)
		return ret;
	else
		return count;
}

static DEVICE_ATTR_WO(wake);

static ssize_t reset_store(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count)
{
	long value;
	const char *state_name;
	int ret = kstrtol(buf, 10, &value);

	if (ret < 0)
		return ret;

	if (value > 0)
		state_name = "aeon_reset_stm32_high";
	else
		state_name = "aeon_reset_stm32_low";

	ret = aeon_gpio_set(state_name);

	if (ret < 0)
		return ret;
	else
		return count;
}

static DEVICE_ATTR_WO(reset);

static struct attribute *stm32_attrs[] = {
      &dev_attr_download_fw.attr,
      &dev_attr_wake.attr,
      &dev_attr_reset.attr,
      NULL,
};

ATTRIBUTE_GROUPS(stm32);

static struct platform_driver stm32_pdrv = {
	.probe = stm32_pdrv_probe,
	.remove = stm32_pdrv_remove,
	.driver = {
		.name = STM32_NAME,
		.owner = THIS_MODULE,
		.of_match_table = stm32_of_match,
		.groups = stm32_groups
	}
};

static int __init stm32_mod_init(void)
{
	int ret = platform_driver_register(&stm32_pdrv);

	if (ret < 0)
		pr_err(STM32_NAME ": Failed to register platform driver\n");
	return ret;
}

static void __exit stm32_mod_exit(void)
{
	platform_driver_unregister(&stm32_pdrv);
}

module_init(stm32_mod_init);
module_exit(stm32_mod_exit);

MODULE_AUTHOR("June Carlson <notvelleda@gmail.com>");
MODULE_DESCRIPTION("Cosmo Communicator STM32 driver");
MODULE_LICENSE("GPL");