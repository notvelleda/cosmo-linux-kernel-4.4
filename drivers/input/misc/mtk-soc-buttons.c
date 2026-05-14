/*
 * Driver for the buttons handled by the SOC (i.e. not the keyboard) on the
 * Cosmo Communicator
 *
 * Copyright (C) 2010 Terry Chang <terry.chang@mediatek.com>
 * Copyright (C) 2018? yucong.xiong <yucong.xiong@mediatek.com>
 * Copyright (C) 2026 June Carlson <notvelleda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <mt-plat/mtk_boot_common.h>
#include <mt-plat/sync_write.h>
#include <mt-plat/upmu_common.h>

#define KPD_NAME "mtk-soc-buttons"

/* used to determine whether to send KEY_ESC or KEY_POWER when the power key is
 * pressed
 */
extern struct input_dev *aw9523_kpd_input_dev;
extern bool aw9523MetaKeyPressed;

static struct input_dev *kpd_input_dev = NULL;

static unsigned int stm32_key_irqnr;
static struct workqueue_struct *stm32_wake_queue = NULL;
static struct work_struct stm32_wake_work;

static unsigned int finger_key_irqnr;
static struct workqueue_struct *finger_key_queue = NULL;
static struct work_struct finger_key_work;

static const struct of_device_id kpd_of_match[] = {
	{.compatible = "mediatek,kp"},
	{},
};

/* these two functions are called by the PMIC driver */
void kpd_pwrkey_pmic_handler(unsigned long pressed)
{
	static bool power_key_was_pressed = false;

	if (kpd_input_dev == NULL)
		return;

	if (aw9523MetaKeyPressed || power_key_was_pressed) {
		pr_info(KPD_NAME ": KEY_POWER");

		input_report_key(kpd_input_dev, KEY_POWER, pressed);
		input_sync(kpd_input_dev);

		power_key_was_pressed = pressed;
	} else {
		pr_info(KPD_NAME ": KEY_ESC");

		input_report_key(aw9523_kpd_input_dev, KEY_ESC, pressed);
		input_sync(aw9523_kpd_input_dev);
	}
}

void kpd_rstkey_pmic_handler(unsigned long pressed)
{
	if (kpd_input_dev == NULL)
		return;

	input_report_key(kpd_input_dev, BTN_LEFT, pressed);
	input_sync(kpd_input_dev);
}

static void kpd_stm32_wake_work_handler(struct work_struct *work)
{
	input_report_key(kpd_input_dev, KEY_STM32_WAKE_MTK, 1);
	input_sync(kpd_input_dev);	
	mdelay(1);
	input_report_key(kpd_input_dev, KEY_STM32_WAKE_MTK, 0);
	input_sync(kpd_input_dev);

	enable_irq(stm32_key_irqnr);
}

static irqreturn_t kpd_stm32_wake_eint_handler(int irq, void *dev_id)
{
	disable_irq_nosync(stm32_key_irqnr);
	queue_work(stm32_wake_queue, &stm32_wake_work);

	return IRQ_HANDLED;
}

static void kpd_finger_key_work_handler(struct work_struct * work)
{
	static bool key_state = true;

	key_state = !key_state;

	input_report_key(kpd_input_dev, BTN_RIGHT, !key_state);
	input_sync(kpd_input_dev);

	if (key_state) {
		mdelay(1);
		irq_set_irq_type(finger_key_irqnr, IRQ_TYPE_LEVEL_LOW);
	} else
		irq_set_irq_type(finger_key_irqnr, IRQ_TYPE_LEVEL_HIGH);

	enable_irq(finger_key_irqnr);
}

static irqreturn_t kpd_finger_key_eint_handler(int irq, void *dev_id)
{
	disable_irq_nosync(finger_key_irqnr);
	queue_work(finger_key_queue, &finger_key_work);

	return IRQ_HANDLED;
}

static void init_stm32_eint(struct device *dev)
{
	struct device_node *node;
	int ret;

	stm32_wake_queue = create_singlethread_workqueue("stm32_key");
	INIT_WORK(&stm32_wake_work, kpd_stm32_wake_work_handler);

	node = of_find_compatible_node(NULL, NULL, "mediatek, STM32_KEY-eint");

	if (IS_ERR_OR_NULL(node)) {
		dev_warn(dev, "Couldn't find the STM32 wake input IRQ"
		         " number\n");
		return;
	}

	stm32_key_irqnr = irq_of_parse_and_map(node, 0);
	ret = request_irq(stm32_key_irqnr,
	                  (irq_handler_t) kpd_stm32_wake_eint_handler,
	                  IRQ_TYPE_EDGE_RISING, "stm32_wake_eint", NULL);

	if (ret < 0) {
		dev_err(dev, "Couldn't request an IRQ for the"
		         " STM32 wake input\n");
		return;
	}

	input_set_capability(kpd_input_dev, EV_KEY, KEY_STM32_WAKE_MTK);

	enable_irq_wake(stm32_key_irqnr);
	enable_irq(stm32_key_irqnr);
}

static void init_finger_eint(struct device *dev)
{
	struct device_node *node;
	int ret;

	finger_key_queue = create_singlethread_workqueue("finger_key");
	INIT_WORK(&finger_key_work, kpd_finger_key_work_handler);

	node = of_find_compatible_node(NULL, NULL,
	                               "mediatek, FINGER_KEY-eint");

	if (IS_ERR_OR_NULL(node)) {
		dev_warn(dev, "Couldn't find the \"finger key\" IRQ number\n");
	}

	finger_key_irqnr = irq_of_parse_and_map(node, 0);
	ret = request_irq(finger_key_irqnr,
	                  (irq_handler_t) kpd_finger_key_eint_handler,
	                  IRQ_TYPE_LEVEL_LOW, "finger_key_eint", NULL);

	if (ret < 0) {
		dev_err(dev, "Couldn't request an IRQ for the"
		         " \"finger key\"\n");
		return;
	}

	input_set_capability(kpd_input_dev, EV_KEY, BTN_RIGHT);
	enable_irq(finger_key_irqnr);
}

static int kpd_pdrv_probe(struct platform_device *pdev)
{
	int ret = 0;
	void __iomem *kp_base;

	/* map keypad I/O so that it can be disabled */
	kp_base = of_iomap(pdev->dev.of_node, 0);

	if (IS_ERR(kp_base))
		dev_warn(&pdev->dev, "Failed to map keypad I/O\n");
	else
		mt_reg_sync_writew(0, kp_base + 0x0024); /* KP_EN */

	/* initialize and register input device (/dev/input/eventX) */
	kpd_input_dev = devm_input_allocate_device(&pdev->dev);

	if (IS_ERR(kpd_input_dev)) {
		dev_err(&pdev->dev, "Failed to allocate an input device\n");
		return PTR_ERR(kpd_input_dev);
	}

	kpd_input_dev->name = KPD_NAME;
	kpd_input_dev->id.bustype = BUS_HOST;
	kpd_input_dev->id.vendor = 0x2454;
	kpd_input_dev->id.product = 0x6500;
	kpd_input_dev->id.version = 0x0010;
	kpd_input_dev->dev.parent = &pdev->dev;

	/* there's no way to detect presence for these, we just have to hope
	 * that the PMIC will call our handler functions
	 */
	input_set_capability(kpd_input_dev, EV_KEY, KEY_POWER);
	input_set_capability(kpd_input_dev, EV_KEY, BTN_LEFT);

	ret = input_register_device(kpd_input_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register input device\n");
		return ret;
	}

	init_stm32_eint(&pdev->dev);
	init_finger_eint(&pdev->dev);

	if (get_boot_mode() == NORMAL_BOOT) {
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x00);
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_TD,
		                        CONFIG_KPD_PMIC_LPRST_TD);
	} else {
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_EN, 0x01);
		pmic_set_register_value(PMIC_RG_HOMEKEY_RST_EN, 0x00);
		pmic_set_register_value(PMIC_RG_PWRKEY_RST_TD,
		                        CONFIG_KPD_PMIC_LPRST_TD);
	}

	dev_info(&pdev->dev, "Successfully initialized driver\n");
	return 0;
}

static struct platform_driver kpd_pdrv = {
	.probe = kpd_pdrv_probe,
	.driver = {
		.name = KPD_NAME,
		.owner = THIS_MODULE,
		.of_match_table = kpd_of_match,
	},
};

static int __init kpd_mod_init(void)
{
	int ret = platform_driver_register(&kpd_pdrv);

	if (ret < 0)
		pr_err("Failed to register keypad platform driver\n");
	return ret;
}

static void __exit kpd_mod_exit(void)
{
	platform_driver_unregister(&kpd_pdrv);
}

module_init(kpd_mod_init);
module_exit(kpd_mod_exit);

MODULE_AUTHOR("June Carlson <notvelleda@gmail.com>");
MODULE_DESCRIPTION("MT6771 built in buttons driver");
MODULE_LICENSE("GPL");