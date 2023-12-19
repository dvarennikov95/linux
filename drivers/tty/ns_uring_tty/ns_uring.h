#ifndef __NS_URING_H__
#define __NS_URING_H__
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



#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include "tty_utils.h"

#define NS_URING_INVALID_PID (0xFF)
#define NS_URING_INVALID_TYPE (0xFF)
#define NS_URING_RESERVED_WORDS_COUNT (4)

typedef void ns_uring_msg_cb(void *context, void *msg, unsigned size);
typedef void ns_uring_event_cb(void *context);

struct ns_uring_ch {
    struct ns_uring *downstream;
    struct ns_uring *upstream;
    ns_uring_msg_cb *recv_cb;
    ns_uring_msg_cb *send_compl_cb;
    ns_uring_event_cb *event_cb;
    void *recv_ctx;
    void *send_compl_ctx;
    void *event_ctx;
    uint32_t send_ack_idx;
    bool at_upstream;
    bool event_underway;
    bool event_raised;
    bool ignore_full;
};

/* The metadata of a ring. The definition is exposed so that the upstream
 * side of a channel knows how much of memory to allocate before the actual
 * ring of messages (the "entries").
 */
struct ns_uring {
    uint32_t write_ptr : 31;
    uint32_t w_ptr_event : 1;
    /* word */
    uint32_t read_ptr : 31;
    uint32_t r_ptr_event : 1;
    /* word */
    uint32_t ring_size : 30;
    uint32_t never_full : 1;
    uint32_t rsvd_opts : 1;
    /* word */
    uint8_t pid;
    uint8_t type;
    uint16_t entry_size;
    /* word */
    uint32_t reserved[NS_URING_RESERVED_WORDS_COUNT];
    uint8_t entries[0];
};

struct ns_uring_msg_hdr {
    uint32_t subtype : 8; /* Used on channels that have several entry types */
    uint32_t reserved : 24;
    /* word */
    uint32_t error_code; /* Zero is no error */
};

/**
 * @brief This function creates a ns_uring channel, out of the provided ring
 *		buffers and related parameters. This function is to be called on
 *		upstream side of the channel, and the metadata of the ring buffer
 *		will be filled with the information provided as parameters.
 *
 * @param prealloc_ch Pointer to a preallocated channel struct. If NULL, memory will be allocated
 *instead.
 * @param ring_up Pointer to the upstream ring
 * @param ring_down Pointer to the downstream ring
 * @param up_ring_size Upstream ring size (in bytes)
 * @param up_entry_size Upstream entry size (in bytes)
 * @param down_ring_size Downstream ring size (in bytes)
 * @param down_entry_size Downstream entry size (in bytes)
 * @param type The type of entries on this channel
 * @param pid The PID assigned for this channel. In case there are several
 *			channels with the same entry type, the PID identifies the correct
 *			channel. Mainly used for several injection channels, returning
 *			thread TID providing the PID of the channel.
 * @param ignore_full Whether the channel should be used in a never-full mode,
 *					where overwriting old, unread entries is allowed.
 * @return Pointer to the allocated channel structure, or NULL in case of error
 */
struct ns_uring_ch *ns_uring_create_channel_upstream(struct ns_uring_ch *prealloc_ch, void *ring_up,
                                                     void *ring_down, uint32_t up_ring_size,
                                                     uint16_t up_entry_size,
                                                     uint32_t down_ring_size,
                                                     uint16_t down_entry_size, uint8_t type,
                                                     uint8_t pid, bool ignore_full);

/**
 * @brief This function creates a ns_uring channel, out of the provided pointers
 *		to the ring(s) of the channel. The rings have been initialized by the
 *		upstream and thus their metadata will be used to create the local
 *		channel structure at downstream of the link. This function should be
 *		called by the downstream side of the link.
 *
 * @param prealloc_ch Pointer to a preallocated channel struct. If NULL, memory will be allocated
 *instead.
 * @param ring_up Pointer to the upstream ring
 * @param ring_down Pointer to the downstream ring
 * @return Pointer to the allocated channel structure, or NULL in case of error
 */
struct ns_uring_ch *ns_uring_create_channel_downstream(struct ns_uring_ch *prealloc_ch,
                                                       void *ring_up, void *ring_down);

/**
 * @brief Destroys the channel object, and frees the channel structure.
 *
 * @param ch Pointer to the channel structure
 */
void ns_uring_destroy_channel(struct ns_uring_ch *ch);

/**
 * @brief Closes the channel.
 *
 * @param ch Pointer to the channel structure
 */
void ns_uring_close_channel(struct ns_uring_ch *ch);

/**
 * @brief Sends the provided message on the specified channel.
 *
 * @param ch Pointer to the channel structure
 * @param msg Pointer to the message to be sent
 * @param size Size of the message buffer, pointed to by msg
 * @return The error code, or zero for no error
 */
int ns_uring_send(struct ns_uring_ch *ch, void *msg, unsigned size);

/**
 * @brief Gets the type of entries on the specified channel.
 *
 * @param ch Pointer to the channel structure
 * @return The type of the channel
 */
uint8_t ns_uring_get_type(struct ns_uring_ch *ch);

/**
 * @brief Gets the PID of the specified channel.
 *
 * @param ch Pointer to the channel structure
 * @return The PID of the channel
 */
uint8_t ns_uring_get_pid(struct ns_uring_ch *ch);

/**
 * @brief Gets the entry size of the upstream ring in bytes.
 *
 * @param ch Pointer to the channel structure
 * @return The entry size on the upstream ring
 */
uint16_t ns_uring_get_upstream_entry_size(struct ns_uring_ch *ch);

/**
 * @brief Gets the entry size of the downstream ring in bytes.
 *
 * @param ch Pointer to the channel structure
 * @return The entry size on the downstream ring
 */
uint16_t ns_uring_get_downstream_entry_size(struct ns_uring_ch *ch);

/**
 * @brief Gets the ring size of the upstream ring in bytes.
 *
 * @param ch Pointer to the channel structure
 * @return The ring size of the upstream ring
 */
uint32_t ns_uring_get_upstream_ring_size(struct ns_uring_ch *ch);

/**
 * @brief Gets the ring size of the downstream ring in bytes.
 *
 * @param ch Pointer to the channel structure
 * @return The ring size of the downstream ring
 */
uint32_t ns_uring_get_downstream_ring_size(struct ns_uring_ch *ch);

/**
 * @brief Register callback for pending messages on the upstream ring.
 *
 * @param ch Pointer to the channel structure
 * @param callback The callback to be called for each pending message
 * @param context The context to be passed to the callback
 * @return The error code, or zero for no error
 */
int ns_uring_register_upstream_cb(struct ns_uring_ch *ch, ns_uring_msg_cb *callback, void *context);

/**
 * @brief Register callback for pending messages on the downstream ring.
 *
 * @param ch Pointer to the channel structure
 * @param callback The callback to be called for each pending message
 * @param context The context to be passed to the callback
 * @return The error code, or zero for no error
 */
int ns_uring_register_downstream_cb(struct ns_uring_ch *ch, ns_uring_msg_cb *callback,
                                    void *context);

/**
 * @brief Register callback for channel events (channel close).
 *
 * @param ch Pointer to the channel structure
 * @param callback The callback to be called for a raised event
 * @param context The context to be passed to the callback
 * @return The error code, or zero for no error
 */
int ns_uring_register_event_cb(struct ns_uring_ch *ch, ns_uring_event_cb *callback, void *context);

/**
 * @brief Process the upstream ring of the channel. If there are any pending
 *		messages, then call the registered callback.
 *
 * @param ch Pointer to the channel structure
 * @return The error code, or zero for no error
 */
int ns_uring_process_upstream(struct ns_uring_ch *ch);

/**
 * @brief Process the downstream ring of the channel. If there are any pending
 *		messages, then call the registered callback.
 *
 * @param ch Pointer to the channel structure
 * @return The error code, or zero for no error
 */
int ns_uring_process_downstream(struct ns_uring_ch *ch);

/**
 * @brief Check the number of pending messages on the upstream ring of the channel.
 *
 * @param ch Pointer to the channel structure
 * @param event_pending Pointer to a boolean that if provided, will return true
 *					  if any events are pending
 * @return The number of pending messages
 */
uint32_t ns_uring_messages_pending_upstream(struct ns_uring_ch *ch, bool *event_pending);

/**
 * @brief Check the number of pending messages on the upstream ring of the channel.
 *
 * @param ch Pointer to the channel structure
 * @param event_pending Pointer to a boolean that if provided, will return true
 *					  if any events are pending
 * @return The number of pending messages
 */
uint32_t ns_uring_messages_pending_downstream(struct ns_uring_ch *ch, bool *event_pending);

/**
 * @brief Check if there are any pending events on the channel.
 *
 * @param ch Pointer to the channel structure
 * @return The whether an event is pending or not
 */
bool ns_uring_channel_event_pending(struct ns_uring_ch *ch);

#endif /* __NS_URING_H__ */
