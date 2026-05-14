/*
 * Driver for the hall effect lid sensor on the Cosmo Communicator
 *
 * Copyright (C) 2010 Terry Chang <terry.chang@mediatek.com>
 * Copyright (C) 2018? yucong.xiong <yucong.xiong@mediatek.com>
 * Copyright (C) 2026 June Carlson <notvelleda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <soc/mediatek/hall.h>

#define HALL_NAME "mtk-lid-sensor"

static const struct of_device_id hall_of_match[] = {
	{.compatible = "mediatek, hall-eint"},
	{},
};

/* set in hall_pdrv_probe() and not modified afterwards, any code that reads
 * these will not run until after that function completes
 */
static int gpio_pin, irq_num;

/* this is set in hall_work_handler(), there's no need to lock it since the IRQ
 * is immediately disabled when its handler is called
 */
static int last_gpio_state;

static struct work_struct irq_work;
static struct workqueue_struct *irq_workqueue;

static struct input_dev *hall_input_dev;

static BLOCKING_NOTIFIER_HEAD(hall_notifier_list);

/**
 *      hall_register_client - register a client notifier
 *      @nb: notifier block to callback on events
 */
int hall_register_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&hall_notifier_list, nb);
}
EXPORT_SYMBOL(hall_register_client);

/**
 *      hall_unregister_client - unregister a client notifier
 *      @nb: notifier block to callback on events
 */
int hall_unregister_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&hall_notifier_list, nb);
}
EXPORT_SYMBOL(hall_unregister_client);

/**
 * hall_notifier_call_chain - notify clients of lid switch events
 *
 */
int hall_notifier_call_chain(unsigned long val, void *v)
{
	return blocking_notifier_call_chain(&hall_notifier_list, val, v);
}
EXPORT_SYMBOL_GPL(hall_notifier_call_chain);

static void hall_work_handler(struct work_struct *work)
{
	static bool is_first_call = true;
	int gpio_state = gpio_get_value(gpio_pin);

	/* switch the IRQ type so that the opposite edge of the signal will be
	 * captured next
	 */
	if (gpio_state)
		irq_set_irq_type(irq_num, IRQ_TYPE_LEVEL_LOW);
	else
		irq_set_irq_type(irq_num, IRQ_TYPE_LEVEL_HIGH);

	if (last_gpio_state != gpio_state || is_first_call) {
		hall_notifier_call_chain(gpio_state, NULL);

		input_report_switch(hall_input_dev, SW_LID, !gpio_state);
		input_sync(hall_input_dev);

		pr_info(HALL_NAME ": Lid state is %s\n",
		        gpio_state ? "open" : "closed");
	}

	is_first_call = false;
	last_gpio_state = gpio_state;

	enable_irq(irq_num);
}

static irqreturn_t hall_input_irq_handler(int irq, void *dev_id)
{
	/* use _nosync to avoid deadlock */
	disable_irq_nosync(irq_num);
	queue_work(irq_workqueue, &irq_work);

	return IRQ_HANDLED;
}

static int hall_pdrv_probe(struct platform_device *pdev)
{
	int ret;
	struct device_node *node = NULL;

	irq_workqueue = create_singlethread_workqueue("hall_irq");
	INIT_WORK(&irq_work, hall_work_handler);

	node = of_find_matching_node(node, hall_of_match);
	if (node == NULL) {
		dev_err(&pdev->dev, "Couldn't find matching device node\n");
		return PTR_ERR(node);
	}

	gpio_pin = of_get_named_gpio(node, "deb-gpios", 0);
	if (gpio_pin < 0) {
		dev_err(&pdev->dev, "Couldn't find the GPIO pin for this"
		        " device\n");
		return gpio_pin;
	}

	irq_num = irq_of_parse_and_map(node, 0);
	ret = request_irq(irq_num, (irq_handler_t) hall_input_irq_handler,
	                  IRQF_TRIGGER_NONE, "hall-eint", NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to request IRQ\n");
		return ret;
	}

	hall_input_dev = devm_input_allocate_device(&pdev->dev);

	if (IS_ERR(hall_input_dev)) {
		dev_err(&pdev->dev, "Failed to allocate an input device\n");
		return PTR_ERR(hall_input_dev);
	}

	hall_input_dev->name = HALL_NAME;
	hall_input_dev->id.bustype = BUS_HOST;
	hall_input_dev->dev.parent = &pdev->dev;

	input_set_capability(hall_input_dev, EV_SW, SW_LID);

	ret = input_register_device(hall_input_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register input device\n");
		return ret;
	}

	last_gpio_state = gpio_get_value(gpio_pin);
	queue_work(irq_workqueue, &irq_work);

	dev_info(&pdev->dev, "Successfully initialized driver\n");
	return 0;
}

static int hall_pdrv_remove(struct platform_device *pdev)
{
	cancel_work_sync(&irq_work);
	disable_irq(irq_num);
	return 0;
}

static int hall_pdrv_suspend(struct platform_device *pdev, pm_message_t state)
{
	cancel_work_sync(&irq_work);
	disable_irq(irq_num);
	return 0;
}

static int hall_pdrv_resume(struct platform_device *pdev)
{
	queue_work(irq_workqueue, &irq_work);
	return 0;
}

static struct platform_driver hall_pdrv = {
	.probe = hall_pdrv_probe,
	.remove = hall_pdrv_remove,
	.suspend = hall_pdrv_suspend,
	.resume = hall_pdrv_resume,
	.driver = {
		.name = HALL_NAME,
		.owner = THIS_MODULE,
		.of_match_table = hall_of_match
	}
};

static int __init hall_mod_init(void)
{
	int ret = platform_driver_register(&hall_pdrv);

	if (ret)
		pr_err("Failed to register lid sensor platform driver\n");
	return 0;
}

static void __exit hall_mod_exit(void)
{
	platform_driver_unregister(&hall_pdrv);
}

module_init(hall_mod_init);
module_exit(hall_mod_exit);

MODULE_AUTHOR("June Carlson <notvelleda@gmail.com>");
MODULE_DESCRIPTION("Hall effect lid sensor driver");
MODULE_LICENSE("GPL");