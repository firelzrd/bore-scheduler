/* SPDX-License-Identifier: GPL-2.0 */

/***************************************************************************
 *    copyright		   : (C) 2002 by Frank Mori Hess
 ***************************************************************************/

#ifndef _GPIB_TYPES_H
#define _GPIB_TYPES_H

#ifdef __KERNEL__
/* gpib_interface_t defines the interface
 * between the board-specific details dealt with in the drivers
 * and generic interface provided by gpib-common.
 * This really should be in a different header file.
 */
#include "gpib_user.h"
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>

typedef struct gpib_interface_struct gpib_interface_t;
typedef struct gpib_board_struct gpib_board_t;

/* config parameters that are only used by driver attach functions */
typedef struct {
	/* firmware blob */
	void *init_data;
	int init_data_length;
	/* IO base address to use for non-pnp cards (set by core, driver should make local copy) */
	void *ibbase;
	/* IRQ to use for non-pnp cards (set by core, driver should make local copy) */
	unsigned int ibirq;
	/* dma channel to use for non-pnp cards (set by core, driver should make local copy) */
	unsigned int ibdma;
	/* pci bus of card, useful for distinguishing multiple identical pci cards
	 * (negative means don't care)
	 */
	int pci_bus;
	/* pci slot of card, useful for distinguishing multiple identical pci cards
	 * (negative means don't care)
	 */
	int pci_slot;
	/* sysfs device path of hardware to attach */
	char *device_path;
	/* serial number of hardware to attach */
	char *serial_number;
} gpib_board_config_t;

struct gpib_interface_struct {
	/* name of board */
	char *name;
	/* attach() initializes board and allocates resources */
	int (*attach)(gpib_board_t *board, const gpib_board_config_t *config);
	/* detach() shuts down board and frees resources */
	void (*detach)(gpib_board_t *board);
	/* read() should read at most 'length' bytes from the bus into
	 * 'buffer'.  It should return when it fills the buffer or
	 * encounters an END (EOI and or EOS if appropriate).  It should set 'end'
	 * to be nonzero if the read was terminated by an END, otherwise 'end'
	 * should be zero.
	 * Ultimately, this will be changed into or replaced by an asynchronous
	 * read.  Zero return value for success, negative
	 * return indicates error.
	 * nbytes returns number of bytes read
	 */
	int (*read)(gpib_board_t *board, uint8_t *buffer, size_t length, int *end,
		    size_t *bytes_read);
	/* write() should write 'length' bytes from buffer to the bus.
	 * If the boolean value send_eoi is nonzero, then EOI should
	 * be sent along with the last byte.  Returns number of bytes
	 * written or negative value on error.
	 */
	int (*write)(gpib_board_t *board, uint8_t *buffer, size_t length, int send_eoi,
		     size_t *bytes_written);
	/* command() writes the command bytes in 'buffer' to the bus
	 * Returns zero on success or negative value on error.
	 */
	int (*command)(gpib_board_t *board, uint8_t *buffer, size_t length,
		       size_t *bytes_written);
	/* Take control (assert ATN).  If 'asyncronous' is nonzero, take
	 * control asyncronously (assert ATN immediately without waiting
	 * for other processes to complete first).  Should not return
	 * until board becomes controller in charge.  Returns zero no success,
	 * nonzero on error.
	 */
	int (*take_control)(gpib_board_t *board, int asyncronous);
	/* De-assert ATN.  Returns zero on success, nonzer on error.
	 */
	int (*go_to_standby)(gpib_board_t *board);
	/* request/release control of the IFC and REN lines (system controller) */
	void (*request_system_control)(gpib_board_t *board, int request_control);
	/* Asserts or de-asserts 'interface clear' (IFC) depending on
	 * boolean value of 'assert'
	 */
	void (*interface_clear)(gpib_board_t *board, int assert);
	/* Sends remote enable command if 'enable' is nonzero, disables remote mode
	 * if 'enable' is zero
	 */
	void (*remote_enable)(gpib_board_t *board, int enable);
	/* enable END for reads, when byte 'eos' is received.  If
	 * 'compare_8_bits' is nonzero, then all 8 bits are compared
	 * with the eos bytes.	Otherwise only the 7 least significant
	 * bits are compared.
	 */
	int (*enable_eos)(gpib_board_t *board, uint8_t eos, int compare_8_bits);
	/* disable END on eos byte (END on EOI only)*/
	void (*disable_eos)(gpib_board_t *board);
	/* configure parallel poll */
	void (*parallel_poll_configure)(gpib_board_t *board, uint8_t configuration);
	/* conduct parallel poll */
	int (*parallel_poll)(gpib_board_t *board, uint8_t *result);
	/* set/clear ist (individual status bit) */
	void (*parallel_poll_response)(gpib_board_t *board, int ist);
	/* select local parallel poll configuration mode PP2 versus remote PP1 */
	void (*local_parallel_poll_mode)(gpib_board_t *board, int local);
	/* Returns current status of the bus lines.  Should be set to
	 * NULL if your board does not have the ability to query the
	 * state of the bus lines.
	 */
	int (*line_status)(const gpib_board_t *board);
	/* updates and returns the board's current status.
	 * The meaning of the bits are specified in gpib_user.h
	 * in the IBSTA section.  The driver does not need to
	 * worry about setting the CMPL, END, TIMO, or ERR bits.
	 */
	unsigned int (*update_status)(gpib_board_t *board, unsigned int clear_mask);
	/* Sets primary address 0-30 for gpib interface card.
	 */
	int (*primary_address)(gpib_board_t *board, unsigned int address);
	/* Sets and enables, or disables secondary address 0-30
	 * for gpib interface card.
	 */
	int (*secondary_address)(gpib_board_t *board, unsigned int address,
				 int enable);
	/* Sets the byte the board should send in response to a serial poll.
	 * This function should also start or stop requests for service via
	 * IEEE 488.2 reqt/reqf, based on MSS (bit 6 of the status_byte).
	 * If the more flexible serial_poll_response2 is implemented by the
	 * driver, then this method should be left NULL since it will not
	 * be used.  This method can generate spurious service requests
	 * which are allowed by IEEE 488.2, but not ideal.
	 *
	 * This method should implement the serial poll response method described
	 * by IEEE 488.2 section 11.3.3.4.3 "Allowed Coupled Control of
	 * STB, reqt, and reqf".
	 */
	void (*serial_poll_response)(gpib_board_t *board, uint8_t status_byte);
	/* Sets the byte the board should send in response to a serial poll.
	 * This function should also request service via IEEE 488.2 reqt/reqf
	 * based on MSS (bit 6 of the status_byte) and new_reason_for_service.
	 * reqt should be set true if new_reason_for_service is true,
	 * and reqf should be set true if MSS is false.	 This function
	 * will never be called with MSS false and new_reason_for_service
	 * true simultaneously, so don't worry about that case.
	 *
	 * This method implements the serial poll response method described
	 * by IEEE 488.2 section 11.3.3.4.1 "Preferred Implementation".
	 *
	 * If this method is left NULL by the driver, then the user library
	 * function ibrsv2 will not work.
	 */
	void (*serial_poll_response2)(gpib_board_t *board, uint8_t status_byte,
				      int new_reason_for_service);
	/* returns the byte the board will send in response to a serial poll.
	 */
	uint8_t (*serial_poll_status)(gpib_board_t *board);
	/* adjust T1 delay */
	unsigned int (*t1_delay)(gpib_board_t *board, unsigned int nano_sec);
	/* go to local mode */
	void (*return_to_local)(gpib_board_t *board);
	/* board does not support 7 bit eos comparisons */
	unsigned no_7_bit_eos : 1;
	/* skip check for listeners before trying to send command bytes */
	unsigned skip_check_for_command_acceptors : 1;
};

typedef struct {
	struct list_head event_head;
	spinlock_t lock; // for access to event list
	unsigned int num_events;
	unsigned dropped_event : 1;
} gpib_event_queue_t;

static inline void init_event_queue(gpib_event_queue_t *queue)
{
	INIT_LIST_HEAD(&queue->event_head);
	queue->num_events = 0;
	queue->dropped_event = 0;
	spin_lock_init(&queue->lock);
}

/* struct for supporting polling operation when irq is not available */
struct gpib_pseudo_irq {
	struct timer_list timer;
	irqreturn_t (*handler)(int irq, void *arg);
	gpib_board_t *board;
	atomic_t active;
};

static inline void init_gpib_pseudo_irq(struct gpib_pseudo_irq *pseudo_irq)
{
	pseudo_irq->handler = NULL;
	timer_setup(&pseudo_irq->timer, NULL, 0);
	atomic_set(&pseudo_irq->active, 0);
}

/* list so we can make a linked list of drivers */
typedef struct gpib_interface_list_struct {
	struct list_head list;
	gpib_interface_t *interface;
	struct module *module;
} gpib_interface_list_t;

/* One gpib_board_t is allocated for each physical board in the computer.
 * It provides storage for variables local to each board, and interface
 * functions for performing operations on the board
 */
struct gpib_board_struct {
	/* functions used by this board */
	gpib_interface_t *interface;
	/* Pointer to module whose use count we should increment when
	 * interface is in use
	 */
	struct module *provider_module;
	/* buffer used to store read/write data for this board */
	u8 *buffer;
	/* length of buffer */
	unsigned int buffer_length;
	/* Used to hold the board's current status (see update_status() above)
	 */
	unsigned long status;
	/* Driver should only sleep on this wait queue.	 It is special in that the
	 * core will wake this queue and set the TIMO bit in 'status' when the
	 * watchdog timer times out.
	 */
	wait_queue_head_t wait;
	/* Lock that only allows one process to access this board at a time.
	 * Has to be first in any locking order, since it can be locked over
	 * multiple ioctls.
	 */
	struct mutex user_mutex;
	/* Mutex which compensates for removal of "big kernel lock" from kernel.
	 * Should not be held for extended waits.
	 */
	struct mutex big_gpib_mutex;
	/* pid of last process to lock the board mutex */
	pid_t locking_pid;
	spinlock_t locking_pid_spinlock; // lock for setting locking pid
	/* Spin lock for dealing with races with the interrupt handler */
	spinlock_t spinlock;
	/* Watchdog timer to enable timeouts */
	struct timer_list timer;
	/* device of attached driver if any */
	struct device *dev;
	/* gpib_common device gpibN */
	struct device *gpib_dev;
	/* 'private_data' can be used as seen fit by the driver to
	 * store additional variables for this board
	 */
	void *private_data;
	/* Number of open file descriptors using this board */
	unsigned int use_count;
	/* list of open devices connected to this board */
	struct list_head device_list;
	/* primary address */
	unsigned int pad;
	/* secondary address */
	int sad;
	/* timeout for io operations, in microseconds */
	unsigned int usec_timeout;
	/* board's parallel poll configuration byte */
	u8 parallel_poll_configuration;
	/* t1 delay we are using */
	unsigned int t1_nano_sec;
	/* Count that keeps track of whether board is up and running or not */
	unsigned int online;
	/* number of processes trying to autopoll */
	int autospollers;
	/* autospoll kernel thread */
	struct task_struct *autospoll_task;
	/* queue for recording received trigger/clear/ifc events */
	gpib_event_queue_t event_queue;
	/* minor number for this board's device file */
	int minor;
	/* struct to deal with polling mode*/
	struct gpib_pseudo_irq pseudo_irq;
	/* error dong autopoll */
	atomic_t stuck_srq;
	gpib_board_config_t config;
	/* Flag that indicates whether board is system controller of the bus */
	unsigned master : 1;
	/* individual status bit */
	unsigned ist : 1;
	/* one means local parallel poll mode ieee 488.1 PP2 (or no parallel poll PP0),
	 * zero means remote parallel poll configuration mode ieee 488.1 PP1
	 */
	unsigned local_ppoll_mode : 1;
};

/* element of event queue */
typedef struct {
	struct list_head list;
	short event_type;
} gpib_event_t;

/* Each board has a list of gpib_status_queue_t to keep track of all open devices
 * on the bus, so we know what address to poll when we get a service request
 */
typedef struct {
	/* list_head so we can make a linked list of devices */
	struct list_head list;
	unsigned int pad;	/* primary gpib address */
	int sad;	/* secondary gpib address (negative means disabled) */
	/* stores serial poll bytes for this device */
	struct list_head status_bytes;
	unsigned int num_status_bytes;
	/* number of times this address is opened */
	unsigned int reference_count;
	/* flags loss of status byte error due to limit on size of queue */
	unsigned dropped_byte : 1;
} gpib_status_queue_t;

typedef struct {
	struct list_head list;
	u8 poll_byte;
} status_byte_t;

void init_gpib_status_queue(gpib_status_queue_t *device);

/* Used to store device-descriptor-specific information */
typedef struct {
	unsigned int pad;	/* primary gpib address */
	int sad;	/* secondary gpib address (negative means disabled) */
	atomic_t io_in_progress;
	unsigned is_board : 1;
	unsigned autopoll_enabled : 1;
} gpib_descriptor_t;

typedef struct {
	atomic_t holding_mutex;
	gpib_descriptor_t *descriptors[GPIB_MAX_NUM_DESCRIPTORS];
	/* locked while descriptors are being allocated/deallocated */
	struct mutex descriptors_mutex;
	unsigned got_module : 1;
} gpib_file_private_t;

#endif	/* __KERNEL__ */

#endif	/* _GPIB_TYPES_H */
