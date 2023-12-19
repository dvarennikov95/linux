/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0    */
/* Copyright (c) 2022 NextSilicon. All rights reserved */

#ifndef _NEXTSI_TTY_CHARDEV_H_
#define _NEXTSI_TTY_CHARDEV_H_


#include <linux/tty.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/device.h>
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
#define DEVICE_ECORE_NAME "ttyEcore"
#define DEVICE_SCORE_NAME "ttyScore"
#define DEVICE_PCORE_NAME "ttyPcore"

#define DEFAULT_ENTRY_SIZE (16)
#define DEFAULT_UPSTREAM_ID (0x12)
#define GET_UPSTREAM_ID(tty_dev_id) \
	({DEFAULT_UPSTREAM_ID + tty_dev_id; })
#define RING_SIZE_PER_ENTRY_FROM_PORT_ID(ring_size)                        \
	({                                                                    \
		(ring_size - sizeof(struct ns_uring)) / \
			(DEFAULT_ENTRY_SIZE);                                 \
	})
#define CHAN_SIZE_FROM_PORT_ID(port_id) \
	({ (ns_uring_get_ring_size(port_id) * 2); })

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


struct tty_data {
	uint32_t character : 8;
	uint32_t reserved : 24;
};

// The definition of the entry message payload
struct tty_msg {
	uint32_t reserved; // First 4B word reserved
	struct tty_data data; // Second 4B word
};

// The definition of an TTY entry
struct ns_uring_entry {
	struct ns_uring_msg_hdr hdr;
	struct tty_msg payload;
};

struct ns_uring_ctx {
	struct ns_uring_ch *pch;
	uint32_t tty_port_idx;
	int err;
};

/**
 * @brief This function returns ns_uring channel base linear address which is used for tty device
 *
 * @param tty_dev_id  tty device index
 *
 * @return linear 64bit address
 */
unsigned long ns_uring_get_chan_linear_address(int tty_dev_id);

/**
 * @brief reset channel's context
 *
 * @param ctx pointer to channel context
 */
void ns_uring_channel_ctx_reset(struct ns_uring_ctx *ctx);

/**
 * @brief initialize channel
 *
 * @param chan_tty_dev_id  channel tty device id 
 * @param ctx pointer to channel context
 *
 * @return The error code, or zero for no error
 */
int ns_uring_channel_init(int chan_tty_dev_id, struct ns_uring_ctx *ctx);

/**
 * @brief tty ns_uring main proccess function
 *
 * @param data  pointer data
 *
 * @return The error code, or zero for no error
 */
int ns_uring_process_main(void *data);

#endif /* _NEXTSI_TTY_CHARDEV_H_ */