// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple TV 3 (J33i) MCU: a TI MSP430 on uart1 that receives the IR remote and drives the front
 * LED (ADT mcu0, compatible "mcu-nmi"). 250000 8N1; frames are
 *     53 74 op seq lenHi lenLo body crcHi crcLo
 * with CRC-16/CCITT-FALSE over everything before the CRC (docs/research/j33i-mcu-protocol.md,
 * from iOS's AppleTVIR HID plugin and the MCU firmware).
 *
 * Remote presses become an input device; the LED becomes "atv:white:status" (on/off, and the
 * MCU's own slow/fast blink through blink_set). Only GetVersion (0x02), SetLED (0x04) and
 * SetState (0x07) are ever sent. The MCU's bootloader link (uart0) and its reset/test GPIOs can
 * reflash it, so this driver never touches them.
 */
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

#define MCU_BAUD	250000
#define MCU_MAX_BODY	64
#define KEY_HOLD_MS	130	/* no release frame: a key is up when its repeats stop */

enum { OP_RESET = 0x01, OP_VERSION = 0x02, OP_LED = 0x04, OP_STATE = 0x07,
       OP_IR = 0x10, OP_IR_REPEAT = 0x11, OP_ACK = 0x80, OP_DEBUG = 0xff };
enum { LED_OFF_MODE, LED_ON_MODE, LED_SLOW_BLINK, LED_FAST_BLINK };

static const struct { u16 code; u16 key; } atv_keys[] = {
	{ 0xe05, KEY_UP },    { 0x501, KEY_UP },
	{ 0xe06, KEY_DOWN },  { 0x502, KEY_DOWN },
	{ 0xe04, KEY_LEFT },  { 0x503, KEY_LEFT },
	{ 0xe03, KEY_RIGHT }, { 0x504, KEY_RIGHT },
	{ 0xe01, KEY_BACK },					/* Menu */
	{ 0xe2e, KEY_ENTER }, { 0x505, KEY_ENTER },		/* Select */
	{ 0xe02, KEY_ENTER },					/* white remote's centre */
	{ 0xe2f, KEY_PLAYPAUSE },
};

struct atv_mcu {
	struct serdev_device *serdev;
	struct input_dev *input;
	struct led_classdev led;
	struct timer_list release;
	spinlock_t key_lock;
	unsigned int down;		/* key held, 0 if none */
	struct mutex tx_lock;
	u8 seq;
	/* receive state */
	u8 frame[6 + MCU_MAX_BODY + 2];
	unsigned int got;
};

static u16 mcu_crc(const u8 *p, size_t n)
{
	u16 c = 0xffff;

	while (n--) {
		c ^= *p++ << 8;
		for (int i = 0; i < 8; i++)
			c = c & 0x8000 ? c << 1 ^ 0x1021 : c << 1;
	}
	return c;
}

static int mcu_send(struct atv_mcu *m, u8 op, const u8 *body, u8 len)
{
	u8 f[8 + 4];
	u16 c;
	int ret;

	mutex_lock(&m->tx_lock);
	f[0] = 0x53; f[1] = 0x74; f[2] = op; f[3] = m->seq; f[4] = 0; f[5] = len;
	memcpy(f + 6, body, len);
	c = mcu_crc(f, 6 + len);
	f[6 + len] = c >> 8;
	f[7 + len] = c & 0xff;
	m->seq += 2;	/* odd numbers, as iOS uses */
	ret = serdev_device_write_buf(m->serdev, f, 8 + len);
	mutex_unlock(&m->tx_lock);
	return ret < 0 ? ret : 0;
}

static void mcu_init_state(struct atv_mcu *m)
{
	u8 booted = 0;

	mcu_send(m, OP_VERSION, NULL, 0);
	mcu_send(m, OP_STATE, &booted, 1);
}

static void mcu_key_up(struct atv_mcu *m)
{
	if (m->down) {
		input_report_key(m->input, m->down, 0);
		input_sync(m->input);
		m->down = 0;
	}
}

static void mcu_release(struct timer_list *t)
{
	struct atv_mcu *m = from_timer(m, t, release);
	unsigned long flags;

	spin_lock_irqsave(&m->key_lock, flags);
	mcu_key_up(m);
	spin_unlock_irqrestore(&m->key_lock, flags);
}

static void mcu_ir(struct atv_mcu *m, const u8 *b, bool repeat)
{
	u16 code = (b[0] & 0x1f) << 8 | b[3] >> 1;
	unsigned int key = 0;
	unsigned long flags;

	for (int i = 0; i < ARRAY_SIZE(atv_keys); i++)
		if (atv_keys[i].code == code)
			key = atv_keys[i].key;

	spin_lock_irqsave(&m->key_lock, flags);
	if (!repeat || key != m->down) {
		mcu_key_up(m);
		input_event(m->input, EV_MSC, MSC_SCAN, code);
		if (key) {
			input_report_key(m->input, key, 1);
			m->down = key;
		} else {
			dev_dbg(&m->serdev->dev, "IR code 0x%03x (remote %02x) not mapped\n", code, b[2]);
		}
		input_sync(m->input);
	}
	if (m->down)
		mod_timer(&m->release, jiffies + msecs_to_jiffies(KEY_HOLD_MS));
	spin_unlock_irqrestore(&m->key_lock, flags);
}

static void mcu_frame(struct atv_mcu *m, const u8 *f, unsigned int len)
{
	const u8 *body = f + 6;

	switch (f[2]) {
	case OP_IR:
	case OP_IR_REPEAT:
		if (len >= 4)
			mcu_ir(m, body, f[2] == OP_IR_REPEAT);
		break;
	case OP_RESET:
		dev_info(&m->serdev->dev, "MCU reset itself; setting it up again\n");
		mcu_init_state(m);
		break;
	case OP_ACK:
		if (len > 1 && body[len - 1] == 0)
			dev_info(&m->serdev->dev, "MCU firmware %s\n", body);
		break;
	case OP_DEBUG:
		dev_dbg(&m->serdev->dev, "MCU: %.*s\n", len, body);
		break;
	default:
		if (f[2] > OP_ACK)
			dev_warn(&m->serdev->dev, "MCU refused seq %u (0x%02x)\n", f[3], f[2]);
	}
}

static size_t mcu_receive(struct serdev_device *serdev, const u8 *buf, size_t count)
{
	struct atv_mcu *m = serdev_device_get_drvdata(serdev);

	for (size_t i = 0; i < count; i++) {
		u8 b = buf[i];
		unsigned int len;

		if ((m->got == 0 && b != 0x53) || (m->got == 1 && b != 0x74)) {
			m->got = b == 0x53;
			if (m->got)
				m->frame[0] = b;
			continue;
		}
		m->frame[m->got++] = b;
		if (m->got < 6)
			continue;
		len = m->frame[4] << 8 | m->frame[5];
		if (len > MCU_MAX_BODY) {
			m->got = 0;
			continue;
		}
		if (m->got < 8 + len)
			continue;
		if (mcu_crc(m->frame, 6 + len) == (m->frame[6 + len] << 8 | m->frame[7 + len]))
			mcu_frame(m, m->frame, len);
		else
			dev_dbg(&serdev->dev, "bad CRC on op 0x%02x\n", m->frame[2]);
		m->got = 0;
	}
	return count;
}

static const struct serdev_device_ops mcu_serdev_ops = {
	.receive_buf = mcu_receive,
	.write_wakeup = serdev_device_write_wakeup,
};

static int mcu_led_set(struct led_classdev *led, enum led_brightness b)
{
	struct atv_mcu *m = container_of(led, struct atv_mcu, led);
	u8 mode = b ? LED_ON_MODE : LED_OFF_MODE;

	return mcu_send(m, OP_LED, &mode, 1);
}

/* The MCU blinks on its own at two speeds: take whichever is nearer. */
static int mcu_led_blink(struct led_classdev *led, unsigned long *on, unsigned long *off)
{
	struct atv_mcu *m = container_of(led, struct atv_mcu, led);
	u8 mode = (*on + *off) && *on + *off < 600 ? LED_FAST_BLINK : LED_SLOW_BLINK;

	*on = *off = mode == LED_FAST_BLINK ? 125 : 500;	/* GUESS: the MCU's real periods are unmeasured */
	return mcu_send(m, OP_LED, &mode, 1);
}

static int mcu_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct atv_mcu *m;
	int ret;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->serdev = serdev;
	m->seq = 1;
	mutex_init(&m->tx_lock);
	spin_lock_init(&m->key_lock);
	timer_setup(&m->release, mcu_release, 0);
	serdev_device_set_drvdata(serdev, m);

	m->input = devm_input_allocate_device(dev);
	if (!m->input)
		return -ENOMEM;
	m->input->name = "Apple TV IR remote";
	m->input->phys = "atv-mcu/input0";
	m->input->id.bustype = BUS_HOST;
	input_set_capability(m->input, EV_MSC, MSC_SCAN);
	for (int i = 0; i < ARRAY_SIZE(atv_keys); i++)
		input_set_capability(m->input, EV_KEY, atv_keys[i].key);
	ret = input_register_device(m->input);
	if (ret)
		return ret;

	serdev_device_set_client_ops(serdev, &mcu_serdev_ops);
	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return ret;
	serdev_device_set_baudrate(serdev, MCU_BAUD);
	serdev_device_set_flow_control(serdev, false);
	serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);

	m->led.name = "atv:white:status";
	m->led.max_brightness = 1;
	m->led.brightness_set_blocking = mcu_led_set;
	m->led.blink_set = mcu_led_blink;
	ret = devm_led_classdev_register(dev, &m->led);
	if (ret)
		return ret;

	mcu_init_state(m);
	return 0;
}

static void mcu_remove(struct serdev_device *serdev)
{
	struct atv_mcu *m = serdev_device_get_drvdata(serdev);

	timer_shutdown_sync(&m->release);
}

static const struct of_device_id mcu_of_match[] = {
	{ .compatible = "apple,j33i-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcu_of_match);

static struct serdev_device_driver mcu_driver = {
	.probe = mcu_probe,
	.remove = mcu_remove,
	.driver = {
		.name = "atv-mcu",
		.of_match_table = mcu_of_match,
	},
};
module_serdev_device_driver(mcu_driver);

MODULE_DESCRIPTION("Apple TV 3 MCU: IR remote and front LED");
MODULE_LICENSE("GPL");
