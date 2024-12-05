// SPDX-License-Identifier: GPL-2.0

/***************************************************************************
 *    copyright            : (C) 2001, 2002 by Frank Mori Hess
 ***************************************************************************/

#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <linux/string.h>
#include <linux/init.h>
#include "nec7210.h"
#include "gpibP.h"

// struct which defines private_data for pc2 driver
struct pc2_priv {
	struct nec7210_priv nec7210_priv;
	unsigned int irq;
	// io address that clears interrupt for pc2a (0x2f0 + irq)
	unsigned int clear_intr_addr;
};

// pc2 uses 8 consecutive io addresses
static const int pc2_iosize = 8;
static const int pc2a_iosize = 8;
static const int pc2_2a_iosize = 16;

// offset between io addresses of successive nec7210 registers
static const int pc2a_reg_offset = 0x400;
static const int pc2_reg_offset = 1;

//interrupt service routine
static irqreturn_t pc2_interrupt(int irq, void *arg);
static irqreturn_t pc2a_interrupt(int irq, void *arg);

// pc2 specific registers and bits

// interrupt clear register address
static const int pc2a_clear_intr_iobase = 0x2f0;
static inline unsigned int CLEAR_INTR_REG(unsigned int irq)
{
	return pc2a_clear_intr_iobase + irq;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIB driver for PC2/PC2a and compatible devices");

static int pc2_attach(gpib_board_t *board, const gpib_board_config_t *config);
static int pc2a_attach(gpib_board_t *board, const gpib_board_config_t *config);
static int pc2a_cb7210_attach(gpib_board_t *board, const gpib_board_config_t *config);
static int pc2_2a_attach(gpib_board_t *board, const gpib_board_config_t *config);

static void pc2_detach(gpib_board_t *board);
static void pc2a_detach(gpib_board_t *board);
static void pc2_2a_detach(gpib_board_t *board);

/*
 * GPIB interrupt service routines
 */

irqreturn_t pc2_interrupt(int irq, void *arg)
{
	gpib_board_t *board = arg;
	struct pc2_priv *priv = board->private_data;
	unsigned long flags;
	irqreturn_t retval;

	spin_lock_irqsave(&board->spinlock, flags);
	retval = nec7210_interrupt(board, &priv->nec7210_priv);
	spin_unlock_irqrestore(&board->spinlock, flags);
	return retval;
}

irqreturn_t pc2a_interrupt(int irq, void *arg)
{
	gpib_board_t *board = arg;
	struct pc2_priv *priv = board->private_data;
	int status1, status2;
	unsigned long flags;
	irqreturn_t retval;

	spin_lock_irqsave(&board->spinlock, flags);
	// read interrupt status (also clears status)
	status1 = read_byte(&priv->nec7210_priv, ISR1);
	status2 = read_byte(&priv->nec7210_priv, ISR2);
	/* clear interrupt circuit */
	if (priv->irq)
		outb(0xff, CLEAR_INTR_REG(priv->irq));
	retval = nec7210_interrupt_have_status(board, &priv->nec7210_priv, status1, status2);
	spin_unlock_irqrestore(&board->spinlock, flags);
	return retval;
}

// wrappers for interface functions
static int pc2_read(gpib_board_t *board, uint8_t *buffer, size_t length, int *end,
		    size_t *bytes_read)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_read(board, &priv->nec7210_priv, buffer, length, end, bytes_read);
}

static int pc2_write(gpib_board_t *board, uint8_t *buffer, size_t length, int send_eoi,
		     size_t *bytes_written)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_write(board, &priv->nec7210_priv, buffer, length, send_eoi, bytes_written);
}

static int pc2_command(gpib_board_t *board, uint8_t *buffer, size_t length, size_t *bytes_written)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_command(board, &priv->nec7210_priv, buffer, length, bytes_written);
}

static int pc2_take_control(gpib_board_t *board, int synchronous)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_take_control(board, &priv->nec7210_priv, synchronous);
}

static int pc2_go_to_standby(gpib_board_t *board)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_go_to_standby(board, &priv->nec7210_priv);
}

static void pc2_request_system_control(gpib_board_t *board, int request_control)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_request_system_control(board, &priv->nec7210_priv, request_control);
}

static void pc2_interface_clear(gpib_board_t *board, int assert)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_interface_clear(board, &priv->nec7210_priv, assert);
}

static void pc2_remote_enable(gpib_board_t *board, int enable)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_remote_enable(board, &priv->nec7210_priv, enable);
}

static int pc2_enable_eos(gpib_board_t *board, uint8_t eos_byte, int compare_8_bits)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_enable_eos(board, &priv->nec7210_priv, eos_byte, compare_8_bits);
}

static void pc2_disable_eos(gpib_board_t *board)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_disable_eos(board, &priv->nec7210_priv);
}

static unsigned int pc2_update_status(gpib_board_t *board, unsigned int clear_mask)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_update_status(board, &priv->nec7210_priv, clear_mask);
}

static int pc2_primary_address(gpib_board_t *board, unsigned int address)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_primary_address(board, &priv->nec7210_priv, address);
}

static int pc2_secondary_address(gpib_board_t *board, unsigned int address, int enable)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_secondary_address(board, &priv->nec7210_priv, address, enable);
}

static int pc2_parallel_poll(gpib_board_t *board, uint8_t *result)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_parallel_poll(board, &priv->nec7210_priv, result);
}

static void pc2_parallel_poll_configure(gpib_board_t *board, uint8_t config)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_parallel_poll_configure(board, &priv->nec7210_priv, config);
}

static void pc2_parallel_poll_response(gpib_board_t *board, int ist)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_parallel_poll_response(board, &priv->nec7210_priv, ist);
}

static void pc2_serial_poll_response(gpib_board_t *board, uint8_t status)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_serial_poll_response(board, &priv->nec7210_priv, status);
}

static uint8_t pc2_serial_poll_status(gpib_board_t *board)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_serial_poll_status(board, &priv->nec7210_priv);
}

static unsigned int pc2_t1_delay(gpib_board_t *board, unsigned int nano_sec)
{
	struct pc2_priv *priv = board->private_data;

	return nec7210_t1_delay(board, &priv->nec7210_priv, nano_sec);
}

static void pc2_return_to_local(gpib_board_t *board)
{
	struct pc2_priv *priv = board->private_data;

	nec7210_return_to_local(board, &priv->nec7210_priv);
}

gpib_interface_t pc2_interface = {
name:	"pcII",
attach :	pc2_attach,
detach :	pc2_detach,
read :	pc2_read,
write :	pc2_write,
command :	pc2_command,
take_control :	pc2_take_control,
go_to_standby :	pc2_go_to_standby,
request_system_control :	pc2_request_system_control,
interface_clear :	pc2_interface_clear,
remote_enable :	pc2_remote_enable,
enable_eos :	pc2_enable_eos,
disable_eos :	pc2_disable_eos,
parallel_poll :	pc2_parallel_poll,
parallel_poll_configure :	pc2_parallel_poll_configure,
parallel_poll_response :	pc2_parallel_poll_response,
local_parallel_poll_mode : NULL, // XXX
line_status :	NULL,
update_status :	pc2_update_status,
primary_address :	pc2_primary_address,
secondary_address :	pc2_secondary_address,
serial_poll_response :	pc2_serial_poll_response,
serial_poll_status :	pc2_serial_poll_status,
t1_delay : pc2_t1_delay,
return_to_local : pc2_return_to_local,
};

gpib_interface_t pc2a_interface = {
name:	"pcIIa",
attach :	pc2a_attach,
detach :	pc2a_detach,
read :	pc2_read,
write :	pc2_write,
command :	pc2_command,
take_control :	pc2_take_control,
go_to_standby :	pc2_go_to_standby,
request_system_control :	pc2_request_system_control,
interface_clear :	pc2_interface_clear,
remote_enable :	pc2_remote_enable,
enable_eos :	pc2_enable_eos,
disable_eos :	pc2_disable_eos,
parallel_poll :	pc2_parallel_poll,
parallel_poll_configure :	pc2_parallel_poll_configure,
parallel_poll_response :	pc2_parallel_poll_response,
local_parallel_poll_mode : NULL, // XXX
line_status :	NULL,
update_status :	pc2_update_status,
primary_address :	pc2_primary_address,
secondary_address :	pc2_secondary_address,
serial_poll_response :	pc2_serial_poll_response,
serial_poll_status :	pc2_serial_poll_status,
t1_delay : pc2_t1_delay,
return_to_local : pc2_return_to_local,
};

gpib_interface_t pc2a_cb7210_interface = {
name:	"pcIIa_cb7210",
attach :	pc2a_cb7210_attach,
detach :	pc2a_detach,
read :	pc2_read,
write :	pc2_write,
command :	pc2_command,
take_control :	pc2_take_control,
go_to_standby :	pc2_go_to_standby,
request_system_control :	pc2_request_system_control,
interface_clear :	pc2_interface_clear,
remote_enable :	pc2_remote_enable,
enable_eos :	pc2_enable_eos,
disable_eos :	pc2_disable_eos,
parallel_poll :	pc2_parallel_poll,
parallel_poll_configure :	pc2_parallel_poll_configure,
parallel_poll_response :	pc2_parallel_poll_response,
local_parallel_poll_mode : NULL, // XXX
line_status :	NULL, //XXX
update_status :	pc2_update_status,
primary_address :	pc2_primary_address,
secondary_address :	pc2_secondary_address,
serial_poll_response :	pc2_serial_poll_response,
serial_poll_status :	pc2_serial_poll_status,
t1_delay : pc2_t1_delay,
return_to_local : pc2_return_to_local,
};

gpib_interface_t pc2_2a_interface = {
name:	"pcII_IIa",
attach :	pc2_2a_attach,
detach :	pc2_2a_detach,
read :	pc2_read,
write :	pc2_write,
command :	pc2_command,
take_control :	pc2_take_control,
go_to_standby :	pc2_go_to_standby,
request_system_control :	pc2_request_system_control,
interface_clear :	pc2_interface_clear,
remote_enable :	pc2_remote_enable,
enable_eos :	pc2_enable_eos,
disable_eos :	pc2_disable_eos,
parallel_poll :	pc2_parallel_poll,
parallel_poll_configure :	pc2_parallel_poll_configure,
parallel_poll_response :	pc2_parallel_poll_response,
local_parallel_poll_mode : NULL, // XXX
line_status :	NULL,
update_status :	pc2_update_status,
primary_address :	pc2_primary_address,
secondary_address :	pc2_secondary_address,
serial_poll_response :	pc2_serial_poll_response,
serial_poll_status :	pc2_serial_poll_status,
t1_delay : pc2_t1_delay,
return_to_local : pc2_return_to_local,
};

static int allocate_private(gpib_board_t *board)
{
	struct pc2_priv *priv;

	board->private_data = kmalloc(sizeof(struct pc2_priv), GFP_KERNEL);
	if (!board->private_data)
		return -1;
	priv = board->private_data;
	memset(priv, 0, sizeof(struct pc2_priv));
	init_nec7210_private(&priv->nec7210_priv);
	return 0;
}

static void free_private(gpib_board_t *board)
{
	kfree(board->private_data);
	board->private_data = NULL;
}

static int pc2_generic_attach(gpib_board_t *board, const gpib_board_config_t *config,
			      enum nec7210_chipset chipset)
{
	struct pc2_priv *pc2_priv;
	struct nec7210_priv *nec_priv;

	board->status = 0;
	if (allocate_private(board))
		return -ENOMEM;
	pc2_priv = board->private_data;
	nec_priv = &pc2_priv->nec7210_priv;
	nec_priv->read_byte = nec7210_ioport_read_byte;
	nec_priv->write_byte = nec7210_ioport_write_byte;
	nec_priv->type = chipset;

#ifndef PC2_DMA
	/* board->dev hasn't been initialized, so forget about DMA until this driver
	 *  is adapted to use isa_register_driver.
	 */
	if (config->ibdma)
		pr_err("DMA disabled for pc2 gpib, driver needs to be adapted to use isa_register_driver to get a struct device*");
#else
	if (config->ibdma) {
		nec_priv->dma_buffer_length = 0x1000;
		nec_priv->dma_buffer = dma_alloc_coherent(board->dev,
							  nec_priv->dma_buffer_length, &
							  nec_priv->dma_buffer_addr, GFP_ATOMIC);
		if (!nec_priv->dma_buffer)
			return -ENOMEM;

		// request isa dma channel
		if (request_dma(config->ibdma, "pc2")) {
			pr_err("gpib: can't request DMA %d\n", config->ibdma);
			return -1;
		}
		nec_priv->dma_channel = config->ibdma;
	}
#endif

	return 0;
}

int pc2_attach(gpib_board_t *board, const gpib_board_config_t *config)
{
	int isr_flags = 0;
	struct pc2_priv *pc2_priv;
	struct nec7210_priv *nec_priv;
	int retval;

	retval = pc2_generic_attach(board, config, NEC7210);
	if (retval)
		return retval;

	pc2_priv = board->private_data;
	nec_priv = &pc2_priv->nec7210_priv;
	nec_priv->offset = pc2_reg_offset;

	if (request_region((unsigned long)config->ibbase, pc2_iosize, "pc2") == 0) {
		pr_err("gpib: ioports are already in use\n");
		return -1;
	}
	nec_priv->iobase = config->ibbase;

	nec7210_board_reset(nec_priv, board);

	// install interrupt handler
	if (config->ibirq) {
		if (request_irq(config->ibirq, pc2_interrupt, isr_flags, "pc2", board))	{
			pr_err("gpib: can't request IRQ %d\n", config->ibirq);
			return -1;
		}
	}
	pc2_priv->irq = config->ibirq;
	/* poll so we can detect assertion of ATN */
	if (gpib_request_pseudo_irq(board, pc2_interrupt)) {
		pr_err("pc2_gpib: failed to allocate pseudo_irq\n");
		return -1;
	}
	/* set internal counter register for 8 MHz input clock */
	write_byte(nec_priv, ICR | 8, AUXMR);

	nec7210_board_online(nec_priv, board);

	return 0;
}

void pc2_detach(gpib_board_t *board)
{
	struct pc2_priv *pc2_priv = board->private_data;
	struct nec7210_priv *nec_priv;

	if (pc2_priv) {
		nec_priv = &pc2_priv->nec7210_priv;
#ifdef PC2_DMA
		if (nec_priv->dma_channel)
			free_dma(nec_priv->dma_channel);
#endif
		gpib_free_pseudo_irq(board);
		if (pc2_priv->irq)
			free_irq(pc2_priv->irq, board);
		if (nec_priv->iobase) {
			nec7210_board_reset(nec_priv, board);
			release_region((unsigned long)(nec_priv->iobase), pc2_iosize);
		}
		if (nec_priv->dma_buffer) {
			dma_free_coherent(board->dev, nec_priv->dma_buffer_length,
					  nec_priv->dma_buffer, nec_priv->dma_buffer_addr);
			nec_priv->dma_buffer = NULL;
		}
	}
	free_private(board);
}

static int pc2a_common_attach(gpib_board_t *board, const gpib_board_config_t *config,
			      unsigned int num_registers, enum nec7210_chipset chipset)
{
	unsigned int i, j;
	struct pc2_priv *pc2_priv;
	struct nec7210_priv *nec_priv;
	int retval;

	retval = pc2_generic_attach(board, config, chipset);
	if (retval)
		return retval;

	pc2_priv = board->private_data;
	nec_priv = &pc2_priv->nec7210_priv;
	nec_priv->offset = pc2a_reg_offset;

	switch ((unsigned long)(config->ibbase)) {
	case 0x02e1:
	case 0x22e1:
	case 0x42e1:
	case 0x62e1:
		break;
	default:
		pr_err("PCIIa base range invalid, must be one of 0x[0246]2e1, but is 0x%p\n",
		       config->ibbase);
		return -1;
	}

	if (config->ibirq) {
		if (config->ibirq < 2 || config->ibirq > 7) {
			pr_err("pc2_gpib: illegal interrupt level %i\n", config->ibirq);
			return -1;
		}
	} else	{
		pr_err("pc2_gpib: interrupt disabled, using polling mode (slow)\n");
	}
#ifdef CHECK_IOPORTS
	unsigned int err = 0;

	for (i = 0; i < num_registers; i++) {
		if (check_region((unsigned long)config->ibbase + i * pc2a_reg_offset, 1))
			err++;
	}
	if (config->ibirq && check_region(pc2a_clear_intr_iobase + config->ibirq, 1))
		err++;
	if (err) {
		pr_err("gpib: ioports are already in use");
		return -1;
	}
#endif
	for (i = 0; i < num_registers; i++) {
		if (!request_region((unsigned long)config->ibbase +
					i * pc2a_reg_offset, 1, "pc2a")) {
			pr_err("gpib: ioports are already in use");
			for (j = 0; j < i; j++)
				release_region((unsigned long)(config->ibbase) +
					j * pc2a_reg_offset, 1);
			return -1;
		}
	}
	nec_priv->iobase = config->ibbase;
	if (config->ibirq) {
		if (!request_region(pc2a_clear_intr_iobase + config->ibirq, 1, "pc2a"))  {
			pr_err("gpib: ioports are already in use");
			return -1;
		}
		pc2_priv->clear_intr_addr = pc2a_clear_intr_iobase + config->ibirq;
		if (request_irq(config->ibirq, pc2a_interrupt, 0, "pc2a", board)) {
			pr_err("gpib: can't request IRQ %d\n", config->ibirq);
			return -1;
		}
	}
	pc2_priv->irq = config->ibirq;
	/* poll so we can detect assertion of ATN */
	if (gpib_request_pseudo_irq(board, pc2_interrupt)) {
		pr_err("pc2_gpib: failed to allocate pseudo_irq\n");
		return -1;
	}

	// make sure interrupt is clear
	if (pc2_priv->irq)
		outb(0xff, CLEAR_INTR_REG(pc2_priv->irq));

	nec7210_board_reset(nec_priv, board);

	/* set internal counter register for 8 MHz input clock */
	write_byte(nec_priv, ICR | 8, AUXMR);

	nec7210_board_online(nec_priv, board);

	return 0;
}

int pc2a_attach(gpib_board_t *board, const gpib_board_config_t *config)
{
	return pc2a_common_attach(board, config, pc2a_iosize, NEC7210);
}

int pc2a_cb7210_attach(gpib_board_t *board, const gpib_board_config_t *config)
{
	return pc2a_common_attach(board, config, pc2a_iosize, CB7210);
}

int pc2_2a_attach(gpib_board_t *board, const gpib_board_config_t *config)
{
	return pc2a_common_attach(board, config, pc2_2a_iosize, NAT4882);
}

static void pc2a_common_detach(gpib_board_t *board, unsigned int num_registers)
{
	int i;
	struct pc2_priv *pc2_priv = board->private_data;
	struct nec7210_priv *nec_priv;

	if (pc2_priv) {
		nec_priv = &pc2_priv->nec7210_priv;
#ifdef PC2_DMA
		if (nec_priv->dma_channel)
			free_dma(nec_priv->dma_channel);
#endif
		gpib_free_pseudo_irq(board);
		if (pc2_priv->irq)
			free_irq(pc2_priv->irq, board);
		if (nec_priv->iobase) {
			nec7210_board_reset(nec_priv, board);
			for (i = 0; i < num_registers; i++)
				release_region((unsigned long)nec_priv->iobase +
					       i * pc2a_reg_offset, 1);
		}
		if (pc2_priv->clear_intr_addr)
			release_region(pc2_priv->clear_intr_addr, 1);
		if (nec_priv->dma_buffer) {
			dma_free_coherent(board->dev, nec_priv->dma_buffer_length,
					  nec_priv->dma_buffer,
					  nec_priv->dma_buffer_addr);
			nec_priv->dma_buffer = NULL;
		}
	}
	free_private(board);
}

void pc2a_detach(gpib_board_t *board)
{
	pc2a_common_detach(board, pc2a_iosize);
}

void pc2_2a_detach(gpib_board_t *board)
{
	pc2a_common_detach(board, pc2_2a_iosize);
}

static int __init pc2_init_module(void)
{
	gpib_register_driver(&pc2_interface, THIS_MODULE);
	gpib_register_driver(&pc2a_interface, THIS_MODULE);
	gpib_register_driver(&pc2a_cb7210_interface, THIS_MODULE);
	gpib_register_driver(&pc2_2a_interface, THIS_MODULE);

	return 0;
}

static void __exit pc2_exit_module(void)
{
	gpib_unregister_driver(&pc2_interface);
	gpib_unregister_driver(&pc2a_interface);
	gpib_unregister_driver(&pc2a_cb7210_interface);
	gpib_unregister_driver(&pc2_2a_interface);
}

module_init(pc2_init_module);
module_exit(pc2_exit_module);

