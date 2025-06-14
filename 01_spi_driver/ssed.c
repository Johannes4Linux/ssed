#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

struct ssed_net {
	struct spi_device *spi;
	struct work_struct work;
};

void ssed_irq_work_handler(struct work_struct *work)
{
	u8 data = 0x08;
	struct ssed_net *ssed = container_of(work, struct ssed_net, work);

	spi_write(ssed->spi, &data, 1);
	spi_read(ssed->spi, &data, 1);
}

static irqreturn_t ssed_irq(int irq, void *irq_data)
{
	struct ssed_net *ssed = (struct ssed_net *) irq_data;
	dev_info(&ssed->spi->dev, "IRQ occured!\n");

	schedule_work(&ssed->work);

	return 0;
}

static int ssed_probe(struct spi_device *spi)
{
	int status;
	struct ssed_net *ssed;
	u8 data = 0xa;

	dev_info(&spi->dev, "Probe function\n");

	ssed = kzalloc(sizeof(struct ssed_net), GFP_KERNEL);
	
	if (!ssed)
		return -ENOMEM;

	ssed->spi = spi;
	INIT_WORK(&ssed->work, ssed_irq_work_handler);

	spi_set_drvdata(spi, ssed);

	/* Check alive */
	data = spi_w8r8(spi, data);
	dev_info(&spi->dev, "Got 0x%x\n", data);

	/* Test IRQ */
	status = request_irq(spi->irq, ssed_irq, 0, "ssed", ssed);
	if (status) {
		dev_err(&spi->dev, "Error requesting interrupt\n");
		return status;
	}

	data = 0xb;
	spi_write(spi, &data, 1);

	return 0;
}

static void ssed_remove(struct spi_device *spi)
{
	struct ssed_net *ssed = spi_get_drvdata(spi);

	dev_info(&spi->dev, "Remove function\n");
	free_irq(spi->irq, ssed);
	kfree(ssed);

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

