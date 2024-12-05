// SPDX-License-Identifier: GPL-2.0
/*
 * AD7606 SPI ADC driver
 *
 * Copyright 2011 Analog Devices Inc.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/types.h>

#include <linux/iio/iio.h>
#include "ad7606.h"

#define MAX_SPI_FREQ_HZ		23500000	/* VDRIVE above 4.75 V */

#define AD7616_CONFIGURATION_REGISTER	0x02
#define AD7616_OS_MASK			GENMASK(4, 2)
#define AD7616_BURST_MODE		BIT(6)
#define AD7616_SEQEN_MODE		BIT(5)
#define AD7616_RANGE_CH_A_ADDR_OFF	0x04
#define AD7616_RANGE_CH_B_ADDR_OFF	0x06
/*
 * Range of channels from a group are stored in 2 registers.
 * 0, 1, 2, 3 in a register followed by 4, 5, 6, 7 in second register.
 * For channels from second group(8-15) the order is the same, only with
 * an offset of 2 for register address.
 */
#define AD7616_RANGE_CH_ADDR(ch)	((ch) >> 2)
/* The range of the channel is stored in 2 bits */
#define AD7616_RANGE_CH_MSK(ch)		(0b11 << (((ch) & 0b11) * 2))
#define AD7616_RANGE_CH_MODE(ch, mode)	((mode) << ((((ch) & 0b11)) * 2))

#define AD7606_CONFIGURATION_REGISTER	0x02
#define AD7606_SINGLE_DOUT		0x00

/*
 * Range for AD7606B channels are stored in registers starting with address 0x3.
 * Each register stores range for 2 channels(4 bits per channel).
 */
#define AD7606_RANGE_CH_MSK(ch)		(GENMASK(3, 0) << (4 * ((ch) & 0x1)))
#define AD7606_RANGE_CH_MODE(ch, mode)	\
	((GENMASK(3, 0) & mode) << (4 * ((ch) & 0x1)))
#define AD7606_RANGE_CH_ADDR(ch)	(0x03 + ((ch) >> 1))
#define AD7606_OS_MODE			0x08

static const struct iio_chan_spec ad7616_sw_channels[] = {
	IIO_CHAN_SOFT_TIMESTAMP(16),
	AD7616_CHANNEL(0),
	AD7616_CHANNEL(1),
	AD7616_CHANNEL(2),
	AD7616_CHANNEL(3),
	AD7616_CHANNEL(4),
	AD7616_CHANNEL(5),
	AD7616_CHANNEL(6),
	AD7616_CHANNEL(7),
	AD7616_CHANNEL(8),
	AD7616_CHANNEL(9),
	AD7616_CHANNEL(10),
	AD7616_CHANNEL(11),
	AD7616_CHANNEL(12),
	AD7616_CHANNEL(13),
	AD7616_CHANNEL(14),
	AD7616_CHANNEL(15),
};

static const struct iio_chan_spec ad7606b_sw_channels[] = {
	IIO_CHAN_SOFT_TIMESTAMP(8),
	AD7606_SW_CHANNEL(0, 16),
	AD7606_SW_CHANNEL(1, 16),
	AD7606_SW_CHANNEL(2, 16),
	AD7606_SW_CHANNEL(3, 16),
	AD7606_SW_CHANNEL(4, 16),
	AD7606_SW_CHANNEL(5, 16),
	AD7606_SW_CHANNEL(6, 16),
	AD7606_SW_CHANNEL(7, 16),
};

static const struct iio_chan_spec ad7606c_18_sw_channels[] = {
	IIO_CHAN_SOFT_TIMESTAMP(8),
	AD7606_SW_CHANNEL(0, 18),
	AD7606_SW_CHANNEL(1, 18),
	AD7606_SW_CHANNEL(2, 18),
	AD7606_SW_CHANNEL(3, 18),
	AD7606_SW_CHANNEL(4, 18),
	AD7606_SW_CHANNEL(5, 18),
	AD7606_SW_CHANNEL(6, 18),
	AD7606_SW_CHANNEL(7, 18),
};

static const unsigned int ad7606B_oversampling_avail[9] = {
	1, 2, 4, 8, 16, 32, 64, 128, 256
};

static u16 ad7616_spi_rd_wr_cmd(int addr, char isWriteOp)
{
	/*
	 * The address of register consist of one w/r bit
	 * 6 bits of address followed by one reserved bit.
	 */
	return ((addr & 0x7F) << 1) | ((isWriteOp & 0x1) << 7);
}

static u16 ad7606B_spi_rd_wr_cmd(int addr, char is_write_op)
{
	/*
	 * The address of register consists of one bit which
	 * specifies a read command placed in bit 6, followed by
	 * 6 bits of address.
	 */
	return (addr & 0x3F) | (((~is_write_op) & 0x1) << 6);
}

static int ad7606_spi_read_block(struct device *dev,
				 int count, void *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	int i, ret;
	unsigned short *data = buf;
	__be16 *bdata = buf;

	ret = spi_read(spi, buf, count * 2);
	if (ret < 0) {
		dev_err(&spi->dev, "SPI read error\n");
		return ret;
	}

	for (i = 0; i < count; i++)
		data[i] = be16_to_cpu(bdata[i]);

	return 0;
}

static int ad7606_spi_read_block14to16(struct device *dev,
				       int count, void *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct spi_transfer xfer = {
		.bits_per_word = 14,
		.len = count * sizeof(u16),
		.rx_buf = buf,
	};

	return spi_sync_transfer(spi, &xfer, 1);
}

static int ad7606_spi_read_block18to32(struct device *dev,
				       int count, void *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct spi_transfer xfer = {
		.bits_per_word = 18,
		.len = count * sizeof(u32),
		.rx_buf = buf,
	};

	return spi_sync_transfer(spi, &xfer, 1);
}

static int ad7606_spi_reg_read(struct ad7606_state *st, unsigned int addr)
{
	struct spi_device *spi = to_spi_device(st->dev);
	struct spi_transfer t[] = {
		{
			.tx_buf = &st->d16[0],
			.len = 2,
			.cs_change = 0,
		}, {
			.rx_buf = &st->d16[1],
			.len = 2,
		},
	};
	int ret;

	st->d16[0] = cpu_to_be16(st->bops->rd_wr_cmd(addr, 0) << 8);

	ret = spi_sync_transfer(spi, t, ARRAY_SIZE(t));
	if (ret < 0)
		return ret;

	return be16_to_cpu(st->d16[1]);
}

static int ad7606_spi_reg_write(struct ad7606_state *st,
				unsigned int addr,
				unsigned int val)
{
	struct spi_device *spi = to_spi_device(st->dev);

	st->d16[0] = cpu_to_be16((st->bops->rd_wr_cmd(addr, 1) << 8) |
				  (val & 0x1FF));

	return spi_write(spi, &st->d16[0], sizeof(st->d16[0]));
}

static int ad7606_spi_write_mask(struct ad7606_state *st,
				 unsigned int addr,
				 unsigned long mask,
				 unsigned int val)
{
	int readval;

	readval = st->bops->reg_read(st, addr);
	if (readval < 0)
		return readval;

	readval &= ~mask;
	readval |= val;

	return st->bops->reg_write(st, addr, readval);
}

static int ad7616_write_scale_sw(struct iio_dev *indio_dev, int ch, int val)
{
	struct ad7606_state *st = iio_priv(indio_dev);
	unsigned int ch_addr, mode, ch_index;


	/*
	 * Ad7616 has 16 channels divided in group A and group B.
	 * The range of channels from A are stored in registers with address 4
	 * while channels from B are stored in register with address 6.
	 * The last bit from channels determines if it is from group A or B
	 * because the order of channels in iio is 0A, 0B, 1A, 1B...
	 */
	ch_index = ch >> 1;

	ch_addr = AD7616_RANGE_CH_ADDR(ch_index);

	if ((ch & 0x1) == 0) /* channel A */
		ch_addr += AD7616_RANGE_CH_A_ADDR_OFF;
	else	/* channel B */
		ch_addr += AD7616_RANGE_CH_B_ADDR_OFF;

	/* 0b01 for 2.5v, 0b10 for 5v and 0b11 for 10v */
	mode = AD7616_RANGE_CH_MODE(ch_index, ((val + 1) & 0b11));
	return st->bops->write_mask(st, ch_addr, AD7616_RANGE_CH_MSK(ch_index),
				     mode);
}

static int ad7616_write_os_sw(struct iio_dev *indio_dev, int val)
{
	struct ad7606_state *st = iio_priv(indio_dev);

	return st->bops->write_mask(st, AD7616_CONFIGURATION_REGISTER,
				     AD7616_OS_MASK, val << 2);
}

static int ad7606_write_scale_sw(struct iio_dev *indio_dev, int ch, int val)
{
	struct ad7606_state *st = iio_priv(indio_dev);

	return ad7606_spi_write_mask(st,
				     AD7606_RANGE_CH_ADDR(ch),
				     AD7606_RANGE_CH_MSK(ch),
				     AD7606_RANGE_CH_MODE(ch, val));
}

static int ad7606_write_os_sw(struct iio_dev *indio_dev, int val)
{
	struct ad7606_state *st = iio_priv(indio_dev);

	return ad7606_spi_reg_write(st, AD7606_OS_MODE, val);
}

static int ad7616_sw_mode_config(struct iio_dev *indio_dev)
{
	struct ad7606_state *st = iio_priv(indio_dev);

	/*
	 * Scale can be configured individually for each channel
	 * in software mode.
	 */
	indio_dev->channels = ad7616_sw_channels;

	st->write_scale = ad7616_write_scale_sw;
	st->write_os = &ad7616_write_os_sw;

	/* Activate Burst mode and SEQEN MODE */
	return st->bops->write_mask(st,
			      AD7616_CONFIGURATION_REGISTER,
			      AD7616_BURST_MODE | AD7616_SEQEN_MODE,
			      AD7616_BURST_MODE | AD7616_SEQEN_MODE);
}

static int ad7606B_sw_mode_config(struct iio_dev *indio_dev)
{
	struct ad7606_state *st = iio_priv(indio_dev);
	DECLARE_BITMAP(os, 3);

	bitmap_fill(os, 3);
	/*
	 * Software mode is enabled when all three oversampling
	 * pins are set to high. If oversampling gpios are defined
	 * in the device tree, then they need to be set to high,
	 * otherwise, they must be hardwired to VDD
	 */
	if (st->gpio_os) {
		gpiod_set_array_value(st->gpio_os->ndescs,
				      st->gpio_os->desc, st->gpio_os->info, os);
	}
	/* OS of 128 and 256 are available only in software mode */
	st->oversampling_avail = ad7606B_oversampling_avail;
	st->num_os_ratios = ARRAY_SIZE(ad7606B_oversampling_avail);

	st->write_scale = ad7606_write_scale_sw;
	st->write_os = &ad7606_write_os_sw;

	/* Configure device spi to output on a single channel */
	st->bops->reg_write(st,
			    AD7606_CONFIGURATION_REGISTER,
			    AD7606_SINGLE_DOUT);

	/*
	 * Scale can be configured individually for each channel
	 * in software mode.
	 */
	indio_dev->channels = ad7606b_sw_channels;

	return 0;
}

static int ad7606c_18_sw_mode_config(struct iio_dev *indio_dev)
{
	int ret;

	ret = ad7606B_sw_mode_config(indio_dev);
	if (ret)
		return ret;

	indio_dev->channels = ad7606c_18_sw_channels;

	return 0;
}

static const struct ad7606_bus_ops ad7606_spi_bops = {
	.read_block = ad7606_spi_read_block,
};

static const struct ad7606_bus_ops ad7607_spi_bops = {
	.read_block = ad7606_spi_read_block14to16,
};

static const struct ad7606_bus_ops ad7608_spi_bops = {
	.read_block = ad7606_spi_read_block18to32,
};

static const struct ad7606_bus_ops ad7616_spi_bops = {
	.read_block = ad7606_spi_read_block,
	.reg_read = ad7606_spi_reg_read,
	.reg_write = ad7606_spi_reg_write,
	.write_mask = ad7606_spi_write_mask,
	.rd_wr_cmd = ad7616_spi_rd_wr_cmd,
	.sw_mode_config = ad7616_sw_mode_config,
};

static const struct ad7606_bus_ops ad7606b_spi_bops = {
	.read_block = ad7606_spi_read_block,
	.reg_read = ad7606_spi_reg_read,
	.reg_write = ad7606_spi_reg_write,
	.write_mask = ad7606_spi_write_mask,
	.rd_wr_cmd = ad7606B_spi_rd_wr_cmd,
	.sw_mode_config = ad7606B_sw_mode_config,
};

static const struct ad7606_bus_ops ad7606c_18_spi_bops = {
	.read_block = ad7606_spi_read_block18to32,
	.reg_read = ad7606_spi_reg_read,
	.reg_write = ad7606_spi_reg_write,
	.write_mask = ad7606_spi_write_mask,
	.rd_wr_cmd = ad7606B_spi_rd_wr_cmd,
	.sw_mode_config = ad7606c_18_sw_mode_config,
};

static const struct ad7606_bus_info ad7605_4_bus_info = {
	.chip_info = &ad7605_4_info,
	.bops = &ad7606_spi_bops,
};

static const struct ad7606_bus_info ad7606_8_bus_info = {
	.chip_info = &ad7606_8_info,
	.bops = &ad7606_spi_bops,
};

static const struct ad7606_bus_info ad7606_6_bus_info = {
	.chip_info = &ad7606_6_info,
	.bops = &ad7606_spi_bops,
};

static const struct ad7606_bus_info ad7606_4_bus_info = {
	.chip_info = &ad7606_4_info,
	.bops = &ad7606_spi_bops,
};

static const struct ad7606_bus_info ad7606b_bus_info = {
	.chip_info = &ad7606b_info,
	.bops = &ad7606b_spi_bops,
};

static const struct ad7606_bus_info ad7606c_16_bus_info = {
	.chip_info = &ad7606c_16_info,
	.bops = &ad7606b_spi_bops,
};

static const struct ad7606_bus_info ad7606c_18_bus_info = {
	.chip_info = &ad7606c_18_info,
	.bops = &ad7606c_18_spi_bops,
};

static const struct ad7606_bus_info ad7607_bus_info = {
	.chip_info = &ad7607_info,
	.bops = &ad7607_spi_bops,
};

static const struct ad7606_bus_info ad7608_bus_info = {
	.chip_info = &ad7608_info,
	.bops = &ad7608_spi_bops,
};

static const struct ad7606_bus_info ad7609_bus_info = {
	.chip_info = &ad7609_info,
	.bops = &ad7608_spi_bops,
};

static const struct ad7606_bus_info ad7616_bus_info = {
	.chip_info = &ad7616_info,
	.bops = &ad7616_spi_bops,
};

static int ad7606_spi_probe(struct spi_device *spi)
{
	const struct ad7606_bus_info *bus_info = spi_get_device_match_data(spi);

	return ad7606_probe(&spi->dev, spi->irq, NULL,
			    bus_info->chip_info, bus_info->bops);
}

static const struct spi_device_id ad7606_id_table[] = {
	{ "ad7605-4", (kernel_ulong_t)&ad7605_4_bus_info },
	{ "ad7606-4", (kernel_ulong_t)&ad7606_4_bus_info },
	{ "ad7606-6", (kernel_ulong_t)&ad7606_6_bus_info },
	{ "ad7606-8", (kernel_ulong_t)&ad7606_8_bus_info },
	{ "ad7606b",  (kernel_ulong_t)&ad7606b_bus_info },
	{ "ad7606c-16", (kernel_ulong_t)&ad7606c_16_bus_info },
	{ "ad7606c-18", (kernel_ulong_t)&ad7606c_18_bus_info },
	{ "ad7607",   (kernel_ulong_t)&ad7607_bus_info },
	{ "ad7608",   (kernel_ulong_t)&ad7608_bus_info },
	{ "ad7609",   (kernel_ulong_t)&ad7609_bus_info },
	{ "ad7616",   (kernel_ulong_t)&ad7616_bus_info },
	{ }
};
MODULE_DEVICE_TABLE(spi, ad7606_id_table);

static const struct of_device_id ad7606_of_match[] = {
	{ .compatible = "adi,ad7605-4", .data = &ad7605_4_bus_info },
	{ .compatible = "adi,ad7606-4", .data = &ad7606_4_bus_info },
	{ .compatible = "adi,ad7606-6", .data = &ad7606_6_bus_info },
	{ .compatible = "adi,ad7606-8", .data = &ad7606_8_bus_info },
	{ .compatible = "adi,ad7606b", .data = &ad7606b_bus_info },
	{ .compatible = "adi,ad7606c-16", .data = &ad7606c_16_bus_info },
	{ .compatible = "adi,ad7606c-18", .data = &ad7606c_18_bus_info },
	{ .compatible = "adi,ad7607", .data = &ad7607_bus_info },
	{ .compatible = "adi,ad7608", .data = &ad7608_bus_info },
	{ .compatible = "adi,ad7609", .data = &ad7609_bus_info },
	{ .compatible = "adi,ad7616", .data = &ad7616_bus_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ad7606_of_match);

static struct spi_driver ad7606_driver = {
	.driver = {
		.name = "ad7606",
		.of_match_table = ad7606_of_match,
		.pm = AD7606_PM_OPS,
	},
	.probe = ad7606_spi_probe,
	.id_table = ad7606_id_table,
};
module_spi_driver(ad7606_driver);

MODULE_AUTHOR("Michael Hennerich <michael.hennerich@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD7606 ADC");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(IIO_AD7606);
