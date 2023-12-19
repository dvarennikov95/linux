#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include "tty_chardev.h"
#include "tty_utils.h"
#include "ns_uring.h"

#ifdef SELF_HOSTED_MODE
#define NS_URING_CHAN_INIT_IS_DOWNSTREAM
#elif defined(ENDPOINT_MODE)
#define NS_URING_CHAN_INIT_IS_UPSTREAM
#endif

extern uint32_t m_ecore_chan_base_addr_offset;
extern unsigned long m_pcore_chan_base_addr;
extern unsigned long m_score_chan_base_addr;
extern uint32_t m_ecore_ring_size;
extern uint32_t m_pcore_ring_size;
extern uint32_t m_score_ring_size;

extern struct tty_port tty_ports[NUM_TTY_DEVICES];

struct ns_uring_ctx ns_uring_channel_ctx[NUM_TTY_DEVICES] = {};

//////////////////////
/* static functions */
//////////////////////

/**
 * @brief This function returns memory size of the ns_uring ring which is used for tty device
 *
 * @param tty_dev_id  tty device id
 *
 * @return size in bytes
 */
static uint32_t ns_uring_get_ring_size(int tty_dev_id)
{
	int port_type;
	uint32_t size;

	port_type = TTY_PORT_TYPE_FROM_ID(tty_dev_id);
	switch (port_type) {
	case ECORE:
		size = m_ecore_ring_size;
		break;
	case SCORE:
		size = m_score_ring_size;
		break;
	case PCORE:
		size = m_pcore_ring_size;
		break;
	default:
		printk(KERN_DEBUG "ns_uring_get_ring_size: invalid port type %d\n",
		       port_type);
		break;
	}
	return size;
}

/**
 * @brief This function validates if ns_uring channel parameters were initiated correctly
 *
 * @param ns_uring_ch  a pointer to channel
 * @param tty_dev_id  tty device index
 *
 * @return 0 in case it's ok
 */
static int
ns_uring_channel_init_validation(struct ns_uring_ch *ns_uring_ch, int tty_dev_id)
{
	uint8_t upstream_id_expected = GET_UPSTREAM_ID(tty_dev_id);
	uint8_t upstream_id = ns_uring_get_pid(ns_uring_ch);
	uint8_t chan_type = ns_uring_get_type(ns_uring_ch);

	if (upstream_id != upstream_id_expected) {
		printk("ns_uring_channel_init_validation: failed! invalid upstream_id (%d) expected (%d)\n",
		       upstream_id, upstream_id_expected);
		return -EINVAL;
	}

	if (chan_type != NS_URING_CHANNEL_TYPE_TTY) {
		printk("ns_uring_channel_init_validation: failed! invalid chan_type (%d) expected (%d)\n",
		       chan_type, NS_URING_CHANNEL_TYPE_TTY);
		return -EINVAL;
	}

	debug_print(KERN_DEBUG
		    "ns_uring_channel_init_validation: passed! upstream_id (%d) chan_type (%d)\n",
		    upstream_id, chan_type);

	return 0;
}

/**
 * @brief insert data to tty port buffer
 *
 * @param port  a pointer to the tty port
 * @param data  a pointer to the data
 * @param length  the length of the data
 *
 * @return 0 in case it's ok
 */
static int
tty_port_insert_data(struct tty_port *port, unsigned char *data, int length)
{
	int queued, len;
	unsigned char str[sizeof(struct tty_data)] = {};

	if (length != sizeof(struct tty_data)) {
		printk("tty_port_insert_data: ERROR: data not sent! length (%d) is invalid\n",
		       length);
		return -EINVAL;
	}

	snprintf(str, length, "%s", data);
	len = strlen(str);

	queued = tty_insert_flip_string(port, str, len);

	if (queued < len) {
		printk("tty_port_insert_data: ERROR: queued < len\n");
	}

	tty_flip_buffer_push(port);
    
    return 0;

}

//////////////////////
/* global functions */
//////////////////////

unsigned long ns_uring_get_chan_linear_address(int tty_dev_id)
{
	unsigned long chan_base_address = 0;
	unsigned long quad_addr_offset, die_addr_offset, core_addr_offset;
	int port_type = TTY_PORT_TYPE_FROM_ID(tty_dev_id);
	uint32_t chan_size = CHAN_SIZE_FROM_PORT_ID(tty_dev_id);
	int quad, die, cpu_id;

	switch (port_type) {
	case ECORE:
		quad = GET_ECORE_QUAD_FROM_PORT_ID(tty_dev_id);
		die = GET_ECORE_DIE_FROM_PORT_ID(tty_dev_id);
		cpu_id = GET_ECORE_ID_FROM_PORT_ID(tty_dev_id);
		chan_base_address = DEFAULT_ECORE_NS_URING_CHAN_BASE_ADDRESS;
		die_addr_offset = die * DIE_ADDR_OFFSET;
		quad_addr_offset = quad * QUAD_ADDR_OFFSET;
		core_addr_offset = cpu_id * chan_size;
		chan_base_address |= m_ecore_chan_base_addr_offset +
				     die_addr_offset + quad_addr_offset +
				     core_addr_offset;
		debug_print(KERN_DEBUG
			    "ns_uring_get_chan_linear_address: ecore die (%d) quad (%d) cpu_id(%d) chan addr (0x%lx) chan size (0x%x) tty id (%d)\n",
			    die, quad, cpu_id, chan_base_address, chan_size, tty_dev_id);
		break;
	case SCORE:
		if (tty_dev_id == TTY_DEV_SCORE_D0_ID)
			chan_base_address = m_score_chan_base_addr;
		if (tty_dev_id == TTY_DEV_SCORE_D1_ID)
			chan_base_address = m_score_chan_base_addr +
					    DIE_ADDR_OFFSET;
		debug_print(KERN_DEBUG
			    "ns_uring_get_chan_linear_address: score chan addr (0x%lx) size (0x%x) tty id (%d)\n",
			    chan_base_address, chan_size, tty_dev_id);
		break;
	case PCORE:
		chan_base_address = m_pcore_chan_base_addr;
		debug_print(KERN_DEBUG
			    "ns_uring_get_chan_linear_address: pcore chan addr (0x%lx) size (0x%x) tty id (%d)\n",
			    chan_base_address, chan_size, tty_dev_id);
		break;
	default:
		printk(KERN_DEBUG
		       "ns_uring_get_chan_linear_address: invalid port type (%d) tty id (%d)\n",
		       port_type, tty_dev_id);
		break;
	}
	return chan_base_address;
}

void ns_uring_channel_ctx_reset(struct ns_uring_ctx *ctx)
{
	ctx->pch = NULL;
	ctx->err = 0;
	ctx->tty_port_idx = INVALID_TTY_PORT_ID;
}

void ns_uring_recv_cb(void *context, void *msg, unsigned size)
{
	struct ns_uring_ctx *ctx = (struct ns_uring_ctx *)context;
	struct ns_uring_entry *entry = (struct ns_uring_entry *)msg;
	int status = 0;

	// check if chan is already linked to tty port
	if (ctx->tty_port_idx == INVALID_TTY_PORT_ID) {
		return;
	}

	if (size != sizeof(struct ns_uring_entry)) {
		ctx->err = -EINVAL;
		printk(KERN_DEBUG
		       "ns_uring_recv_cb: ERROR: unexpected entry size (%d) \n",
		       size);
		return;
	}

	if (entry->hdr.subtype != NS_URING_ENTRY_TYPE_SINGLE_CHARACTER) {
		ctx->err = -EINVAL;
		printk(KERN_DEBUG
		       "ns_uring_recv_cb: ERROR: unexpected msg subtype (%d) \n",
		       entry->hdr.subtype);
		return;
	}

	status = tty_port_insert_data(&tty_ports[ctx->tty_port_idx], 
				      (unsigned char *)&entry->payload.data,
				      sizeof(entry->payload.data));
	if (status) {
		ctx->err = status;
		printk(KERN_DEBUG "ns_uring_recv_cb: ERROR: (%d)\n", status);
	}
}

int ns_uring_channel_init(int chan_tty_dev_id, struct ns_uring_ctx *ctx)
{
	struct ns_uring_ch *ns_uring_ch;
	int status;
	uint32_t chan_ring_size;
	uint8_t *downstream, *upstream;
	uint64_t chan_base_addr;

	debug_print(KERN_DEBUG "ns_uring_channel_init: channel tty device id (%d)\n", chan_tty_dev_id);

	chan_base_addr = ns_uring_get_chan_linear_address(chan_tty_dev_id);
	chan_ring_size = ns_uring_get_ring_size(chan_tty_dev_id);

    downstream = get_address_mapping(chan_base_addr, chan_ring_size);
    upstream = get_address_mapping(chan_base_addr + chan_ring_size, chan_ring_size);

#ifdef NS_URING_CHAN_INIT_IS_DOWNSTREAM
	ns_uring_ch = ns_uring_create_channel_downstream(
		NULL, // channel will be allocated dynamically
		(void *)upstream, (void *)downstream);
#else // NS_URING_CHAN_INIT_IS_UPSTREAM
	ns_uring_ch = ns_uring_create_channel_upstream(
		NULL, // channel will be allocated dynamically
		(void *)upstream, (void *)downstream,
		RING_SIZE_PER_ENTRY_FROM_PORT_ID(chan_ring_size),
		DEFAULT_ENTRY_SIZE, RING_SIZE_PER_ENTRY_FROM_PORT_ID(chan_ring_size),
		DEFAULT_ENTRY_SIZE, NS_URING_CHANNEL_TYPE_TTY, GET_UPSTREAM_ID(chan_tty_dev_id), true);
#endif
	if (ns_uring_ch == NULL) {
		printk("ns_uring_channel_init: FAILED creating channel\n");
		return -ENODEV;
	}

	// validate channel init
	if (ns_uring_channel_init_validation(ns_uring_ch, chan_tty_dev_id)) {
		return -ENODEV;
	}

    ctx->tty_port_idx = INVALID_TTY_PORT_ID; /* tty port index will be set at dev/tty open */
	ctx->pch = ns_uring_ch;
	ctx->err = 0;

#ifdef NS_URING_CHAN_INIT_IS_DOWNSTREAM
	status = ns_uring_register_downstream_cb(ctx->pch, ns_uring_recv_cb, ctx);
#else
	status = ns_uring_register_upstream_cb(ctx->pch, ns_uring_recv_cb, ctx);
#endif
	if (status) {
		printk("ns_uring_channel_init: FAILED register callback\n");
		return -ENODEV;
	}

	return 0;
}

int ns_uring_process_main(void *data)
{
	int dev_idx, err = 0;
	struct ns_uring_ctx *ctx;

	while (!kthread_should_stop()) {
		for (dev_idx = 0; dev_idx < NUM_TTY_DEVICES; dev_idx++) {
			ctx = &ns_uring_channel_ctx[dev_idx];
			if (ctx->pch) {
#ifdef NS_URING_CHAN_INIT_IS_DOWNSTREAM
				err = ns_uring_process_downstream(ctx->pch);
#else
				err = ns_uring_process_upstream(ctx->pch);
#endif
				if ((err) || (ctx->err)) {
					printk("ns_uring_process_main:  FAILED processing dev index = %d err = %d, \n",
					       dev_idx, err);
					ns_uring_destroy_channel(ctx->pch);
					ns_uring_channel_ctx_reset(ctx);
				}
			}
		}
		usleep_range(10, 20);
	}

	debug_print(KERN_DEBUG "ns_uring_process_main: EXIT\n");
	return 0;
}