/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0    */
/* Copyright (c) 2022 NextSilicon. All rights reserved */

#ifndef _NEXTSI_TTY_CHARDEV_H_
#define _NEXTSI_TTY_CHARDEV_H_


#include <linux/tty.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>

#include "tty_utils.h"
#include "tty_dev_mmap.h"
#include "ns_uring.h"

#define DRIVER_NAME "tty_driver" 
#define DEVICE_NAME "tty_device"
#define DEVICE_ECORE_NAME "tty_device_ecore"
#define DEVICE_SCORE_NAME "tty_device_score"
#define DEVICE_PCORE_NAME "ttypcore"

extern unsigned long ring_base_addr;

#define RING_SIZE           (0x20000)
#define CHAN_SIZE           (RING_SIZE * 2)
#define DEFAULT_ENTRY_SIZE	(16)
#define DEFAULT_DATA_SIZE	(4)
#define RING_SIZE_PER_ENTRY	((RING_SIZE) / (DEFAULT_ENTRY_SIZE * 2))
#define EXPECTED_MSG_TYPE   (0x12)

#define GET_DOWNSTREAM_RING_BASE(chan_idx)  (ring_base_addr + (chan_idx * CHAN_SIZE))
#define GET_UPSTREAM_RING_BASE(chan_idx) (ring_base_addr + (chan_idx * CHAN_SIZE) + RING_SIZE)
#ifndef UNUSED_PARAM
#define UNUSED_PARAM(_x) (void)(_x)
#endif


struct ns_uring_ctx {
    struct ns_uring_ch *pch;
    uint32_t id; //channel core id [0-31] Ecore, [32] Score [33] pcore
    int err;
    bool in_data_received;
	unsigned char *p_in_data;
};

enum core_type{
    ECORE=0,
    SCORE=1,
    PCORE=2,
    MAX_CORE_TYPE,
};

enum ns_uring_channel_types {
	NS_URING_CHANNEL_TYPE_GENERAL = 0,
	NS_URING_CHANNEL_TYPE_TTY = 1,
	/* Invalid type */
	NS_URING_CHANNEL_TYPE_INVALID_TYPE = 255,
};

enum ns_uring_entry_types {
	NS_URING_ENTRY_TYPE_GENERAL = 0,
	NS_URING_ENTRY_TYPE_SINGLE_CHARACTER = 1,
	NS_URING_ENTRY_TYPE_MULTI_CHARACTER = 2,
	/* Invalid type */
	NS_URING_ENTRY_TYPE_INVALID_TYPE = 255,
};

// The definition of the entry body
struct uart_over_mem_msg {
	uint32_t reserved; // First 4B word
	uint32_t character; // Second 4B word
};

// The definition of uart over mem entry
struct ns_uring_entry {
	struct ns_uring_msg_hdr hdr;
	struct uart_over_mem_msg payload;
};

uint8_t *get_ring_vaddress(unsigned long channel_paddress);
unsigned long get_channel_paddress(int type, int coreid);


/**
 * @brief reset channel's context
 *
 * @param ctx pointer to channle context
 */
void ns_uring_channel_ctx_reset(struct ns_uring_ctx *ctx);

/**
 * @brief initialized an upstream channel
 *
 * @param core_chan_idx the core channel index
 * @param pch pointer to store channel address
 * 
 * @return The error code, or zero for no error
 */
int ns_uring_channel_init(int core_chan_idx);

/**
 * @brief tty ns_uring main proccess function 
 *

 * @param data  pointer data 
 * 
 * @return The error code, or zero for no error
 */
int ns_uring_process_main(void *data);

/**
 * @brief transmit data out of the tty driver
 * 
 * 
 * @param dev  a pointer to the device
 * @param port  a pointer to the tty port struct
 * @param data  a pointer to the data
 * @param length  the length of the data
 * 
 * @return 0 in case it's ok
 */
int  tty_tx_buffer(struct tty_port *port, unsigned char *data, int length);

#endif /* _NEXTSI_TTY_CHARDEV_H_ */
