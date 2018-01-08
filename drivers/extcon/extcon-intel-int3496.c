/*
 * Intel INT3496 ACPI device extcon driver
 *
 * Copyright (c) 2016 Hans de Goede <hdegoede@redhat.com>
 *
 * Based on android x86 kernel code which is:
 *
 * Copyright (c) 2014, Intel Corporation.
 * Author: David Cohen <david.a.cohen@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/acpi.h>
#include <linux/connection.h>
#include <linux/extcon-provider.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/usb/role.h>

#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>

#define INT3496_GPIO_USB_ID	0
#define INT3496_GPIO_VBUS_EN	1
#define INT3496_GPIO_USB_MUX	2
#define DEBOUNCE_TIME		msecs_to_jiffies(50)

struct int3496_data {
	struct device *dev;
	struct extcon_dev *edev;
	struct delayed_work work;
	struct notifier_block vbus_nb;
	struct extcon_dev *vbus_extcon;
	struct usb_role_switch *role_sw;
	struct gpio_desc *gpio_usb_id;
	struct gpio_desc *gpio_vbus_en;
	struct gpio_desc *gpio_usb_mux;
	int usb_id_irq;
};

struct int3496_vbus_extcon_info {
	const char *hid;
	int hrv;
	const char *extcon;
};

static const unsigned int int3496_cable[] = {
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static const struct acpi_gpio_params id_gpios = { INT3496_GPIO_USB_ID, 0, false };
static const struct acpi_gpio_params vbus_gpios = { INT3496_GPIO_VBUS_EN, 0, false };
static const struct acpi_gpio_params mux_gpios = { INT3496_GPIO_USB_MUX, 0, false };

static const struct acpi_gpio_mapping acpi_int3496_default_gpios[] = {
	{ "id-gpios", &id_gpios, 1 },
	{ "vbus-gpios", &vbus_gpios, 1 },
	{ "mux-gpios", &mux_gpios, 1 },
	{ },
};

static const struct x86_cpu_id cherry_trail_cpu_ids[] = {
	{ X86_VENDOR_INTEL, 6, INTEL_FAM6_ATOM_AIRMONT, X86_FEATURE_ANY },
	{}
};

/*
 * Other (PMIC) extcon provider which can give us vbus status, which we need to
 * select between USB_ROLE_NONE and USB_ROLE_DEVICE when the id-pin is high.
 */
static const struct int3496_vbus_extcon_info vbus_providers[] = {
	{ .hid = "INT33F4", .hrv = -1, .extcon = "axp288_extcon" },
	{ .hid = "INT34D3", .hrv =  3, .extcon = "cht_wcove_pwrsrc" },
};

static bool int3496_get_vbus_present(struct int3496_data *data)
{
	const unsigned int vbus_cables[] = {
		EXTCON_CHG_USB_SDP, EXTCON_CHG_USB_CDP, EXTCON_CHG_USB_DCP,
		EXTCON_CHG_USB_ACA, EXTCON_CHG_USB_FAST,
	};
	bool vbus_present = false;
	int i;

	/*
	 * If we've no way to determine vbus presence, assume it is present,
	 * so that device-mode will work except for disconnection detection,
	 * which will be delayed until another cable is plugged in.
	 */
	if (!data->vbus_extcon)
		return true;

	for (i = 0; i < ARRAY_SIZE(vbus_cables); i++) {
		if (extcon_get_state(data->vbus_extcon, vbus_cables[i]) > 0) {
			vbus_present = true;
			break;
		}
	}

	return vbus_present;
}

static void int3496_work(struct work_struct *work)
{
	struct int3496_data *data =
		container_of(work, struct int3496_data, work.work);
	int ret, id = gpiod_get_value_cansleep(data->gpio_usb_id);
	enum usb_role role;

	/* id == 1: PERIPHERAL, id == 0: HOST */
	dev_dbg(data->dev, "Connected %s cable\n", id ? "PERIPHERAL" : "HOST");

	/*
	 * Peripheral: set USB mux to peripheral and disable VBUS
	 * Host: set USB mux to host and enable VBUS
	 */
	if (!IS_ERR(data->gpio_usb_mux))
		gpiod_direction_output(data->gpio_usb_mux, id);

	if (data->role_sw) {
		if (id == 0)
			role = USB_ROLE_HOST;
		else if (int3496_get_vbus_present(data))
			role = USB_ROLE_DEVICE;
		else
			role = USB_ROLE_NONE;

		ret = usb_role_switch_set_role(data->role_sw, role);
		if (ret)
			dev_err(data->dev, "Error setting role: %d\n", ret);
	}

	if (!IS_ERR(data->gpio_vbus_en))
		gpiod_direction_output(data->gpio_vbus_en, !id);

	extcon_set_state_sync(data->edev, EXTCON_USB_HOST, !id);
}

static irqreturn_t int3496_thread_isr(int irq, void *priv)
{
	struct int3496_data *data = priv;

	/* Let the pin settle before processing it */
	mod_delayed_work(system_wq, &data->work, DEBOUNCE_TIME);

	return IRQ_HANDLED;
}

static int int3496_vbus_extcon_evt(struct notifier_block *nb,
				   unsigned long event, void *param)
{
	struct int3496_data *data =
		container_of(nb, struct int3496_data, vbus_nb);

	queue_delayed_work(system_wq, &data->work, 0);

	return NOTIFY_OK;
}

static int int3496_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct int3496_data *data;
	int i, ret;

	ret = devm_acpi_dev_add_driver_gpios(dev, acpi_int3496_default_gpios);
	if (ret) {
		dev_err(dev, "can't add GPIO ACPI mapping\n");
		return ret;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	INIT_DELAYED_WORK(&data->work, int3496_work);
	data->vbus_nb.notifier_call = int3496_vbus_extcon_evt;

	data->role_sw = usb_role_switch_get(dev);
	if (IS_ERR(data->role_sw))
		return PTR_ERR(data->role_sw);

	/* See comment above vbus_providers[] declaration */
	for (i = 0 ; i < ARRAY_SIZE(vbus_providers); i++) {
		if (!acpi_dev_present(vbus_providers[i].hid, NULL,
				      vbus_providers[i].hrv))
			continue;

		data->vbus_extcon = extcon_get_extcon_dev(
						vbus_providers[i].extcon);
		if (data->vbus_extcon == NULL)
			return -EPROBE_DEFER;

		dev_info(dev, "Using extcon '%s' for vbus-valid\n",
			 vbus_providers[i].extcon);
		break;
	}

	data->gpio_usb_id = devm_gpiod_get(dev, "id", GPIOD_IN);
	if (IS_ERR(data->gpio_usb_id)) {
		ret = PTR_ERR(data->gpio_usb_id);
		dev_err(dev, "can't request USB ID GPIO: %d\n", ret);
		goto out_unregister_role_sw;
	} else if (gpiod_get_direction(data->gpio_usb_id) != GPIOF_DIR_IN) {
		dev_warn(dev, FW_BUG "USB ID GPIO not in input mode, fixing\n");
		gpiod_direction_input(data->gpio_usb_id);
	}

	data->usb_id_irq = gpiod_to_irq(data->gpio_usb_id);
	if (data->usb_id_irq < 0) {
		dev_err(dev, "can't get USB ID IRQ: %d\n", data->usb_id_irq);
		ret = data->usb_id_irq;
		goto out_unregister_role_sw;
	}

	data->gpio_vbus_en = devm_gpiod_get(dev, "vbus", GPIOD_ASIS);
	if (IS_ERR(data->gpio_vbus_en))
		dev_info(dev, "can't request VBUS EN GPIO\n");

	data->gpio_usb_mux = devm_gpiod_get(dev, "mux", GPIOD_ASIS);
	if (IS_ERR(data->gpio_usb_mux))
		dev_info(dev, "can't request USB MUX GPIO\n");

	/* register extcon device */
	data->edev = devm_extcon_dev_allocate(dev, int3496_cable);
	if (IS_ERR(data->edev)) {
		ret = -ENOMEM;
		goto out_unregister_role_sw;
	}

	ret = devm_extcon_dev_register(dev, data->edev);
	if (ret < 0) {
		dev_err(dev, "can't register extcon device: %d\n", ret);
		goto out_unregister_role_sw;
	}

	ret = devm_request_threaded_irq(dev, data->usb_id_irq,
					NULL, int3496_thread_isr,
					IRQF_SHARED | IRQF_ONESHOT |
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING,
					dev_name(dev), data);
	if (ret < 0) {
		dev_err(dev, "can't request IRQ for USB ID GPIO: %d\n", ret);
		goto out_unregister_role_sw;
	}

	if (data->vbus_extcon) {
		ret = devm_extcon_register_notifier_all(dev, data->vbus_extcon,
							&data->vbus_nb);
		if (ret) {
			dev_err(dev, "Error registering notifier: %d\n", ret);
			goto out_unregister_role_sw;
		}
	}

	/* queue initial processing of id-pin */
	queue_delayed_work(system_wq, &data->work, 0);

	platform_set_drvdata(pdev, data);

	return 0;

out_unregister_role_sw:
	if (data->role_sw)
		usb_role_switch_put(data->role_sw);

	return ret;
}

static int int3496_remove(struct platform_device *pdev)
{
	struct int3496_data *data = platform_get_drvdata(pdev);

	devm_free_irq(&pdev->dev, data->usb_id_irq, data);
	cancel_delayed_work_sync(&data->work);

	if (data->role_sw)
		usb_role_switch_put(data->role_sw);

	return 0;
}

static const struct acpi_device_id int3496_acpi_match[] = {
	{ "INT3496" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, int3496_acpi_match);

static struct platform_driver int3496_driver = {
	.driver = {
		.name = "intel-int3496",
		.acpi_match_table = int3496_acpi_match,
	},
	.probe = int3496_probe,
	.remove = int3496_remove,
};

static struct devcon int3496_role_sw_conn = {
	.endpoint[0] = "INT3496:00",
	.endpoint[1] = "intel_cht_usb_sw-role-switch",
	.id = "usb-role-switch",
};

static int __init int3496_init(void)
{
	if (x86_match_cpu(cherry_trail_cpu_ids))
		add_device_connection(&int3496_role_sw_conn);

	return platform_driver_register(&int3496_driver);
}
module_init(int3496_init);

static void __exit int3496_exit(void)
{
	if (x86_match_cpu(cherry_trail_cpu_ids))
		remove_device_connection(&int3496_role_sw_conn);

	platform_driver_unregister(&int3496_driver);
}
module_exit(int3496_exit);

MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_DESCRIPTION("Intel INT3496 ACPI device extcon driver");
MODULE_LICENSE("GPL");
