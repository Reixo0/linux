// SPDX-License-Identifier: GPL-2.0-only
/*
 * NCI over I2C for the ST54L (NFC controller and secure element), as on the
 * Nubia RedMagic 11 Pro.
 *
 * The ST54L speaks plain NCI 2.0 on I2C: each packet is read as its 3-byte
 * header, then its payload in a second transfer. It raises its interrupt
 * line while it has something to say. Between packets it may fall asleep;
 * the first transfer then wakes it and is not acknowledged, so transfers
 * are retried a few times.
 *
 * Out of reset the controller is in a mode without RF. A proprietary
 * command turns NFC on; the controller then resets itself and needs
 * CORE_INIT again. The mode is kept across resets once set, and the last
 * byte of each CORE_RESET_NTF says whether it is on. Its PLL runs from a
 * clock the board feeds it, which has to be on while the controller is.
 *
 * The controller serves either the kernel's NCI stack (nfc0) or, while that
 * is down, /dev/nfc-nci: one NCI packet per read() or write(), for a
 * userspace NCI stack such as Android's. Opening it powers the controller
 * and turns NFC mode on if it is off; the stack then resets and initialises
 * it itself. Closing it powers the controller off.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nfc.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <net/nfc/nci.h>
#include <net/nfc/nci_core.h>

#define ST54L_DRIVER_NAME	"st54l-i2c"

/* Transfers the controller refuses while it wakes up */
#define ST54L_WAKE_TRIES	20

/* Out of reset, before the controller says it is ready */
#define ST54L_BOOT_TIMEOUT_MS	200

/* After NFC mode on, before the controller has reset itself */
#define ST54L_MODE_TIMEOUT_MS	1000

/* Proprietary NFC mode command: sub-command 2, set, on */
#define ST54L_PROP_NFC_MODE	0x02
static const u8 st54l_nfc_mode_on[] = { 0x02, 0x01 };

static const u8 st54l_core_init_v2[] = { 0x00, 0x00 };

/* The last byte of CORE_RESET_NTF while NFC mode is on */
#define ST54L_RESET_NTF_LEN	(NCI_CTRL_HDR_SIZE + 0x1f)
#define ST54L_MODE_NFC		0x01

/* Packets /dev/nfc-nci holds for a reader that is behind */
#define ST54L_RAW_QUEUE		64

/* Firmware debug traces, sent while it polls when traces are on */
#define ST54L_PROP_DEBUG_NTF	0x02

#define ST54L_PROTOCOLS (NFC_PROTO_JEWEL_MASK | NFC_PROTO_MIFARE_MASK | \
			 NFC_PROTO_FELICA_MASK | NFC_PROTO_ISO14443_MASK | \
			 NFC_PROTO_ISO14443_B_MASK | NFC_PROTO_ISO15693_MASK | \
			 NFC_PROTO_NFC_DEP_MASK)

enum st54l_owner {
	ST54L_FREE,
	ST54L_NCI,
	ST54L_RAW,
};

struct st54l {
	struct i2c_client *client;
	struct nci_dev *ndev;
	struct gpio_desc *reset;
	struct clk *clk;

	/* Who has the controller; changed under lock */
	struct mutex lock;
	enum st54l_owner owner;

	/* The controller is out of reset and its packets go to the owner */
	bool running;
	/* A CORE_RESET_NTF arrived, and its last byte */
	struct completion reset_ntf;
	u8 mode;

	/* /dev/nfc-nci: packets for its reader */
	struct miscdevice misc;
	struct sk_buff_head raw_rx;
	wait_queue_head_t raw_wait;
};

static int st54l_recv(struct st54l *st, u8 *buf, int len)
{
	int ret = -EIO;
	int i;

	for (i = 0; i < ST54L_WAKE_TRIES; i++) {
		ret = i2c_master_recv(st->client, buf, len);
		if (ret == len)
			return 0;
		usleep_range(1000, 1500);
	}

	return ret < 0 ? ret : -EIO;
}

static int st54l_send_buf(struct st54l *st, const u8 *buf, int len)
{
	int ret = -EIO;
	int i;

	for (i = 0; i < ST54L_WAKE_TRIES; i++) {
		ret = i2c_master_send(st->client, buf, len);
		if (ret == len)
			return 0;
		usleep_range(1000, 1500);
	}

	return ret < 0 ? ret : -EIO;
}

static struct sk_buff *st54l_read_packet(struct st54l *st)
{
	struct device *dev = &st->client->dev;
	struct sk_buff *skb;
	u8 hdr[NCI_CTRL_HDR_SIZE];
	int ret;

	ret = st54l_recv(st, hdr, sizeof(hdr));
	if (ret) {
		dev_dbg(dev, "no header: %d\n", ret);
		return NULL;
	}

	skb = alloc_skb(sizeof(hdr) + hdr[2], GFP_KERNEL);
	if (!skb)
		return NULL;
	skb_put_data(skb, hdr, sizeof(hdr));

	if (hdr[2]) {
		ret = st54l_recv(st, skb_put(skb, hdr[2]), hdr[2]);
		if (ret) {
			dev_err(dev, "payload of %u bytes lost: %d\n", hdr[2], ret);
			kfree_skb(skb);
			return NULL;
		}
	}

	return skb;
}

static irqreturn_t st54l_irq(int irq, void *data)
{
	struct st54l *st = data;
	struct sk_buff *skb;
	u8 mt, gid, oid;

	skb = st54l_read_packet(st);
	if (!skb)
		return IRQ_HANDLED;

	mt = nci_mt(skb->data);
	gid = nci_opcode_gid(nci_opcode(skb->data));
	oid = nci_opcode_oid(nci_opcode(skb->data));

	if (mt == NCI_MT_NTF_PKT && gid == NCI_GID_CORE &&
	    oid == nci_opcode_oid(NCI_OP_CORE_RESET_NTF)) {
		if (skb->len == ST54L_RESET_NTF_LEN)
			st->mode = skb->data[skb->len - 1];
		complete(&st->reset_ntf);
	}

	if (!st->running ||
	    (mt == NCI_MT_NTF_PKT && gid == NCI_GID_PROPRIETARY &&
	     oid == ST54L_PROP_DEBUG_NTF)) {
		kfree_skb(skb);
		return IRQ_HANDLED;
	}

	if (st->owner == ST54L_RAW) {
		if (skb_queue_len(&st->raw_rx) >= ST54L_RAW_QUEUE) {
			dev_warn_ratelimited(&st->client->dev,
					     "/dev/nfc-nci reader behind, packet dropped\n");
			kfree_skb(skb);
		} else {
			skb_queue_tail(&st->raw_rx, skb);
			wake_up_interruptible(&st->raw_wait);
		}
		return IRQ_HANDLED;
	}

	nci_recv_frame(st->ndev, skb);

	return IRQ_HANDLED;
}

static void st54l_power_off(struct st54l *st)
{
	st->running = false;
	gpiod_set_value_cansleep(st->reset, 1);
	clk_disable_unprepare(st->clk);
}

/* Out of reset, with its greeting gone; its packets still go nowhere */
static int st54l_power_on(struct st54l *st)
{
	int ret;

	ret = clk_prepare_enable(st->clk);
	if (ret)
		return ret;

	reinit_completion(&st->reset_ntf);
	gpiod_set_value_cansleep(st->reset, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(st->reset, 0);

	if (!wait_for_completion_timeout(&st->reset_ntf,
					 msecs_to_jiffies(ST54L_BOOT_TIMEOUT_MS)))
		dev_warn(&st->client->dev, "no reset notification after power on\n");

	/* Let the rest of its greeting pass */
	msleep(20);

	return 0;
}

static int st54l_take(struct st54l *st, enum st54l_owner owner)
{
	guard(mutex)(&st->lock);

	if (st->owner != ST54L_FREE)
		return -EBUSY;
	st->owner = owner;

	return 0;
}

static void st54l_release(struct st54l *st)
{
	guard(mutex)(&st->lock);

	st->owner = ST54L_FREE;
}

static int st54l_open(struct nci_dev *ndev)
{
	struct st54l *st = nci_get_drvdata(ndev);
	int ret;

	ret = st54l_take(st, ST54L_NCI);
	if (ret)
		return ret;

	ret = st54l_power_on(st);
	if (ret) {
		st54l_release(st);
		return ret;
	}
	st->running = true;

	return 0;
}

static int st54l_close(struct nci_dev *ndev)
{
	struct st54l *st = nci_get_drvdata(ndev);

	st54l_power_off(st);
	st54l_release(st);

	return 0;
}

static int st54l_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct st54l *st = nci_get_drvdata(ndev);
	int ret;

	ret = st54l_send_buf(st, skb->data, skb->len);
	if (!ret)
		consume_skb(skb);
	else
		kfree_skb(skb);

	return ret;
}

/*
 * After CORE_INIT: NFC mode on. The controller acknowledges, resets itself
 * into that mode and wants CORE_INIT again.
 */
static int st54l_post_setup(struct nci_dev *ndev)
{
	struct st54l *st = nci_get_drvdata(ndev);
	int ret;

	reinit_completion(&st->reset_ntf);
	ret = nci_prop_cmd(ndev, ST54L_PROP_NFC_MODE,
			   sizeof(st54l_nfc_mode_on), st54l_nfc_mode_on);
	if (ret) {
		dev_err(&st->client->dev, "NFC mode refused: %d\n", ret);
		return ret;
	}

	if (!wait_for_completion_timeout(&st->reset_ntf,
					 msecs_to_jiffies(ST54L_MODE_TIMEOUT_MS))) {
		dev_err(&st->client->dev, "no reset into NFC mode\n");
		return -ETIMEDOUT;
	}

	/* NCI 2.0 CORE_INIT: both feature bytes off, as the core sends it */
	return nci_core_cmd(ndev, NCI_OP_CORE_INIT_CMD, sizeof(st54l_core_init_v2),
			    st54l_core_init_v2);
}

static int st54l_prop_rsp(struct nci_dev *ndev, struct sk_buff *skb)
{
	nci_req_complete(ndev, skb->len ? skb->data[0] : NCI_STATUS_SYNTAX_ERROR);

	return 0;
}

static const struct nci_driver_ops st54l_prop_ops[] = {
	{
		.opcode = nci_opcode_pack(NCI_GID_PROPRIETARY, ST54L_PROP_NFC_MODE),
		.rsp = st54l_prop_rsp,
	},
};

static const struct nci_ops st54l_ops = {
	.open = st54l_open,
	.close = st54l_close,
	.send = st54l_send,
	.post_setup = st54l_post_setup,
	.prop_ops = st54l_prop_ops,
	.n_prop_ops = ARRAY_SIZE(st54l_prop_ops),
};

/*
 * NFC mode on for /dev/nfc-nci, as post_setup does for the core: CORE_INIT,
 * the mode command, and the reset that follows. Its answers are dropped.
 */
static int st54l_raw_nfc_mode(struct st54l *st)
{
	static const u8 core_init[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
	static const u8 mode_on[] = { 0x2f, ST54L_PROP_NFC_MODE, 0x02, 0x02, 0x01 };
	int ret;

	ret = st54l_send_buf(st, core_init, sizeof(core_init));
	if (ret)
		return ret;
	msleep(50);

	reinit_completion(&st->reset_ntf);
	ret = st54l_send_buf(st, mode_on, sizeof(mode_on));
	if (ret)
		return ret;
	if (!wait_for_completion_timeout(&st->reset_ntf,
					 msecs_to_jiffies(ST54L_MODE_TIMEOUT_MS)))
		return -ETIMEDOUT;
	msleep(20);

	return st->mode == ST54L_MODE_NFC ? 0 : -EIO;
}

static int st54l_raw_open(struct inode *inode, struct file *file)
{
	struct st54l *st = container_of(file->private_data, struct st54l, misc);
	int ret;

	ret = st54l_take(st, ST54L_RAW);
	if (ret)
		return ret;

	ret = st54l_power_on(st);
	if (ret)
		goto err_release;

	if (st->mode != ST54L_MODE_NFC) {
		ret = st54l_raw_nfc_mode(st);
		if (ret) {
			dev_err(&st->client->dev, "NFC mode not on: %d\n", ret);
			st54l_power_off(st);
			goto err_release;
		}
	}

	skb_queue_purge(&st->raw_rx);
	st->running = true;
	file->private_data = st;

	return 0;

err_release:
	st54l_release(st);
	return ret;
}

static int st54l_raw_release(struct inode *inode, struct file *file)
{
	struct st54l *st = file->private_data;

	st54l_power_off(st);
	skb_queue_purge(&st->raw_rx);
	st54l_release(st);

	return 0;
}

static ssize_t st54l_raw_read(struct file *file, char __user *buf, size_t count,
			      loff_t *ppos)
{
	struct st54l *st = file->private_data;
	struct sk_buff *skb;
	ssize_t ret;

	for (;;) {
		skb = skb_dequeue(&st->raw_rx);
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(st->raw_wait,
					       !skb_queue_empty(&st->raw_rx));
		if (ret)
			return ret;
	}

	if (count < skb->len) {
		skb_queue_head(&st->raw_rx, skb);
		return -EMSGSIZE;
	}

	ret = copy_to_user(buf, skb->data, skb->len) ? -EFAULT : skb->len;
	kfree_skb(skb);

	return ret;
}

static ssize_t st54l_raw_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct st54l *st = file->private_data;
	u8 packet[NCI_CTRL_HDR_SIZE + NCI_MAX_PAYLOAD_SIZE];
	int ret;

	if (count < NCI_CTRL_HDR_SIZE || count > sizeof(packet))
		return -EINVAL;
	if (copy_from_user(packet, buf, count))
		return -EFAULT;
	if (packet[2] != count - NCI_CTRL_HDR_SIZE)
		return -EINVAL;

	ret = st54l_send_buf(st, packet, count);

	return ret ? ret : count;
}

static __poll_t st54l_raw_poll(struct file *file, poll_table *wait)
{
	struct st54l *st = file->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &st->raw_wait, wait);
	if (!skb_queue_empty(&st->raw_rx))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static const struct file_operations st54l_raw_fops = {
	.owner = THIS_MODULE,
	.open = st54l_raw_open,
	.release = st54l_raw_release,
	.read = st54l_raw_read,
	.write = st54l_raw_write,
	.poll = st54l_raw_poll,
	.llseek = noop_llseek,
};

static int st54l_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct st54l *st;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->client = client;
	init_completion(&st->reset_ntf);
	mutex_init(&st->lock);
	skb_queue_head_init(&st->raw_rx);
	init_waitqueue_head(&st->raw_wait);
	i2c_set_clientdata(client, st);

	/* Held in reset until the device is opened */
	st->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(st->reset))
		return dev_err_probe(dev, PTR_ERR(st->reset), "no reset gpio\n");

	st->clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(st->clk))
		return dev_err_probe(dev, PTR_ERR(st->clk), "no clock\n");

	st->ndev = nci_allocate_device(&st54l_ops, ST54L_PROTOCOLS, 0, 0);
	if (!st->ndev)
		return -ENOMEM;
	nci_set_parent_dev(st->ndev, dev);
	nci_set_drvdata(st->ndev, st);

	ret = devm_request_threaded_irq(dev, client->irq, NULL, st54l_irq,
					IRQF_ONESHOT, ST54L_DRIVER_NAME, st);
	if (ret) {
		dev_err_probe(dev, ret, "no interrupt\n");
		goto err_free;
	}

	ret = nci_register_device(st->ndev);
	if (ret) {
		dev_err_probe(dev, ret, "could not register\n");
		goto err_free;
	}

	st->misc.minor = MISC_DYNAMIC_MINOR;
	st->misc.name = "nfc-nci";
	st->misc.fops = &st54l_raw_fops;
	st->misc.parent = dev;
	ret = misc_register(&st->misc);
	if (ret) {
		dev_err_probe(dev, ret, "could not register /dev/nfc-nci\n");
		goto err_unregister;
	}

	return 0;

err_unregister:
	nci_unregister_device(st->ndev);
err_free:
	nci_free_device(st->ndev);
	return ret;
}

static void st54l_remove(struct i2c_client *client)
{
	struct st54l *st = i2c_get_clientdata(client);

	misc_deregister(&st->misc);
	nci_unregister_device(st->ndev);
	nci_free_device(st->ndev);
	skb_queue_purge(&st->raw_rx);
}

static const struct of_device_id st54l_of_match[] = {
	{ .compatible = "st,st54l-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, st54l_of_match);

static const struct i2c_device_id st54l_id[] = {
	{ ST54L_DRIVER_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, st54l_id);

static struct i2c_driver st54l_driver = {
	.driver = {
		.name = ST54L_DRIVER_NAME,
		.of_match_table = st54l_of_match,
	},
	.probe = st54l_probe,
	.remove = st54l_remove,
	.id_table = st54l_id,
};
module_i2c_driver(st54l_driver);

MODULE_DESCRIPTION("NCI over I2C for the ST54L");
MODULE_LICENSE("GPL");
