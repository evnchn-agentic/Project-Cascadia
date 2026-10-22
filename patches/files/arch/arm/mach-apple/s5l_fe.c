// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arasan "FE" 10/100 Ethernet MAC on the Apple S5L8947X (Apple TV 3, J33i), with its SMSC
 * LAN8742 PHY.  Register map reverse-engineered from iOS 8.4's AppleArasanFEController:
 * docs/research/j33i-arasan-fe-regmap.md.  Bring-up driver: one descriptor per frame,
 * no checksum offload, no multicast filter (promiscuous off, broadcast + own address).
 *
 * Lives under mach-apple for now only to avoid touching drivers/net glue.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#define FE_RESET	0x00
#define FE_DMA_EN	0x04
#define FE_ISR		0x08	/* write 1 to clear */
#define FE_IMR		0x0c
#define FE_TX_KICK	0x14
#define FE_RX_POLL	0x18
#define FE_TX_BASE	0x1c
#define FE_RX_BASE	0x20
#define FE_X40		0x40
#define FE_MAC_CTL	0x100	/* bit0 100 Mbit, bit2 full duplex */
#define FE_MAC_EN_A	0x104
#define FE_MAC_EN_B	0x108
#define FE_MAX_FRAME	0x10c
#define FE_X110		0x110
#define FE_X114		0x114
#define FE_FILTER	0x118	/* bit8 promiscuous */
#define FE_ADDR0	0x120
#define FE_MDIO_CMD	0x1a0	/* phy | reg<<5, bit10 read, bit15 start/busy */
#define FE_MDIO_DATA	0x1a4
#define FE_X1C0		0x1c0
#define FE_X1C4		0x1c4
#define FE_STAT_IRQ	0x1e0

#define FE_IRQ_TX	0x001
#define FE_IRQ_RX	0x010
#define FE_IRQ_RXNB	0x020
#define FE_IRQ_STAT	0x100
#define FE_IRQ_MASK	0x1f1

#define D_OWN		BIT(31)
#define D_TX_LAST	(BIT(31) | BIT(30))
#define D_TX_FIRST	BIT(29)
#define D_EOR		BIT(26)
#define D_CHAIN		BIT(25)
#define D_RX_ERR	BIT(14)
#define D_RX_LEN	0x7fff

#define RING		128
#define BUF_SZ		1536

#define PMGR_ETH_GATE	0x3f101144	/* ADT device-clocks ETHERNET id 109, index 0x51 */

struct fe_desc {
	__le32 status, ctl, buf, next;
	u32 pad[4];			/* 32-byte stride, as iOS uses */
};

struct fe_priv {
	void __iomem *base;
	struct net_device *ndev;
	struct device *dev;
	struct mii_bus *mii;
	struct fe_desc *tx, *rx;
	dma_addr_t tx_dma, rx_dma;
	struct sk_buff *tx_skb[RING], *rx_skb[RING];
	dma_addr_t tx_map[RING], rx_map[RING];
	unsigned int tx_head, tx_tail, rx_next;
	spinlock_t lock;
	int speed, duplex, link;
};

static inline u32 fe_r(struct fe_priv *p, u32 o) { return readl(p->base + o); }
static inline void fe_w(struct fe_priv *p, u32 o, u32 v) { writel(v, p->base + o); }

static int fe_mdio_wait(struct fe_priv *p)
{
	u32 v;

	return readl_poll_timeout_atomic(p->base + FE_MDIO_CMD, v, !(v & BIT(15)), 10, 100000);
}

static int fe_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	struct fe_priv *p = bus->priv;

	fe_w(p, FE_MDIO_CMD, phy | reg << 5 | BIT(10));
	fe_w(p, FE_MDIO_CMD, fe_r(p, FE_MDIO_CMD) | BIT(15));
	if (fe_mdio_wait(p))
		return -ETIMEDOUT;
	return fe_r(p, FE_MDIO_DATA) & 0xffff;
}

static int fe_mdio_write(struct mii_bus *bus, int phy, int reg, u16 val)
{
	struct fe_priv *p = bus->priv;

	fe_w(p, FE_MDIO_CMD, phy | reg << 5);
	fe_w(p, FE_MDIO_DATA, val);
	fe_w(p, FE_MDIO_CMD, fe_r(p, FE_MDIO_CMD) | BIT(15));
	return fe_mdio_wait(p);
}

static int fe_rx_fill(struct fe_priv *p, unsigned int i)
{
	struct sk_buff *skb = netdev_alloc_skb_ip_align(p->ndev, BUF_SZ);
	dma_addr_t a;

	if (!skb)
		return -ENOMEM;
	a = dma_map_single(p->dev, skb->data, BUF_SZ, DMA_FROM_DEVICE);
	if (dma_mapping_error(p->dev, a)) {
		dev_kfree_skb(skb);
		return -ENOMEM;
	}
	p->rx_skb[i] = skb;
	p->rx_map[i] = a;
	p->rx[i].buf = cpu_to_le32(a);
	p->rx[i].ctl = cpu_to_le32(BUF_SZ | (i == RING - 1 ? D_EOR : 0));
	dma_wmb();
	p->rx[i].status = cpu_to_le32(D_OWN);
	return 0;
}

static void fe_rx(struct fe_priv *p)
{
	struct net_device *ndev = p->ndev;

	for (;;) {
		unsigned int i = p->rx_next;
		u32 st = le32_to_cpu(p->rx[i].status);
		struct sk_buff *skb = p->rx_skb[i];
		dma_addr_t map = p->rx_map[i];
		unsigned int len = st & D_RX_LEN;
		bool bad = (st & D_RX_ERR) || len < ETH_ZLEN + 4 || len > BUF_SZ;

		if (st & D_OWN)
			break;
		dma_rmb();
		p->rx_next = (i + 1) % RING;
		/* A bad frame, or no buffer to replace this one: hand the same buffer back, so the
		 * ring never loses a slot (a lost slot ends in a "no RX buffer" interrupt storm). */
		if (bad || fe_rx_fill(p, i)) {
			if (bad)
				ndev->stats.rx_errors++;
			else
				ndev->stats.rx_dropped++;
			p->rx_skb[i] = skb;
			p->rx_map[i] = map;
			p->rx[i].buf = cpu_to_le32(map);
			dma_wmb();
			p->rx[i].status = cpu_to_le32(D_OWN);
			continue;
		}
		dma_unmap_single(p->dev, map, BUF_SZ, DMA_FROM_DEVICE);
		skb_put(skb, len - 4);	/* the length includes the CRC */
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len - 4;
		netif_rx(skb);
	}
	fe_w(p, FE_RX_POLL, 1);
}

static void fe_tx_done(struct fe_priv *p)
{
	while (p->tx_tail != p->tx_head) {
		unsigned int i = p->tx_tail;
		struct sk_buff *skb = p->tx_skb[i];

		if (le32_to_cpu(p->tx[i].status) & D_OWN)
			break;
		dma_unmap_single(p->dev, p->tx_map[i], skb->len, DMA_TO_DEVICE);
		p->ndev->stats.tx_packets++;
		p->ndev->stats.tx_bytes += skb->len;
		dev_consume_skb_irq(skb);
		p->tx_skb[i] = NULL;
		p->tx_tail = (i + 1) % RING;
	}
	if (netif_queue_stopped(p->ndev) && (p->tx_head + 1) % RING != p->tx_tail)
		netif_wake_queue(p->ndev);
}

static irqreturn_t fe_irq(int irq, void *data)
{
	struct fe_priv *p = data;
	u32 st = fe_r(p, FE_ISR);

	if (!st)
		return IRQ_NONE;
	fe_w(p, FE_ISR, st);
	spin_lock(&p->lock);
	if (st & (FE_IRQ_RX | FE_IRQ_RXNB))	/* ring full: drain it, then poll */
		fe_rx(p);
	if (st & FE_IRQ_TX)
		fe_tx_done(p);
	if (st & FE_IRQ_STAT)
		fe_w(p, FE_STAT_IRQ, fe_r(p, FE_STAT_IRQ));
	spin_unlock(&p->lock);
	return IRQ_HANDLED;
}

static netdev_tx_t fe_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct fe_priv *p = netdev_priv(ndev);
	unsigned long flags;
	unsigned int i;
	dma_addr_t a;

	if (skb_put_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;
	a = dma_map_single(p->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(p->dev, a)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&p->lock, flags);
	i = p->tx_head;
	p->tx_skb[i] = skb;
	p->tx_map[i] = a;
	p->tx[i].buf = cpu_to_le32(a);
	p->tx[i].ctl = cpu_to_le32(skb->len | D_TX_FIRST | D_TX_LAST | D_CHAIN);
	dma_wmb();
	p->tx[i].status = cpu_to_le32(D_OWN);
	p->tx_head = (i + 1) % RING;
	if ((p->tx_head + 1) % RING == p->tx_tail)
		netif_stop_queue(ndev);
	wmb();
	fe_w(p, FE_TX_KICK, 1);
	spin_unlock_irqrestore(&p->lock, flags);
	return NETDEV_TX_OK;
}

static void fe_adjust_link(struct net_device *ndev)
{
	struct fe_priv *p = netdev_priv(ndev);
	struct phy_device *phy = ndev->phydev;

	if (phy->link == p->link && phy->speed == p->speed && phy->duplex == p->duplex)
		return;
	p->link = phy->link;
	p->speed = phy->speed;
	p->duplex = phy->duplex;
	if (phy->link) {
		fe_w(p, FE_MAC_CTL, (phy->speed == SPEED_100 ? BIT(0) : 0) |
				    (phy->duplex == DUPLEX_FULL ? BIT(2) : 0));
		fe_w(p, FE_MAC_EN_B, fe_r(p, FE_MAC_EN_B) | 1);
		fe_w(p, FE_MAC_EN_A, fe_r(p, FE_MAC_EN_A) | 1);
		fe_w(p, FE_DMA_EN, fe_r(p, FE_DMA_EN) | 3);
		fe_w(p, FE_IMR, FE_IRQ_MASK);
		fe_w(p, FE_RX_POLL, 1);
	} else {
		fe_w(p, FE_DMA_EN, fe_r(p, FE_DMA_EN) & ~3);
		fe_w(p, FE_IMR, 0);
		fe_w(p, FE_ISR, fe_r(p, FE_ISR));
		fe_w(p, FE_MAC_EN_B, fe_r(p, FE_MAC_EN_B) & ~1);
		fe_w(p, FE_MAC_EN_A, fe_r(p, FE_MAC_EN_A) & ~1);
	}
	phy_print_status(phy);
}

static void fe_hw_init(struct fe_priv *p)
{
	const u8 *a = p->ndev->dev_addr;
	unsigned int i;

	/* AppleArasanFEController::_prepareHW, in its order. */
	fe_w(p, FE_X40, 0x80000000);
	fe_r(p, FE_X40);
	fe_w(p, FE_RESET, 1);
	fe_w(p, FE_RESET, 0);
	fe_w(p, FE_RESET, 0x410);
	fe_w(p, FE_MAC_CTL, 0x18);
	fe_r(p, FE_MAC_CTL);
	fe_w(p, FE_MAC_CTL, 0);
	fe_w(p, FE_X1C4, 0x80);
	fe_w(p, FE_X1C0, 0x1f8);
	fe_w(p, FE_MAC_EN_A, 8);
	fe_w(p, FE_MAC_EN_B, 0x18);
	fe_w(p, FE_ADDR0, a[0] | a[1] << 8);
	fe_w(p, FE_ADDR0 + 4, a[2] | a[3] << 8);
	fe_w(p, FE_ADDR0 + 8, a[4] | a[5] << 8);
	fe_w(p, FE_FILTER, 1);
	fe_w(p, FE_MAX_FRAME, 1518 + 4);
	fe_w(p, FE_X110, 0x600);
	fe_w(p, FE_X114, 0x600);

	for (i = 0; i < RING; i++) {
		p->tx[i].status = 0;
		p->tx[i].ctl = cpu_to_le32(D_CHAIN);
		p->tx[i].next = cpu_to_le32(p->tx_dma + ((i + 1) % RING) * sizeof(struct fe_desc));
	}
	p->tx_head = p->tx_tail = p->rx_next = 0;
	fe_w(p, FE_TX_BASE, p->tx_dma);
	fe_w(p, FE_RX_BASE, p->rx_dma);
}

static int fe_open(struct net_device *ndev)
{
	struct fe_priv *p = netdev_priv(ndev);
	struct phy_device *phy;
	unsigned int i;
	int ret;

	for (i = 0; i < RING; i++) {
		ret = fe_rx_fill(p, i);
		if (ret)
			return ret;
	}
	fe_hw_init(p);
	p->link = p->speed = p->duplex = -1;
	phy = phy_find_first(p->mii);
	if (!phy)
		return -ENODEV;
	ret = phy_connect_direct(ndev, phy, fe_adjust_link, PHY_INTERFACE_MODE_RMII);
	if (ret)
		return ret;
	phy_set_max_speed(phy, SPEED_100);
	phy_start(phy);
	netif_start_queue(ndev);
	return 0;
}

static int fe_stop(struct net_device *ndev)
{
	struct fe_priv *p = netdev_priv(ndev);
	unsigned int i;

	netif_stop_queue(ndev);
	phy_stop(ndev->phydev);
	phy_disconnect(ndev->phydev);
	fe_w(p, FE_DMA_EN, 0);
	fe_w(p, FE_IMR, 0);
	fe_w(p, FE_MAC_EN_A, fe_r(p, FE_MAC_EN_A) & ~1);
	fe_w(p, FE_MAC_EN_B, fe_r(p, FE_MAC_EN_B) & ~1);
	for (i = 0; i < RING; i++) {
		if (p->rx_skb[i]) {
			dma_unmap_single(p->dev, p->rx_map[i], BUF_SZ, DMA_FROM_DEVICE);
			dev_kfree_skb(p->rx_skb[i]);
			p->rx_skb[i] = NULL;
		}
		if (p->tx_skb[i]) {
			dma_unmap_single(p->dev, p->tx_map[i], p->tx_skb[i]->len, DMA_TO_DEVICE);
			dev_kfree_skb(p->tx_skb[i]);
			p->tx_skb[i] = NULL;
		}
	}
	return 0;
}

static const struct net_device_ops fe_ops = {
	.ndo_open		= fe_open,
	.ndo_stop		= fe_stop,
	.ndo_start_xmit		= fe_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static int fe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk *clk;
	void __iomem *gate;
	struct net_device *ndev;
	struct fe_priv *p;
	int irq, ret;

	/* The block reads as a constant until its clock gate is on (PMGR +0x1144: 0x200 -> 0x2ff). */
	gate = ioremap(PMGR_ETH_GATE, 4);
	if (!gate)
		return -ENOMEM;
	writel((readl(gate) & ~0x10f) | 0xf, gate);
	udelay(10);
	dev_info(dev, "clock gate %#x\n", readl(gate));
	iounmap(gate);

	/* The CDMA power gate also feeds this block: switching it off killed eth0 at once. */
	clk = devm_clk_get_optional_enabled(dev, "cdma");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "no CDMA power state\n");

	ndev = devm_alloc_etherdev(dev, sizeof(*p));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	p = netdev_priv(ndev);
	p->ndev = ndev;
	p->dev = dev;
	spin_lock_init(&p->lock);

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	p->tx = dmam_alloc_coherent(dev, RING * sizeof(struct fe_desc), &p->tx_dma, GFP_KERNEL);
	p->rx = dmam_alloc_coherent(dev, RING * sizeof(struct fe_desc), &p->rx_dma, GFP_KERNEL);
	if (!p->tx || !p->rx)
		return -ENOMEM;

	if (of_get_ethdev_address(dev->of_node, ndev))
		eth_hw_addr_random(ndev);

	ret = devm_request_irq(dev, irq, fe_irq, 0, dev_name(dev), p);
	if (ret)
		return ret;

	p->mii = devm_mdiobus_alloc(dev);
	if (!p->mii)
		return -ENOMEM;
	p->mii->name = "s5l-fe-mdio";
	p->mii->priv = p;
	p->mii->read = fe_mdio_read;
	p->mii->write = fe_mdio_write;
	p->mii->parent = dev;
	p->mii->phy_mask = ~1u;		/* the LAN8742 sits at address 0 */
	snprintf(p->mii->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	ret = devm_mdiobus_register(dev, p->mii);
	if (ret)
		return ret;

	ndev->netdev_ops = &fe_ops;
	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return ret;
	dev_info(dev, "Arasan FE at %pa, irq %d, MAC %pM\n", &pdev->resource[0].start, irq,
		 ndev->dev_addr);
	return 0;
}

static const struct of_device_id fe_of_match[] = {
	{ .compatible = "apple,s5l8947x-fe" },
	{ }
};

static struct platform_driver fe_driver = {
	.probe = fe_probe,
	.driver = {
		.name = "s5l-fe",
		.of_match_table = fe_of_match,
	},
};
builtin_platform_driver(fe_driver);
