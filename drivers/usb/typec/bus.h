#ifndef __USB_TYPEC_ALTMODE_H__
#define __USB_TYPEC_ALTMODE_H__

#include <linux/usb/typec.h>

struct bus_type;

struct altmode {
	unsigned int			id;
	struct typec_altmode		adev;

	enum typec_port_type		roles;

	struct attribute		*attrs[5];
	char				group_name[6];
	struct attribute_group		group;
	const struct attribute_group	*groups[2];

	struct altmode			*partner;
	struct altmode			*plug[2];
	const struct typec_altmode_ops	*ops;

	struct blocking_notifier_head	nh;
};

#define to_altmode(d) container_of(d, struct altmode, adev)

extern struct class *typec_class;
extern struct bus_type typec_bus;
extern const struct device_type typec_altmode_dev_type;
extern const struct device_type typec_port_dev_type;

#define is_typec_altmode(_dev_) (_dev_->type == &typec_altmode_dev_type)
#define is_typec_port(_dev_) (_dev_->type == &typec_port_dev_type)

int typec_enter_mode(struct altmode *alt, bool enter);

#endif /* __USB_TYPEC_ALTMODE_H__ */
