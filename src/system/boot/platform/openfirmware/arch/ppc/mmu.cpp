/*
 * Copyright 2003-2009, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2010-2011, Haiku, Inc. All Rights Reserved.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de.
 *		Alexander von Gluck, kallisti5@unixzen.com
 */


#include <OS.h>

#include <stdarg.h>
#include <string.h>

#include <platform_arch.h>
#include <boot/addr_range.h>
#include <boot/kernel_args.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <platform/openfirmware/openfirmware.h>
#include <arch_cpu.h>
#include <arch_mmu.h>
#include <kernel.h>

#include "support.h"


// set protection to WIMGNPP: -----PP
// PP:	00 - no access
//		01 - read only
//		10 - read/write
//		11 - read only
#define PAGE_READ_ONLY	0x01
#define PAGE_READ_WRITE	0x02

// NULL is actually a possible physical address...
//#define PHYSINVAL ((void *)-1)
#define PHYSINVAL NULL

//#define TRACE_MMU
#ifdef TRACE_MMU
#   define TRACE(x...) dprintf(x)
#else
#   define TRACE(x...) ;
#endif


segment_descriptor sSegments[16];
page_table_entry_group *sPageTable;
uint32 sPageTableHashMask;


// begin and end of the boot loader
extern "C" uint8 __text_begin;
extern "C" uint8 _end;


static status_t
insert_virtual_range_to_keep(void *start, uint32 size)
{
	return insert_address_range(gKernelArgs.arch_args.virtual_ranges_to_keep,
		&gKernelArgs.arch_args.num_virtual_ranges_to_keep,
		MAX_VIRTUAL_RANGES_TO_KEEP, (addr_t)start, size);
}


static status_t
remove_virtual_range_to_keep(void *start, uint32 size)
{
	return remove_address_range(gKernelArgs.arch_args.virtual_ranges_to_keep,
		&gKernelArgs.arch_args.num_virtual_ranges_to_keep,
		MAX_VIRTUAL_RANGES_TO_KEEP, (addr_t)start, size);
}


static status_t
find_physical_memory_ranges(size_t &total)
{
	int memory;
	dprintf("checking for memory...\n");
	if (of_getprop(gChosen, "memory", &memory, sizeof(int)) == OF_FAILED)
		return B_ERROR;
	int package = of_instance_to_package(memory);

	total = 0;

	// Memory base addresses are provided in 32 or 64 bit flavors
	// #address-cells and #size-cells matches the number of 32-bit 'cells'
	// representing the length of the base address and size fields
	int root = of_finddevice("/");
	int32 regAddressCells = of_address_cells(root);
	int32 regSizeCells = of_size_cells(root);
	if (regAddressCells == OF_FAILED || regSizeCells == OF_FAILED) {
		dprintf("finding base/size length counts failed, assume 32-bit.\n");
		regAddressCells = 1;
		regSizeCells = 1;
	}

	// NOTE : Size Cells of 2 is possible in theory... but I haven't seen it yet.
	if (regAddressCells > 2 || regSizeCells > 1) {
		panic("%s: Unsupported OpenFirmware cell count detected.\n"
		"Address Cells: %" B_PRId32 "; Size Cells: %" B_PRId32
		" (CPU > 64bit?).\n", __func__, regAddressCells, regSizeCells);
		return B_ERROR;
	}

	// On 64-bit PowerPC systems (G5), our mem base range address is larger
	if (regAddressCells == 2) {
		struct of_region<uint64, uint32> regions[64];
		int count = of_getprop(package, "reg", regions, sizeof(regions));
		if (count == OF_FAILED)
			count = of_getprop(memory, "reg", regions, sizeof(regions));
		if (count == OF_FAILED)
			return B_ERROR;
		count /= sizeof(regions[0]);

		for (int32 i = 0; i < count; i++) {
			if (regions[i].size <= 0) {
				dprintf("%ld: empty region\n", i);
				continue;
			}
			dprintf("%" B_PRIu32 ": base = %" B_PRIu64 ","
				"size = %" B_PRIu32 "\n", i, regions[i].base, regions[i].size);

			total += regions[i].size;

			if (insert_physical_memory_range((addr_t)regions[i].base,
					regions[i].size) != B_OK) {
				dprintf("cannot map physical memory range "
					"(num ranges = %" B_PRIu32 ")!\n",
					gKernelArgs.num_physical_memory_ranges);
				return B_ERROR;
			}
		}
		return B_OK;
	}

	// Otherwise, normal 32-bit PowerPC G3 or G4 have a smaller 32-bit one
	struct of_region<uint32, uint32> regions[64];
	int count = of_getprop(package, "reg", regions, sizeof(regions));
	if (count == OF_FAILED)
		count = of_getprop(memory, "reg", regions, sizeof(regions));
	if (count == OF_FAILED)
		return B_ERROR;
	count /= sizeof(regions[0]);

	for (int32 i = 0; i < count; i++) {
		if (regions[i].size <= 0) {
			dprintf("%ld: empty region\n", i);
			continue;
		}
		dprintf("%" B_PRIu32 ": base = %" B_PRIu32 ","
			"size = %" B_PRIu32 "\n", i, regions[i].base, regions[i].size);

		total += regions[i].size;

		if (insert_physical_memory_range((addr_t)regions[i].base,
				regions[i].size) != B_OK) {
			dprintf("cannot map physical memory range "
				"(num ranges = %" B_PRIu32 ")!\n",
				gKernelArgs.num_physical_memory_ranges);
			return B_ERROR;
		}
	}

	return B_OK;
}


static bool
is_virtual_allocated(void *address, size_t size)
{
	uint64 foundBase;
	return !get_free_address_range(gKernelArgs.virtual_allocated_range,
		gKernelArgs.num_virtual_allocated_ranges, (addr_t)address, size,
		&foundBase) || foundBase != (addr_t)address;
}


static bool
is_physical_allocated(void *address, size_t size)
{
	uint64 foundBase;
	return !get_free_address_range(gKernelArgs.physical_allocated_range,
		gKernelArgs.num_physical_allocated_ranges, (addr_t)address, size,
		&foundBase) || foundBase != (addr_t)address;
}


static bool
is_physical_memory(void *address, size_t size)
{
	return is_address_range_covered(gKernelArgs.physical_memory_range,
		gKernelArgs.num_physical_memory_ranges, (addr_t)address, size);
}


static bool
is_physical_memory(void *address)
{
	return is_physical_memory(address, 1);
}


static void
fill_page_table_entry(page_table_entry *entry, uint32 virtualSegmentID,
	void *virtualAddress, void *physicalAddress, uint8 mode, bool secondaryHash)
{
	// lower 32 bit - set at once
	((uint32 *)entry)[1]
		= (((uint32)physicalAddress / B_PAGE_SIZE) << 12) | mode;
	/*entry->physical_page_number = (uint32)physicalAddress / B_PAGE_SIZE;
	entry->_reserved0 = 0;
	entry->referenced = false;
	entry->changed = false;
	entry->write_through = (mode >> 6) & 1;
	entry->caching_inhibited = (mode >> 5) & 1;
	entry->memory_coherent = (mode >> 4) & 1;
	entry->guarded = (mode >> 3) & 1;
	entry->_reserved1 = 0;
	entry->page_protection = mode & 0x3;*/
	eieio();
		// we need to make sure that the lower 32 bit were
		// already written when the entry becomes valid

	// upper 32 bit
	entry->virtual_segment_id = virtualSegmentID;
	entry->secondary_hash = secondaryHash;
	entry->abbr_page_index = ((uint32)virtualAddress >> 22) & 0x3f;
	entry->valid = true;
}


/*!	Looks up the valid page table entry mapping \a virtualAddress under
	\a virtualSegmentID, searching both the primary and the secondary hash
	group. Returns NULL if the address is not currently mapped.
*/
static page_table_entry *
lookup_page_table_entry(uint32 virtualSegmentID, void *virtualAddress)
{
	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID,
		(uint32)virtualAddress);
	uint32 api = ((uint32)virtualAddress >> 22) & 0x3f;

	page_table_entry_group *group = &sPageTable[hash & sPageTableHashMask];
	for (int32 i = 0; i < 8; i++) {
		page_table_entry &entry = group->entry[i];
		if (entry.valid && !entry.secondary_hash
			&& entry.virtual_segment_id == virtualSegmentID
			&& entry.abbr_page_index == api)
			return &entry;
	}

	group = &sPageTable[page_table_entry::SecondaryHash(hash)
		& sPageTableHashMask];
	for (int32 i = 0; i < 8; i++) {
		page_table_entry &entry = group->entry[i];
		if (entry.valid && entry.secondary_hash
			&& entry.virtual_segment_id == virtualSegmentID
			&& entry.abbr_page_index == api)
			return &entry;
	}

	return NULL;
}


/*!	Checks whether any page in the given virtual range already has a valid
	page table entry. If so, returns true and sets \a _firstMappedPage to
	the first such page. The firmware faults device mappings into the (now
	shared) page table lazily, so ranges that look free in
	virtual_allocated_range may still contain live firmware mappings.
*/
static bool
is_virtual_range_mapped_in_page_table(void *virtualAddress, size_t size,
	void **_firstMappedPage)
{
	addr_t base = ROUNDDOWN((addr_t)virtualAddress, B_PAGE_SIZE);
	for (addr_t va = base; va < (addr_t)virtualAddress + size;
			va += B_PAGE_SIZE) {
		uint32 virtualSegmentID = sSegments[va >> 28].virtual_segment_id;
		if (lookup_page_table_entry(virtualSegmentID, (void *)va) != NULL) {
			*_firstMappedPage = (void *)va;
			return true;
		}
	}
	return false;
}


static void
map_page(void *virtualAddress, void *physicalAddress, uint8 mode)
{
	uint32 virtualSegmentID
		= sSegments[addr_t(virtualAddress) >> 28].virtual_segment_id;

	// If a valid PTE for this exact VA already exists, replace it in place.
	// Inserting a second valid PTE for the same VA instead makes it ambiguous
	// which entry the MMU uses - observed on QEMU/OpenBIOS, where preserved
	// identity mappings of the mac-io device space at 0x80000000 shadowed
	// freshly mapped kernel pages, silently redirecting kernel image data
	// into device memory.
	page_table_entry *existing = lookup_page_table_entry(virtualSegmentID,
		virtualAddress);
	if (existing != NULL) {
		if (existing->physical_page_number
				!= (uint32)physicalAddress / B_PAGE_SIZE) {
			dprintf("map_page: replacing existing mapping for va %p "
				"(-> pa 0x%x) with pa %p\n", virtualAddress,
				(unsigned int)(existing->physical_page_number * B_PAGE_SIZE),
				physicalAddress);
		}
		bool secondaryHash = existing->secondary_hash;
		existing->valid = false;
		ppc_sync();
		asm volatile("tlbie %0" : : "r" (virtualAddress));
		eieio();
		tlbsync();
		ppc_sync();
		fill_page_table_entry(existing, virtualSegmentID, virtualAddress,
			physicalAddress, mode, secondaryHash);
		return;
	}

	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID,
		(uint32)virtualAddress);
	page_table_entry_group *group = &sPageTable[hash & sPageTableHashMask];

	for (int32 i = 0; i < 8; i++) {
		// 8 entries in a group
		if (group->entry[i].valid)
			continue;

		fill_page_table_entry(&group->entry[i], virtualSegmentID,
			virtualAddress, physicalAddress, mode, false);
		//TRACE("map: va = %p -> %p, mode = %d, hash = %lu\n",
		//	virtualAddress, physicalAddress, mode, hash);
		return;
	}

	hash = page_table_entry::SecondaryHash(hash);
	group = &sPageTable[hash & sPageTableHashMask];

	for (int32 i = 0; i < 8; i++) {
		if (group->entry[i].valid)
			continue;

		fill_page_table_entry(&group->entry[i], virtualSegmentID,
			virtualAddress, physicalAddress, mode, true);
		//TRACE("map: va = %p -> %p, mode = %d, second hash = %lu\n",
		//	virtualAddress, physicalAddress, mode, hash);
		return;
	}

	panic("%s: out of page table entries!\n", __func__);
}


/*!	Reserves the CPU-visible memory ranges of all PCI devices (their
	assigned BARs) as allocated virtual address space.

	The firmware accesses its devices through identity mappings it faults
	into the page table lazily, at any time. Any kernel/loader allocation
	overlapping such a range would shadow the device once mapped, silently
	redirecting firmware device accesses into RAM (or vice versa). On
	QEMU/OpenBIOS the PCI BARs (mac-io at 0x80000000, OpenPIC at 0x80040000,
	VRAM at 0x81000000, ...) sit exactly at KERNEL_BASE, so this is not a
	theoretical concern.
*/
static void
reserve_pci_device_ranges()
{
	intptr_t root = of_peer(0);
	if (root == OF_FAILED || root == 0)
		return;

	for (intptr_t bridge = of_child(root);
			bridge != 0 && bridge != OF_FAILED; bridge = of_peer(bridge)) {
		char type[16];
		memset(type, 0, sizeof(type));
		int typeLength = of_getprop(bridge, "device_type", type,
			sizeof(type) - 1);
		if (typeLength <= 0 || strcmp(type, "pci") != 0)
			continue;

		for (intptr_t device = of_child(bridge);
				device != 0 && device != OF_FAILED;
				device = of_peer(device)) {
			uint32 assigned[60];
			int length = of_getprop(device, "assigned-addresses", assigned,
				sizeof(assigned));
			if (length <= 0)
				continue;

			int count = length / sizeof(uint32);
			for (int i = 0; i + 5 <= count; i += 5) {
				// Each entry: phys-hi, phys-mid, phys-lo, size-hi, size-lo.
				// Only reserve memory space BARs (ss bits 0b10/0b11 in
				// phys-hi); I/O space lives behind a separate window.
				if (((assigned[i] >> 24) & 3) < 2)
					continue;
				uint32 base = assigned[i + 2];
				uint32 size = assigned[i + 4];
				if (base == 0 || size == 0)
					continue;

				addr_t start = ROUNDDOWN(base, B_PAGE_SIZE);
				addr_t end = ROUNDUP((addr_t)base + size, B_PAGE_SIZE);
				dprintf("reserving PCI device range %p - %p\n", (void *)start,
					(void *)end);
				insert_virtual_allocated_range(start, end - start);
			}
		}
	}
}


static void
map_range(void *virtualAddress, void *physicalAddress, size_t size, uint8 mode)
{
	for (uint32 offset = 0; offset < size; offset += B_PAGE_SIZE) {
		map_page((void *)(uint32(virtualAddress) + offset),
			(void *)(uint32(physicalAddress) + offset), mode);
	}
}


static status_t
find_allocated_ranges(void *oldPageTable, void *pageTable,
	page_table_entry_group **_physicalPageTable, void **_exceptionHandlers)
{
	// we have to preserve the OpenFirmware established mappings
	// if we want to continue to use its service after we've
	// taken over (we will probably need less translations once
	// we have proper driver support for the target hardware).
	int mmu;
	if (of_getprop(gChosen, "mmu", &mmu, sizeof(int)) == OF_FAILED) {
		dprintf("%s: Error: no OpenFirmware mmu\n", __func__);
		return B_ERROR;
	}
	mmu = of_instance_to_package(mmu);

	struct translation_map {
		void	*virtual_address;
		int		length;
		void	*physical_address;
		int		mode;
	} translations[64];

	int length = of_getprop(mmu, "translations", &translations,
		sizeof(translations));
	if (length == OF_FAILED) {
		dprintf("Error: no OF translations.\n");
		return B_ERROR;
	}
	length = length / sizeof(struct translation_map);
	uint32 total = 0;
	dprintf("found %d translations\n", length);

	{
		void *here = (void *)&find_allocated_ranges;
		segment_descriptor sr = ppc_get_segment_register(here);
		dprintf("TRACE: our code at %p, current SR.VSID = %lu\n", here,
			(unsigned long)sr.virtual_segment_id);
	}

	for (int i = 0; i < length; i++) {
		struct translation_map *map = &translations[i];
		bool keepRange = true;
		dprintf("%i: map: %p, length %d -> physical: %p, mode %d\n", i,
			map->virtual_address, map->length,
			map->physical_address, map->mode);

		// insert range in physical allocated, if it points to physical memory

		if (is_physical_memory(map->physical_address)
			&& insert_physical_allocated_range((addr_t)map->physical_address,
				map->length) != B_OK) {
			dprintf("cannot map physical allocated range "
				"(num ranges = %" B_PRIu32 ")!\n",
				gKernelArgs.num_physical_allocated_ranges);
			return B_ERROR;
		}

		if (map->virtual_address == pageTable) {
			dprintf("%i: found page table at va %p\n", i,
				map->virtual_address);
			*_physicalPageTable
				= (page_table_entry_group *)map->physical_address;
			keepRange = false;
				// we keep it explicitely anyway
		}
		if ((addr_t)map->physical_address <= 0x100
			&& (addr_t)map->physical_address + map->length >= 0x1000) {
			dprintf("%i: found exception handlers at va %p\n", i,
				map->virtual_address);
			*_exceptionHandlers = map->virtual_address;
			keepRange = false;
				// we keep it explicitely anyway
		}
		if (map->virtual_address == oldPageTable)
			keepRange = false;

		// insert range in virtual allocated

		if (insert_virtual_allocated_range((addr_t)map->virtual_address,
				map->length) != B_OK) {
			dprintf("cannot map virtual allocated range "
				"(num ranges = %" B_PRIu32 ")!\n",
				gKernelArgs.num_virtual_allocated_ranges);
		}

		// map range into the page table

		map_range(map->virtual_address, map->physical_address, map->length,
			map->mode);

		// insert range in virtual ranges to keep

		if (keepRange) {
			TRACE("%i: keeping free range starting at va %p\n", i,
				map->virtual_address);

			if (insert_virtual_range_to_keep(map->virtual_address,
					map->length) != B_OK) {
				dprintf("cannot map virtual range to keep "
					"(num ranges = %" B_PRIu32 ")\n",
					gKernelArgs.num_virtual_allocated_ranges);
			}
		}

		total += map->length;
	}
	dprintf("total size kept: %" B_PRIu32 "\n", total);

	// remove the boot loader code from the virtual ranges to keep in the
	// kernel
	if (remove_virtual_range_to_keep(&__text_begin, &_end - &__text_begin)
			!= B_OK) {
		dprintf("%s: Failed to remove boot loader range "
			"from virtual ranges to keep.\n", __func__);
	}

	return B_OK;
}


/*!	Computes the recommended minimal page table size as
	described in table 7-22 of the PowerPC "Programming
	Environment for 32-Bit Microprocessors".
	The page table size ranges from 64 kB (for 8 MB RAM)
	to 32 MB (for 4 GB RAM).
*/
static size_t
suggested_page_table_size(size_t total)
{
	uint32 max = 23;
		// 2^23 == 8 MB

	while (max < 32) {
		if (total <= (1UL << max))
			break;

		max++;
	}

	// Give the hashed page table 4x the classic RAM/128 rule of thumb: a full
	// multi-process Haiku desktop (launch_daemon + registrar + net_server +
	// media_server + app_server + Tracker + ... each mapping libbe/libroot and
	// their own heaps/stacks) overflows an individual PTEG at RAM/128, since
	// Map() panics on a full PTEG rather than evicting. RAM/32 keeps every
	// bucket comfortably below the 16-slot limit and stays within the PPC
	// architectural 32 MB page-table ceiling.
	size_t size = 1UL << (max - 5);
		// RAM/32

	// HARD CAP at 32 MB: the classic PPC hashed page table is limited to 32 MB
	// by SDR1's 9-bit HTABMASK ((0x1ff + 1) * 64 KB = 32 MB). Requesting more
	// (>1 GB RAM -> this formula yields 64 MB) cannot be encoded in SDR1 - the
	// hardware ends up using a SMALLER table than fPageTableHashMask assumes, so
	// Map() inserts PTEs into groups the CPU's hash walk never checks. The
	// result is random "PTE not found" DSI panics under memory pressure (seen on
	// a 1.25 GB PowerBook G4 as a slab-allocator/create_pipe crash, and as USB
	// I/O hard-locks). The comment here previously claimed this cap but the code
	// never applied it.
	if (size > 32UL * 1024 * 1024)
		size = 32UL * 1024 * 1024;

	return size;
}


static void *
find_physical_memory_range(size_t size)
{
	for (uint32 i = 0; i < gKernelArgs.num_physical_memory_ranges; i++) {
		if (gKernelArgs.physical_memory_range[i].size > size)
			return (void *)(addr_t)gKernelArgs.physical_memory_range[i].start;
	}
	return PHYSINVAL;
}


static void *
find_free_physical_range(size_t size)
{
	// just do a simple linear search at the end of the allocated
	// ranges (dumb memory allocation)
	if (gKernelArgs.num_physical_allocated_ranges == 0) {
		if (gKernelArgs.num_physical_memory_ranges == 0)
			return PHYSINVAL;

		return find_physical_memory_range(size);
	}

	for (uint32 i = 0; i < gKernelArgs.num_physical_allocated_ranges; i++) {
		void *address
			= (void *)(addr_t)(gKernelArgs.physical_allocated_range[i].start
				+ gKernelArgs.physical_allocated_range[i].size);
		if (!is_physical_allocated(address, size)
			&& is_physical_memory(address, size))
			return address;
	}
	return PHYSINVAL;
}


static void *
find_free_virtual_range(void *base, size_t size)
{
	if (base && !is_virtual_allocated(base, size))
		return base;

	void *firstFound = NULL;
	void *firstBaseFound = NULL;
	for (uint32 i = 0; i < gKernelArgs.num_virtual_allocated_ranges; i++) {
		void *address
			= (void *)(addr_t)(gKernelArgs.virtual_allocated_range[i].start
				+ gKernelArgs.virtual_allocated_range[i].size);
		if (!is_virtual_allocated(address, size)) {
			if (!base)
				return address;

			if (firstFound == NULL)
				firstFound = address;
			if (address >= base
				&& (firstBaseFound == NULL || address < firstBaseFound)) {
				firstBaseFound = address;
			}
		}
	}
	return (firstBaseFound ? firstBaseFound : firstFound);
}


extern "C" void *
arch_mmu_allocate(void *_virtualAddress, size_t size, uint8 _protection,
	bool exactAddress)
{
	// we only know page sizes
	size = ROUNDUP(size, B_PAGE_SIZE);

	uint8 protection = 0;
	if (_protection & B_WRITE_AREA)
		protection = PAGE_READ_WRITE;
	else
		protection = PAGE_READ_ONLY;

	// If no address is given, use an address well above KERNEL_BASE, since
	// that avoids trouble in the kernel, when we decide to keep the region.
	//
	// NOTE: this used to default to exactly KERNEL_BASE. That is a real
	// problem: kernel_args_malloc() (src/system/boot/loader/kernel_args.cpp,
	// shared/generic code, not ppc-specific) allocates its first 512 KB
	// pool chunk via platform_allocate_region() with no address preference
	// at all - which used to land it at exactly KERNEL_BASE too, via this
	// same fallback. Since the kernel image itself is also allocated
	// starting at KERNEL_BASE (see ELFLoader<Class>::Load(), which prefers
	// but does not strictly require that address), whichever of the two
	// allocations happens to run first silently claims KERNEL_BASE, and
	// the other one - if it is the kernel - ends up loaded somewhere else
	// entirely. That is fatal for the kernel specifically: it relocates
	// itself (including position-dependent .got2 / R_PPC_RELATIVE entries)
	// assuming it will run from wherever it actually got allocated, but
	// later unconditionally maps itself to execute from KERNEL_BASE via
	// its own MMU setup - without ever redoing those relocations for the
	// new address. Most code survives the silent move fine, since normal
	// branches and calls are PC-relative; absolute-address tables like
	// .got2 do not, and end up wrong by exactly the gap between the two
	// addresses. Push the fallback well past where the kernel could
	// plausibly need to grow, so "no preference" allocations stop
	// contending for the one address that truly must be exact.
	void *virtualAddress = _virtualAddress;
	if (!virtualAddress)
		virtualAddress = (void*)(KERNEL_BASE + 0x10000000);

	// Find a free address large enough to hold "size". A range that looks
	// free in virtual_allocated_range may still contain mappings the
	// firmware faulted into the page table lazily (on QEMU/OpenBIOS the
	// mac-io device apertures sit at 0x80000000, exactly at KERNEL_BASE),
	// so also steer clear of anything with live page table entries.
	for (int32 attempts = 0; ; attempts++) {
		if (attempts >= 1024) {
			dprintf("arch_mmu_allocate(): no virtual range free of existing "
				"mappings found (base: %p, size: %" B_PRIuSIZE ")\n",
				_virtualAddress, size);
			return NULL;
		}

		virtualAddress = find_free_virtual_range(virtualAddress, size);
		if (virtualAddress == NULL)
			return NULL;

		void *firstMappedPage;
		if (!is_virtual_range_mapped_in_page_table(virtualAddress, size,
				&firstMappedPage)) {
			break;
		}

		dprintf("arch_mmu_allocate(): range %p - %p overlaps existing "
			"mapping at %p, retrying beyond it\n", virtualAddress,
			(void *)((addr_t)virtualAddress + size), firstMappedPage);
		virtualAddress = (void *)((addr_t)firstMappedPage + B_PAGE_SIZE);
	}

	// fail if the exact address was requested, but is not free
	if (exactAddress && _virtualAddress && virtualAddress != _virtualAddress) {
		dprintf("arch_mmu_allocate(): exact address requested, but virtual "
			"range (base: %p, size: %" B_PRIuSIZE ") is not free.\n",
			_virtualAddress, size);
		return NULL;
	}

	// we have a free virtual range for the allocation, now
	// have a look for free physical memory as well (we assume
	// that a) there is enough memory, and b) failing is fatal
	// so that we don't have to optimize for these cases :)

	void *physicalAddress = find_free_physical_range(size);
	if (physicalAddress == PHYSINVAL) {
		dprintf("arch_mmu_allocate(base: %p, size: %" B_PRIuSIZE ") "
			"no free physical address\n", virtualAddress, size);
		return NULL;
	}

	// everything went fine, so lets mark the space as used.

	dprintf("mmu_alloc: va %p, pa %p, size %" B_PRIuSIZE "\n", virtualAddress,
		physicalAddress, size);
	insert_virtual_allocated_range((addr_t)virtualAddress, size);
	insert_physical_allocated_range((addr_t)physicalAddress, size);

	map_range(virtualAddress, physicalAddress, size, protection);

	return virtualAddress;
}


extern "C" void *
arch_mmu_map_device(void *physicalAddress, size_t size)
{
	// Map a physical device range (cache-inhibited + guarded) so the loader
	// itself can program hardware registers. Prefers an identity mapping so
	// the caller can reason in physical addresses.
	size = ROUNDUP(size, B_PAGE_SIZE);

	void *virtualAddress = physicalAddress;
	if (is_virtual_allocated(virtualAddress, size)) {
		virtualAddress = find_free_virtual_range(NULL, size);
		if (virtualAddress == NULL)
			return NULL;
	}
	if (insert_virtual_allocated_range((addr_t)virtualAddress, size) < B_OK)
		return NULL;

	map_range(virtualAddress, physicalAddress, size,
		0x2a);
			// caching inhibited, guarded, read/write
	return virtualAddress;
}


extern "C" status_t
arch_mmu_free(void *address, size_t size)
{
	// TODO: implement freeing a region!
	return B_OK;
}


static inline void
invalidate_tlb(void)
{
	//asm volatile("tlbia");
		// "tlbia" is obviously not available on every CPU...

	// Note: this flushes the whole 4 GB address space - it
	//		would probably be a good idea to do less here

	addr_t address = 0;
	for (uint32 i = 0; i < 0x100000; i++) {
		asm volatile("tlbie %0" : : "r" (address));
		address += B_PAGE_SIZE;
	}
	tlbsync();
}


//	#pragma mark - OpenFirmware callbacks and public API


static int
map_callback(struct of_arguments *args)
{
	void *physicalAddress = (void *)args->Argument(0);
	void *virtualAddress = (void *)args->Argument(1);
	int length = args->Argument(2);
	int mode = args->Argument(3);
	intptr_t &error = args->ReturnValue(0);

	// insert range in physical allocated if needed

	if (is_physical_memory(physicalAddress)
		&& insert_physical_allocated_range((addr_t)physicalAddress, length)
			!= B_OK) {
		error = -1;
		return OF_FAILED;
	}

	// insert range in virtual allocated

	if (insert_virtual_allocated_range((addr_t)virtualAddress, length)
			!= B_OK) {
		error = -2;
		return OF_FAILED;
	}

	// map range into the page table

	map_range(virtualAddress, physicalAddress, length, mode);

	return B_OK;
}


static int
unmap_callback(struct of_arguments *args)
{
/*	void *address = (void *)args->Argument(0);
	int length = args->Argument(1);
	int &error = args->ReturnValue(0);
*/
	// TODO: to be implemented

	return OF_FAILED;
}


static int
translate_callback(struct of_arguments *args)
{
	addr_t virtualAddress = (addr_t)args->Argument(0);
	intptr_t &error = args->ReturnValue(0);
	intptr_t &physicalAddress = args->ReturnValue(1);
	intptr_t &mode = args->ReturnValue(2);

	// Find page table entry for this address

	uint32 virtualSegmentID
		= sSegments[addr_t(virtualAddress) >> 28].virtual_segment_id;

	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID,
		(uint32)virtualAddress);
	page_table_entry_group *group = &sPageTable[hash & sPageTableHashMask];
	page_table_entry *entry = NULL;

	for (int32 i = 0; i < 8; i++) {
		entry = &group->entry[i];

		if (entry->valid
			&& entry->virtual_segment_id == virtualSegmentID
			&& entry->secondary_hash == false
			&& entry->abbr_page_index == ((virtualAddress >> 22) & 0x3f))
			goto success;
	}

	hash = page_table_entry::SecondaryHash(hash);
	group = &sPageTable[hash & sPageTableHashMask];

	for (int32 i = 0; i < 8; i++) {
		entry = &group->entry[i];

		if (entry->valid
			&& entry->virtual_segment_id == virtualSegmentID
			&& entry->secondary_hash == true
			&& entry->abbr_page_index == ((virtualAddress >> 22) & 0x3f))
			goto success;
	}

	// could not find the translation
	error = B_ENTRY_NOT_FOUND;
	return OF_FAILED;

success:
	// we found the entry in question
	physicalAddress = (int)(entry->physical_page_number * B_PAGE_SIZE);
	mode = (entry->write_through << 6)		// WIMGxPP
		| (entry->caching_inhibited << 5)
		| (entry->memory_coherent << 4)
		| (entry->guarded << 3)
		| entry->page_protection;
	error = B_OK;

	return B_OK;
}


static int
alloc_real_mem_callback(struct of_arguments *args)
{
/*	addr_t minAddress = (addr_t)args->Argument(0);
	addr_t maxAddress = (addr_t)args->Argument(1);
	int length = args->Argument(2);
	int mode = args->Argument(3);
	int &error = args->ReturnValue(0);
	int &physicalAddress = args->ReturnValue(1);
*/
	// ToDo: to be implemented

	return OF_FAILED;
}


/** Dispatches the callback to the responsible function */

static int
callback(struct of_arguments *args)
{
	const char *name = args->name;
	TRACE("OF CALLBACK: %s\n", name);

	if (!strcmp(name, "map"))
		return map_callback(args);
	else if (!strcmp(name, "unmap"))
		return unmap_callback(args);
	else if (!strcmp(name, "translate"))
		return translate_callback(args);
	else if (!strcmp(name, "alloc-real-mem"))
		return alloc_real_mem_callback(args);

	return OF_FAILED;
}


extern "C" status_t
arch_set_callback(void)
{
	// set OpenFirmware callbacks - it will ask us for memory after that
	// instead of maintaining it itself

	void *oldCallback = NULL;
	if (of_call_client_function("set-callback", 1, 1, &callback, &oldCallback)
			== OF_FAILED) {
		dprintf("Error: OpenFirmware set-callback failed\n");
		return B_ERROR;
	}
	TRACE("old callback = %p; new callback = %p\n", oldCallback, callback);

	return B_OK;
}


// ---------------------------------------------------------------------------
//	Audio (mac-io i2s / DBDMA / i2c / GPIO) device-tree probe
//
//	Sound on these Macs is spread over four mac-io cells whose addresses are
//	machine specific and exist only in the Open Firmware device tree: the i2s
//	serial bus, its two DBDMA channels (play and record), a Keywest i2c bus
//	carrying the codec, and a handful of GPIOs that mute the amplifier and
//	report the headphone jack. The kernel cannot call OF, so the loader reads
//	all of it here - as a structured description the sound driver consumes, and
//	as a text dump the kernel prints into the syslog. The subtree is entirely
//	new territory, and one boot that shows all of it is worth more than five
//	that each answer a single question.
// ---------------------------------------------------------------------------

static char* sAudioDump;
static size_t sAudioDumpSize;
static size_t sAudioDumpUsed;

// Nodes referenced by a "platform-do-*" property. Apple moved operations that
// used to be plain register writes into device-tree bytecode, and the property
// on the client node names the provider by phandle; the bytecode that actually
// does the work lives over there.
#define AUDIO_MAX_PLATFORM_TARGETS	6
static uint32 sPlatformTargets[AUDIO_MAX_PLATFORM_TARGETS];
static uint32 sPlatformTargetCount;


static void
audio_note_platform_target(uint32 phandle)
{
	if (phandle == 0 || phandle == 0xffffffff)
		return;
	for (uint32 i = 0; i < sPlatformTargetCount; i++) {
		if (sPlatformTargets[i] == phandle)
			return;
	}
	if (sPlatformTargetCount < AUDIO_MAX_PLATFORM_TARGETS)
		sPlatformTargets[sPlatformTargetCount++] = phandle;
}


static void
audio_dump(const char* format, ...)
{
	if (sAudioDump == NULL || sAudioDumpUsed + 1 >= sAudioDumpSize)
		return;

	va_list args;
	va_start(args, format);
	int length = vsnprintf(sAudioDump + sAudioDumpUsed,
		sAudioDumpSize - sAudioDumpUsed, format, args);
	va_end(args);

	if (length > 0) {
		sAudioDumpUsed += (size_t)length;
		if (sAudioDumpUsed >= sAudioDumpSize)
			sAudioDumpUsed = sAudioDumpSize - 1;
	}
}


//! Read a string property. Always leaves \a buffer NUL terminated; a property
//	that is a NUL-separated list (like "compatible") yields its first entry.
static void
audio_string_prop(intptr_t node, const char* property, char* buffer,
	size_t size)
{
	buffer[0] = '\0';
	intptr_t length = of_getprop(node, property, buffer, (intptr_t)size - 1);
	if (length <= 0) {
		buffer[0] = '\0';
		return;
	}
	if ((size_t)length > size - 1)
		length = (intptr_t)size - 1;
	buffer[length] = '\0';
}


//! Read up to \a maxCells cells of a word-array property. Returns the number
//	of cells actually read (0 if the property is absent).
static uint32
audio_cell_prop(intptr_t node, const char* property, uint32* cells,
	uint32 maxCells)
{
	for (uint32 i = 0; i < maxCells; i++)
		cells[i] = 0;
	intptr_t length = of_getprop(node, property, cells,
		(intptr_t)(maxCells * sizeof(uint32)));
	if (length <= 0)
		return 0;
	uint32 count = (uint32)length / 4;
	return count > maxCells ? maxCells : count;
}


static void
audio_dump_cell_prop(intptr_t node, const char* property, const char* label)
{
	uint32 cells[12];
	uint32 count = audio_cell_prop(node, property, cells, 12);
	if (count == 0)
		return;
	audio_dump(" %s=", label);
	for (uint32 i = 0; i < count; i++)
		audio_dump("%s%x", i > 0 ? "," : "", (unsigned)cells[i]);
}


//! One line describing a node: name, compatible, type, reg, interrupts, plus
//	the few properties that identify audio hardware.
static void
audio_dump_node(intptr_t node, int depth)
{
	static const char kIndent[] = "      ";
	int indent = depth * 2;
	if (indent > 6)
		indent = 6;

	char name[40];
	char compatible[56];
	char type[32];
	audio_string_prop(node, "name", name, sizeof(name));
	audio_string_prop(node, "compatible", compatible, sizeof(compatible));
	audio_string_prop(node, "device_type", type, sizeof(type));

	audio_dump("%s%s", kIndent + (6 - indent), name);
	if (compatible[0] != '\0')
		audio_dump(" c=%s", compatible);
	if (type[0] != '\0')
		audio_dump(" t=%s", type);
	audio_dump_cell_prop(node, "reg", "reg");
	audio_dump_cell_prop(node, "interrupts", "int");
	audio_dump_cell_prop(node, "AAPL,interrupts", "aint");
	audio_dump_cell_prop(node, "layout-id", "layout");
	audio_dump_cell_prop(node, "device-id", "devid");
	audio_dump_cell_prop(node, "#address-cells", "#ac");
	audio_dump_cell_prop(node, "audio-gpio-active-state", "act");

	char gpio[32];
	audio_string_prop(node, "audio-gpio", gpio, sizeof(gpio));
	if (gpio[0] != '\0')
		audio_dump(" gpio=%s", gpio);

	audio_dump("\n");
}


//! Every property of a node, by name, with its length and first cells. Used on
//	the handful of nodes the driver has to understand exactly, so that a
//	property nobody thought to ask for still shows up.
static void
audio_dump_all_props(intptr_t node, int depth)
{
	static const char kIndent[] = "        ";
	int indent = depth * 2;
	if (indent > 8)
		indent = 8;

	char property[64];
	char previous[64];
	previous[0] = '\0';

	for (int i = 0; i < 40; i++) {
		property[0] = '\0';
		intptr_t result = of_nextprop(node,
			previous[0] == '\0' ? NULL : previous, property);
		if (result != 1 || property[0] == '\0')
			break;

		intptr_t length = of_getproplen(node, property);
		audio_dump("%s.%s[%d]", kIndent + (8 - indent), property, (int)length);

		// A platform function is the whole point of looking here, so print all
		// of it rather than the first few words, and remember who it acts on.
		if (strncmp(property, "platform-", 9) == 0 && length >= 8) {
			uint32 cells[16];
			uint32 count = audio_cell_prop(node, property, cells, 16);
			for (uint32 c = 0; c < count; c++)
				audio_dump(" %x", (unsigned)cells[c]);
			if (strncmp(property, "platform-do-", 12) == 0 && count > 0)
				audio_note_platform_target(cells[0]);
			audio_dump("\n");
			strlcpy(previous, property, sizeof(previous));
			continue;
		}

		if (length > 0 && length <= 8) {
			// short: could be either cells or a string, so show both
			uint32 cells[2];
			uint32 count = audio_cell_prop(node, property, cells, 2);
			for (uint32 c = 0; c < count; c++)
				audio_dump(" %x", (unsigned)cells[c]);
			char text[16];
			audio_string_prop(node, property, text, sizeof(text));
			bool printable = text[0] >= 0x20 && text[0] < 0x7f;
			if (printable)
				audio_dump(" \"%s\"", text);
		} else if (length > 8 && length <= 64) {
			uint32 cells[4];
			uint32 count = audio_cell_prop(node, property, cells, 4);
			for (uint32 c = 0; c < count; c++)
				audio_dump(" %x", (unsigned)cells[c]);
			char text[24];
			audio_string_prop(node, property, text, sizeof(text));
			if (text[0] >= 0x20 && text[0] < 0x7f)
				audio_dump(" \"%s\"", text);
		}
		audio_dump("\n");

		strlcpy(previous, property, sizeof(previous));
	}
}


//! strstr, which the boot loader's cut-down libroot does not provide.
static bool
audio_contains(const char* haystack, const char* needle)
{
	size_t length = strlen(needle);
	for (const char* p = haystack; *p != '\0'; p++) {
		if (strncmp(p, needle, length) == 0)
			return true;
	}
	return false;
}


static bool
audio_name_is(const char* name, const char* wanted)
{
	// mac-io cells appear both bare ("i2s") and unit-addressed ("i2s@10000");
	// OF's "name" property is the bare form, but be tolerant either way.
	size_t length = strlen(wanted);
	if (strncmp(name, wanted, length) != 0)
		return false;
	return name[length] == '\0' || name[length] == '@';
}


//! True for the nodes whose every property is worth printing.
static bool
audio_is_interesting(const char* name, const char* compatible)
{
	// The i2c cell earns a full property dump because driving it needs
	// "AAPL,address-step" - how far apart its registers are - which is not
	// derivable from anything else.
	return audio_name_is(name, "i2c") || audio_name_is(name, "i2c-bus")
		|| audio_name_is(name, "sound") || audio_name_is(name, "i2s-a")
		|| audio_name_is(name, "i2s-b") || audio_name_is(name, "davbus")
		|| audio_name_is(name, "deq") || audio_name_is(name, "codec")
		|| audio_contains(compatible, "tas")
		|| audio_contains(compatible, "snapper")
		|| audio_contains(compatible, "burgundy")
		|| audio_contains(compatible, "screamer");
}


//! The codec sits on the Keywest i2c bus as "deq" (digital equaliser), and its
//	"reg" is its address on that bus. Note the depth: on the i2s machines the
//	i2c cell has an "i2c-bus" child and the codec hangs off THAT, so this has to
//	be tried at both levels.
static void
audio_record_codec(intptr_t node, const char* name, const char* compatible,
	uint32 channel)
{
	if (!audio_name_is(name, "deq") && !audio_contains(compatible, "tas")
			&& !audio_contains(compatible, "codec"))
		return;

	uint32 reg[4];
	if (audio_cell_prop(node, "reg", reg, 4) == 0)
		return;

	gKernelArgs.arch_args.audio.codec_i2c_addr = reg[0];
	gKernelArgs.arch_args.audio.i2c_channel = channel;
	strlcpy(gKernelArgs.arch_args.audio.codec,
		compatible[0] != '\0' ? compatible : name,
		sizeof(gKernelArgs.arch_args.audio.codec));
}


//! Record one audio GPIO (amp mute, headphone mute/detect, codec reset).
static void
audio_record_gpio(intptr_t node, const char* name, uint32 gpioBase)
{
	uint32 index = gKernelArgs.arch_args.audio.gpio_count;
	if (index >= 8)
		return;

	uint32 cells[4];
	uint32 count = audio_cell_prop(node, "reg", cells, 4);
	if (count == 0)
		return;

	// A GPIO cell's "reg" is an offset relative to the parent gpio cell on
	// some machines and absolute within mac-io on others; anything smaller
	// than the gpio block's own offset is clearly the former.
	uint32 offset = cells[0];
	if (offset < gpioBase)
		offset += gpioBase;

	strlcpy(gKernelArgs.arch_args.audio.gpios[index].name, name,
		sizeof(gKernelArgs.arch_args.audio.gpios[index].name));
	gKernelArgs.arch_args.audio.gpios[index].offset = offset;

	uint32 state[2];
	gKernelArgs.arch_args.audio.gpios[index].active_state
		= audio_cell_prop(node, "audio-gpio-active-state", state, 2) > 0
			? state[0] : 0;

	uint32 intr[4];
	gKernelArgs.arch_args.audio.gpios[index].irq
		= audio_cell_prop(node, "interrupts", intr, 4) > 0 ? intr[0] : 0;

	gKernelArgs.arch_args.audio.gpio_count = index + 1;
}


//! The "sound" node names the audio wiring as a whole: its "layout-id" (later
//	machines) or its "compatible" (earlier ones) is what tells a driver which
//	codec and which speaker routing it is looking at.
static void
audio_record_sound_node(intptr_t node, const char* name)
{
	if (!audio_name_is(name, "sound"))
		return;

	uint32 id[2];
	if (audio_cell_prop(node, "layout-id", id, 2) > 0)
		gKernelArgs.arch_args.audio.layout_id = id[0];
	if (audio_cell_prop(node, "device-id", id, 2) > 0)
		gKernelArgs.arch_args.audio.device_id = id[0];

	// On the pre-i2s machines there is no separate codec node on an i2c bus -
	// the codec IS the sound node ("burgundy", "screamer"), so take its name
	// from here unless an i2c codec has already claimed the field.
	if (gKernelArgs.arch_args.audio.codec[0] == '\0') {
		char compatible[32];
		audio_string_prop(node, "compatible", compatible, sizeof(compatible));
		if (compatible[0] != '\0') {
			strlcpy(gKernelArgs.arch_args.audio.codec, compatible,
				sizeof(gKernelArgs.arch_args.audio.codec));
		}
	}
}


//! Walk the mac-io audio cells: fill in arch_args.audio and build the dump.
static void
probe_audio_device_tree(void)
{
	sAudioDump = gKernelArgs.arch_args.of_audio_dump;
	sAudioDumpSize = sizeof(gKernelArgs.arch_args.of_audio_dump);
	sAudioDumpUsed = 0;
	sAudioDump[0] = '\0';
	memset(&gKernelArgs.arch_args.audio, 0,
		sizeof(gKernelArgs.arch_args.audio));

	intptr_t root = of_finddevice("/");
	if (root == OF_FAILED)
		return;

	for (intptr_t bridge = of_child(root); bridge != 0 && bridge != OF_FAILED;
			bridge = of_peer(bridge)) {
		char bridgeType[32];
		audio_string_prop(bridge, "device_type", bridgeType,
			sizeof(bridgeType));
		if (strcmp(bridgeType, "pci") != 0)
			continue;

		for (intptr_t macio = of_child(bridge);
				macio != 0 && macio != OF_FAILED; macio = of_peer(macio)) {
			char macioType[32];
			audio_string_prop(macio, "device_type", macioType,
				sizeof(macioType));
			if (strcmp(macioType, "mac-io") != 0)
				continue;

			// mac-io's own register window: the 32-bit memory entry of
			// "assigned-addresses" (phys.hi, phys.mid, phys.lo, size.hi,
			// size.lo per entry; space code 0x02 = 32-bit memory).
			uint32 assigned[20];
			uint32 assignedCount = audio_cell_prop(macio,
				"assigned-addresses", assigned, 20);
			for (uint32 e = 0; e + 5 <= assignedCount; e += 5) {
				if (((assigned[e] >> 24) & 0x03) == 0x02) {
					gKernelArgs.arch_args.audio.macio_phys = assigned[e + 2];
					break;
				}
			}

			audio_dump_node(macio, 0);
			audio_dump("  (macio phys %x)\n",
				(unsigned)gKernelArgs.arch_args.audio.macio_phys);

			uint32 gpioBase = 0;

			for (intptr_t cell = of_child(macio);
					cell != 0 && cell != OF_FAILED; cell = of_peer(cell)) {
				char name[40];
				char compatible[56];
				audio_string_prop(cell, "name", name, sizeof(name));
				audio_string_prop(cell, "compatible", compatible,
					sizeof(compatible));

				audio_dump_node(cell, 1);

				bool isI2S = audio_name_is(name, "i2s");
				bool isI2C = audio_name_is(name, "i2c")
					|| audio_name_is(name, "i2c-bus");
				bool isGPIO = audio_name_is(name, "gpio");
				bool isDavbus = audio_name_is(name, "davbus")
					|| audio_name_is(name, "sound");

				if (!isI2S && !isI2C && !isGPIO && !isDavbus)
					continue;

				uint32 cellReg[8];
				uint32 cellRegCount = audio_cell_prop(cell, "reg", cellReg, 8);

				if (isI2C && cellRegCount > 0) {
					gKernelArgs.arch_args.audio.i2c_offset = cellReg[0];
					// How far apart the cell's registers are. Absent means
					// the usual 16 bytes.
					uint32 step[2];
					gKernelArgs.arch_args.audio.i2c_address_step
						= audio_cell_prop(cell, "AAPL,address-step", step, 2)
							> 0 ? step[0] : 0x10;
				}
				if (isGPIO && cellRegCount > 0)
					gpioBase = cellReg[0];

				// The davbus/sound form (Burgundy, Screamer): the cell itself
				// carries the three register blocks.
				if (isDavbus && cellRegCount >= 6) {
					gKernelArgs.arch_args.audio.i2s_offset = cellReg[0];
					gKernelArgs.arch_args.audio.i2s_size = cellReg[1];
					gKernelArgs.arch_args.audio.tx_dbdma_offset = cellReg[2];
					gKernelArgs.arch_args.audio.rx_dbdma_offset = cellReg[4];
					gKernelArgs.arch_args.audio.valid = 1;
					uint32 intr[8];
					uint32 intrCount = audio_cell_prop(cell, "interrupts",
						intr, 8);
					// Cells come in (input, sense) pairs on these machines.
					uint32 stride = intrCount >= 6 ? 2 : 1;
					if (intrCount >= 1)
						gKernelArgs.arch_args.audio.i2s_irq = intr[0];
					if (intrCount >= stride + 1)
						gKernelArgs.arch_args.audio.tx_irq = intr[stride];
					if (intrCount >= 2 * stride + 1)
						gKernelArgs.arch_args.audio.rx_irq = intr[2 * stride];
				}

				if (audio_is_interesting(name, compatible))
					audio_dump_all_props(cell, 2);

				for (intptr_t sub = of_child(cell);
						sub != 0 && sub != OF_FAILED; sub = of_peer(sub)) {
					char subName[40];
					char subCompatible[56];
					audio_string_prop(sub, "name", subName, sizeof(subName));
					audio_string_prop(sub, "compatible", subCompatible,
						sizeof(subCompatible));

					char gpioFunction[32];
					audio_string_prop(sub, "audio-gpio", gpioFunction,
						sizeof(gpioFunction));

					// ★ Print EVERY gpio child, not only the ones that look
					// like audio. The codec's reset line is what we are hunting
					// and it need not be named like audio at all - filtering on
					// the name is exactly how it would stay hidden. Recording
					// is still limited to the audio ones.
					bool isAudioGPIO = gpioFunction[0] != '\0'
						|| audio_contains(subName, "mute")
						|| audio_contains(subName, "detect")
						|| audio_contains(subName, "audio")
						|| audio_contains(subName, "reset")
						|| audio_contains(subName, "headphone")
						|| audio_contains(subName, "lineout")
						|| audio_contains(subName, "amp");

					audio_dump_node(sub, 2);
					audio_record_sound_node(sub, subName);

					if (isGPIO && isAudioGPIO) {
						audio_record_gpio(sub,
							gpioFunction[0] != '\0' ? gpioFunction : subName,
							gpioBase);
					}

					// The i2s form (Tumbler/Snapper/Onyx and later): the
					// i2s-a cell holds three register blocks - the serial bus
					// control, then the transmit and receive DBDMA channels.
					if (isI2S) {
						uint32 subReg[8];
						uint32 subRegCount = audio_cell_prop(sub, "reg",
							subReg, 8);
						if (subRegCount >= 6
								&& gKernelArgs.arch_args.audio.i2s_offset
									== 0) {
							gKernelArgs.arch_args.audio.i2s_offset = subReg[0];
							gKernelArgs.arch_args.audio.i2s_size = subReg[1];
							gKernelArgs.arch_args.audio.tx_dbdma_offset
								= subReg[2];
							gKernelArgs.arch_args.audio.rx_dbdma_offset
								= subReg[4];
							gKernelArgs.arch_args.audio.valid = 1;

							uint32 intr[8];
							uint32 intrCount = audio_cell_prop(sub,
								"interrupts", intr, 8);
							uint32 stride = intrCount >= 6 ? 2 : 1;
							if (intrCount >= 1)
								gKernelArgs.arch_args.audio.i2s_irq = intr[0];
							if (intrCount >= stride + 1)
								gKernelArgs.arch_args.audio.tx_irq
									= intr[stride];
							if (intrCount >= 2 * stride + 1)
								gKernelArgs.arch_args.audio.rx_irq
									= intr[2 * stride];
						}
					}

					// A codec directly under the cell is on channel 0; one
					// under an "i2c-bus@N" child is on channel N, and that
					// number goes in the cell's mode register.
					uint32 busChannel = 0;
					if (isI2C && audio_name_is(subName, "i2c-bus")) {
						uint32 busReg[2];
						if (audio_cell_prop(sub, "reg", busReg, 2) > 0)
							busChannel = busReg[0];
					}
					if (isI2C)
						audio_record_codec(sub, subName, subCompatible, 0);

					if (audio_is_interesting(subName, subCompatible))
						audio_dump_all_props(sub, 3);

					// One more level: the "sound" node hangs off i2s-a, and
					// carries the layout-id that names the whole audio wiring.
					for (intptr_t leaf = of_child(sub);
							leaf != 0 && leaf != OF_FAILED;
							leaf = of_peer(leaf)) {
						char leafName[40];
						char leafCompatible[56];
						audio_string_prop(leaf, "name", leafName,
							sizeof(leafName));
						audio_string_prop(leaf, "compatible", leafCompatible,
							sizeof(leafCompatible));
						audio_dump_node(leaf, 3);

						audio_record_sound_node(leaf, leafName);
						if (isI2C) {
							audio_record_codec(leaf, leafName, leafCompatible,
								busChannel);
						}
						if (audio_is_interesting(leafName, leafCompatible))
							audio_dump_all_props(leaf, 4);
					}
				}
			}
		}
	}

	// Whatever the platform functions act on. In Apple's Open Firmware a
	// phandle IS the package handle, so it can be used directly.
	for (uint32 i = 0; i < sPlatformTargetCount; i++) {
		char path[160];
		path[0] = '\0';
		of_package_to_path((intptr_t)sPlatformTargets[i], path, sizeof(path));
		audio_dump("platform target %x: %s\n", (unsigned)sPlatformTargets[i],
			path[0] != '\0' ? path : "(no path)");
		audio_dump_all_props((intptr_t)sPlatformTargets[i], 1);
	}

	gKernelArgs.arch_args.of_audio_dump_len = (uint32)sAudioDumpUsed;

	dprintf("audio: macio %x i2s %x (tx dma %x irq %u, rx dma %x irq %u) "
		"i2c %x codec %s@%x layout %u gpios %u\n",
		(unsigned)gKernelArgs.arch_args.audio.macio_phys,
		(unsigned)gKernelArgs.arch_args.audio.i2s_offset,
		(unsigned)gKernelArgs.arch_args.audio.tx_dbdma_offset,
		(unsigned)gKernelArgs.arch_args.audio.tx_irq,
		(unsigned)gKernelArgs.arch_args.audio.rx_dbdma_offset,
		(unsigned)gKernelArgs.arch_args.audio.rx_irq,
		(unsigned)gKernelArgs.arch_args.audio.i2c_offset,
		gKernelArgs.arch_args.audio.codec,
		(unsigned)gKernelArgs.arch_args.audio.codec_i2c_addr,
		(unsigned)gKernelArgs.arch_args.audio.layout_id,
		(unsigned)gKernelArgs.arch_args.audio.gpio_count);
}


extern "C" status_t
arch_mmu_init(void)
{
	// get map of physical memory (fill in kernel_args structure)

	size_t total;
	if (find_physical_memory_ranges(total) != B_OK) {
		dprintf("Error: could not find physical memory ranges!\n");
		return B_ERROR;
	}
	dprintf("total physical memory = %" B_PRId32 "MB\n", total / (1024 * 1024));

	// get OpenFirmware's current page table

	page_table_entry_group *oldTable;
	page_table_entry_group *table;
	size_t tableSize;
	ppc_get_page_table(&table, &tableSize);

	oldTable = table;

	bool realMode = false;

	// TODO: read these values out of the OF settings
	// NOTE: I've only ever seen -1 (0xffffffff) for these values in
	//       OpenFirmware.. even after loading the bootloader -- Alex
	addr_t realBase = 0;
	addr_t realSize = 0x400000;

	// can we just keep the page table?
	size_t suggestedTableSize = suggested_page_table_size(total);
	dprintf("current page table size = %" B_PRIuSIZE "\n", tableSize);
	dprintf("suggested page table size = %" B_PRIuSIZE "\n",
		suggestedTableSize);
	if (tableSize < suggestedTableSize) {
		// nah, we need a new one!
		dprintf("need new page table, size = %" B_PRIuSIZE "!\n",
			suggestedTableSize);
		table = (page_table_entry_group *)of_claim(NULL, suggestedTableSize,
			suggestedTableSize);
			// KERNEL_BASE would be better as virtual address, but
			// at least with Apple's OpenFirmware, it makes no
			// difference - we will have to remap it later
		if (table == (void *)OF_FAILED) {
			panic("Could not allocate new page table "
				"(size = %" B_PRIuSIZE ")!!\n", suggestedTableSize);
			return B_NO_MEMORY;
		}
		if (table == NULL) {
			// work-around for the broken Pegasos OpenFirmware
			dprintf("broken OpenFirmware detected (claim doesn't work)\n");
			realMode = true;

			addr_t tableBase = 0;
			for (int32 i = 0; tableBase < realBase + realSize * 3; i++) {
				tableBase = suggestedTableSize * i;
			}

			table = (page_table_entry_group *)tableBase;
		}

		dprintf("OpenFirmware gave us a new page table at: %p\n", table);
		sPageTable = table;
		tableSize = suggestedTableSize;
	} else {
		// ToDo: we could check if the page table is much too large
		//	and create a smaller one in this case (in order to save
		//	memory).
		dprintf("using original OpenFirmware page table at: %p\n", table);
		sPageTable = table;
	}

	sPageTableHashMask = tableSize / sizeof(page_table_entry_group) - 1;
	if (sPageTable != oldTable)
		memset(sPageTable, 0, tableSize);

	// turn off address translation via the page table/segment mechanism,
	// identity map the first 256 MB (where our code/data reside)

	dprintf("MSR: %p\n", (void *)get_msr());

	#if 0
	block_address_translation bat;

	bat.length = BAT_LENGTH_256MB;
	bat.kernel_valid = true;
	bat.memory_coherent = true;
	bat.protection = BAT_READ_WRITE;

	set_ibat0(&bat);
	set_dbat0(&bat);
	isync();
	#endif

	// initialize segment descriptors, but don't set the registers
	// until we're about to take over the page table - we're mapping
	// pages into our table using these values

	// Preserve whatever VSIDs Open Firmware already has active for each
	// segment, rather than inventing our own (e.g. "= i"). Our new page
	// table is populated using these same VSIDs below, so when we later
	// install the new page table (SDR1) there is never a moment where
	// the live segment registers and the active page table disagree on
	// the VSID scheme - which otherwise causes an immediate, silent
	// translation fault for whichever segment covers our own running code.
	for (int32 i = 0; i < 16; i++) {
		sSegments[i].virtual_segment_id
			= ppc_get_segment_register((void *)(i * 0x10000000))
				.virtual_segment_id;
	}

	// find already allocated ranges of physical memory
	// and the virtual address space

	page_table_entry_group *physicalTable = NULL;
	void *exceptionHandlers = (void *)-1;
	if (find_allocated_ranges(oldTable, table, &physicalTable,
			&exceptionHandlers) != B_OK) {
		dprintf("Error: find_allocated_ranges() failed\n");
		return B_ERROR;
	}

	// Keep allocations away from the firmware's device apertures - it maps
	// them lazily, so they don't all show up in the translations above.
	reserve_pci_device_ranges();

#if 0
	block_address_translation bats[8];
	getibats(bats);
	for (int32 i = 0; i < 8; i++) {
		printf("page index %u, length %u, ppn %u\n", bats[i].page_index,
			bats[i].length, bats[i].physical_block_number);
	}
#endif

	if (physicalTable == NULL) {
		dprintf("%s: Didn't find physical address of page table\n", __func__);
		if (!realMode)
			return B_ERROR;

		// Pegasos work-around
		#if 0
		map_range((void *)realBase, (void *)realBase,
			realSize * 2, PAGE_READ_WRITE);
		map_range((void *)(total - realSize), (void *)(total - realSize),
			realSize, PAGE_READ_WRITE);
		map_range((void *)table, (void *)table, tableSize, PAGE_READ_WRITE);
		#endif
		insert_physical_allocated_range(realBase, realSize * 2);
		insert_virtual_allocated_range(realBase, realSize * 2);
		insert_physical_allocated_range(total - realSize, realSize);
		insert_virtual_allocated_range(total - realSize, realSize);
		insert_physical_allocated_range((addr_t)table, tableSize);
		insert_virtual_allocated_range((addr_t)table, tableSize);

		// QEMU OpenHackware work-around
		insert_physical_allocated_range(0x05800000, 0x06000000 - 0x05800000);
		insert_virtual_allocated_range(0x05800000, 0x06000000 - 0x05800000);

		physicalTable = table;
	}

	if (exceptionHandlers == (void *)-1) {
		// TODO: create mapping for the exception handlers
		dprintf("Error: no mapping for the exception handlers!\n");
	}

	// Set the Open Firmware memory callback. From now on the Open Firmware
	// will ask us for memory.
	dprintf("TRACE: before arch_set_callback\n");
	arch_set_callback();
	dprintf("TRACE: after arch_set_callback\n");

	// set up new page table and turn on translation again

	for (uint32 i = 0; i < 16; i++) {
		ppc_set_segment_register((void *)(i * 0x10000000), sSegments[i]);
			// one segment describes 256 MB of memory
	}
	dprintf("TRACE: after segment registers set\n");

	ppc_set_page_table(physicalTable, tableSize);
	dprintf("TRACE: after ppc_set_page_table\n");
	invalidate_tlb();
	dprintf("TRACE: after invalidate_tlb\n");

	if (!realMode) {
		// clear BATs
		reset_ibats();
		dprintf("TRACE: after reset_ibats\n");
		reset_dbats();
		dprintf("TRACE: after reset_dbats\n");
		ppc_sync();
		isync();
		dprintf("TRACE: after ppc_sync/isync\n");
	}

	dprintf("TRACE: before set_msr (enabling translation)\n");
	set_msr(MSR_MACHINE_CHECK_ENABLED | MSR_FP_AVAILABLE
		| MSR_INST_ADDRESS_TRANSLATION | MSR_DATA_ADDRESS_TRANSLATION);
	dprintf("TRACE: after set_msr (translation now on)\n");

	// set kernel args

	dprintf("virt_allocated: %" B_PRIu32 "\n",
		gKernelArgs.num_virtual_allocated_ranges);
	dprintf("phys_allocated: %" B_PRIu32 "\n",
		gKernelArgs.num_physical_allocated_ranges);
	dprintf("phys_memory: %" B_PRIu32 "\n",
		gKernelArgs.num_physical_memory_ranges);

	gKernelArgs.arch_args.page_table.start = (addr_t)sPageTable;
	gKernelArgs.arch_args.page_table.size = tableSize;

	gKernelArgs.arch_args.exception_handlers.start = (addr_t)exceptionHandlers;
	gKernelArgs.arch_args.exception_handlers.size = B_PAGE_SIZE;

	// Detect the PCI host bridge type and its config registers, so the kernel
	// PCI driver knows which config-space mechanism to use. The Grackle's
	// registers are architecturally fixed; UniNorth's are at its base +
	// 0x800000 (address) and + 0xc00000 (data). We resolve the bridge from the
	// boot path's first component (e.g. "/pci@f2000000").
	gKernelArgs.arch_args.pci_host_bridge_type = 0;
	gKernelArgs.arch_args.pci_config_address = 0xfec00000;
	gKernelArgs.arch_args.pci_config_data = 0xfee00000;
	{
		char bootPath[256];
		bootPath[0] = '\0';
		of_getprop(gChosen, "bootpath", bootPath, sizeof(bootPath));
		char hostPath[80];
		hostPath[0] = '\0';
		if (bootPath[0] == '/') {
			const char* slash = strchr(bootPath + 1, '/');
			size_t len = slash != NULL
				? (size_t)(slash - bootPath) : strlen(bootPath);
			if (len > 0 && len < sizeof(hostPath)) {
				memcpy(hostPath, bootPath, len);
				hostPath[len] = '\0';
			}
		}
		intptr_t node = hostPath[0] != '\0' ? of_finddevice(hostPath) : -1;
		if (node != -1 && node != 0) {
			char compatible[64];
			compatible[0] = '\0';
			of_getprop(node, "compatible", compatible, sizeof(compatible));
			if (strcmp(compatible, "uni-north") == 0) {
				uint32 reg[2] = { 0, 0 };
				of_getprop(node, "reg", reg, sizeof(reg));
				gKernelArgs.arch_args.pci_host_bridge_type = 1;
				gKernelArgs.arch_args.pci_config_address
					= (uint64)reg[0] + 0x800000;
				gKernelArgs.arch_args.pci_config_data
					= (uint64)reg[0] + 0xc00000;
			}
		}
		dprintf("pci host bridge: type %u config 0x%x/0x%x (%s)\n",
			(unsigned)gKernelArgs.arch_args.pci_host_bridge_type,
			(unsigned)gKernelArgs.arch_args.pci_config_address,
			(unsigned)gKernelArgs.arch_args.pci_config_data, hostPath);

		// Enumerate EVERY PCI host bridge, not just the boot one. A Power Mac
		// G4 has multiple UniNorth PCI buses and the built-in Ethernet (GMAC)
		// commonly sits on a different bridge than the boot disk. Bridge 0 is
		// the boot bridge (mirrors the legacy fields above) so the proven boot
		// path keeps domain 0 unchanged; additional bridges are appended.
		gKernelArgs.arch_args.pci_host_bridges[0].type
			= gKernelArgs.arch_args.pci_host_bridge_type;
		gKernelArgs.arch_args.pci_host_bridges[0].config_address
			= gKernelArgs.arch_args.pci_config_address;
		gKernelArgs.arch_args.pci_host_bridges[0].config_data
			= gKernelArgs.arch_args.pci_config_data;
		uint32 bridgeCount = 1;
		uint64 bootConfig = gKernelArgs.arch_args.pci_config_address;

		intptr_t root = of_finddevice("/");
		for (intptr_t child = root != -1 ? of_child(root) : 0;
				child != 0 && child != -1
					&& bridgeCount < MAX_PCI_HOST_BRIDGES;
				child = of_peer(child)) {
			char dtype[32];
			dtype[0] = '\0';
			of_getprop(child, "device_type", dtype, sizeof(dtype));
			if (strcmp(dtype, "pci") != 0)
				continue;

			char compatible[64];
			compatible[0] = '\0';
			of_getprop(child, "compatible", compatible, sizeof(compatible));

			uint32 type;
			uint64 configAddress;
			uint64 configData;
			if (strcmp(compatible, "uni-north") == 0) {
				uint32 reg[2] = { 0, 0 };
				of_getprop(child, "reg", reg, sizeof(reg));
				type = 1;
				configAddress = (uint64)reg[0] + 0x800000;
				configData = (uint64)reg[0] + 0xc00000;
			} else {
				// Grackle / other: architecturally-fixed config registers.
				type = 0;
				configAddress = 0xfec00000;
				configData = 0xfee00000;
			}

			// Skip the boot bridge; it is already bridge 0.
			if (configAddress == bootConfig)
				continue;

			gKernelArgs.arch_args.pci_host_bridges[bridgeCount].type = type;
			gKernelArgs.arch_args.pci_host_bridges[bridgeCount]
				.config_address = configAddress;
			gKernelArgs.arch_args.pci_host_bridges[bridgeCount]
				.config_data = configData;
			dprintf("pci host bridge %u: type %u config 0x%x/0x%x\n",
				(unsigned)bridgeCount, (unsigned)type,
				(unsigned)configAddress, (unsigned)configData);
			bridgeCount++;
		}
		gKernelArgs.arch_args.pci_host_bridge_count = bridgeCount;
		dprintf("pci host bridges: %u total\n", (unsigned)bridgeCount);

		// Capture the built-in Ethernet (GMAC) IRQ from OF. The PCI
		// interrupt_line register is unrouted on these Macs; the network
		// driver needs the OpenPIC input number from the device tree.
		gKernelArgs.arch_args.gmac_irq = 0;
		for (intptr_t br = root != -1 ? of_child(root) : 0;
				br != 0 && br != -1; br = of_peer(br)) {
			char brType[32];
			brType[0] = '\0';
			of_getprop(br, "device_type", brType, sizeof(brType));
			if (strcmp(brType, "pci") != 0)
				continue;
			for (intptr_t ch = of_child(br); ch != 0 && ch != -1;
					ch = of_peer(ch)) {
				char chType[32];
				chType[0] = '\0';
				of_getprop(ch, "device_type", chType, sizeof(chType));
				if (strcmp(chType, "network") != 0)
					continue;
				uint32 intr[8];
				for (int i = 0; i < 8; i++)
					intr[i] = 0;
				intptr_t len = of_getprop(ch, "interrupts", intr,
					sizeof(intr));
				if (len < 4) {
					len = of_getprop(ch, "AAPL,interrupts", intr,
						sizeof(intr));
				}
				uint32 resolvedIRQ = (len >= 4) ? intr[0] : 0;

				// The raw "interrupts" value is only the device pin, not the
				// OpenPIC input. Resolve it through the parent PCI bridge's
				// "interrupt-map" (br is that bridge). Entry layout for this
				// bridge: 3 address cells + 1 interrupt cell + 1 parent phandle
				// + 2 parent interrupt cells (OpenPIC input, sense) = 7 cells.
				uint32 childReg[4];
				for (int i = 0; i < 4; i++)
					childReg[i] = 0;
				of_getprop(ch, "reg", childReg, sizeof(childReg));
				uint32 imap[64];
				for (int i = 0; i < 64; i++)
					imap[i] = 0;
				uint32 imask[4] = { 0, 0, 0, 0 };
				intptr_t maplen = of_getprop(br, "interrupt-map", imap,
					sizeof(imap));
				of_getprop(br, "interrupt-map-mask", imask, sizeof(imask));
				if (maplen >= 28) {
					uint32 nCells = (uint32)maplen / 4;
					uint32 keyHi = childReg[0] & imask[0];
					uint32 keyIntr = resolvedIRQ & imask[3];
					for (uint32 e = 0; e + 7 <= nCells; e += 7) {
						if ((imap[e + 0] & imask[0]) == keyHi
								&& (imap[e + 3] & imask[3]) == keyIntr) {
							resolvedIRQ = imap[e + 5];
							break;
						}
					}
				}

				if (resolvedIRQ != 0) {
					gKernelArgs.arch_args.gmac_irq = resolvedIRQ;
					dprintf("gmac irq: pin %u -> openpic %u\n",
						(unsigned)((len >= 4) ? intr[0] : 0),
						(unsigned)resolvedIRQ);
				}
				uint8 mac[6];
				for (int i = 0; i < 6; i++)
					mac[i] = 0;
				intptr_t mlen = of_getprop(ch, "local-mac-address",
					mac, sizeof(mac));
				if (mlen != 6)
					mlen = of_getprop(ch, "mac-address", mac,
						sizeof(mac));
				if (mlen == 6) {
					for (int i = 0; i < 6; i++)
						gKernelArgs.arch_args.gmac_mac[i] = mac[i];
					gKernelArgs.arch_args.gmac_mac_valid = 1;
					dprintf("gmac mac from OF: "
						"%02x:%02x:%02x:%02x:%02x:%02x\n",
						mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
				}
			}
		}

		// Capture the Kauai/Intrepid ATA controller IRQ from OF. On the 12"
		// PowerBook G4 (PowerBook6,4) and other Intrepid/UniNorth-2 Macs the
		// internal disk hangs off a SEPARATE PCI ATA function (compatible
		// "kauai-ata"), whose PCI interrupt_line is unrouted; resolve its
		// OpenPIC input through the parent PCI bridge's interrupt-map, exactly
		// as for the GMAC above.
		gKernelArgs.arch_args.kauai_ata_irq = 0;
		for (intptr_t br = root != -1 ? of_child(root) : 0;
				br != 0 && br != -1; br = of_peer(br)) {
			char brType[32];
			brType[0] = '\0';
			of_getprop(br, "device_type", brType, sizeof(brType));
			if (strcmp(brType, "pci") != 0)
				continue;
			for (intptr_t ch = of_child(br); ch != 0 && ch != -1;
					ch = of_peer(ch)) {
				char chType[32];
				chType[0] = '\0';
				of_getprop(ch, "device_type", chType, sizeof(chType));
				if (strcmp(chType, "ata") != 0)
					continue;
				uint32 intr[8];
				for (int i = 0; i < 8; i++)
					intr[i] = 0;
				intptr_t len = of_getprop(ch, "interrupts", intr,
					sizeof(intr));
				uint32 resolvedIRQ = (len >= 4) ? intr[0] : 0;
				uint32 childReg[4];
				for (int i = 0; i < 4; i++)
					childReg[i] = 0;
				of_getprop(ch, "reg", childReg, sizeof(childReg));
				uint32 imap[64];
				for (int i = 0; i < 64; i++)
					imap[i] = 0;
				uint32 imask[4] = { 0, 0, 0, 0 };
				intptr_t maplen = of_getprop(br, "interrupt-map", imap,
					sizeof(imap));
				of_getprop(br, "interrupt-map-mask", imask, sizeof(imask));
				if (maplen >= 28) {
					uint32 nCells = (uint32)maplen / 4;
					uint32 keyHi = childReg[0] & imask[0];
					uint32 keyIntr = resolvedIRQ & imask[3];
					for (uint32 e = 0; e + 7 <= nCells; e += 7) {
						if ((imap[e + 0] & imask[0]) == keyHi
								&& (imap[e + 3] & imask[3]) == keyIntr) {
							resolvedIRQ = imap[e + 5];
							break;
						}
					}
				}
				if (resolvedIRQ != 0) {
					gKernelArgs.arch_args.kauai_ata_irq = resolvedIRQ;
					dprintf("kauai-ata irq: pin %u -> openpic %u (maplen %ld)\n",
						(unsigned)((len >= 4) ? intr[0] : 0),
						(unsigned)resolvedIRQ, (long)maplen);
				}
			}
		}

		// Capture the USB controller IRQs from OF. Each OHCI/EHCI companion is a
		// PCI function whose interrupt_line is an unrouted placeholder; resolve
		// each one's real OpenPIC input through its parent PCI bridge's
		// interrupt-map (as for the GMAC/ATA above) so the OHCI driver can run
		// interrupt-driven instead of polling (fixes flaky USB writes).
		gKernelArgs.arch_args.usb_irq_count = 0;
		for (intptr_t br = root != -1 ? of_child(root) : 0;
				br != 0 && br != -1; br = of_peer(br)) {
			char brType[32];
			brType[0] = '\0';
			of_getprop(br, "device_type", brType, sizeof(brType));
			if (strcmp(brType, "pci") != 0)
				continue;
			for (intptr_t ch = of_child(br); ch != 0 && ch != -1;
					ch = of_peer(ch)) {
				char chType[32];
				chType[0] = '\0';
				of_getprop(ch, "device_type", chType, sizeof(chType));
				if (strcmp(chType, "usb") != 0)
					continue;
				uint32 intr[8];
				for (int i = 0; i < 8; i++)
					intr[i] = 0;
				intptr_t len = of_getprop(ch, "interrupts", intr,
					sizeof(intr));
				uint32 resolvedIRQ = (len >= 4) ? intr[0] : 0;
				uint32 childReg[4];
				for (int i = 0; i < 4; i++)
					childReg[i] = 0;
				of_getprop(ch, "reg", childReg, sizeof(childReg));
				uint32 imap[64];
				for (int i = 0; i < 64; i++)
					imap[i] = 0;
				uint32 imask[4] = { 0, 0, 0, 0 };
				intptr_t maplen = of_getprop(br, "interrupt-map", imap,
					sizeof(imap));
				of_getprop(br, "interrupt-map-mask", imask, sizeof(imask));
				if (maplen >= 28) {
					uint32 nCells = (uint32)maplen / 4;
					uint32 keyHi = childReg[0] & imask[0];
					uint32 keyIntr = resolvedIRQ & imask[3];
					for (uint32 e = 0; e + 7 <= nCells; e += 7) {
						if ((imap[e + 0] & imask[0]) == keyHi
								&& (imap[e + 3] & imask[3]) == keyIntr) {
							resolvedIRQ = imap[e + 5];
							break;
						}
					}
				}
				uint32 count = gKernelArgs.arch_args.usb_irq_count;
				if (resolvedIRQ != 0 && count < 8) {
					uint32 reg0 = childReg[0];
					uint32 addr = (((reg0 >> 16) & 0xff) << 16)
						| (((reg0 >> 11) & 0x1f) << 8)
						| ((reg0 >> 8) & 0x7);
					gKernelArgs.arch_args.usb_irqs[count].address = addr;
					gKernelArgs.arch_args.usb_irqs[count].irq = resolvedIRQ;
					gKernelArgs.arch_args.usb_irq_count = count + 1;
					dprintf("usb irq: %02x:%02x.%x pin %u -> openpic %u\n",
						(unsigned)((reg0 >> 16) & 0xff),
						(unsigned)((reg0 >> 11) & 0x1f),
						(unsigned)((reg0 >> 8) & 0x7),
						(unsigned)((len >= 4) ? intr[0] : 0),
						(unsigned)resolvedIRQ);
				}
			}
		}

		// Capture the CardBus (PC Card) bridge IRQ from OF. Its interrupt_line is
		// unrouted; the inserted card's INTA routes through the bridge, so resolve
		// the cardbus node's interrupt through the parent PCI bridge's
		// interrupt-map, exactly as for the GMAC above.
		gKernelArgs.arch_args.cardbus_irq = 0;
		for (intptr_t br = root != -1 ? of_child(root) : 0;
				br != 0 && br != -1; br = of_peer(br)) {
			char brType[32];
			brType[0] = '\0';
			of_getprop(br, "device_type", brType, sizeof(brType));
			if (strcmp(brType, "pci") != 0)
				continue;
			for (intptr_t ch = of_child(br); ch != 0 && ch != -1;
					ch = of_peer(ch)) {
				char chType[32];
				chType[0] = '\0';
				of_getprop(ch, "device_type", chType, sizeof(chType));
				if (strcmp(chType, "cardbus") != 0)
					continue;
				uint32 intr[8];
				for (int i = 0; i < 8; i++)
					intr[i] = 0;
				intptr_t len = of_getprop(ch, "interrupts", intr,
					sizeof(intr));
				uint32 resolvedIRQ = (len >= 4) ? intr[0] : 0;
				uint32 childReg[4];
				for (int i = 0; i < 4; i++)
					childReg[i] = 0;
				of_getprop(ch, "reg", childReg, sizeof(childReg));
				uint32 imap[64];
				for (int i = 0; i < 64; i++)
					imap[i] = 0;
				uint32 imask[4] = { 0, 0, 0, 0 };
				intptr_t maplen = of_getprop(br, "interrupt-map", imap,
					sizeof(imap));
				of_getprop(br, "interrupt-map-mask", imask, sizeof(imask));
				if (maplen >= 28) {
					uint32 nCells = (uint32)maplen / 4;
					uint32 keyHi = childReg[0] & imask[0];
					uint32 keyIntr = resolvedIRQ & imask[3];
					for (uint32 e = 0; e + 7 <= nCells; e += 7) {
						if ((imap[e + 0] & imask[0]) == keyHi
								&& (imap[e + 3] & imask[3]) == keyIntr) {
							resolvedIRQ = imap[e + 5];
							break;
						}
					}
				}
				if (resolvedIRQ != 0) {
					gKernelArgs.arch_args.cardbus_irq = resolvedIRQ;
					dprintf("cardbus irq: pin %u -> openpic %u\n",
						(unsigned)((len >= 4) ? intr[0] : 0),
						(unsigned)resolvedIRQ);
				}

				// Capture the CardBus memory-window base from the node's `ranges`
				// (the address OF designated for the card, guaranteed to be
				// forwarded by the host bridge). Entry: child[3] parent[3] size[2];
				// pick the 32-bit memory entry (space code 0x02) and take the
				// parent low address (cell 5).
				uint32 rng[32];
				for (int i = 0; i < 32; i++)
					rng[i] = 0;
				intptr_t rlen = of_getprop(ch, "ranges", rng, sizeof(rng));
				if (rlen >= 32) {
					uint32 rCells = (uint32)rlen / 4;
					for (uint32 e = 0; e + 8 <= rCells; e += 8) {
						if (((rng[e] >> 24) & 0x03) == 0x02) {
							gKernelArgs.arch_args.cardbus_mem_base = rng[e + 5];
							dprintf("cardbus mem base: 0x%x\n",
								(unsigned)rng[e + 5]);
							break;
						}
					}
				}
			}
		}
	}

	// Capture the built-in AirPort (Broadcom BCM4306) IRQ from OF. Like the
	// GMAC/CardBus, its PCI interrupt_line is unrouted; resolve the radio's
	// interrupt through the parent PCI bridge's interrupt-map so bwi's
	// bus_alloc_resource(SYS_RES_IRQ) finds a usable OpenPIC vector.
	gKernelArgs.arch_args.airport_irq = 0;
	{
		intptr_t apRoot = of_finddevice("/");
		for (intptr_t br = apRoot != -1 ? of_child(apRoot) : 0;
				br != 0 && br != -1; br = of_peer(br)) {
			char brType[32];
			brType[0] = '\0';
			of_getprop(br, "device_type", brType, sizeof(brType));
			if (strcmp(brType, "pci") != 0)
				continue;
			for (intptr_t ch = of_child(br); ch != 0 && ch != -1;
					ch = of_peer(ch)) {
				char chName[32];
				chName[0] = '\0';
				of_getprop(ch, "name", chName, sizeof(chName));
				if (strcmp(chName, "pci80211") != 0)
					continue;
				uint32 intr[8];
				for (int i = 0; i < 8; i++)
					intr[i] = 0;
				intptr_t len = of_getprop(ch, "interrupts", intr, sizeof(intr));
				uint32 resolvedIRQ = (len >= 4) ? intr[0] : 0;
				uint32 childReg[4];
				for (int i = 0; i < 4; i++)
					childReg[i] = 0;
				of_getprop(ch, "reg", childReg, sizeof(childReg));
				uint32 imap[64];
				for (int i = 0; i < 64; i++)
					imap[i] = 0;
				uint32 imask[4] = { 0, 0, 0, 0 };
				intptr_t maplen = of_getprop(br, "interrupt-map", imap,
					sizeof(imap));
				of_getprop(br, "interrupt-map-mask", imask, sizeof(imask));
				if (maplen >= 28) {
					uint32 nCells = (uint32)maplen / 4;
					uint32 keyHi = childReg[0] & imask[0];
					uint32 keyIntr = resolvedIRQ & imask[3];
					for (uint32 e = 0; e + 7 <= nCells; e += 7) {
						if ((imap[e + 0] & imask[0]) == keyHi
								&& (imap[e + 3] & imask[3]) == keyIntr) {
							resolvedIRQ = imap[e + 5];
							break;
						}
					}
				}
				if (resolvedIRQ != 0) {
					gKernelArgs.arch_args.airport_irq = resolvedIRQ;
					dprintf("airport irq: pin %u -> openpic %u\n",
						(unsigned)((len >= 4) ? intr[0] : 0),
						(unsigned)resolvedIRQ);
				}
			}
		}
	}

	// Power on the built-in AirPort (Broadcom BCM4306) via Apple Open
	// Firmware's own mac-io methods. The generic KeyLargo register sequence
	// (KL2_AIRPORT_RESET_N + radio GPIOs, as in Linux core99_airport_enable)
	// is insufficient on Intrepid (PowerBook6,4): the radio core stays gated
	// (config space answers but its BAR master-aborts). The mac-io node
	// exposes the machine-specific "enable-cardslot" + "init-cardslot-radio"
	// methods (the OF enable sequence that the generic C reproduces only
	// approximately). OF is callable from the loader, so invoke them here, so
	// the radio is powered before the kernel enumerates PCI. Both are 0-arg,
	// 0-return; of_call_method catches any throw, so a missing method is safe.
	intptr_t airRoot = of_finddevice("/");
	for (intptr_t br = airRoot != -1 ? of_child(airRoot) : 0;
			br != 0 && br != -1; br = of_peer(br)) {
		char brType[32];
		brType[0] = '\0';
		of_getprop(br, "device_type", brType, sizeof(brType));
		if (strcmp(brType, "pci") != 0)
			continue;
		for (intptr_t ch = of_child(br); ch != 0 && ch != -1;
				ch = of_peer(ch)) {
			char chType[32];
			chType[0] = '\0';
			of_getprop(ch, "device_type", chType, sizeof(chType));
			if (strcmp(chType, "mac-io") != 0)
				continue;
			char macioPath[256];
			macioPath[0] = '\0';
			if (of_package_to_path(ch, macioPath, sizeof(macioPath)) <= 0)
				continue;
			intptr_t ih = of_open(macioPath);
			if (ih == 0 || ih == -1) {
				dprintf("airport: could not open mac-io %s\n", macioPath);
				continue;
			}
			// Do NOT call the OF enable-cardslot / init-cardslot-radio methods.
			// OpenFirmware has already powered the AirPort radio; re-invoking
			// these (re-asserting the radio reset / re-poking the KeyLargo GPIOs)
			// puts this PowerBook (Intrepid) into a state where the PMU
			// autonomously powers the machine off every few minutes. Skipping
			// them keeps WiFi working (via OF power) and fixes the random
			// shutoffs. See the AirPort bring-up history in git.
			dprintf("airport: OF enable-cardslot skipped for %s (OF already "
				"powered the radio; calling it trips the PMU power-off)\n",
				macioPath);
			of_close(ih);
		}
	}

	probe_audio_device_tree();

	return B_OK;
}

