/**
 * @file ns_uring.c
 * @author Karri Kivelä (karri.kivela@nextsilicon.com)
 * @brief Base ns_uring protocol library.
 * @version 0.1
 * @date 2022-05-19
 *
 * @copyright Copyright (c) 2022 NextSilicon LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The initial implementation of the platform independent, base library of
 * ns_uring.
 *
 * The ns_uring somewhat resembles IO_uring and liburing of the Linux kernel,
 * or RTIO of Zephyr. The basic principle is to allow using host memory,
 * allocating and mapping host memory as a ring of messages, which then both
 * kernel space and user space can directly access, needing to do minimal or
 * no system calls from user space, nor copying of data. Even though the protocol
 * was designed to work with host memory, the protocol allows using of any memory,
 * be it host memory, device HBM memory, or an SRAM on the device.
 * The NextSilicon implementation, the ns_uring, is essentially designed
 * to be used for communication between two endpoints, upstream and downstream.
 * The communication is over channels, each channel consisting of either an
 * upstream ring, a downstream ring, or both. There are no submission queues or
 * completion queues. Instead, completion is done by advancing ring pointers.
 * For more specific details, the ns_uring design document can be referred to.
 *
 * The ns_uring is planned to replace the current Gen1 communication channel between
 * the host and the RISC management firmware, which today uses a
 * combination of VFIDs and messages in the device HBM memory. Also
 * ns_uring allows communication with the RISC complex from several source,
 * i.e. from different loaders.
 */

#include "ns_uring.h"
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>





#ifndef UNUSED_PARAM
#define UNUSED_PARAM(_x) (void)(_x)
#endif

#define NS_URING_RESERVED_WRAP_AROUND_MASK (0x80000000)

#define MEM_FREE(mem) kfree(mem)
#define MEM_ALLOC(size) kzalloc(size, GFP_KERNEL)



/**
 * @brief Get the next index value for the index and ring specified.
 *
 * @param ring Pointer to the ring struct
 * @param index The index that should be updated
 * @return The new index value
 */
static uint32_t ns_uring_next_index(struct ns_uring *ring, uint32_t index)
{
    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    return (index + 1) % (2 * ring->ring_size);
}

/**
 * @brief Get the number of unread messages of the specified ring.
 *
 * @param ring Pointer to the ring struct
 * @return The number of unread messages
 */
static uint32_t ns_uring_read_delta(struct ns_uring *ring)
{
    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    const uint32_t overlap_desc_count = 2 * ring->ring_size;

    return (overlap_desc_count + ring->write_ptr - ring->read_ptr) % overlap_desc_count;
}

/**
 * @brief Get the number of unacked messages of the specified ring. That is,
 *		the number of messages that were consumed but not yet freed by the
 *		producer of the ring.
 *
 * @param ring Pointer to the ring struct
 * @param ack_idx The ack index, depending on the producer of the ring
 * @return The number of unacked messages
 */
static uint32_t ns_uring_ack_delta(struct ns_uring *ring, uint32_t ack_idx)
{
    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    const uint32_t overlap_desc_count = 2 * ring->ring_size;

    return (overlap_desc_count + ring->read_ptr - ack_idx) % overlap_desc_count;
}

/**
 * @brief Check if the specified ring has write entries to be acked or not.
 *
 * @param ring Pointer to the ring struct
 * @param ack_idx The ack index, depending on the producer of the ring
 * @return True if there are unacked messages
 */
static bool ns_uring_is_ring_acked(struct ns_uring *ring, uint32_t ack_idx)
{
    return ring->write_ptr == ack_idx;
}

static bool ns_uring_is_ring_empty(struct ns_uring *ring)
{
    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    return ring->write_ptr == ring->read_ptr;
}

static bool ns_uring_is_ring_full(struct ns_uring *ring)
{
    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    debug_print(KERN_DEBUG "ns_uring_is_ring_full! pid = %d\n", ring->pid);
    return (ring->write_ptr ^ ring->ring_size) == ring->read_ptr;
}

/**
 * @brief Get the absolute index to the ring.
 *
 * @param ring Pointer to the ring struct
 * @param index The ring index, including the wrap around
 * @return The index to the ring
 */
static uint32_t ns_uring_real_ring_index(struct ns_uring *ring, uint16_t index)
{
    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    return index % ring->ring_size;
}

static void *ns_uring_get_next_entry(struct ns_uring *ring, uint32_t idx)
{
    uint32_t real_idx = ns_uring_real_ring_index(ring, idx);

    return (void *)(&ring->entries[real_idx * ring->entry_size]);
}

static int ns_uring_create_ring(struct ns_uring *ring, uint32_t ring_size, uint32_t entry_size,
                                uint8_t type, uint8_t pid, bool ignore_full)
{

    debug_print(KERN_DEBUG "ns_uring_create_ring: ring_size = %d ,entry_size = %d, type = %d, pid = %d ignore_full = %d\n",
                                                  ring_size, entry_size, type, pid, ignore_full);

    if (ring_size >= NS_URING_RESERVED_WRAP_AROUND_MASK)
        return -EINVAL;

    /* Size of zero will also be detected as power of two, which is ok */
    if (ring_size & (ring_size - 1))
        return -EINVAL;

    if (entry_size % 8)
        return -EINVAL;

    ring->write_ptr = 0;
    ring->read_ptr = 0;
    ring->ring_size = ring_size;
    ring->never_full = ignore_full;
    ring->entry_size = entry_size;
    ring->type = type;
    ring->pid = pid;

    memset(ring->reserved, 0, NS_URING_RESERVED_WORDS_COUNT * sizeof(ring->reserved[0]));

    return 0;
}

struct ns_uring_ch *ns_uring_create_channel_upstream(struct ns_uring_ch *prealloc_ch, void *ring_up,
                                                     void *ring_down, uint32_t up_ring_size,
                                                     uint16_t up_entry_size,
                                                     uint32_t down_ring_size,
                                                     uint16_t down_entry_size, uint8_t type,
                                                     uint8_t pid, bool ignore_full)
{
    struct ns_uring_ch *ch;
    int err;

    if ((!ring_up && !ring_down) || (!up_ring_size && !down_ring_size))
        return NULL;

    if (!prealloc_ch) {
        ch = MEM_ALLOC(sizeof(*ch));
        if (!ch)
            return NULL;
    } else {
        ch = prealloc_ch;
    }

    ch->downstream = (struct ns_uring *)ring_down;
    ch->upstream = (struct ns_uring *)ring_up;

    if (ch->upstream) {
        err = ns_uring_create_ring(ch->upstream, up_ring_size, up_entry_size, type, pid, ignore_full);
        if (err)
            goto out_err;
    }
    if (ch->downstream) {
        err = ns_uring_create_ring(ch->downstream, up_ring_size, up_entry_size, type, pid, ignore_full);
        if (err)
            goto out_err;
    }

    ch->ignore_full = ignore_full;
    ch->event_underway = false;
    ch->event_raised = false;
    ch->at_upstream = true;
    ch->send_ack_idx = 0;

    return ch;
out_err:
    if (!prealloc_ch) {
        MEM_FREE(ch);
    }
    return NULL;
}

struct ns_uring_ch *ns_uring_create_channel_downstream(struct ns_uring_ch *prealloc_ch,
                                                       void *ring_up, void *ring_down)
{
    struct ns_uring_ch *ch;

    if (!ring_up && !ring_down)
        return NULL;

    if (!prealloc_ch) {
        ch = MEM_ALLOC(sizeof(*ch));
        if (!ch)
            return NULL;
    } else {
        ch = prealloc_ch;
    }

    ch->downstream = (struct ns_uring *)ring_down;
    ch->upstream = (struct ns_uring *)ring_up;

    debug_print(KERN_DEBUG "ns_uring_create_channel_downstream: ring_size = %d ,entry_size = %d, type = %d, pid = %d ignore_full = %d\n",
                            ns_uring_get_downstream_ring_size(ch),
                            ch->downstream->entry_size,
                            ch->downstream->type,
                            ch->downstream->pid,
                            ch->downstream->never_full);
    
    if (!ch->downstream->type || !ch->downstream->entry_size % 8 || !ch->downstream->ring_size)
        goto out_err;

    ch->ignore_full = !!ch->downstream->never_full;
    ch->event_underway = false;
    ch->event_raised = false;
    ch->at_upstream = false;
    ch->send_ack_idx = 0;

    return ch;
out_err:
    if (!prealloc_ch) {
        MEM_FREE(ch);
    }
    return NULL;
}

void ns_uring_destroy_channel(struct ns_uring_ch *ch)
{
#ifdef CHAN_DYNAMICALY_ALLOCATED
    if (ch)
        MEM_FREE(ch);
#else
    UNUSED_PARAM(ch);
#endif
}

static bool ns_uring_raised_event_finished(struct ns_uring_ch *ch)
{
    if (ch->at_upstream) {
        if (ch->downstream) {
            if (!ch->downstream->w_ptr_event)
                return true;
        } else if (ch->upstream) {
            if (ch->upstream->r_ptr_event)
                return true;
        }
    } else { /* At downstream */
        if (ch->upstream) {
            if (ch->upstream->w_ptr_event)
                return true;
        } else if (ch->downstream) {
            if (ch->downstream->r_ptr_event)
                return true;
        }
    }

    return false;
}

static void ns_uring_set_event_bit(struct ns_uring_ch *ch)
{
    if (ch->at_upstream) {
        if (ch->downstream)
            ch->downstream->w_ptr_event = 1;
        else if (ch->upstream)
            ch->upstream->r_ptr_event = 1;
    } else { /* At downstream */
        if (ch->upstream)
            ch->upstream->w_ptr_event = 1;
        else if (ch->downstream)
            ch->downstream->r_ptr_event = 1;
    }
}

static void ns_uring_clear_event_bit(struct ns_uring_ch *ch)
{
    if (ch->at_upstream) {
        if (ch->downstream)
            ch->downstream->w_ptr_event = 0;
        if (ch->upstream)
            ch->upstream->r_ptr_event = 0;
    } else { /* At downstream */
        if (ch->upstream)
            ch->upstream->w_ptr_event = 0;
        if (ch->downstream)
            ch->downstream->r_ptr_event = 0;
    }
}

void ns_uring_close_channel(struct ns_uring_ch *ch)
{
    ch->event_raised = true;
    ns_uring_set_event_bit(ch);
}

int ns_uring_send(struct ns_uring_ch *ch, void *msg, unsigned size)
{
    struct ns_uring *send_ring;
    void *entry;

    if (!ch || !msg)
        return -EINVAL;

    send_ring = ch->at_upstream ? ch->downstream : ch->upstream;

    // debug_print(KERN_DEBUG "ns_uring_send: pid = %d entry_size = %d ,msg size = %d\n", send_ring->pid, send_ring->entry_size, size);

    if (!send_ring)
        return -EINVAL;

    if (!ch->ignore_full && ns_uring_is_ring_full(send_ring))
        return -EAGAIN;

    if (size > send_ring->entry_size || size % 8)
        return -EINVAL;

    entry = ns_uring_get_next_entry(send_ring, send_ring->write_ptr);
    if (!entry)
        return -ENOENT;

    memcpy(entry, msg, size);

    send_ring->write_ptr = ns_uring_next_index(send_ring, send_ring->write_ptr);

    return 0;
}

uint8_t ns_uring_get_type(struct ns_uring_ch *ch)
{
    if (!ch)
        goto err_out;

    if (ch->downstream)
        return ch->downstream->type;

    if (ch->upstream)
        return ch->upstream->type;

err_out:
    return NS_URING_INVALID_TYPE;
}

uint8_t ns_uring_get_pid(struct ns_uring_ch *ch)
{
    if (!ch)
        goto err_out;

    if (ch->downstream)
        return ch->downstream->pid;

    if (ch->upstream)
        return ch->upstream->pid;

err_out:
    return NS_URING_INVALID_PID;
}

uint16_t ns_uring_get_upstream_entry_size(struct ns_uring_ch *ch)
{
    if (!ch || !ch->upstream)
        return 0;

    return ch->upstream->entry_size;
}

uint16_t ns_uring_get_downstream_entry_size(struct ns_uring_ch *ch)
{
    if (!ch || !ch->downstream)
        return 0;

    return ch->downstream->entry_size;
}

uint32_t ns_uring_get_upstream_ring_size(struct ns_uring_ch *ch)
{
    if (!ch || !ch->upstream)
        return 0;

    return ch->upstream->ring_size;
}

uint32_t ns_uring_get_downstream_ring_size(struct ns_uring_ch *ch)
{
    if (!ch || !ch->downstream)
        return 0;

    return ch->downstream->ring_size;
}

int ns_uring_register_upstream_cb(struct ns_uring_ch *ch, ns_uring_msg_cb *callback, void *context)
{
    if (!ch)
        return -EINVAL;

    if (ch->at_upstream) {
        ch->recv_cb = callback;
        ch->recv_ctx = context;
    } else {
        ch->send_compl_cb = callback;
        ch->send_compl_ctx = context;
    }

    return 0;
}

int ns_uring_register_downstream_cb(struct ns_uring_ch *ch, ns_uring_msg_cb *callback,
                                    void *context)
{
    if (!ch)
        return -EINVAL;

    if (ch->at_upstream) {
        ch->send_compl_cb = callback;
        ch->send_compl_ctx = context;
    } else {
        ch->recv_cb = callback;
        ch->recv_ctx = context;
    }

    return 0;
}

int ns_uring_register_event_cb(struct ns_uring_ch *ch, ns_uring_event_cb *callback, void *context)
{
    if (!ch)
        return -EINVAL;

    ch->event_cb = callback;
    ch->event_ctx = context;

    return 0;
}

static void ns_uring_handle_channel_close_event(struct ns_uring_ch *ch)
{
    ch->downstream = NULL;
    ch->upstream = NULL;
    ns_uring_clear_event_bit(ch);
}

static int ns_uring_handle_channel_event(struct ns_uring_ch *ch)
{
    ch->event_underway = true;

    if (ch->event_cb)
        ch->event_cb(ch->event_ctx);

    ns_uring_handle_channel_close_event(ch);

    return 0;
}

static int ns_uring_process_next_entry(struct ns_uring *ring, uint32_t idx, ns_uring_msg_cb *cb,
                                       void *ctx)
{
    void *entry;

    entry = ns_uring_get_next_entry(ring, ns_uring_real_ring_index(ring, idx));
    if (!entry)
        return -ENOENT;

    if (cb)
        cb(ctx, entry, ring->entry_size);

    return 0;
}

int ns_uring_process_upstream(struct ns_uring_ch *ch)
{
    ns_uring_msg_cb *cb;
    uint32_t idx;
    void *ctx;
    int err;

    if (!ch)
        return -EINVAL;

    if (!ch->upstream)
        return -ENOENT;

    if (ns_uring_channel_event_pending(ch) && !ch->event_underway)
        return ns_uring_handle_channel_event(ch);

    if (ch->at_upstream) {
        if (ns_uring_is_ring_empty(ch->upstream))
            return 0;
        idx = ch->upstream->read_ptr;
        cb = ch->recv_cb;
        ctx = ch->recv_ctx;
    } else {
        if (ch->ignore_full || ns_uring_is_ring_acked(ch->upstream, ch->send_ack_idx))
            return 0;
        idx = ch->send_ack_idx;
        cb = ch->send_compl_cb;
        ctx = ch->send_compl_ctx;
    }

    err = ns_uring_process_next_entry(ch->upstream, idx, cb, ctx);
    if (err)
        return err;
    idx = ns_uring_next_index(ch->upstream, idx);
    if (ch->at_upstream)
        ch->upstream->read_ptr = idx;
    else
        ch->send_ack_idx = idx;

    return 0;
}

int ns_uring_process_downstream(struct ns_uring_ch *ch)
{
    ns_uring_msg_cb *cb;
    uint32_t idx;
    void *ctx;
    int err;

    if (!ch)
        return -EINVAL;

    if (!ch->downstream)
        return -ENOENT;

    if (ns_uring_channel_event_pending(ch) && !ch->event_underway)
        return ns_uring_handle_channel_event(ch);

    if (ch->at_upstream) {
        if (ch->ignore_full || ns_uring_is_ring_acked(ch->downstream, ch->send_ack_idx))
            return 0;
        idx = ch->send_ack_idx;
        cb = ch->send_compl_cb;
        ctx = ch->send_compl_ctx;
    } else {
        if (ns_uring_is_ring_empty(ch->downstream))
            return 0;
        idx = ch->downstream->read_ptr;
        cb = ch->recv_cb;
        ctx = ch->recv_ctx;
    }

    err = ns_uring_process_next_entry(ch->downstream, idx, cb, ctx);
    if (err)
        return err;
    idx = ns_uring_next_index(ch->downstream, idx);
    if (ch->at_upstream)
        ch->send_ack_idx = idx;
    else
        ch->downstream->read_ptr = idx;

    return 0;
}

uint32_t ns_uring_messages_pending_upstream(struct ns_uring_ch *ch, bool *event_pending)
{
    uint32_t cnt;
    bool event;

    if (!ch || !ch->upstream)
        return 0;

    if (ch->at_upstream) {
        cnt = ns_uring_read_delta(ch->upstream);
        event = ch->upstream->w_ptr_event;
    } else {
        cnt = ns_uring_ack_delta(ch->upstream, ch->send_ack_idx);
        event = ch->upstream->r_ptr_event;
    }

    if (event_pending)
        *event_pending = event;

    return cnt;
}

uint32_t ns_uring_messages_pending_downstream(struct ns_uring_ch *ch, bool *event_pending)
{
    uint32_t cnt;
    bool event;

    if (!ch || !ch->downstream)
        return 0;

    if (ch->at_upstream) {
        cnt = ns_uring_ack_delta(ch->downstream, ch->send_ack_idx);
        event = ch->downstream->r_ptr_event;
    } else {
        cnt = ns_uring_read_delta(ch->downstream);
        event = ch->downstream->w_ptr_event;
    }

    if (event_pending)
        *event_pending = event;

    return cnt;
}

bool ns_uring_channel_event_pending(struct ns_uring_ch *ch)
{
    if (ch->event_underway)
        return false;

    if (ch->event_raised && ns_uring_raised_event_finished(ch))
        return true;

    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    if (ch->downstream && (ch->downstream->w_ptr_event || ch->downstream->r_ptr_event))
        return true;

    /* NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions) */
    if (ch->upstream && (ch->upstream->w_ptr_event || ch->upstream->r_ptr_event))
        return true;

    return false;
}
