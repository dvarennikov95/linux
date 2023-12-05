/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0    */
/* Copyright (c) 2022 NextSilicon. All rights reserved */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/tty.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>
#include <linux/console.h>

#include "tty_utils.h"
#include "tty_chardev.h"
#include "tty_dev_mmap.h"
#include "ns_uring.h"


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NextSilicon TTY Driver");
MODULE_VERSION("0.1.0");


static const struct of_device_id ns_uring_tty_of_match[] = {
	{ .compatible = "nxt,uart-over-mem", .data = NULL },
	{ /* nextsilicon tty */ }
};
MODULE_DEVICE_TABLE(of, ns_uring_tty_of_match);

#define DEFAULT_NS_URING_BASE_ADDRESS      (0x8DFFE0000000)

// extern bool is_pci_dev_initiated;
extern struct ns_uring_ctx ns_uring_channel_ctx[MAX_TTY_DEVICES];

static struct task_struct *main_thread;
static struct tty_driver *tty_driver;
struct tty_port tty_ports[MAX_TTY_DEVICES];
unsigned long ring_base_addr = DEFAULT_NS_URING_BASE_ADDRESS;

int cnt2 = 0, cnt = 0, initialized = 0;

static char *tty_get_port_name_from_id(int port_id)
{
    char *port_name;
#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
    if ((port_id >= TTY_ECORE_ID_START) && (port_id <= TTY_ECORE_ID_END))
    {
        port_name = DEVICE_ECORE_NAME;
    } else if (port_id == TTY_SCORE_ID) {
        port_name = DEVICE_SCORE_NAME;
    } else if (port_id == TTY_PCORE_ID) {
        port_name = DEVICE_PCORE_NAME;
    }
#else // UPSTREAM
    port_name = DEVICE_PCORE_NAME;
#endif
    return port_name;
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
    strcpy(tty->name, tty_get_port_name_from_id(tty->index));

    debug_print(KERN_DEBUG "TTY device INSTALL (%s)\n", tty->name);
        
	return 0;
}

/**
 * @brief close the tty device
 * 
 * @param tty  a pointer to the tty srtruct
 * @param file  a pointer to the file
 * 
 */
static void serial_device_tty_close(struct tty_struct *tty, struct file *file)
{
    struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[tty->index];
    struct tty_struct *tty_p;
    
    printk("serial_device_tty_close: serial port (%s) id(%d)\n", tty->name, tty->index);
    if (tty->count > 1)
    {
        tty_p = ((struct tty_file_private *)file->private_data)->tty;
        printk("serial_device_tty_close: do nothing! serial port (%s) is still open\n", tty->name);
        return;
    }

    if(tty->index < MAX_TTY_DEVICES)
    {
        ns_uring_destroy_channel(ctx->pch);
        ns_uring_channel_ctx_reset(ctx);
        debug_print(KERN_DEBUG "TTY device CLOSE: core_idx = %d pch = %p \n",tty->index, ctx->pch);
    }

    return;
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
    struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[tty->index];

    printk("serial_device_tty_open: serial port (%s) id(%d) flags(%d)\n", tty->name, tty->index, port->flags);

    if (!tty_port_initialized(port))
    {
        printk("serial_device_tty_open: initialize serial port (%s)\n", tty->name);
        tty_port_set_initialized(port, true);
        initialized = 1;
    }
            
    if (tty->count > 1)
    {
    /* Any further open() on the tty
       that is currently open on descriptor fd will fail with errno == EACCES */
        printk("serial_device_tty_open: serial port (%s) id(%d) already in use, cannot open multiple times (%d)\n", tty->name, tty->index, tty->count);
        err = -EACCES;
        return err;
    }

    if(tty->index < MAX_TTY_DEVICES)
    {
        if(ctx->pch == NULL)
        {
            /*init ns_uring channel*/
            err = ns_uring_channel_init(tty->index);
            if(err != 0)
            {
                printk("serial_device_tty_open: failed! cannot initiate channel error = %d\n", err);
                goto closeport;
            }
            debug_print(KERN_DEBUG "ns_uring_channel_init: Finished! pointer to ch = %p\n", ctx->pch);
        }
    }
    
    if (!tty_port_core_mem_access_check(tty->index, (unsigned long*)ctx->pch->downstream))
    {
        printk("serial_device_tty_open: no access to port target core memory !\n");
        err = -ENODEV;
        goto closeport;
    }


    return 0;

    closeport:
        tty_port_close(port, tty, file);
        tty_release_struct(tty, tty->index);
        tty_unlock(tty);
        return err;
}



static int tx_send_test(const unsigned char ch)
{
    uint32_t msg[4];
    int result = 0;
    struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[0];
    struct ns_uring *send_ring;
    uint32_t msg_data = 0;
    // uint8_t pid = ns_uring_get_pid(ctx->pch);
    
    /* copy message data from tty device */
    // memcpy((void*)&msg_data, (void *)ch, 1); 

    // debug_print(KERN_DEBUG "console: tx_send_test: data = 0x%c length = %d upstream_id (pid) = %d \n", ch, pid);

    msg[0] = NS_URING_ENTRY_TYPE_SINGLE_CHARACTER;
    msg[1] = 0;
    msg[2] = 0;
    msg[3] = (uint32_t)ch;//msg_data;

    result = ns_uring_send(ctx->pch, (void *)&msg, sizeof(msg));

    if(result != 0)
    {
        return -1;
    }

    return 0;
}

/**
 * @brief write to the tty device
 * 
 * @param tty  a pointer to the tty srtruct
 * @param data  a pointer to the data we want to write
 * @param length  the length of the data
 * 
 * @return length value in case it's ok
 */
static int serial_device_tty_write(struct tty_struct *tty, const unsigned char *data, int length)
{
    int result = 0;
    struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[tty->index];
    int count = length;

    
    if(tty->index >= MAX_TTY_DEVICES)
    {
        printk("TTY device WRITE: invalid tty id = %d \n", tty->index);
        return -1;
    }

    if (!initialized)
    {
        printk("TTY device WRITE: device not initialized \n");
        return -1;
    }

    while (count--)
    {
        result = tx_send_test(*data);
        if ((cnt2 == 0) || (*data == '%')) {
            debug_print(KERN_DEBUG "serial_device_tty_write: data (%c) length (%d)\n", *data, count);
            cnt2++;
        }
        data++;
    }

    if(result != 0)
    {
        debug_print(KERN_DEBUG "TTY device WRITE: ERROR: result = %d\n", result);
        return -1;
    }

    return length;
}

#ifdef CONFIG_CONSOLE_POLL
static int uart_over_mem_poll_get_char(struct tty_driver *driver, int line)
{
    int status;
    struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[line];
#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
	status = ns_uring_process_downstream(ctx->pch);
#else // UPSTREAM
    status = ns_uring_process_upstream(ctx->pch);
#endif

	if (ctx->in_data_received == false)
		return NO_POLL_CHAR; // no data, buffer is empty

	if (status < 0) { // data error
		ns_uring_destroy_channel(ctx->pch);
		ctx->err = status;
		return NO_POLL_CHAR;
	}
	// If we reached this point that means data was received, processed and copied to caller
	// data buffer (p_in_data) clear data receive flag
	ctx->in_data_received = false;
	return *ctx->p_in_data;
}

static void uart_over_mem_poll_put_char(struct tty_driver *driver, int line,
					char ch)
{
    const unsigned char data = (unsigned char)ch; //hack
	struct tty_struct *tty = driver->ttys[line];

	tty->ops->write(tty, &data, 1);
}
#endif

/**
 * @brief This routine returns the numbers of characters the tty driver will accept for queuing to be written
 * 
 *	Return the number of bytes that can be queued to this device
 *	at the present time. The result should be treated as a guarantee
 *	and the driver cannot offer a value it later shrinks by more than
 *	the number of bytes written. If no method is provided 2K is always
 *	returned and data may be lost as there will be no flow control.
  * @param tty  a pointer to the tty srtruct 
 * 
 * @return the available space
 */
static unsigned int serial_device_tty_write_room(struct tty_struct *tty)
{   
    // Designed for slow data rate (char device)
    // For faster data rate this will probably have to be optimized.
    if (tty->hw_stopped) 
    {
        return 0;
    }
		
    return tty_buffer_space_avail(tty->port);
}

/**
 * @brief the ability to perform more actions in the driver
 * 
 * @param tty  a pointer to the tty srtruct
 * @param cmd  the command we want to perform
 * @param arg  additional parameter
 * 
 * @return 0 value in case it's ok
 */
static int serial_device_tty_ioctl(struct tty_struct *tty, unsigned int cmd, unsigned long arg)
{
    // debug_print(KERN_DEBUG "tty_ioctl: arg = 0x%lx cmd = 0x%x\n", arg, cmd);

    return 0;
}

/**
 * @brief Set the permission of the device
 * 
 * NOTE: The notation "crw-rw-rw-" represents the permissions of a file or device in a Linux/Unix operating system. Let's break down what each character in the notation means:
 *         1. The first character ('c' or 'b'): This indicates the type of file. In this case, it is 'c', which stands for "character device." If it were a 'b', it would stand for "block device."
 *         2. The next nine characters: These characters are divided into three sets of three and represent the file permissions for the owner, group, and others, respectively.
 * - The first set ('rw-') represents the permissions for the owner of the file. 'rw-' means the owner has read and write permissions but not execute permissions.
 * - The second set ('rw-') represents the permissions for the group. 'rw-' means the group has read and write permissions but not execute permissions.
 * - The third set ('rw-') represents the permissions for others (everyone else). 'rw-' means others have read and write permissions but not execute permissions.
 * 
 * So, in summary, the permissions "crw-rw-rw-" mean that the file/device is a character device, and all users (owner, group, and others) have read and write permissions but no execute permissions. 
 * In Linux, character devices are typically used for devices that transfer data character by character, like serial ports, terminals, etc.
 * 
 * @param port  a pointer to the device struct
 * @param mode  a pointer to the mode(permmision)
 * 
 * @return NULL
 * 
 */
// #if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE
// static char *tty_devnode(const struct device *device, umode_t *mode)
// #else
// static char *tty_devnode(struct device *device, umode_t *mode)
// #endif
// {
// 	if (mode)
//     {
//         debug_print(KERN_DEBUG "set tty_devnode\n");
//         *mode = 0666;
//     }
		
// 	return NULL;
// }

/* TTY operations structure */
static const struct tty_operations serial_device_tty_ops = {
	.install = tty_driver_install,
	.open = serial_device_tty_open,
	.close = serial_device_tty_close,
	.write = serial_device_tty_write,
	.write_room = serial_device_tty_write_room,
	.ioctl = serial_device_tty_ioctl,
#ifdef CONFIG_CONSOLE_POLL
	.poll_get_char = uart_over_mem_poll_get_char,
	.poll_put_char = uart_over_mem_poll_put_char,
#endif
};

static void tty_devices_remove(void)
{
    int i;
    struct ns_uring_ctx *ctx;
    
    for(i = 0; i < MAX_TTY_DEVICES ; ++i)
	{
        printk("unregister tty device number %d\n", i);
        ctx = &ns_uring_channel_ctx[i];
        if(ctx->pch)
        {
            ns_uring_destroy_channel(ctx->pch);
            ns_uring_channel_ctx_reset(ctx);
        }
		tty_unregister_device(tty_driver, i);
	}
}

// #if defined(CONFIG_NXT_TTY_CONSOLE)

/*
 * ------------------------------------------------------------
 * Serial console driver
 * ------------------------------------------------------------
 */

/*
 *	Print a string to the serial port trying not to disturb
 *	any possible real use of the port...
 *
 *	The console must be locked when we get here.
 */
static void serial_console_write(struct console *co, const char *s,
				 unsigned count)
{
    if (cnt == 0) {
        debug_print(KERN_DEBUG "serial_console_write: ns_uring tty driver not up yet!\n");
        cnt++;
    }

    if (!initialized)
    {
        return;
    }

    while (count--) 
    {
        tx_send_test((unsigned char)*s);
        if ((cnt == 1) || (*s == '%')) {
            debug_print(KERN_DEBUG "serial_console_write: data (%c) length (%d)\n", *s, count);
            cnt++;
        }
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
	.flags = CON_PRINTBUFFER,// | CON_BOOT,
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

// #endif /* CONFIG_NXT_TTY_CONSOLE */

static int __init tty_driver_init(void)
{ 
    int ret;
	int port_id;
    struct device *dev;
    // struct pci_dev *p_pci_dev;
    struct tty_port *p_port;

    debug_print(KERN_DEBUG "tty_driver_init, base ring address = 0x%lx\n", ring_base_addr);

    // tty_driver = alloc_tty_driver(MAX_TTY_DEVICES); 
    tty_driver = tty_alloc_driver(MAX_TTY_DEVICES, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);

    debug_print(KERN_DEBUG "tty_driver_init, driver allocated\n");
    
    if (!tty_driver)
    { 
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
	tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV | TTY_DRIVER_UNNUMBERED_NODE;
    tty_driver->init_termios = tty_std_termios;
    tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_driver->init_termios.c_ispeed = 9600;
	tty_driver->init_termios.c_ospeed = 9600;

    tty_set_operations(tty_driver, &serial_device_tty_ops);

    /*register the driver*/
    printk("register tty driver (%s)\n", tty_driver->driver_name);
    ret = tty_register_driver(tty_driver); 
    if (ret) 
    { 
        printk(KERN_ERR "Unable to register tty driver\n"); 
        tty_driver_kref_put(tty_driver);
        return ret; 
    }

    /*register the tty devices*/
    for (port_id = 0; port_id < MAX_TTY_DEVICES; ++port_id)
    {
        p_port =  &tty_ports[port_id];
        tty_port_init(p_port);
        tty_port_link_device(p_port, tty_driver, port_id);
        //overwrite device name for readable dev/tty file names 
        tty_driver->name = tty_get_port_name_from_id(port_id); 

        dev = tty_register_device(tty_driver, port_id, NULL);
        if(!dev)
        {
            printk(KERN_ERR "tty_driver_init: Unable to register tty device number %d\n", port_id);
            return -ENODEV;
        }

        printk("tty_driver_init: register tty device name (%s) port id(%d)\n", tty_driver->name, port_id); 
    }

#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
    // Create a tty driver main thread
    debug_print(KERN_DEBUG "tty_driver_init: run main thread\n");
    main_thread = kthread_run(ns_uring_process_main, NULL, "main_thread");

    if (IS_ERR(main_thread)) 
    {
        printk(KERN_ERR "Failed to create main thread\n");
        goto teardown;
    }
#else // UPSTREAM
    // register_console(&ns_uring_tty_console);
#endif
    
    return 0; 

    teardown:
        printk("tty_driver_init failed! Unregister tty driver and devices\n"); 
        tty_devices_remove();
        tty_unregister_driver(tty_driver);
        return -ENODEV; 
} 

static void __exit tty_driver_exit(void)
{ 
	if (tty_driver == NULL)
	{
        printk("tty_driver_exit: nothing to unregister, no tty driver\n");
		return;
	}

    //Stop the thread and wait for it to terminate
    if(main_thread)
    {
        kthread_stop(main_thread);
    }

    tty_devices_remove();
    tty_unregister_driver(tty_driver);
    printk("tty_driver_exit: unregister tty driver\n");
}

module_param(ring_base_addr, ulong, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_init(tty_driver_init);
module_exit(tty_driver_exit);
