#ifndef __TTY_DEV_MMAP_H__
#define __TTY_DEV_MMAP_H__

#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/pci.h>

#include "tty_chardev.h"

/**
 * @brief in this function we save the base address in memory
 * 
 * @param base_addr  the physical address with shift (according to the memory size chunk)
 * @param dwin_to_map  the number of the dwin
 * @param virtual_address  a pointer to the virtual address
 * 
 */
void tty_pci_map_dwin(int base_addr, int dwin_to_map);


/**
 * @brief in this function we return the offset to the correct address
 * 
 * @param addr  the physical address
 * @param dwin_to_map  the number of the dwin
 * @param virtual_address  a pointer to the virtual address
 * 
 */
int tty_pci_map_addr(uint64_t addr, int dwin_to_map);

/**
 * @brief in this function we check if pci device was initiated
 * 
 * @param virtual_address  a pointer to the virtual address
 * 
 */
int tty_pci_device_init_check(struct pci_dev *p_pci_dev);

/**
 * @brief in this function we get the currect addres which we can read/write from/to
 * 
 * @param address  the physical address
 * @param dwin_to_map  the number of the dwin
 * @param virtual_address  a pointer to the virtual address
 * 
 */
uint8_t *tty_pci_get_address(uint64_t address, int dwin_to_map);

/**
 * @brief in this function we return the base virtual address of the mapped pci bar
 * 
 * @param dev  a pointer to the device srtruct
 * @param bar_num  the nunber of the BAR (memory area)
 * 
 */
uint8_t *tty_get_pci_bar_va(struct pci_dev *dev, int bar_num);

/**
 * @brief function checks if port's target core memory can be accessed
 * 
 * @param port_id index of the target port translation from tty_core_id_type
 * @return true for success false for failure
 * 
 */
bool tty_port_core_mem_access_check(uint32_t port_id, unsigned long *test_addr);

#endif
