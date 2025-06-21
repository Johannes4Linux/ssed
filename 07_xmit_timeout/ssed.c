#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/etherdevice.h>

#define SET_SMI_OP 0x1
#define GET_SMI 0x2
#define SET_SMI 0x3
#define SET_MAC 0x4
#define SEND_FRAME 0x6
#define RECV_FRAME 0x7
#define GET_IRQ 0x8

struct ssed_net {
	struct net_device *net;
	struct spi_device *spi;
	struct work_struct work;
	struct work_struct xmit_work;
	struct phy_device *phy;
	struct mii_bus *mii_bus;
	struct mutex lock;
	struct sk_buff *tx_skb;
};

static int ssed_read_write(struct ssed_net *priv, u8 *wdata, u8 wlen, u8 *rdata, u8 rlen)
{
	int status;

	/* Write out data */
	status = spi_write(priv->spi, wdata, wlen);
	if (status)
		return status;
	/* Small delay, so W7500 can react */
	udelay(25);
	/* Read back data */
	return spi_read(priv->spi, rdata, rlen);
}

static int ssed_w8r8(struct ssed_net *priv, u8 cmd)
{
	int status;
	u8 resp;

	mutex_lock(&priv->lock);
	status = ssed_read_write(priv, &cmd, 1, &resp, 1);
	mutex_unlock(&priv->lock);

	if (status >= 0)
		return resp;
	else
		return status;
}

void ssed_recv_frames(struct ssed_net *priv)
{
	u8 data[2], cmd = RECV_FRAME;
	u16 len;
	struct sk_buff *skb = NULL;

	/* Allocate space for one package */
	u8 *pkg = kmalloc(ETH_FRAME_LEN, GFP_KERNEL);

	do {
		mutex_lock(&priv->lock);
		/* Get length of received frame */
		ssed_read_write(priv, &cmd, 1, data, 2);
		len = (data[0] << 8) | data[1];

		/* Read out package over SPI */
		if (len)
			spi_read(priv->spi, pkg, len);
		mutex_unlock(&priv->lock);

		/* Pass package to next layer */
		if (len) {
			dev_info(&priv->spi->dev, "Frame with %d bytes recv\n", len);
			skb = dev_alloc_skb(len);

			if (!skb) {
				dev_err(&priv->spi->dev, "Out of memory, drop RX'd frame\n");
				continue;
			}

			/* Copy data */
			skb->dev = priv->net;
			memcpy(skb_put(skb, len), pkg, len);
			skb->protocol = eth_type_trans(skb, priv->net);

			dev_info(&priv->spi->dev, "Call netif_rx...\n");
			netif_receive_skb(skb);
			dev_info(&priv->spi->dev, "Done\n");
		}
	} while (len);

	kfree(pkg);
}

void ssed_irq_work_handler(struct work_struct *work)
{
	u8 ir;
	struct ssed_net *priv = container_of(work, struct ssed_net, work);

	 /* Read out and clear IRQ */
	ir = ssed_w8r8(priv, GET_IRQ);

	if (ir & 0x10) {
		dev_info(&priv->spi->dev, "Frame send\n");
		netif_wake_queue(priv->net);
	}
	if (ir & 0x04) {
		dev_info(&priv->spi->dev, "Frame reveived\n");
		ssed_recv_frames(priv);
	}
}

static irqreturn_t ssed_irq(int irq, void *irq_data)
{
	struct ssed_net *priv = (struct ssed_net *) irq_data;
	dev_info(&priv->spi->dev, "IRQ occured!\n");

	schedule_work(&priv->work);

	return 0;
}

static int ssed_mdio_read(struct mii_bus *bus, int phy_id, int reg)
{
	int status;
	u8 data[3];
	struct ssed_net *priv = bus->priv;
//	struct device *dev = &priv->spi->dev;

//	dev_info(dev, "ssed_mdio_read phy_id: %d, reg: 0x%x\n", phy_id, reg);

	data[0] = SET_SMI_OP;
	data[1] = (phy_id >> 4);
	data[2] = reg | (phy_id << 5);

	mutex_lock(&priv->lock);

	status = spi_write(priv->spi, data, sizeof(data));
	if (status)
		goto out;

	/* Wait 1ms for the SMI transfer to finish */
	udelay(1000);

	data[0] = GET_SMI;
	status = spi_write(priv->spi, data, 1);
	if (status)
		goto out;

	status = spi_read(priv->spi, data, 2);
	if (status)
		goto out;

	status = (data[0] << 8) | data[1];
//	dev_info(dev, "ssed_mdio_read returned: 0x%x\n", status);
out:
	mutex_unlock(&priv->lock);
	return status;
}

static int ssed_mdio_write(struct mii_bus *bus, int phy_id, int reg, u16 val)
{
	int status;
	u8 data[3];
	struct ssed_net *priv = bus->priv;
//	struct device *dev = &priv->spi->dev;

//	dev_info(dev, "ssed_mdio_write phy_id: %d, reg: 0x%x, val: 0x%x\n", phy_id, reg, val);

	data[0] = SET_SMI;
	data[1] = (val >> 4);
	data[2] = val;

	mutex_lock(&priv->lock);

	status = spi_write(priv->spi, data, sizeof(data));
	if (status)
		goto out;

	data[0] = SET_SMI_OP;
	data[1] = (1 << 2) | (phy_id >> 4);
	data[2] = reg | (phy_id << 5);

	status = spi_write(priv->spi, data, sizeof(data));
	if (status)
		goto out;

out:
	mutex_unlock(&priv->lock);
	return status;
}

static int ssed_mdio_init(struct ssed_net *priv)
{
	struct mii_bus *bus;
	int status;
	struct device *dev = &priv->spi->dev;

	bus = mdiobus_alloc();
	if (!bus) {
		dev_err(dev, "Failed to allocate mdiobus\n");
		return -ENOMEM;
	}

	snprintf(bus->id, MII_BUS_ID_SIZE, "mdio-ssed");
	bus->priv = priv;
	bus->name = "SSED MDIO";
	bus->read = ssed_mdio_read;
	bus->write = ssed_mdio_write;
	bus->parent = &priv->spi->dev;

	status = mdiobus_register(bus);

	if (status) {
		dev_err(dev, "Failed to register mdiobus\n");
		goto out;
	}

	priv->phy = phy_find_first(bus);
	if (!priv->phy) {
		dev_err(dev, "No PHY found!\n");
		mdiobus_unregister(bus);
		status = -1;
		goto out;
	}

	printk("Found PHY %s\n", priv->phy->drv->name);
	priv->net->phydev = priv->phy;

	priv->mii_bus = bus;

	return 0;
out:
	mdiobus_free(bus);
	return status;
}

static void ssed_xmit_timeout(struct net_device *net, unsigned int txqueue)
{
	struct ssed_net *priv = netdev_priv(net);
	dev_info(&priv->spi->dev, "XMIT timeout\n");
	netif_trans_update(priv->net);
	netif_wake_queue(priv->net);
}

static int ssed_ioctl(struct net_device *net, struct ifreq *rq, int cmd)
{
	if (!net->phydev) {
		dev_err(&net->dev, "No phydev\n");
		return -EINVAL;
	}

	if (!netif_running(net)) {
		dev_err(&net->dev, "Netdev not running\n");
		return -EINVAL;
	}

	switch (cmd) {
		case SIOCGMIIPHY:
		case SIOCGMIIREG:
		case SIOCSMIIREG:
			return phy_mii_ioctl(net->phydev, rq, cmd);
		default:
			return -EOPNOTSUPP;
	}
}

static int ssed_set_mac_addr(struct net_device *net, void *address)
{
	struct ssed_net *priv = netdev_priv(net);
	struct sockaddr *addr = address;
	u8 data[7];
	int status;

	if (netif_running(net))
		return -EBUSY;

	eth_hw_addr_set(net, addr->sa_data);
	data[0] = SET_MAC;
	memcpy(&data[1], addr->sa_data, ETH_ALEN);

	mutex_lock(&priv->lock);
	status =  spi_write(priv->spi, data, sizeof(data));
	mutex_unlock(&priv->lock);

	return 0;
}

void ssed_hw_xmit(struct work_struct *work)
{
	u8 spi_data[3], *data, shortpkt[ETH_ZLEN];
	int len, status;
	struct ssed_net *priv = container_of(work, struct ssed_net, xmit_work);

	data = priv->tx_skb->data;
	len = priv->tx_skb->len;
	if (len < ETH_ZLEN) {
		memset(shortpkt, 0, ETH_ZLEN);
		memcpy(shortpkt, data, len);
		len = ETH_ZLEN;
		data = shortpkt;
	}

	/* Transmit the package */
	spi_data[0] = SEND_FRAME;
	spi_data[1] = len >> 8;
	spi_data[2] = len;

	mutex_lock(&priv->lock);
	status = spi_write(priv->spi, spi_data, 3);
	if (status)
		goto out;

	status = spi_write(priv->spi, data, len);
	if (status)
		goto out;
	
out:
	mutex_unlock(&priv->lock);

	dev_info(&priv->spi->dev, "Packet with %d was transfered\n", len);
}

static netdev_tx_t ssed_send(struct sk_buff *skb, struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	/* Stop the queue, we can only handle a package... */
	netif_stop_queue(net);

	dev_info(&priv->spi->dev, "add a packet to queue\n");
	priv->tx_skb = skb;

	schedule_work(&priv->xmit_work);

	return NETDEV_TX_OK;
}

static int ssed_net_open(struct net_device *net)
{
	dev_info(&net->dev, "ssed_net_open\n");
	return 0;
}

static int ssed_net_release(struct net_device *net)
{
	dev_info(&net->dev, "ssed_net_release\n");
	return 0;
}

static const struct net_device_ops ssed_net_ops = {
	.ndo_open = ssed_net_open,
	.ndo_stop = ssed_net_release,
	.ndo_start_xmit = ssed_send,
	.ndo_tx_timeout = ssed_xmit_timeout,
	.ndo_eth_ioctl = ssed_ioctl,
	.ndo_set_mac_address = ssed_set_mac_addr,
};

static void ssed_net_init(struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	dev_info(&net->dev, "ssed_net_init\n");

	ether_setup(net);
	net->netdev_ops = &ssed_net_ops;
	net->watchdog_timeo = msecs_to_jiffies(10);

	memset(priv, 0, sizeof(struct ssed_net));
	priv->net = net;
}


static int ssed_probe(struct spi_device *spi)
{
	int status;
	struct net_device *net;
	struct ssed_net *priv;

	dev_info(&spi->dev, "Probe function\n");

	net = alloc_netdev(sizeof(struct ssed_net), "ssed%d", NET_NAME_UNKNOWN, ssed_net_init);

	if (!net)
		return -ENOMEM;

	priv = netdev_priv(net);
	
	priv->spi = spi;
	INIT_WORK(&priv->work, ssed_irq_work_handler);
	INIT_WORK(&priv->xmit_work, ssed_hw_xmit);
	mutex_init(&priv->lock);

	spi_set_drvdata(spi, priv);

	status = ssed_mdio_init(priv);
	if (status) {
		dev_err(&spi->dev, "Error init mdiobus\n");
		goto out;
	}

	printk("ssed - Set the MAC address\n");

	/* Set a random MAC address */
	eth_hw_addr_random(net);
	dev_info(&spi->dev, "MAC address is now %pM\n", net->dev_addr);

	/* Request IRQ */
	status = request_irq(spi->irq, ssed_irq, 0, "ssed", priv);
	if (status) {
		dev_err(&spi->dev, "Error requesting interrupt\n");
		goto out;
	}

	printk("ssed - Probing done!\n");

	return register_netdev(net);
out:
	free_netdev(net);
	return status;
}

static void ssed_remove(struct spi_device *spi)
{
	struct ssed_net *priv = spi_get_drvdata(spi);

	dev_info(&spi->dev, "Remove function\n");
	if (priv->mii_bus) {
		mdiobus_unregister(priv->mii_bus);
		mdiobus_free(priv->mii_bus);
	}
	free_irq(spi->irq, priv);
	unregister_netdev(priv->net);
	free_netdev(priv->net);
}

static const struct of_device_id ssed_dt_ids[] = {
        { .compatible = "brightlight,ssed" },
        { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ssed_dt_ids);

static struct spi_driver ssed_driver = {
        .driver = {
                .name = "ssed",
                .of_match_table = ssed_dt_ids,
         },
        .probe = ssed_probe,
        .remove = ssed_remove,
};
module_spi_driver(ssed_driver);

MODULE_DESCRIPTION("Simple SPI Ethernet device network driver");
MODULE_AUTHOR("Johannes 4Linux");
MODULE_LICENSE("GPL");

