#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>

struct ssed_net {
	struct net_device *net;
	struct spi_device *spi;
	struct work_struct work;
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

	spi_set_drvdata(spi, priv);

	/* Request IRQ */
	status = request_irq(spi->irq, ssed_irq, 0, "ssed", priv);
	if (status) {
		dev_err(&spi->dev, "Error requesting interrupt\n");
		goto out;
	}

	return register_netdev(net);

	return 0;
out:
	free_netdev(net);
	return status;
}

static void ssed_remove(struct spi_device *spi)
{
	struct ssed_net *ssed = spi_get_drvdata(spi);

	dev_info(&spi->dev, "Remove function\n");
	free_irq(spi->irq, ssed);
	unregister_netdev(ssed->net);
	free_netdev(ssed->net);
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

