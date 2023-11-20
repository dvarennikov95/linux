#ifndef __TTY_UTILS_H__
#define __TTY_UTILS_H__

#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/types.h>

#define NS_URING_CHAN_INIT_IS_DOWNSTREAM 0

#if (NS_URING_CHAN_INIT_IS_DOWNSTREAM == 1)
enum tty_core_id_type {
    TTY_ECORE_ID_START = 0,
    TTY_ECORE_ID_END = 31,
    TTY_SCORE_ID = 32,
    TTY_PCORE_ID = 33,
    MAX_TTY_DEVICES, 
};
#else // UPSTREAM only 1 channel can be configured
#define MAX_TTY_DEVICES 1
#endif

#define GET_DWIN_INDEX(index)               (10 + index * 2)
#define GET_DOWNSTREAM_DWIN_INDEX(index)    (GET_DWIN_INDEX(index) + 1)
#define GET_UPSTREAM_DWIN_INDEX(index)      (GET_DWIN_INDEX(index))

#define DEBUG_PRINT 1

#undef debug_print
#define debug_print(fmt, ...) \
            do { if (DEBUG_PRINT) printk(fmt, ##__VA_ARGS__); } while (0)

#endif
