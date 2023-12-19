#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>
#include <linux/random.h>
#include <linux/io.h>

#include "tty_dev_mmap.h"

uint8_t *get_address_mapping(unsigned long address, unsigned long size)
{
    unsigned long mmio_start, mmio_len;

    mmio_start = address;
    mmio_len = size;

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
