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
#include "ns_uring.h"

extern struct tty_port tty_ports[MAX_TTY_DEVICES];
extern int tty_tx_buffer(struct tty_port *port, unsigned char *data, int length);
extern int print;
extern int close_tty_write;
extern int close_console_write;


struct ns_uring_ctx ns_uring_channel_ctx[MAX_TTY_DEVICES] = {};

unsigned long get_channel_paddress(int type, int coreid)
{
    unsigned long chan_base_address = 0;
    switch (type)
    {
        case ECORE:
            chan_base_address = 0x8C4383700000;
            break;
        case SCORE:
            chan_base_address = 0x8DFFE0100000;
            break;
        case PCORE:
            chan_base_address = 0x8DFFE0000000;
            break;
        default:
            break;
    }
    printk("get_channel_paddress: core type  %d addr (0x%lx)\n", type, chan_base_address);
    return chan_base_address + coreid*CHAN_SIZE;
}

uint8_t *get_ring_vaddress(unsigned long paddress)
{
    unsigned long mmio_start, mmio_len;

    
    /* Get bar start and stop memory offsets */
    mmio_start = paddress;
    mmio_len = RING_SIZE;

    /* MEMREMAP_WB - matches the default mapping for "System RAM" on
    * the architecture.  This is usually a read-allocate write-back cache.
    * Morever, if MEMREMAP_WB is specified and the requested remap region is RAM
    * memremap() will bypass establishing a new mapping and instead return
    * a pointer into the direct map.
    *
    * MEMREMAP_WT - establish a mapping whereby writes either bypass the
    * cache or are written through to memory and never exist in a
    * cache-dirty state with respect to program visibility.  Attempts to
    * map "System RAM" with this mapping type will fail.
    */
	return memremap(mmio_start, mmio_len, MEMREMAP_WT);
}

void ns_uring_channel_ctx_reset(struct ns_uring_ctx *ctx)
{
    ctx->pch = NULL;
    ctx->err = 0;
    ctx->id = MAX_TTY_DEVICES + 1;  // set invalid id 
}

int tty_tx_buffer(struct tty_port *port, unsigned char *data, int length)
{
    int queued, len; 
    unsigned char str[DEFAULT_DATA_SIZE] = {};

    // debug_print(KERN_DEBUG "tty_tx_buffer: data = 0x%x data_len = %d \n", *data, length);

    if (length != DEFAULT_DATA_SIZE)
    {
        printk("tty_tx_buffer: ERROR: data not sent! length (%d) is invalid\n", length);
        return -EINVAL;
    }


    if ((*data == '%')) {
        print = 1;
    }
    if ((*data == '1')) {
        close_console_write = 0;
    }
    if ((*data == '2')) {
        close_tty_write = 0;
    }
    if ((*data == '3')) {
        close_console_write = 1;
        close_tty_write = 1;
    }



    snprintf(str, length, "%s", data);
    len = strlen(str);

    queued = tty_insert_flip_string(port, str, len);

    if (queued < len)
    {
        printk("tty_tx_buffer: ERROR: queued < len\n");
    }

    /*here we push the data outside - need to check why port->buf.tail->used not set to 0*/
	tty_flip_buffer_push(port);
    
    return 0;
}

void ns_uring_recv_cb(void *context, void *msg, unsigned size)
{
	struct ns_uring_ctx *ctx = (struct ns_uring_ctx *)context;
	struct ns_uring_entry *entry = (struct ns_uring_entry *)msg;
	int status = 0;
    uint8_t upstream_id = ns_uring_get_pid(ctx->pch);

	if (size != sizeof(struct ns_uring_entry)) {
		ctx->err = -EINVAL;
		printk(KERN_DEBUG "ns_uring_recv_cb: ERROR: unexpected entry size (%d) \n",
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

    // debug_print(KERN_DEBUG "ns_uring_recv_cb: from upstream_id (%d) data (0x%x)\n", upstream_id, entry->payload.character);

    status = tty_tx_buffer(&tty_ports[ctx->id],
				      (unsigned char *)&entry->payload.character,
				      sizeof(entry->payload.character));
	if (status) {
		ctx->err = status;
		printk(KERN_DEBUG "ns_uring_recv_cb: ERROR: (%d)\n", status);
        return;
	}

    // todo remove
    // *ctx->p_in_data = (unsigned char)entry->payload.character;
    // ctx->in_data_received = true;
}

int ns_uring_channel_init(int core_chan_idx)
{
    struct ns_uring_ctx *ctx = &ns_uring_channel_ctx[core_chan_idx];
    struct ns_uring_ch *ns_uring_ch;
    int status;
    uint8_t *downstream_address;
    uint8_t *upstream_address;

    debug_print(KERN_DEBUG "ns_uring_channel_init portid %d\n", core_chan_idx);

#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
    downstream_address = 0; /* todo Alex */
    upstream_address = downstream_address + RING_SIZE;
    ns_uring_ch = ns_uring_create_channel_downstream( NULL,    // channel will be allocated dynamically
                                                      (void *)upstream_address,
                                                      (void *)downstream_address);
#else //IS_UPSTREAM
    downstream_address = get_ring_vaddress(get_channel_paddress(PCORE, 0));
    upstream_address = get_ring_vaddress(get_channel_paddress(PCORE, 0) + RING_SIZE);
    ns_uring_ch = ns_uring_create_channel_upstream( NULL,     // channel will be allocated dynamically
                                                    (void*)upstream_address, 
                                                    (void*)downstream_address, 
                                                    RING_SIZE_PER_ENTRY,
                                                    DEFAULT_ENTRY_SIZE, 
                                                    RING_SIZE_PER_ENTRY,
                                                    DEFAULT_ENTRY_SIZE,
                                                    NS_URING_CHANNEL_TYPE_TTY,
                                                    84/*core_chan_idx*/, 1);/* todo Alex */
#endif
    if (ns_uring_ch == NULL)
    {
        printk("ns_uring_channel_init: FAILED creating channel\n");
        return -ENODEV;
    }

    debug_print(KERN_DEBUG "ns_uring_create_channel_upstream: ring_size = %d ,entry_size = %d, chan_type = %d, upstream_id (pid) = %d ignore_full = %d\n",
                ns_uring_get_upstream_ring_size(ns_uring_ch),
                ns_uring_ch->downstream->entry_size,
                ns_uring_ch->downstream->type,
                ns_uring_ch->downstream->pid,
                ns_uring_ch->downstream->never_full);
    ctx->pch = ns_uring_ch;
    ctx->id = core_chan_idx;
    ctx->err = 0;
    ctx->in_data_received = 0;

#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
    status = ns_uring_register_downstream_cb(ctx->pch, ns_uring_recv_cb, ctx);
#else
    status = ns_uring_register_upstream_cb(ctx->pch, ns_uring_recv_cb, ctx);
#endif
    if (status)
    {
        printk("ns_uring_channel_init: FAILED register callback\n");
        return -ENODEV;
    }
    
    return 0;
} 


int ns_uring_process_main(void *data)
{
    int chan_idx, err=0;
    struct ns_uring_ctx *ctx;

    debug_print(KERN_DEBUG "ns_uring_process_main: Start\n");
    while (!kthread_should_stop())
    {
        for(chan_idx=0 ; chan_idx < MAX_TTY_DEVICES ; chan_idx++)
        {
            ctx = &ns_uring_channel_ctx[chan_idx];
            if(ctx->pch)
            {
#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
                err = ns_uring_process_downstream(ctx->pch);
#else
                err = ns_uring_process_upstream(ctx->pch);
#endif
                if(err < 0)
                {
                    printk("ns_uring_process_main:  FAILED processing channel index = %d err = %d, \n", chan_idx, err);
                    ns_uring_destroy_channel(ctx->pch);
                    ns_uring_channel_ctx_reset(ctx);
                }
            }
        }
        // usleep_range(10, 20); //// usleep_range_state(min, max, TASK_UNINTERRUPTIBLE);
        msleep(10); 
    }

    debug_print(KERN_DEBUG "ns_uring_process_main: EXIT\n");
    return 0;
}

