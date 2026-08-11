/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef KERNEL_ARCH_PPC_KERNEL_ARGS_H
#define KERNEL_ARCH_PPC_KERNEL_ARGS_H

#ifndef KERNEL_BOOT_KERNEL_ARGS_H
#	error This file is included from <boot/kernel_args.h> only
#endif

#define _PACKED __attribute__((packed))

#define MAX_VIRTUAL_RANGES_TO_KEEP	32
#define MAX_PCI_HOST_BRIDGES		4

// kernel args
typedef struct {
	// architecture specific
	uint64		cpu_frequency;
	uint64		bus_frequency;
	uint64		time_base_frequency;

	addr_range	page_table;		// virtual address and size of the page table
	addr_range	exception_handlers;
	addr_range	framebuffer;		// maps where the framebuffer is located, in physical memory
	int 		screen_x, screen_y, screen_depth;

	// The virtual ranges we want to keep in the kernel. E.g. those belonging
	// to the Open Firmware.
	uint32		num_virtual_ranges_to_keep;
	addr_range	virtual_ranges_to_keep[MAX_VIRTUAL_RANGES_TO_KEEP];

	// platform type we booted from
	int			platform;

	// PCI host bridge, detected by the boot loader from Open Firmware.
	// pci_host_bridge_type: 0 = Grackle (MPC106), 1 = UniNorth.
	// Legacy single-bridge fields (kept for compatibility): mirror bridge 0,
	// which is always the boot bridge.
	uint32		pci_host_bridge_type;
	uint64		pci_config_address;
	uint64		pci_config_data;

	// All PCI host bridges. A Power Mac G4 has three UniNorth PCI buses; the
	// boot disk, the built-in Ethernet (GMAC), and FireWire can each live on a
	// different one, so the kernel must enumerate every bridge, not just the
	// one the boot path came through. Bridge 0 is always the boot bridge.
	uint32		pci_host_bridge_count;
	struct {
		uint32	type;			// 0 = Grackle, 1 = UniNorth
		uint64	config_address;
		uint64	config_data;
	} pci_host_bridges[MAX_PCI_HOST_BRIDGES];

	// OpenPIC IRQ of the built-in GMAC Ethernet, read from the Open Firmware
	// device tree by the boot loader. 0 = none/unknown. The PCI
	// interrupt_line register is unrouted on these Macs, so the kernel needs
	// this to give the network driver a usable interrupt.
	uint32		gmac_irq;
	// Built-in GMAC Ethernet MAC address, read from OF by the loader
	// (OF is not reliably callable from the kernel). gmac_mac_valid is 0
	// when no address was found.
	uint8		gmac_mac[6];
	uint32		gmac_mac_valid;
	// OpenPIC IRQ of the CardBus (PC Card) bridge, resolved from the OF
	// interrupt-map by the loader; the card's INTA routes through the bridge.
	uint32		cardbus_irq;
	// CPU/PCI base of the CardBus bridge's memory window (from the OF
	// `ranges`); the card's register BAR is placed here. 0 = unknown.
	uint32		cardbus_mem_base;
	// OpenPIC IRQ of the Kauai/Intrepid ATA controller (compatible
	// "kauai-ata", a separate PCI function, not a mac-io cell), resolved from
	// the OF interrupt-map by the loader. 0 = none/unknown.
	uint32		kauai_ata_irq;
} arch_kernel_args;

#endif	/* KERNEL_ARCH_PPC_KERNEL_ARGS_H */
