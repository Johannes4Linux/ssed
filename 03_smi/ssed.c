#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/phy.h>

#define SET_SMI_OP 0x1
#define GET_SMI 0x2
#define SET_SMI 0x3

struct ssed_net {
	struct net_device *net;
	struct spi_device *spi;
	struct work_struct work;
	struct phy_device *phy;
	struct mii_bus *mii_bus;
	struct mutex lock;
};

static int ssed_read_write(struct ssed_net *priv, u8 *wdata, u8 wlen, u8 *rdata, u8 rlen)
{
	int status;

	/* Lock mutex */
	mutex_lock(&priv->lock);

	/* Write out data */
	status = spi_write(priv->spi, wdata, wlen);
	if (status)
		goto out;
	/* Small delay, so W7500 can react */
	udelay(25);
	/* Read back data */
	status = spi_read(priv->spi, rdata, rlen);

out:
	mutex_unlock(&priv->lock);
	return status;
}

static int ssed_w8r8(struct ssed_net *priv, u8 cmd)
{
	int status;
	u8 resp;

	status = ssed_read_write(priv, &cmd, 1, &resp, 1);
	if (status >= 0)
		return resp;
	else
		return status;
}

void ssed_irq_work_handler(struct work_struct *work)
{
	u8 data = 0x08;
	struct ssed_net *priv = container_of(work, struct ssed_net, work);

	 /* Read out and clear IRQ */
	ssed_w8r8(priv, data);
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
	struct device *dev = &priv->net->dev;

	dev_info(dev, "ssed_mdio_read phy_id: %d, reg: 0x%x\n", phy_id, reg);

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
	dev_info(dev, "ssed_mdio_read returned: 0x%x\n", status);
out:
	mutex_unlock(&priv->lock);
	return status;
}

static int ssed_mdio_write(struct mii_bus *bus, int phy_id, int reg, u16 val)
{
	int status;
	u8 data[3];
	struct ssed_net *priv = bus->priv;
	struct device *dev = &priv->net->dev;

	dev_info(dev, "ssed_mdio_write phy_id: %d, reg: 0x%x, val: 0x%x\n", phy_id, reg, val);

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
	struct device *dev = &priv->net->dev;

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


	dev_info(dev, "Found PHY %s\n", priv->phy->drv->name);

	priv->mii_bus = bus;

	return 0;
out:
	mdiobus_free(bus);
	return status;
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
};

static void ssed_net_init(struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	dev_info(&net->dev, "ssed_net_init\n");

	ether_setup(net);
	net->netdev_ops = &ssed_net_ops;

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
	mutex_init(&priv->lock);

	spi_set_drvdata(spi, priv);

	status = ssed_mdio_init(priv);
	if (status) {
		dev_err(&spi->dev, "Error init mdiobus\n");
		goto out;
	}

	/* Request IRQ */
	status = request_irq(spi->irq, ssed_irq, 0, "ssed", priv);
	if (status) {
		dev_err(&spi->dev, "Error requesting interrupt\n");
		goto out;
	}

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

