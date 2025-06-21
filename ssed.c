#include <linux/spi/spi.h>
#include <linux/delay.h>

static int ssed_probe(struct spi_device *spi)
{
	int status;
	u8 data = 10;

	status = spi_write(spi, &data, 1);

	if (status) {
		dev_err(&spi->dev, "SPI write failed\n");
		return status;
	}

	udelay(25);

	status = spi_read(spi, &data, 1);

	if (status) {
		dev_err(&spi->dev, "SPI read failed\n");
		return status;
	}

	dev_info(&spi->dev, "data: 0x%x\n", data);

	return 0;
}

static void ssed_remove(struct spi_device *spi)
{
	dev_info(&spi->dev, "ssed_remove\n");
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

