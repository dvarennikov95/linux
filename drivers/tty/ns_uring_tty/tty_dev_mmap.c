#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>
#include <linux/random.h>

#include "tty_dev_mmap.h"
#include "tty_chardev.h"


#define PCI_BAR0 0

#define P2A_CONFIG_REMAP_BASE_ADDR  0x4000

#define PCIE_BAR01_ADDR  0x6000

#define BAR_BITS_1G 30
#define DWIN_BITS 8
#define DWIN_OFFSET_BITS_1G (BAR_BITS_1G-DWIN_BITS) //in case of 1G size the range is [0:21]
#define DWIN_OFFSET_MASK_1G ((1 << DWIN_OFFSET_BITS_1G) - 1)

// bool is_pci_dev_initiated = false;
uint8_t __iomem *BAR0_VA = NULL; // the Virtual Address we map for the ns_uring

void tty_pci_map_dwin(int base_addr, int dwin_to_map)
{
	int dwin_cfg_addr;//offset
	volatile uint8_t *correct_address;//VA + offset

	/*calculation of the offset to write the base address*/
	dwin_cfg_addr = P2A_CONFIG_REMAP_BASE_ADDR + dwin_to_map * 4;

	correct_address = BAR0_VA + dwin_cfg_addr;

	debug_print(KERN_DEBUG "tty_pci_map_dwin: va_addr = 0x%px dwin = %d base_addr = 0x%x\n",correct_address, dwin_to_map, base_addr);
	iowrite32(base_addr, (void*)correct_address);
	debug_print(KERN_DEBUG "tty_pci_map_dwin: read addr = 0x%x\n", (uint32_t)ioread32((void*)correct_address));
}

int tty_pci_map_addr(uint64_t physical_address, int dwin_to_map)
{
	int base_addr;

	base_addr = physical_address >> DWIN_OFFSET_BITS_1G;

	/*write the base address*/
	tty_pci_map_dwin(base_addr, dwin_to_map);

	return ((dwin_to_map << DWIN_OFFSET_BITS_1G) | (physical_address & DWIN_OFFSET_MASK_1G));
}

uint8_t *tty_pci_get_address(uint64_t physical_address, int dwin_to_map)
{
	unsigned long dwin_offset = 0;

	dwin_offset = tty_pci_map_addr(physical_address, dwin_to_map);

	debug_print(KERN_DEBUG "tty_pci_get_address: physical_addr = 0x%llx dwin_offset = 0x%lx VA addr = 0x%px\n", physical_address, dwin_offset, BAR0_VA+dwin_offset);
	return (BAR0_VA + dwin_offset);
}


int tty_pci_device_init_check(struct pci_dev * p_pci_dev)
{

	int pcie_bar01_ref = 0;
	uint32_t read_val = 0;
	volatile uint8_t *read_address = NULL; //VA + offset
    pcie_bar01_ref = BAR_BITS_1G/*bar_size*/ | (1 << 16) /*bar_dwin_p2a_init_done*/;

	// if (is_pci_dev_initiated)
	// 	return 0;

	if (p_pci_dev == NULL)
    {
        printk(KERN_ERR "tty_pci_device_init_check: Unable to get pci device\n");
        return -ENODEV;
    }

    if (!pci_is_pcie(p_pci_dev)) {
		pci_err(p_pci_dev, KBUILD_MODNAME ": Device is not PCI Express\n");
		return -ENODEV;
	}

    if((p_pci_dev->current_state == PCI_UNKNOWN) || (p_pci_dev->current_state == PCI_POWER_ERROR))
    {
        printk(KERN_ERR "tty_pci_device_init_check: pci device invalid state\n");
        return -ENODEV;
    }

	// retrieve nschip pci device bar 0 virtual address
	BAR0_VA = tty_get_pci_bar_va(p_pci_dev, PCI_BAR0);
	if (BAR0_VA == NULL)
	{
	   debug_print(KERN_ERR "tty_pci_device_init_check: FAILED to get BAR0_VA\n");
       return -ENOMEM;
    }
	debug_print("tty_pci_device_init_check: PCI BAR 0 virtual address = 0x%p\n", BAR0_VA);

	// check if pci bar0 was initiated
	read_address = BAR0_VA + PCIE_BAR01_ADDR;//using dwin 0
	read_val = (uint32_t)ioread32((void*)read_address);

	if(pcie_bar01_ref != read_val)
	{
		printk("tty_pci_device_init_check: ERROR: pcie_bar01=0x%x value is wrong! \n", read_val);
		//in case the bar is not initialized 
		return -EINVAL;
	}
	debug_print(KERN_DEBUG "tty_pci_device_init_check: bar init val = 0x%x is correct!",read_val);

	// is_pci_dev_initiated = true;

	return 0;
}

uint8_t *tty_get_pci_bar_va(struct pci_dev *dev, int bar_num)
{
	unsigned long mmio_start, mmio_len;

    /* Get bar start and stop memory offsets */
    mmio_start = pci_resource_start(dev, bar_num);
    mmio_len = pci_resource_len(dev, bar_num);

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

bool tty_port_core_mem_access_check(uint32_t port_id, unsigned long *test_addr)
{
	unsigned long orig_val, read_val;
	unsigned long test_val = 0xDEADBEAFDEADBEAF;

	debug_print(KERN_DEBUG "tty_port_core_mem_access_check: PCORE read addr (0x%lx)\n", &test_addr);
	orig_val = (unsigned long)*test_addr;
	debug_print(KERN_DEBUG "original val (%lx)\n", orig_val);
	*test_addr = test_val;
	read_val = *test_addr;
	debug_print(KERN_DEBUG "set test val (%lx)\n", read_val);
	*test_addr = orig_val;
	debug_print(KERN_DEBUG "restore original val (%lx)\n", *test_addr);

	return (read_val == test_val);
}
