#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/etherdevice.h>

struct ssed_net {
	struct spi_device *spi;
	struct mutex lock;
	struct net_device *net;
	struct work_struct irq_work;
	struct mii_bus *bus;
	struct phy_device *phy;
	struct work_struct xmit_work;
	struct sk_buff *tx_skb;
};

#define SET_SMI_OP	0x1
#define GET_SMI		0x2
#define SET_SMI		0x3
#define SET_MAC		0x4
#define SEND_FRAME	0x6
#define RECV_FRAME	0x7
#define GET_IRQ		0x8

static int ssed_write_read(struct ssed_net *priv, u8 *wr_buf, u8 wr_size, u8 *rd_buf, u8 rd_size)
{
	int status;

	status = spi_write(priv->spi, wr_buf, wr_size);
	if (status)
		return status;

	usleep_range(10, 20);

	return spi_read(priv->spi, rd_buf, rd_size);
}

static int ssed_w8r8(struct ssed_net *priv, u8 cmd)
{
	int status;
	u8 resp;

	mutex_lock(&priv->lock);
	status = ssed_write_read(priv, &cmd, 1, &resp, 1);
	mutex_unlock(&priv->lock);

	if (status)
		return status;
	return resp;
}

static int ssed_mdio_read(struct mii_bus *bus, int phy_addr, int reg_addr)
{
	int status;
	u8 data[3];
	struct ssed_net *priv = bus->priv;

	data[0] = SET_SMI_OP;
	data[1] = (phy_addr >> 4);
	data[2] = (phy_addr << 5) | reg_addr;

	mutex_lock(&priv->lock);

	status = ssed_write_read(priv, data, sizeof(data), NULL, 0);
	if (status)
		goto out;

	usleep_range(900, 1100);

	data[0] = GET_SMI;
	status = ssed_write_read(priv, data, 1, &data[1], 2);
	if (status)
		goto out;

	status = (data[1] << 8) | data[2];

	dev_info(&priv->spi->dev, "MDIO Read: phy_addr: %d, reg_addr: %d, value: 0x%04x\n", phy_addr, reg_addr, status);

out:
	mutex_unlock(&priv->lock);
	return status;
}

static int ssed_mdio_write(struct mii_bus *bus, int phy_addr, int reg_addr, u16 value)
{
	int status;
	u8 data[3];
	struct ssed_net *priv = bus->priv;

	data[0] = SET_SMI;
	data[1] = value >> 8;
	data[2] = value;

	mutex_lock(&priv->lock);

	status = ssed_write_read(priv, data, sizeof(data), NULL, 0);
	if (status)
		goto out;

	data[0] = SET_SMI_OP;
	data[1] = (1<<2) | (phy_addr >> 4);
	data[2] = (phy_addr << 5) | reg_addr;


	status = ssed_write_read(priv, data, sizeof(data), NULL, 0);
	if (status)
		goto out;

	dev_info(&priv->spi->dev, "MDIO Write: phy_addr: %d, reg_addr: %d, value: 0x%04x\n", phy_addr, reg_addr, value);

out:
	mutex_unlock(&priv->lock);
	return status;
}

static int ssed_mdio_init(struct ssed_net *priv)
{
	int status;
	struct device *dev = &priv->spi->dev;
	struct mii_bus *bus = mdiobus_alloc();

	if(!bus)
		return -ENOMEM;

	snprintf(bus->id, MII_BUS_ID_SIZE, "mdio-ssed");
	bus->priv = priv;
	bus->name = "SSED MDIO";
	bus->read = ssed_mdio_read;
	bus->write = ssed_mdio_write;
	bus->parent = dev;

	status = mdiobus_register(bus);
	if (status) {
		dev_err(dev, "Error registering MDIO bus\n");
		goto out;
	}

	priv->phy = phy_find_first(bus);
	if (!priv->phy) {
		dev_err(dev, "Error: No Ethernet PHY found\n");
		status = -ENODEV;
		mdiobus_unregister(bus);
		goto out;
	}

	dev_info(dev, "Found Ethernet PHY: %s\n", priv->phy->drv->name);
	priv->bus = bus;
	priv->net->phydev = priv->phy;

	return 0;
out:
	mdiobus_free(bus);
	return status;
}

static void ssed_recv_frames(struct ssed_net *priv)
{
	u8 data[2], cmd = RECV_FRAME;
	u16 len;
	int status;
	struct sk_buff *rx_skb;

	do {
		mutex_lock(&priv->lock);
		status = ssed_write_read(priv, &cmd, 1, data, 2);
		len = (data[0] << 8) | data[1];

		if ((len > 0) && (status == 0)) {
			do {
				rx_skb = dev_alloc_skb(len);
				if (!rx_skb) {
					dev_err(&priv->spi->dev, "Could not allocate memory for rx_skb\n");
					continue;
				}
			} while (!rx_skb);

			status = spi_read(priv->spi, skb_put(rx_skb, len), len);
		}
		mutex_unlock(&priv->lock);

		if ((len > 0) && (status == 0)) {
			rx_skb->dev = priv->net;
			rx_skb->protocol = eth_type_trans(rx_skb, priv->net);

			netif_receive_skb(rx_skb);
			dev_info(&priv->spi->dev, "Received frame with %d bytes\n", len);
		}
	} while (len);
}

static void ssed_irq_work_handler(struct work_struct *w)
{
	struct ssed_net *priv = container_of(w, struct ssed_net, irq_work);

	u8 resp = ssed_w8r8(priv, GET_IRQ);

	if (resp & 0x10) {
		dev_info(&priv->spi->dev, "Frame was send successfully!\n");
		netif_wake_queue(priv->net);
	}
	if (resp & 0x04)
		ssed_recv_frames(priv);

	dev_info(&priv->spi->dev, "IRQ Work handler is running. IRQ flags: 0x%x\n", resp);
}

static irqreturn_t ssed_irq(int irq, void *irq_data)
{
	struct ssed_net *priv = irq_data;
	pr_info("IRQ was triggered!\n");

	schedule_work(&priv->irq_work);
	return IRQ_HANDLED;
}

static int ssed_ioctl(struct net_device *net, struct ifreq *rq, int cmd)
{
	if (!net->phydev)
		return -EINVAL;
	if (!netif_running(net))
		return -EINVAL;

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
	status = spi_write(priv->spi, data, sizeof(data));
	mutex_unlock(&priv->lock);

	return status;
}

static void ssed_hw_xmit(struct work_struct *work)
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

	spi_data[0] = SEND_FRAME;
	spi_data[1] = len >> 8;
	spi_data[2] = len;

	mutex_lock(&priv->lock);
	status = spi_write(priv->spi, spi_data, 3);
	if (status)
		goto out;

	status = spi_write(priv->spi, data, len);
out:
	mutex_unlock(&priv->lock);

	if (!status)
		dev_info(&priv->spi->dev, "Frame with %d bytes was send to SSED\n", len);

	dev_kfree_skb(priv->tx_skb);

}

static netdev_tx_t ssed_send(struct sk_buff *skb, struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	/* stop the transmit queue, because SSED can only handle one frame at a time */
	netif_stop_queue(net);

	priv->tx_skb = skb;
	schedule_work(&priv->xmit_work);

	return NETDEV_TX_OK;
}

static int ssed_net_open(struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	dev_info(&priv->spi->dev, "ssed_net_open\n");

	return 0;
}

static int ssed_net_release(struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	dev_info(&priv->spi->dev, "ssed_net_release\n");

	return 0;
}

static const struct net_device_ops ssed_net_ops = {
	.ndo_open = ssed_net_open,
	.ndo_stop = ssed_net_release,
	.ndo_eth_ioctl = ssed_ioctl,
	.ndo_set_mac_address = ssed_set_mac_addr,
	.ndo_start_xmit = ssed_send,
};

static void ssed_net_init(struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	pr_info("ssed_net_init\n");

	ether_setup(net);

	net->netdev_ops = &ssed_net_ops;

	memset(priv, 0, sizeof(struct ssed_net));
	priv->net = net;
}

static int ssed_probe(struct spi_device *spi)
{
	int status;
	struct ssed_net *priv;
	struct net_device *net;

	//net = alloc_etherdev(sizeof(struct ssed_net));
	net = alloc_netdev(sizeof(struct ssed_net), "ssed%d", NET_NAME_UNKNOWN, ssed_net_init);
	if (!net)
		return -ENOMEM;

	priv = netdev_priv(net);

	priv->spi = spi;
	mutex_init(&priv->lock);
	INIT_WORK(&priv->irq_work, ssed_irq_work_handler);
	INIT_WORK(&priv->xmit_work, ssed_hw_xmit);

	spi_set_drvdata(spi, priv);

	status = request_irq(spi->irq, ssed_irq, 0, "ssed", priv);
	if (status) {
		goto free_net;
	}

	status = ssed_mdio_init(priv);
	if (status) 
		goto free_irq;

	status = register_netdev(net);

	if (status)
		goto free_bus;

	/* Set a random MAC */
	eth_hw_addr_random(net);
	dev_info(&spi->dev, "MAC addr: %pM\n", net->dev_addr);

	return 0;

free_bus:
	mdiobus_unregister(priv->bus);
	mdiobus_free(priv->bus);
free_irq:
	free_irq(spi->irq, priv);
free_net:
	free_netdev(net);
	return status;

}

static void ssed_remove(struct spi_device *spi)
{
	struct ssed_net *priv;
	dev_info(&spi->dev, "ssed_remove\n");

	priv = spi_get_drvdata(spi);
	if (priv->bus) {
		mdiobus_unregister(priv->bus);
		mdiobus_free(priv->bus);
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

