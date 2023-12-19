/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0    */
/* Copyright (c) 2023 NextSilicon. All rights reserved */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/tty.h>
#include <linux/mod_devicetable.h>
#include <linux/tty_flip.h>
#include <linux/console.h>
#include <linux/delay.h>

#include "tty_utils.h"
#include "tty_chardev.h"
#include "tty_dev_mmap.h"
#include "ns_uring.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NextSilicon TTY Driver");
MODULE_VERSION("0.1.0");

// module parameters
uint32_t m_ecore_chan_base_addr_offset =
	DEFAULT_ECORE_NS_URING_CHAN_BASE_ADDRESS_OFFSET;
unsigned long m_pcore_chan_base_addr = DEFAULT_PCORE_NS_URING_CHAN_BASE_ADDRESS;
unsigned long m_score_chan_base_addr = DEFAULT_SCORE_NS_URING_CHAN_BASE_ADDRESS;
uint32_t m_ecore_ring_size = DEFAULT_ECORE_NS_URING_CHAN_RING_SIZE;
uint32_t m_pcore_ring_size = DEFAULT_PCORE_NS_URING_CHAN_RING_SIZE;
uint32_t m_score_ring_size = DEFAULT_SCORE_NS_URING_CHAN_RING_SIZE;

extern struct ns_uring_ctx ns_uring_channel_ctx[NUM_TTY_DEVICES];

static struct task_struct *main_thread;
static struct tty_driver *tty_driver;
struct tty_port tty_ports[NUM_TTY_DEVICES];
char port_name[60];

//////////////////////
/* static functions */
//////////////////////

/**
 * @brief function returns tty port name based on target port type
 *
 * @param tty_dev_id  tty device id
 *
 * @return name
 */
static char *tty_get_port_name(int tty_dev_id)
{
#ifdef SELF_HOSTED_MODE
	int port_type = TTY_PORT_TYPE_FROM_ID(tty_dev_id);

	switch (port_type) {
	case ECORE:
		sprintf(port_name, "%s%d", DEVICE_ECORE_NAME, tty_dev_id);
		break;
	case SCORE:
		sprintf(port_name, "%s%d", DEVICE_SCORE_NAME,
			tty_dev_id - TTY_DEV_SCORE_D0_ID);
		break;
	case PCORE:
		sprintf(port_name, "%s", DEVICE_PCORE_NAME);
		break;
	default:
		printk(KERN_DEBUG "tty_get_port_name: invalid port type %d\n",
		       port_type);
		break;
	}
	return port_name;
#elif defined(ENDPOINT_MODE)
	return DEVICE_PCORE_NAME;
#endif
}

/**
 * @brief installs the tty driver
 *
 * @param driver  a pointer to the tty driver
 * @param tty  a pointer to the tty struct
 *
 * @return 0 in case it's ok
 */
static int tty_driver_install(struct tty_driver *driver, struct tty_struct *tty)
{
	tty_driver_kref_get(driver);
	tty->count++;
	driver->ttys[tty->index] = tty;
    strcpy(tty->name, tty_get_port_name(tty->index));
	debug_print(KERN_DEBUG "tty_driver_install: device name (%s) id (%d)\n",
		    tty->name, tty->index);

	return 0;
}

/**
 * @brief close the tty device
 *
 * @param tty  a pointer to the tty struct
 * @param file  a pointer to the file
 *
 */
static void serial_device_tty_close(struct tty_struct *tty, struct file *file)
{
	struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[tty->index];
	struct tty_struct *tty_p;

	if (tty->count > 1) {
		tty_p = ((struct tty_file_private *)file->private_data)->tty;
		debug_print(KERN_DEBUG
			    "serial_device_tty_close: do nothing! device (%s) tty_id (%d) is still open\n",
			    tty->name, tty->index);
		return;
	}

	if (tty->index < NUM_TTY_DEVICES) {
		ns_uring_destroy_channel(ctx->pch);
		ns_uring_channel_ctx_reset(ctx);
		debug_print(KERN_DEBUG
			    "serial_device_tty_close: device (%s) ttyindex (%d)\n",
			    tty->name, tty->index);
	}
}

/**
 * @brief open the tty device
 *
 * @param tty  a pointer to the tty srtruct
 * @param file  a pointer to the file
 *
 * @return 0 in case it's ok
 */
static int serial_device_tty_open(struct tty_struct *tty, struct file *file)
{
	int err;
	struct tty_port *port = tty->driver->ports[tty->index];
	struct ns_uring_ctx *ctx;

	if (tty->index >= NUM_TTY_DEVICES) {
		printk("serial_device_tty_open: invalid device index = %d \n",
		       tty->index);
		err = -EACCES;
		goto closeport;
	}

	ctx = &ns_uring_channel_ctx[tty->index];
	
#ifdef SELF_HOSTED_MODE
	int chan_tty_dev_id = tty->index;
#elif defined(ENDPOINT_MODE)
	int chan_tty_dev_id = TTY_DEV_PCORE_ID;
#endif

	if (tty->count > 1) {
		/* Any further open() on the tty
       	   that is currently open on descriptor fd will fail with errno == EACCES */
		printk("serial_device_tty_open: can't open dev/(%s) already in use tty_id (%d) tty_count (%d)\n",
		       tty->name, tty->index, tty->count);
		err = -EACCES;
		return err;
	}

	if (tty->index < NUM_TTY_DEVICES) {
		if (ctx->pch == NULL) {
			/* init ns_uring channel */
			err = ns_uring_channel_init(chan_tty_dev_id, ctx);
			if (err != 0) {
				printk("serial_device_tty_open: failed! cannot initiate channel (error = %d)\n",
				       err);
				goto closeport;
			}
			// save tty port index linked to this channel
			ctx->tty_port_idx = tty->index;
			debug_print(KERN_DEBUG
				    "serial_device_tty_open: Finished! tty_port_idx = %d chan_dev_id (%d)\n",
				    tty->index, chan_tty_dev_id);
		}
	}

	return 0;

closeport:
	tty_port_close(port, tty, file);
	tty_release_struct(tty, tty->index);
	tty_unlock(tty);
	return err;
}

/**
 * @brief write character to ns_uring channel
 * 
 * @param ch data character
 * @param ctx pointer to channel context
 * 
 * @return 0 for success  
 */
static int ns_uring_put_char(const unsigned char ch, struct ns_uring_ctx *ctx)
{
    struct ns_uring_entry msg = { 0 };

    if (!ctx->pch)
    {
        return -ENODEV;
    }

    msg.hdr.subtype = NS_URING_ENTRY_TYPE_SINGLE_CHARACTER;
	msg.payload.data.character = ch;

    return ns_uring_send(ctx->pch, (void *)&msg, sizeof(msg));
}

/**
 * @brief write user data from tty device to target
 *
 * @param tty  a pointer to the tty struct
 * @param data  a pointer to the data we want to write
 * @param length  the length of the data
 *
 * @return length value in case it's ok
 */
static int serial_device_tty_write(struct tty_struct *tty,
				   const unsigned char *data, int length)
{
	int count = length;
    struct ns_uring_ctx *ctx;
	
	if (tty->index >= NUM_TTY_DEVICES) {
		printk("serial_device_tty_write: invalid device index = %d \n",
		       tty->index);
		return -1;
	}

	ctx = &ns_uring_channel_ctx[tty->index];

    if ((!ctx->pch) || (ctx->err))
    {
        printk("serial_device_tty_write: device not ready err = %d\n",
			       ctx->err);
        return -1;
    }

	while (count--) {
		if (*data == '\n')
			ctx->err |= ns_uring_put_char('\r', ctx);
		ctx->err |= ns_uring_put_char(*data, ctx);

		if (ctx->err != 0) {
			printk("serial_device_tty_write: ERROR: err = %d\n",
			       ctx->err);
			return -1;
		}
		data++;
	}

	return (length);
}

/**
 * @brief This routine returns the numbers of characters the tty driver will accept for queuing to be written
 *
 *	Return the number of bytes that can be queued to this device
 *	at the present time. The result should be treated as a guarantee
 *	and the driver cannot offer a value it later shrinks by more than
 *	the number of bytes written. If no method is provided 2K is always
 *	returned and data may be lost as there will be no flow control.
  * @param tty  a pointer to the tty struct
 *
 * @return the available space
 */
static unsigned serial_device_tty_write_room(struct tty_struct *tty)
{
	// Designed for slow data rate (char device)
	// For faster data rate this will probably have to be optimized.
	if (tty->hw_stopped) {
		return 0;
	}

	return tty_buffer_space_avail(tty->port);
}


/* TTY operations structure */
static const struct tty_operations serial_device_tty_ops = {
	.install = tty_driver_install,
	.open = serial_device_tty_open,
	.close = serial_device_tty_close,
	.write = serial_device_tty_write,
	.write_room = serial_device_tty_write_room,
};

static void tty_devices_remove(void)
{
	int i;
	struct ns_uring_ctx *ctx;

	for (i = 0; i < NUM_TTY_DEVICES; ++i) {
		printk("unregister tty device number %d\n", i);
		ctx = &ns_uring_channel_ctx[i];
		if (ctx->pch) {
			ns_uring_destroy_channel(ctx->pch);
			ns_uring_channel_ctx_reset(ctx);
		}
		tty_unregister_device(tty_driver, i);
	}
}

#if defined(CONFIG_NS_URING_TTY_CONSOLE) && !defined(SELF_HOSTED_MODE)
/*
 *	Print a string to the serial port trying not to disturb
 *	any possible real use of the port...
 *
 *	The console must be locked when we get here.
 */
static void serial_console_write(struct console *co, const char *s,
				 unsigned count)
{
	struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[CONSOLE_TTY_PORT_ID];

	// check if channel is already initiated
    if (!ctx->pch)
		return;

	while (count--) 
	{
		if (*s == '\n')
			ctx->err |= ns_uring_put_char('\r', ctx);
		ctx->err |= ns_uring_put_char((unsigned char)*s, ctx);
		s++;
	}
}

static struct tty_driver *serial_console_device(struct console *c, int *index)
{
    if (index) 
		*index = 0;

	return tty_driver;
}

static struct console ns_uring_tty_console = {
	.name = DEVICE_PCORE_NAME,
	.write = serial_console_write,
	.device = serial_console_device,
	.flags = CON_PRINTBUFFER,
	.index = -1,
};

/*
 *	Register console.
 */
static int __init ns_uring_tty_console_init(void)
{
	register_console(&ns_uring_tty_console);
	return 0;
}
console_initcall(ns_uring_tty_console_init);

#endif /* CONFIG_NS_URING_TTY_CONSOLE */

static int __init tty_driver_init(void)
{
	int ret;
	int port_id;
	struct device *dev;
	struct tty_port *p_port;

	debug_print(KERN_DEBUG
		    "tty_driver_init: ecore channel: base address offset (0x%x) ring size (0x%x)\n",
		    m_ecore_chan_base_addr_offset, m_ecore_ring_size);
	debug_print(KERN_DEBUG
		    "tty_driver_init: pcore channel: base address linear (0x%lx) ring size (0x%x)\n",
		    m_pcore_chan_base_addr, m_pcore_ring_size);
	debug_print(KERN_DEBUG
		    "tty_driver_init: score channel: base address linear (0x%lx) ring size (0x%x)\n",
		    m_score_chan_base_addr, m_score_ring_size);

	tty_driver = tty_alloc_driver(NUM_TTY_DEVICES, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (!tty_driver) {
		printk(KERN_ERR "Unable to allocate tty driver\n");
		return -ENOMEM;
	}

	tty_driver->owner = THIS_MODULE;
	tty_driver->driver_name = DRIVER_NAME;
	tty_driver->name = DEVICE_NAME;
	tty_driver->major = 0; /*Dynamic*/
	tty_driver->minor_start = 0;
	tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	tty_driver->subtype = SERIAL_TYPE_NORMAL;
	tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV |
			    TTY_DRIVER_UNNUMBERED_NODE;
	tty_driver->init_termios = tty_std_termios;
	tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_driver->init_termios.c_ispeed = 9600;
	tty_driver->init_termios.c_ospeed = 9600;

	tty_set_operations(tty_driver, &serial_device_tty_ops);

	/*register the driver*/
	printk("tty_driver_init: register NXT driver (%s)\n",
	       tty_driver->driver_name);
	ret = tty_register_driver(tty_driver);
	if (ret) {
		printk(KERN_ERR "Unable to register tty driver\n");
		tty_driver_kref_put(tty_driver);
		return ret;
	}

	/*register the tty devices*/
	for (port_id = 0; port_id < NUM_TTY_DEVICES; ++port_id) {
		p_port = &tty_ports[port_id];
		tty_port_init(p_port);
		tty_port_link_device(p_port, tty_driver, port_id);

		//overwrite tty port name for readable dev/tty files
		tty_driver->name = tty_get_port_name(port_id);
		dev = tty_register_device(tty_driver, port_id, NULL);
		if (!dev) {
			printk(KERN_ERR
			       "tty_driver_init: Unable to register tty device number %d\n",
			       port_id);
			return -ENODEV;
		}

		printk("tty_driver_init: register tty device name (%s) indx (%d)\n",
		       tty_driver->name, port_id);
	}

	// Create a tty driver main thread
	debug_print(KERN_DEBUG "tty_driver_init: run main thread\n");
	main_thread = kthread_run(ns_uring_process_main, NULL, "main_thread");

	if (IS_ERR(main_thread)) {
		printk(KERN_ERR "Failed to create main thread\n");
		goto teardown;
	}

	return 0;

teardown:
	printk("tty_driver_init failed! Unregister tty driver and devices\n");
	tty_devices_remove();
	tty_unregister_driver(tty_driver);
	return -EPERM;
}

static void __exit tty_driver_exit(void)
{
	if (tty_driver == NULL) {
		printk("tty_driver_exit: nothing to unregister, no tty driver\n");
		return;
	}

	//Stop the thread and wait for it to terminate
	if (main_thread) {
		kthread_stop(main_thread);
	}

	tty_devices_remove();
	tty_unregister_driver(tty_driver);
	printk("tty_driver_exit: unregister NXT tty driver\n");
}

module_param(m_ecore_chan_base_addr_offset, uint,
	     S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(m_ecore_ring_size, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(m_pcore_chan_base_addr, ulong, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(m_pcore_ring_size, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(m_score_chan_base_addr, ulong, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(m_score_ring_size, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_init(tty_driver_init);
module_exit(tty_driver_exit);