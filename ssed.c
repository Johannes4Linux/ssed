#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>

struct ssed_net {
	struct spi_device *spi;
	struct mutex lock;
	struct net_device *net;
	struct work_struct irq_work;
};

static int ssed_write_read(struct ssed_net *priv, u8 *wr_buf, u8 wr_size, u8 *rd_buf, u8 rd_size)
{
	int status;

	status = spi_write(priv->spi, wr_buf, wr_size);
	if (status)
		return status;

	udelay(15);

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

static void ssed_irq_work_handler(struct work_struct *w)
{
	struct ssed_net *priv = container_of(w, struct ssed_net, irq_work);
	u8 resp = ssed_w8r8(priv, 0x8);

	dev_info(&priv->spi->dev, "IRQ Work handler is running. IRQ flags: 0x%x\n", resp);
}

static irqreturn_t ssed_irq(int irq, void *irq_data)
{
	struct ssed_net *priv = irq_data;
	pr_info("IRQ was triggered!\n");

	schedule_work(&priv->irq_work);
	return IRQ_HANDLED;
}

static int ssed_net_open(struct net_device *net)
{
	struct ssed_net *priv = netdev_priv(net);

	dev_info(&priv->spi->dev, "ssed_net_open\n");

	netif_stop_queue(net);

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

	spi_set_drvdata(spi, priv);

	status = request_irq(spi->irq, ssed_irq, 0, "ssed", priv);
	if (status) {
		free_netdev(net);
		return status;
	}

	status = register_netdev(net);

	if (status) {
		free_netdev(net);
		return status;
	}

	return 0;
}

static void ssed_remove(struct spi_device *spi)
{
	struct ssed_net *priv;
	dev_info(&spi->dev, "ssed_remove\n");

	priv = spi_get_drvdata(spi);
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

