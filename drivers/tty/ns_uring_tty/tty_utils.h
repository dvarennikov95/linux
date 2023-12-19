#ifndef __TTY_UTILS_H__
#define __TTY_UTILS_H__

#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/tty.h>

/*  ns uring tty driver has 2 modes (define at least one of them)
	ENDPOINT_MODE:
		1. ttyPcore is the only tty device created
		2. ttyPcore is used as primary kernel console (CONFIG_BOOTARGS=root=/dev/ram0 rw console=ttyPcore)
	SELF_HOSTED_MODE:
	    1. tty devices created for all cores (MAX_TTY_DEVICES)
		2. driver console is not registered (ttyPcore is not used)
*/
#define ENDPOINT_MODE
// #define SELF_HOSTED_MODE

/* driver log level operational(0) debug(1) */
#define DEBUG_PRINT 0 

#define CHAN_DYNAMICALY_ALLOCATED

#undef debug_print
#define debug_print(fmt, ...)                       \
	do {                                        \
		if (DEBUG_PRINT)                    \
			printk(fmt, ##__VA_ARGS__); \
	} while (0)

enum tty_dev_id_type {
	TTY_DEV_ECORE_D0_Q0_ID_START = 0,
	TTY_DEV_ECORE_D0_Q0_ID_END = 7,
	TTY_DEV_ECORE_D0_Q1_ID_START = 8,
	TTY_DEV_ECORE_D0_Q1_ID_END = 15,
	TTY_DEV_ECORE_D0_Q2_ID_START = 16,
	TTY_DEV_ECORE_D0_Q2_ID_END = 23,
	TTY_DEV_ECORE_D0_Q3_ID_START = 24,
	TTY_DEV_ECORE_D0_Q3_ID_END = 31,
	TTY_DEV_ECORE_D1_Q0_ID_START = 32,
	TTY_DEV_ECORE_D1_Q0_ID_END = 39,
	TTY_DEV_ECORE_D1_Q1_ID_START = 40,
	TTY_DEV_ECORE_D1_Q1_ID_END = 47,
	TTY_DEV_ECORE_D1_Q2_ID_START = 48,
	TTY_DEV_ECORE_D1_Q2_ID_END = 55,
	TTY_DEV_ECORE_D1_Q3_ID_START = 56,
	TTY_DEV_ECORE_D1_Q3_ID_END = 63,
	TTY_DEV_SCORE_D0_ID = 64,
	TTY_DEV_SCORE_D1_ID = 65,
	TTY_DEV_PCORE_ID = 66,
	MAX_TTY_DEVICES,
};

#ifdef SELF_HOSTED_MODE
#define NUM_TTY_DEVICES (MAX_TTY_DEVICES)
#elif defined(ENDPOINT_MODE)
#define NUM_TTY_DEVICES (1)
#define CONSOLE_TTY_PORT_ID (0)
#endif
#define INVALID_TTY_PORT_ID (NUM_TTY_DEVICES)

enum core_type {
	ECORE = 0,
	SCORE = 1,
	PCORE = 2,
	MAX_CORE_TYPES,
};

enum ecore_die {
	ECORE_D0 = 0,
	ECORE_D1 = 1,
	MAX_ECORE_DIES,
};

enum ecore_quad {
	ECORE_Q0 = 0,
	ECORE_Q1 = 1,
	ECORE_Q2 = 2,
	ECORE_Q3 = 3,
	MAX_ECORE_QUADS,
};

#define DEFAULT_ECORE_NS_URING_CHAN_BASE_ADDRESS (0x8C4383600000)
#define DEFAULT_ECORE_NS_URING_CHAN_BASE_ADDRESS_OFFSET (0x100000)
#define DEFAULT_PCORE_NS_URING_CHAN_BASE_ADDRESS (0x8DFFE0000000)
#define DEFAULT_SCORE_NS_URING_CHAN_BASE_ADDRESS (0x8DFFE0040000)
#define DEFAULT_ECORE_NS_URING_CHAN_RING_SIZE (0x4020)
#define DEFAULT_PCORE_NS_URING_CHAN_RING_SIZE (0x10020)
#define DEFAULT_SCORE_NS_URING_CHAN_RING_SIZE (0x4020)
#define DIE_ADDR_OFFSET (1UL << 41)
#define QUAD_ADDR_OFFSET (1UL << 39)
#define CORES_PER_QUAD 8
#define CORES_PER_DIE (MAX_ECORE_QUADS * CORES_PER_QUAD)

#define TTY_PORT_TYPE_FROM_ID(port_id)                           \
	({                                                       \
		int port_type;                                   \
		if ((port_id >= TTY_DEV_ECORE_D0_Q0_ID_START) && \
		    (port_id <= TTY_DEV_ECORE_D1_Q3_ID_END)) {   \
			port_type = ECORE;                       \
		} else if ((port_id == TTY_DEV_SCORE_D0_ID) ||   \
			   (port_id == TTY_DEV_SCORE_D1_ID)) {   \
			port_type = SCORE;                       \
		} else if (port_id == TTY_DEV_PCORE_ID) {        \
			port_type = PCORE;                       \
		} else {                                         \
			port_type = MAX_CORE_TYPES;              \
		}                                                \
		port_type;                                       \
	})

#define GET_ECORE_DIE_FROM_PORT_ID(port_id)                             \
	({                                                              \
		int die;                                                \
		if ((port_id >= TTY_DEV_ECORE_D0_Q0_ID_START) &&        \
		    (port_id <= TTY_DEV_ECORE_D0_Q3_ID_END)) {          \
			die = ECORE_D0;                                 \
		} else if ((port_id >= TTY_DEV_ECORE_D1_Q0_ID_START) && \
			   (port_id <= TTY_DEV_ECORE_D1_Q3_ID_END)) {   \
			die = ECORE_D1;                                 \
		} else {                                                \
			die = MAX_ECORE_DIES;                           \
		}                                                       \
		die;                                                    \
	})

#define GET_ECORE_QUAD_FROM_PORT_ID(port_id)                             \
	({                                                               \
		int quad;                                                \
		if (((port_id >= TTY_DEV_ECORE_D0_Q0_ID_START) &&        \
		     (port_id < TTY_DEV_ECORE_D0_Q0_ID_END)) ||          \
		    ((port_id >= TTY_DEV_ECORE_D1_Q0_ID_START) &&        \
		     (port_id < TTY_DEV_ECORE_D1_Q0_ID_END))) {          \
			quad = ECORE_Q0;                                 \
		} else if (((port_id >= TTY_DEV_ECORE_D0_Q1_ID_START) && \
			    (port_id < TTY_DEV_ECORE_D0_Q1_ID_END)) ||   \
			   ((port_id >= TTY_DEV_ECORE_D1_Q1_ID_START) && \
			    (port_id < TTY_DEV_ECORE_D1_Q1_ID_END))) {   \
			quad = ECORE_Q1;                                 \
		} else if (((port_id >= TTY_DEV_ECORE_D0_Q2_ID_START) && \
			    (port_id < TTY_DEV_ECORE_D0_Q2_ID_END)) ||   \
			   ((port_id >= TTY_DEV_ECORE_D1_Q2_ID_START) && \
			    (port_id < TTY_DEV_ECORE_D1_Q2_ID_END))) {   \
			quad = ECORE_Q2;                                 \
		} else if (((port_id >= TTY_DEV_ECORE_D0_Q3_ID_START) && \
			    (port_id < TTY_DEV_ECORE_D0_Q3_ID_END)) ||   \
			   ((port_id >= TTY_DEV_ECORE_D1_Q3_ID_START) && \
			    (port_id < TTY_DEV_ECORE_D1_Q3_ID_END))) {   \
			quad = ECORE_Q3;                                 \
		} else {                                                 \
			quad = MAX_ECORE_QUADS;                          \
		}                                                        \
		quad;                                                    \
	})

#define GET_ECORE_ID_FROM_PORT_ID(port_id)                                         \
	({                                                                         \
		int core_id;                                                       \
		core_id = port_id -                                                \
			  (GET_ECORE_DIE_FROM_PORT_ID(port_id) * CORES_PER_DIE) -  \
			  (GET_ECORE_QUAD_FROM_PORT_ID(port_id) * CORES_PER_QUAD); \
		core_id;                                                           \
	})

#endif