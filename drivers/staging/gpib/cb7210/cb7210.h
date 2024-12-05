/* SPDX-License-Identifier: GPL-2.0 */

/***************************************************************************
 *    copyright            : (C) 2002 by Frank Mori Hess
 ***************************************************************************/

#include "nec7210.h"
#include "gpibP.h"
#include "amccs5933.h"

#include <linux/delay.h>
#include <linux/interrupt.h>

enum {
	PCI_DEVICE_ID_CBOARDS_PCI_GPIB = 0x6,
	PCI_DEVICE_ID_CBOARDS_CPCI_GPIB = 0xe,
};

enum pci_chip {
	PCI_CHIP_NONE = 0,
	PCI_CHIP_AMCC_S5933,
	PCI_CHIP_QUANCOM
};

// struct which defines private_data for cb7210 boards
struct cb7210_priv {
	struct nec7210_priv nec7210_priv;
	struct pci_dev *pci_device;
	// base address of amccs5933 pci chip
	unsigned long amcc_iobase;
	unsigned long fifo_iobase;
	unsigned int irq;
	enum pci_chip pci_chip;
	u8 hs_mode_bits;
	unsigned out_fifo_half_empty : 1;
	unsigned in_fifo_half_full : 1;
};

// interfaces
extern gpib_interface_t cb_pcmcia_interface;
extern gpib_interface_t cb_pcmcia_accel_interface;
extern gpib_interface_t cb_pcmcia_unaccel_interface;

// interrupt service routines
irqreturn_t cb_pci_interrupt(int irq, void *arg);
irqreturn_t cb7210_interrupt(int irq, void *arg);
irqreturn_t cb7210_internal_interrupt(gpib_board_t *board);

// interface functions
int cb7210_read(gpib_board_t *board, uint8_t *buffer, size_t length,
		int *end, size_t *bytes_read);
int cb7210_accel_read(gpib_board_t *board, uint8_t *buffer, size_t length,
		      int *end, size_t *bytes_read);
int cb7210_write(gpib_board_t *board, uint8_t *buffer, size_t length,
		 int send_eoi, size_t *bytes_written);
int cb7210_accel_write(gpib_board_t *board, uint8_t *buffer, size_t length,
		       int send_eoi, size_t *bytes_written);
int cb7210_command(gpib_board_t *board, uint8_t *buffer, size_t length, size_t *bytes_written);
int cb7210_take_control(gpib_board_t *board, int synchronous);
int cb7210_go_to_standby(gpib_board_t *board);
void cb7210_request_system_control(gpib_board_t *board, int request_control);
void cb7210_interface_clear(gpib_board_t *board, int assert);
void cb7210_remote_enable(gpib_board_t *board, int enable);
int cb7210_enable_eos(gpib_board_t *board, uint8_t eos_byte,
		      int compare_8_bits);
void cb7210_disable_eos(gpib_board_t *board);
unsigned int cb7210_update_status(gpib_board_t *board, unsigned int clear_mask);
int cb7210_primary_address(gpib_board_t *board, unsigned int address);
int cb7210_secondary_address(gpib_board_t *board, unsigned int address,
			     int enable);
int cb7210_parallel_poll(gpib_board_t *board, uint8_t *result);
void cb7210_serial_poll_response(gpib_board_t *board, uint8_t status);
uint8_t cb7210_serial_poll_status(gpib_board_t *board);
void cb7210_parallel_poll_configure(gpib_board_t *board, uint8_t configuration);
void cb7210_parallel_poll_response(gpib_board_t *board, int ist);
int cb7210_line_status(const gpib_board_t *board);
unsigned int cb7210_t1_delay(gpib_board_t *board, unsigned int nano_sec);
void cb7210_return_to_local(gpib_board_t *board);

// utility functions
void cb7210_generic_detach(gpib_board_t *board);
int cb7210_generic_attach(gpib_board_t *board);
int cb7210_init(struct cb7210_priv *priv, gpib_board_t *board);

// pcmcia init/cleanup
int cb_pcmcia_init_module(void);
void cb_pcmcia_cleanup_module(void);

// pci-gpib register offset
static const int cb7210_reg_offset = 1;

// uses 10 ioports
static const int cb7210_iosize = 10;

// fifo size in bytes
static const int cb7210_fifo_size = 2048;
static const int cb7210_fifo_width = 2;

// cb7210 specific registers and bits
enum cb7210_regs {
	BUS_STATUS = 0x7,
};

enum cb7210_page_in {
	BUS_STATUS_PAGE = 1,
};

enum hs_regs {
	//write registers
	HS_MODE = 0x8,	/* HS_MODE register */
	HS_INT_LEVEL = 0x9,	/* HS_INT_LEVEL register */
	//read registers
	HS_STATUS = 0x8,	/* HS_STATUS register */
};

static inline unsigned long nec7210_iobase(const struct cb7210_priv *cb_priv)
{
	return (unsigned long)(cb_priv->nec7210_priv.iobase);
}

static inline int cb7210_page_in_bits(unsigned int page)
{
	return 0x50 | (page & 0xf);
}

static inline uint8_t cb7210_paged_read_byte(struct cb7210_priv *cb_priv,
					     unsigned int register_num, unsigned int page)
{
	struct nec7210_priv *nec_priv = &cb_priv->nec7210_priv;
	u8 retval;
	unsigned long flags;

	spin_lock_irqsave(&nec_priv->register_page_lock, flags);
	outb(cb7210_page_in_bits(page), nec7210_iobase(cb_priv) + AUXMR * nec_priv->offset);
	udelay(1);
	retval = inb(nec7210_iobase(cb_priv) + register_num * nec_priv->offset);
	spin_unlock_irqrestore(&nec_priv->register_page_lock, flags);
	return retval;
}

// don't use for register_num < 8, since it doesn't lock
static inline uint8_t cb7210_read_byte(const struct cb7210_priv *cb_priv,
				       enum hs_regs register_num)
{
	const struct nec7210_priv *nec_priv = &cb_priv->nec7210_priv;
	u8 retval;

	retval = inb(nec7210_iobase(cb_priv) + register_num * nec_priv->offset);
	return retval;
}

static inline void cb7210_paged_write_byte(struct cb7210_priv *cb_priv, uint8_t data,
					   unsigned int register_num, unsigned int page)
{
	struct nec7210_priv *nec_priv = &cb_priv->nec7210_priv;
	unsigned long flags;

	spin_lock_irqsave(&nec_priv->register_page_lock, flags);
	outb(cb7210_page_in_bits(page), nec7210_iobase(cb_priv) + AUXMR * nec_priv->offset);
	udelay(1);
	outb(data, nec7210_iobase(cb_priv) + register_num * nec_priv->offset);
	spin_unlock_irqrestore(&nec_priv->register_page_lock, flags);
}

// don't use for register_num < 8, since it doesn't lock
static inline void cb7210_write_byte(const struct cb7210_priv *cb_priv, uint8_t data,
				     enum hs_regs register_num)
{
	const struct nec7210_priv *nec_priv = &cb_priv->nec7210_priv;

	outb(data, nec7210_iobase(cb_priv) + register_num * nec_priv->offset);
}

enum bus_status_bits {
	BSR_ATN_BIT = 0x1,
	BSR_EOI_BIT = 0x2,
	BSR_SRQ_BIT = 0x4,
	BSR_IFC_BIT = 0x8,
	BSR_REN_BIT = 0x10,
	BSR_DAV_BIT = 0x20,
	BSR_NRFD_BIT = 0x40,
	BSR_NDAC_BIT = 0x80,
};

/* CBI 488.2 HS control */

/* when both bit 0 and 1 are set, it
 *   1 clears the transmit state machine to an initial condition
 *   2 clears any residual interrupts left latched on cbi488.2
 *   3 resets all control bits in HS_MODE to zero
 *   4 enables TX empty interrupts
 * when both bit 0 and 1 are zero, then the high speed mode is disabled
 */
enum hs_mode_bits {
	HS_ENABLE_MASK = 0x3,
	HS_TX_ENABLE = (1 << 0),
	HS_RX_ENABLE = (1 << 1),
	HS_HF_INT_EN = (1 << 3),
	HS_CLR_SRQ_INT = (1 << 4),
	HS_CLR_EOI_EMPTY_INT = (1 << 5),
	HS_CLR_HF_INT = (1 << 6),
	HS_SYS_CONTROL = (1 << 7),
};

/* CBI 488.2 status */
enum hs_status_bits {
	HS_FIFO_FULL = (1 << 0),
	HS_HALF_FULL = (1 << 1),
	HS_SRQ_INT = (1 << 2),
	HS_EOI_INT = (1 << 3),
	HS_TX_MSB_NOT_EMPTY = (1 << 4),
	HS_RX_MSB_NOT_EMPTY = (1 << 5),
	HS_TX_LSB_NOT_EMPTY = (1 << 6),
	HS_RX_LSB_NOT_EMPTY = (1 << 7),
};

/* CBI488.2 hs_int_level register */
enum hs_int_level_bits {
	HS_RESET7210 = (1 << 7),
};

static inline unsigned int irq_bits(unsigned int irq)
{
	switch (irq) {
	case 2:
	case 3:
	case 4:
	case 5:
		return irq - 1;
	case 7:
		return 0x5;
	case 10:
		return 0x6;
	case 11:
		return 0x7;
	default:
		return 0;
	}
}

enum cb7210_aux_cmds {
/* AUX_RTL2 is an undocumented aux command which causes cb7210 to assert
 *	(and keep asserted) local rtl message.  This is used in conjunction
 *	with the (stupid) cb7210 implementation
 *	of the normal nec7210 AUX_RTL aux command, which
 *	causes the rtl message to toggle between on and off.
 */
	AUX_RTL2 = 0xd,
	AUX_LO_SPEED = 0x40,
	AUX_HI_SPEED = 0x41,
};
