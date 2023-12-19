#ifndef __TTY_DEV_MMAP_H__
#define __TTY_DEV_MMAP_H__

#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>

#include "tty_chardev.h"

/**
 * @brief remap section in memory and return pointer 
 * 
 * @param address memory section start physical address
 * @param size  memory section size 
 * @return  virtual address pointer to new mapped area
 * 
 */
uint8_t *get_address_mapping(unsigned long address, unsigned long size);


#endif