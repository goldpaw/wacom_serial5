/*
 * Wacom protocol 4 serial tablet driver
 *
 * To do:
 *  - support pad buttons;
 *  - support (protocol 4-style) tilt;
 *  - support suppress;
 *  - support Graphire relative wheel.
 *
 * Sections I have been unable to test personally due to lack of available
 * hardware are marked UNTESTED.  Much of what is marked UNTESTED comes from
 * reading the wcmSerial code in linuxwacom 0.9.0.
 *
 * This driver was developed with reference to much code written by others,
 * particularly:
 *  - elo, gunze drivers by Vojtech Pavlik <vojtech@ucw.cz>;
 *  - wacom_w8001 driver by Jaya Kumar <jayakumar.lkml@gmail.com>;
 *  - the USB wacom input driver, credited to many people
 *    (see drivers/input/tablet/wacom.h);
 *  - new and old versions of linuxwacom / xf86-input-wacom credited to
 *    Frederic Lepied, France. <Lepied@XFree86.org> and
 *    Ping Cheng, Wacom. <pingc@wacom.com>;
 *  - and xf86wacom.c (a presumably ancient version of the linuxwacom code), by
 *    Frederic Lepied and Raph Levien <raph@gtk.org>.
 */

/* XXX To be removed before (widespread) release. */
#define DEBUG

#include <linux/string.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/slab.h>
#include <linux/completion.h>

/* XXX To be removed before (widespread) release. */
#ifndef SERIO_WACOM_IV
#define SERIO_WACOM_IV 0x3d
#endif

#define DRIVER_AUTHOR	"Julian Squires <julian@cipht.net>"
#define DEVICE_NAME	"Wacom protocol 4 serial tablet"
#define DRIVER_DESC	DEVICE_NAME " driver"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define REQUEST_MODEL_AND_ROM_VERSION	"~#\r"
#define REQUEST_MAX_COORDINATES		"~C\r"
#define REQUEST_CONFIGURATION_STRING	"~R\r"
#define REQUEST_RESET_TO_PROTOCOL_IV	"\r#\r"

#define COMMAND_START_SENDING_PACKETS		"ST\r"
#define COMMAND_STOP_SENDING_PACKETS		"SP\r"
#define COMMAND_MULTI_MODE_INPUT		"MU1\r"
#define COMMAND_ORIGIN_IN_UPPER_LEFT		"OC1\r"
#define COMMAND_ENABLE_ALL_MACRO_BUTTONS	"~M0\r"
#define COMMAND_DISABLE_GROUP_1_MACRO_BUTTONS	"~M1\r"
#define COMMAND_TRANSMIT_AT_MAX_RATE		"IT0\r"
#define COMMAND_DISABLE_INCREMENTAL_MODE	"IN0\r"
#define COMMAND_ENABLE_CONTINUOUS_MODE		"SR\r"
#define COMMAND_ENABLE_PRESSURE_MODE		"PH1\r"
#define COMMAND_Z_FILTER			"ZF1\r"

/* Note that this is a protocol 4 packet without tilt information. */
#define PACKET_LENGTH 9 //TODO: was originally 7 -- should also be 9 for protocol4 with tilt?

/* device IDs from wacom_wac.h */
#define STYLUS_DEVICE_ID	0x02
#define TOUCH_DEVICE_ID         0x03
#define CURSOR_DEVICE_ID        0x06
#define ERASER_DEVICE_ID        0x0A
#define PAD_DEVICE_ID           0x0F

#define TILT_SIGN_BIT   0x40
#define TILT_BITS       0x3F
#define PROXIMITY_BIT   0x40




#define PEN(id)         ((id & 0x07ff) == 0x0022 || \
                         (id & 0x07ff) == 0x0042 || \
                         (id & 0x07ff) == 0x0052)
#define STROKING_PEN(id) ((id & 0x07ff) == 0x0032)
#define AIRBRUSH(id)     ((id & 0x07ff) == 0x0112)
#define MOUSE_4D(id)     ((id & 0x07ff) == 0x0094)
#define MOUSE_2D(id)     ((id & 0x07ff) == 0x0007)
#define LENS_CURSOR(id)  ((id & 0x07ff) == 0x0096)
#define INKING_PEN(id)   ((id & 0x07ff) == 0x0012)
#define STYLUS_TOOL(id)  (PEN(id) || STROKING_PEN(id) || INKING_PEN(id) || \
                         AIRBRUSH(id))
#define CURSOR_TOOL(id)  (MOUSE_4D(id) || LENS_CURSOR(id) || MOUSE_2D(id))


struct wacom {
	struct input_dev *dev;
	struct completion cmd_done;
	int extra_z_bits;
	int idx;
	unsigned char data[32];
	char phys[32];
};


enum {
	MODEL_CINTIQ		= 0x504C, /* PL */
	MODEL_CINTIQ2		= 0x4454, /* DT */
	MODEL_DIGITIZER_II	= 0x5544, /* UD */
	MODEL_GRAPHIRE		= 0x4554, /* ET */
	MODEL_INTUOS		= 0x4744, /* GD */
	MODEL_INTUOS2		= 0x5844, /* XD */
	MODEL_PENPARTNER	= 0x4354, /* CT */
	MODEL_UNKNOWN		= 0
};

static void handle_model_response(struct wacom *wacom)
{
	int major_v, minor_v, max_z;
	char *p;

	dev_dbg(&wacom->dev->dev, "Model string: %s\n", wacom->data);

	major_v = minor_v = 0;
	p = strrchr(wacom->data, 'V');
	if (p)
		sscanf(p+1, "%u.%u", &major_v, &minor_v);

	switch (wacom->data[2] << 8 | wacom->data[3]) {
	case MODEL_INTUOS:
	case MODEL_INTUOS2: /* Intuos 2 is UNTESTED */
		p = "Intuos";
		wacom->dev->id.version = MODEL_INTUOS;
		wacom->extra_z_bits = 3;
		break;
	case MODEL_CINTIQ:	/* UNTESTED */
	case MODEL_CINTIQ2:
		p = "Cintiq";
		wacom->dev->id.version = MODEL_CINTIQ;
		switch (wacom->data[5]<<8 | wacom->data[6]) {
		case 0x3731: /* PL-710 */
			/* wcmSerial sets res to 2540x2540 in this case. */
			/* fall through */
		case 0x3535: /* PL-550 */
		case 0x3830: /* PL-800 */
			wacom->extra_z_bits = 2;
		}
		break;
	case MODEL_PENPARTNER:	/* UNTESTED */
		p = "Penpartner";
		wacom->dev->id.version = MODEL_PENPARTNER;
		/* wcmSerial sets res 1000x1000 in this case. */
		break;
	case MODEL_GRAPHIRE:	/* UNTESTED */
		p = "Graphire";
		wacom->dev->id.version = MODEL_GRAPHIRE;
		/* UNTESTED: Apparently Graphire models do not answer coordinate
		   requests; see also wacom_setup(). */
		input_set_abs_params(wacom->dev, ABS_X, 0, 5103, 0, 0);
		input_set_abs_params(wacom->dev, ABS_Y, 0, 3711, 0, 0);
		input_abs_set_res(wacom->dev, ABS_X, 1016);
		input_abs_set_res(wacom->dev, ABS_Y, 1016);
		wacom->extra_z_bits = 2;
		break;
	case MODEL_DIGITIZER_II:
		p = "Digitizer II";
		wacom->dev->id.version = MODEL_DIGITIZER_II;
		if (major_v == 1 && minor_v <= 2)
			wacom->extra_z_bits = 0; /* UNTESTED */
		break;
	default:		/* UNTESTED */
		dev_dbg(&wacom->dev->dev, "Didn't understand Wacom model "
			                  "string: %s\n", wacom->data);
		p = "Unknown Protocol IV";
		wacom->dev->id.version = MODEL_UNKNOWN;
		break;
	}
	max_z = (1<<(7+wacom->extra_z_bits))-1;
	dev_info(&wacom->dev->dev, "Wacom tablet: %s, version %u.%u\n", p,
		 major_v, minor_v);
	dev_dbg(&wacom->dev->dev, "Max pressure: %d.\n", max_z);
	input_set_abs_params(wacom->dev, ABS_PRESSURE, 0, max_z, 0, 0);
	input_set_abs_params(wacom->dev, ABS_TILT_X,
					-(TILT_BITS + 1), TILT_BITS, 0, 0);
	input_set_abs_params(wacom->dev, ABS_TILT_Y,
					-(TILT_BITS + 1), TILT_BITS, 0, 0);
}


static void handle_configuration_response(struct wacom *wacom)
{
	int x, y, skip;

	dev_dbg(&wacom->dev->dev, "Configuration string: %s\n", wacom->data);
	sscanf(wacom->data, REQUEST_CONFIGURATION_STRING
			"%x,%u,%u,%u,%u", &skip, &skip, &skip, &x, &y);
	input_abs_set_res(wacom->dev, ABS_X, x);
	input_abs_set_res(wacom->dev, ABS_Y, y);
}

static void handle_coordinates_response(struct wacom *wacom)
{
	int x, y;

	dev_dbg(&wacom->dev->dev, "Coordinates string: %s\n", wacom->data);
	sscanf(wacom->data, REQUEST_MAX_COORDINATES"%u,%u", &x, &y);
	input_set_abs_params(wacom->dev, ABS_X, 0, x, 0, 0);
	input_set_abs_params(wacom->dev, ABS_Y, 0, y, 0, 0);
}

static void handle_response(struct wacom *wacom)
{
	if (wacom->data[0] != '~' || wacom->idx < 2) {
		dev_dbg(&wacom->dev->dev, "got a garbled response of length "
			                  "%d.\n", wacom->idx);
		return;
	}

	switch (wacom->data[1]) {
	case '#':
		handle_model_response(wacom);
		break;
	case 'R':
		handle_configuration_response(wacom);
		break;
	case 'C':
		handle_coordinates_response(wacom);
		break;
	default:
		dev_dbg(&wacom->dev->dev, "got an unexpected response: %s\n",
			wacom->data);
		break;
	}

	complete(&wacom->cmd_done);
}

static void handle_packet(struct wacom *wacom)
{
	int in_proximity_p, stylus_p, button, x, y, z;
	int device;

	in_proximity_p = wacom->data[0] & 0x40;
	stylus_p = wacom->data[0] & 0x20;
	button = (wacom->data[3] & 0x78) >> 3;
	x = (wacom->data[0] & 3) << 14 | wacom->data[1]<<7 | wacom->data[2];
	y = (wacom->data[3] & 3) << 14 | wacom->data[4]<<7 | wacom->data[5];
	z = wacom->data[6] & 0x7f;
	if(wacom->extra_z_bits >= 1)
		z = z << 1 | (wacom->data[3] & 0x4) >> 2;
	if(wacom->extra_z_bits > 1)
		z = z << 1 | (wacom->data[0] & 0x4);
	z = z ^ (0x40 << wacom->extra_z_bits);

	device = stylus_p ? STYLUS_DEVICE_ID : CURSOR_DEVICE_ID;
	/* UNTESTED Graphire eraser (according to old wcmSerial code) */
	if (button & 8)
		device = ERASER_DEVICE_ID;
	input_report_key(wacom->dev, ABS_MISC, device);
	input_report_abs(wacom->dev, ABS_X, x);
	input_report_abs(wacom->dev, ABS_Y, y);
	input_report_abs(wacom->dev, ABS_PRESSURE, z);
	input_report_key(wacom->dev, BTN_TOOL_MOUSE, in_proximity_p &&
			                             !stylus_p);
	input_report_key(wacom->dev, BTN_TOOL_RUBBER, in_proximity_p &&
			                              stylus_p && button&4);
	input_report_key(wacom->dev, BTN_TOOL_PEN, in_proximity_p && stylus_p &&
                                                   !(button&4));
	input_report_key(wacom->dev, BTN_TOUCH, button & 1);
	input_report_key(wacom->dev, BTN_STYLUS, button & 2);
	input_sync(wacom->dev);
}

static void send_position5(struct wacom *wacom) {
	int x, y;
	unsigned char *data = wacom->data;

	x = ((data[1] & 0x7f) << 9) |
	    ((data[2] & 0x7f) << 2) |
	    ((data[3] & 0x60) >> 5);
	y = ((data[3] & 0x1f) << 11) |
	    ((data[4] & 0x7f) <<  4) |
	    ((data[5] & 0x78) >>  3);
	input_report_abs(wacom->dev, ABS_X, x);
	input_report_abs(wacom->dev, ABS_Y, y);
}

#define REPORT_KEY(wacom, buttons, bit, code) (\
		input_report_key((wacom)->dev, (code), \
		((buttons) & (1 << (bit))) >> (bit)))
static void send_buttons5(struct wacom *wacom, int buttons, int device) {
	/* Reversed mappings of buttonmask to button codes.
	 * Found in wcmUSB.c of xf86-input-wacom, commit 
	 * df3c61392f82dac42a04aadc5ae79daff720d3c4 
	 *
	 * bit	event code
	 * 0	BTN_LEFT
	 * 1	BTN_STYLUS or BTN_MIDDLE
	 * 2	BTN_STYLUS2 or BTN_RIGHT
	 * 3	BTN_SIDE
	 * 4	BTN_EXTRA
	 * not too sure of these:
	 * 5	BTN_FORWARD
	 * 6	BTN_BACK
	 * 7	BTN_TASK
	 *
	 * Note that we are doing "double" work here, as the wacom driver 
	 * will make the reverse transformation.
	 * We could probably in-line this so we only look at the relevant 
	 * bits that aren't masked anyway, but that leads to a bit of code 
	 * duplication. Let's hope the compiler is smart enough to do that 
	 * automagically.
	 */
	REPORT_KEY(wacom, buttons, 0, BTN_LEFT);
	REPORT_KEY(wacom, buttons, 1, (device == CURSOR_DEVICE_ID ?
						BTN_MIDDLE : BTN_STYLUS));
	REPORT_KEY(wacom, buttons, 2, (device == CURSOR_DEVICE_ID ?
						BTN_RIGHT : BTN_STYLUS2));
	REPORT_KEY(wacom, buttons, 3, BTN_SIDE);
	REPORT_KEY(wacom, buttons, 4, BTN_EXTRA);
	REPORT_KEY(wacom, buttons, 5, BTN_FORWARD);
	REPORT_KEY(wacom, buttons, 6, BTN_BACK);
	REPORT_KEY(wacom, buttons, 7, BTN_TASK);
}

static void handle_packet5(struct wacom *wacom)
{
	int proximity, buttons, abswheel, relwheel, rotation, throttle;
	int z, tiltx, tilty;
	static int device_id, device;
	static int discard_first = 0;

	unsigned char *data = wacom->data;


	if (data[0] & 1)
		/* Channels can be implemented by sending serial numbers to 
		 * wacom driver? */
		dev_info(&wacom->dev->dev,
				"Received something from second channel. "
				"Not implemented yet -> Dropped!\n");

	/* Device ID packet */
	if ((data[0] & 0xfc) == 0xc0) {
		proximity = 1;

		device_id = ((data[1] & 0x7f) << 5) | ((data[2] & 0x7c) >> 2);
		
		device = (STYLUS_TOOL(device_id) ? STYLUS_DEVICE_ID :
			  CURSOR_TOOL(device_id) ? CURSOR_DEVICE_ID :
						   ERASER_DEVICE_ID);

		input_report_key(wacom->dev, ABS_MISC, device);
		
		//discard_first = (device_id & 0xf06) != 0x802;
		/* TODO: I don't really know why this was there in the old 
		 * code -- doesn't seem to work with my intuos mouse 
		 * (whenever the mouse sends its ID, this will stay 1, but 
		 * my mouse only sends packets of the first kind) */
	}

	/* Out of proximity packet */
	else if ((data[0] & 0xfe) == 0x80)
	{
		proximity = 0;
	}

	/* General pen packet or eraser packet or airbrush first packet
	 * airbrush second packet */
	else if (((data[0] & 0xb8) == 0xa0) ||
			((data[0] & 0xbe) == 0xb4))
	{
		send_position5(wacom);

		if ((data[0] & 0xb8) == 0xa0)
		{
			z = (((data[5] & 0x07) << 7) | (data[6] & 0x7f));
			buttons = (data[0] & 0x06);
			send_buttons5(wacom, buttons, device);
		}
		else
		{
			/* 10 bits for absolute wheel position */
			abswheel = (((data[5] & 0x07) << 7) |
				(data[6] & 0x7f));
		}
		tiltx = (data[7] & TILT_BITS);
		tilty = (data[8] & TILT_BITS);
		if (data[7] & TILT_SIGN_BIT)
			tiltx -= (TILT_BITS + 1);
		if (data[8] & TILT_SIGN_BIT)
			tilty -= (TILT_BITS + 1);
		proximity = (data[0] & PROXIMITY_BIT);

		input_report_abs(wacom->dev, ABS_TILT_X, tiltx);
		input_report_abs(wacom->dev, ABS_TILT_Y, tilty);
		input_report_abs(wacom->dev, ABS_PRESSURE, z);
	}

	/* 4D mouse 1st packet or Lens cursor packet or 2D mouse packet*/
	//largely UNTESTED
	else if (((data[0] & 0xbe) == 0xa8) ||
			((data[0] & 0xbe) == 0xb0))
	{
		if (!discard_first)
			send_position5(wacom);

		/* 4D mouse */
		if (MOUSE_4D(device_id))
		{
			throttle = (((data[5] & 0x07) << 7) |
				(data[6] & 0x7f));
			if (data[8] & 0x08)
				throttle = -throttle;
			input_report_abs(wacom->dev, ABS_THROTTLE, throttle); //TODO range?
		}

		/* Lens cursor */
		else if (LENS_CURSOR(device_id))
		{
			buttons = data[8];
			send_buttons5(wacom, buttons, device);
		}

		/* 2D mouse */
		else if (MOUSE_2D(device_id))
		{
			buttons = (data[8] & 0x1C) >> 2;
			send_buttons5(wacom, buttons, device);
			relwheel = - (data[8] & 1) +
					((data[8] & 2) >> 1);
			input_report_rel(wacom->dev, REL_WHEEL, relwheel);
				//TODO range(relative?)/resolution -- need to flag capabliity?
		}

		proximity = (data[0] & PROXIMITY_BIT);
	} /* end 4D mouse 1st packet */

	/* 4D mouse 2nd packet */
	//UNTESTED
	else if ((data[0] & 0xbe) == 0xaa)
	{
		send_position5(wacom);
		rotation = (((data[6] & 0x0f) << 7) |
			(data[7] & 0x7f));
		if (rotation < 900)
			rotation = -rotation;
		else 
			rotation = 1799 - rotation;

		//TODO: send rotation

		proximity = (data[0] & PROXIMITY_BIT);
		discard_first = 0;
	}

	else {
		dev_info(&wacom->dev->dev,
				"Received unknown protocol V packet type!\n");
		return;
	}

	//TODO: remove redundancy if possible
	if (!discard_first) {
		input_report_key(wacom->dev, BTN_TOOL_MOUSE, proximity &&
						device == CURSOR_DEVICE_ID);
		input_report_key(wacom->dev, BTN_TOOL_RUBBER, proximity &&
						device == ERASER_DEVICE_ID);
		input_report_key(wacom->dev, BTN_TOOL_PEN, proximity &&
						device == STYLUS_DEVICE_ID);
	}

	input_sync(wacom->dev);
}

static irqreturn_t wacom_interrupt(struct serio *serio, unsigned char data,
				   unsigned int flags)
{
	struct wacom *wacom = serio_get_drvdata(serio);

	if (wacom == NULL) {
		printk(KERN_ERR DRIVER_DESC ": Something went VERY WRONG!\n");
		return IRQ_HANDLED;
	}

	if (data & 0x80)
		wacom->idx = 0;
	if (wacom->idx >= sizeof(wacom->data)) {
		dev_dbg(&wacom->dev->dev, "throwing away %d bytes of garbage\n",
			wacom->idx);
		wacom->idx = 0;
	}

	wacom->data[wacom->idx++] = data;

	/* we're either expecting a carriage return-terminated ASCII
	 * response string, or a seven-byte packet with the MSB set on
	 * the first byte */
	if (wacom->idx == PACKET_LENGTH && (wacom->data[0] & 0x80)) {
		//handle_packet(wacom);
		handle_packet5(wacom);
		wacom->idx = 0;
	} else if (data == '\r' && !(wacom->data[0] & 0x80)) {
		wacom->data[wacom->idx-1] = 0;
		handle_response(wacom);
		wacom->idx = 0;
	}
	return IRQ_HANDLED;
}

static void wacom_disconnect(struct serio *serio)
{
	struct wacom *wacom = serio_get_drvdata(serio);

	serio_close(serio);
	serio_set_drvdata(serio, NULL);
	input_unregister_device(wacom->dev);
	kfree(wacom);
}

static int wacom_send(struct serio *serio, const char *command)
{
	int err = 0;
	for (; !err && *command; command++)
		err = serio_write(serio, *command);
	return err;
}

static int send_setup_string(struct wacom *wacom, struct serio *serio)
{
	const char *s;
	switch (wacom->dev->id.version) {
	case MODEL_CINTIQ:	/* UNTESTED */
		s = COMMAND_ORIGIN_IN_UPPER_LEFT
			COMMAND_TRANSMIT_AT_MAX_RATE
			COMMAND_ENABLE_CONTINUOUS_MODE
			COMMAND_START_SENDING_PACKETS;
		break;
	case MODEL_PENPARTNER:	/* UNTESTED */
		s = COMMAND_ENABLE_PRESSURE_MODE
			COMMAND_START_SENDING_PACKETS;
		break;
	default:
		s = COMMAND_MULTI_MODE_INPUT
			COMMAND_ORIGIN_IN_UPPER_LEFT
			COMMAND_ENABLE_ALL_MACRO_BUTTONS
			COMMAND_DISABLE_GROUP_1_MACRO_BUTTONS
			COMMAND_TRANSMIT_AT_MAX_RATE
			COMMAND_DISABLE_INCREMENTAL_MODE
			COMMAND_ENABLE_CONTINUOUS_MODE
			COMMAND_Z_FILTER
			COMMAND_START_SENDING_PACKETS;
		break;
	}
	return wacom_send(serio, s);
}

static int wacom_setup(struct wacom *wacom, struct serio *serio)
{
	int err;
	unsigned long u;

	/* Note that setting the link speed is the job of inputattach.
	 * We assume that reset negotiation has already happened,
	 * here. */
	err = wacom_send(serio, COMMAND_STOP_SENDING_PACKETS);
	if (err)
		return err;

	init_completion(&wacom->cmd_done);
	err = wacom_send(serio, REQUEST_MODEL_AND_ROM_VERSION);
	if (err)
		return err;

	u = wait_for_completion_timeout(&wacom->cmd_done, HZ);
	if (u == 0) {
		dev_info(&wacom->dev->dev, "Timed out waiting for tablet to "
			 "respond with model and version.\n");
		return -EIO;
	}

	init_completion(&wacom->cmd_done);

	/* My Intuos tablet won't respond to a configuration request, so it 
	 * seems */
	if (wacom->dev->id.version != MODEL_INTUOS) {
		err = wacom_send(serio, REQUEST_CONFIGURATION_STRING);
		if (err)
			return err;

		u = wait_for_completion_timeout(&wacom->cmd_done, HZ);
		if (u == 0) {
			dev_info(&wacom->dev->dev, "Timed out waiting for "
					"tablet to respond with "
					"configuration string.\n");
			return -EIO;
		}
	}

	/* UNTESTED: Apparently Graphire models do not answer coordinate
	   requests. */
	if (wacom->dev->id.version != MODEL_GRAPHIRE) {
		init_completion(&wacom->cmd_done);
		err = wacom_send(serio, REQUEST_MAX_COORDINATES);
		if (err)
			return err;
		wait_for_completion_timeout(&wacom->cmd_done, HZ);
	}

	return send_setup_string(wacom, serio);
}

static int wacom_connect(struct serio *serio, struct serio_driver *drv)
{
	struct wacom *wacom;
	struct input_dev *input_dev;
	int err = -ENOMEM;

	wacom = kzalloc(sizeof(struct wacom), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!wacom || !input_dev)
		goto fail0;

	wacom->dev = input_dev;
	wacom->extra_z_bits = 1;
	snprintf(wacom->phys, sizeof(wacom->phys), "%s/input0", serio->phys);

	input_dev->name = DEVICE_NAME;
	input_dev->phys = wacom->phys;
	input_dev->id.bustype = BUS_RS232;
	input_dev->id.vendor  = SERIO_WACOM_IV;
	input_dev->id.product = serio->id.extra;
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &serio->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY)
			    | BIT_MASK(EV_ABS)
			    | BIT_MASK(EV_REL);
	__set_bit(BTN_LEFT,		input_dev->keybit);
	__set_bit(BTN_MIDDLE,		input_dev->keybit);
	__set_bit(BTN_RIGHT,		input_dev->keybit);
	__set_bit(BTN_SIDE,		input_dev->keybit);
	__set_bit(BTN_EXTRA,		input_dev->keybit);
	__set_bit(BTN_FORWARD,		input_dev->keybit);
	__set_bit(BTN_BACK,		input_dev->keybit);
	__set_bit(BTN_TASK,		input_dev->keybit);
	__set_bit(BTN_STYLUS,		input_dev->keybit);
	__set_bit(BTN_STYLUS2,		input_dev->keybit);
	__set_bit(BTN_TOOL_PEN,		input_dev->keybit);
	__set_bit(BTN_TOOL_RUBBER,	input_dev->keybit);
	__set_bit(BTN_TOOL_MOUSE,	input_dev->keybit);
	//__set_bit(BTN_TOUCH,		input_dev->keybit);

	serio_set_drvdata(serio, wacom);

	err = serio_open(serio, drv);
	if (err)
		goto fail1;

	err = wacom_setup(wacom, serio);
	if (err)
		goto fail2;

	err = input_register_device(wacom->dev);
	if (err)
		goto fail2;

	return 0;

 fail2:	serio_close(serio);
 fail1:	serio_set_drvdata(serio, NULL);
 fail0:	input_free_device(input_dev);
	kfree(wacom);
	return err;
}

static struct serio_device_id wacom_serio_ids[] = {
	{
		.type	= SERIO_RS232,
		.proto	= SERIO_WACOM_IV,
		.id	= SERIO_ANY,
		.extra	= SERIO_ANY,
	},
	{ 0 }
};

MODULE_DEVICE_TABLE(serio, wacom_serio_ids);

static struct serio_driver wacom_drv = {
	.driver		= {
		.name	= "wacom_serial",
	},
	.description	= DRIVER_DESC,
	.id_table	= wacom_serio_ids,
	.interrupt	= wacom_interrupt,
	.connect	= wacom_connect,
	.disconnect	= wacom_disconnect,
};

static int __init wacom_init(void)
{
	return serio_register_driver(&wacom_drv);
}

static void __exit wacom_exit(void)
{
	serio_unregister_driver(&wacom_drv);
}

module_init(wacom_init);
module_exit(wacom_exit);
