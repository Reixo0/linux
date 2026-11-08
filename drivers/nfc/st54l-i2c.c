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
 * CORE_INIT again. Its PLL runs from a clock the board feeds it, which has
 * to be on while the controller is.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/nfc.h>
#include <linux/of.h>

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

/* Firmware debug traces, sent while it polls when traces are on */
#define ST54L_PROP_DEBUG_NTF	0x02

#define ST54L_PROTOCOLS (NFC_PROTO_JEWEL_MASK | NFC_PROTO_MIFARE_MASK | \
			 NFC_PROTO_FELICA_MASK | NFC_PROTO_ISO14443_MASK | \
			 NFC_PROTO_ISO14443_B_MASK | NFC_PROTO_ISO15693_MASK | \
			 NFC_PROTO_NFC_DEP_MASK)

struct st54l {
	struct i2c_client *client;
	struct nci_dev *ndev;
	struct gpio_desc *reset;
	struct clk *clk;

	/* The controller is out of reset and its packets go to the core */
	bool running;
	/* A CORE_RESET_NTF arrived */
	struct completion reset_ntf;
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
	    oid == nci_opcode_oid(NCI_OP_CORE_RESET_NTF))
		complete(&st->reset_ntf);

	if (!st->running ||
	    (mt == NCI_MT_NTF_PKT && gid == NCI_GID_PROPRIETARY &&
	     oid == ST54L_PROP_DEBUG_NTF)) {
		kfree_skb(skb);
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

static int st54l_open(struct nci_dev *ndev)
{
	struct st54l *st = nci_get_drvdata(ndev);
	int ret;

	ret = clk_prepare_enable(st->clk);
	if (ret)
		return ret;

	/* Out of reset it says hello; that greeting is not for the core */
	reinit_completion(&st->reset_ntf);
	gpiod_set_value_cansleep(st->reset, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(st->reset, 0);

	if (!wait_for_completion_timeout(&st->reset_ntf,
					 msecs_to_jiffies(ST54L_BOOT_TIMEOUT_MS)))
		dev_warn(&st->client->dev, "no reset notification after power on\n");

	/* Let the rest of its greeting pass */
	msleep(20);
	st->running = true;

	return 0;
}

static int st54l_close(struct nci_dev *ndev)
{
	st54l_power_off(nci_get_drvdata(ndev));

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

	return 0;

err_free:
	nci_free_device(st->ndev);
	return ret;
}

static void st54l_remove(struct i2c_client *client)
{
	struct st54l *st = i2c_get_clientdata(client);

	nci_unregister_device(st->ndev);
	nci_free_device(st->ndev);
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
