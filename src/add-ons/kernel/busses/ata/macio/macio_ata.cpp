/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sean Malseed, actionretro@pm.me
 *		Claude (Anthropic), paired via Claude Code
 *
 * ATA (IDE) controller driver for the on-chip IDE of the Apple "mac-io" chips
 * (Grand Central / Heathrow / Paddington / KeyLargo). This is the internal
 * disk controller of the iMac G3 and Old-World / early New-World Power Macs.
 *
 * Unlike a PCI IDE controller, the mac-io IDE is not a separate PCI function
 * with I/O-port BARs: it is a block of memory-mapped registers inside the
 * mac-io device's register space, and the ATA task-file registers are spaced
 * 16 bytes apart (register N is at offset N*0x10). So the generic ata_adapter
 * (which does PCI I/O-port access) cannot drive it - this driver implements
 * the ata_controller_interface directly against the mac-io register layout.
 *
 * The first channel (ide0) lives at mac-io offset 0x20000, the second (ide1)
 * at 0x21000; interrupts are delivered through the mac-io interrupt controller
 * (see the heathrow driver) at IRQ 0x0D (ide0) and 0x0E (ide1). DMA (DBDMA) is
 * not yet implemented - the channel advertises PIO-only.
 */

#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <ata_types.h>
#include <bus/ATA.h>
#include <bus/PCI.h>
#include <device_manager.h>


#define MACIO_ATA_CONTROLLER_MODULE_NAME	"busses/ata/macio/driver_v1"
#define MACIO_ATA_CHANNEL_MODULE_NAME		"busses/ata/macio/channel/v1"

// our private channel-node attributes
#define MACIO_ATA_REG_OFFSET	"macio_ata/reg_offset"	// uint32, within mac-io
#define MACIO_ATA_IRQ			"macio_ata/irq"			// uint8
#define MACIO_ATA_CHANNEL_INDEX	"macio_ata/channel_index"	// uint8

// mac-io register layout: ATA task-file registers are 0x10 bytes apart
#define MACIO_ATA_REG_SHIFT		4
// control / alt-status register index (Apple ASIC)
#define MACIO_ATA_ALTSTATUS_REG	0x16
// channel base offsets within the mac-io register space
#define MACIO_ATA_IDE0_OFFSET	0x20000
#define MACIO_ATA_IDE1_OFFSET	0x21000
#define MACIO_ATA_IDE0_IRQ		0x0D
#define MACIO_ATA_IDE1_IRQ		0x0E

#define CHECK_RET(err) { status_t _err = (err); if (_err < B_OK) return _err; }


// A mac-io ATA cell: its register-block offset within the mac-io register
// space and its ATA (task-file) interrupt number. DMA is not used (PIO only),
// so the separate DMA interrupt in the device tree is ignored.
struct macio_ata_channel_def {
	uint32	reg_offset;
	uint8	irq;
};

#define MACIO_ATA_MAX_CHANNELS	3

struct macio_ata_supported_device {
	uint16	vendor_id;
	uint16	device_id;
	uint8	channel_count;
	macio_ata_channel_def channels[MACIO_ATA_MAX_CHANNELS];
};

static macio_ata_supported_device sSupportedDevices[] = {
	// Grand Central: a single ata-3 channel.
	{ 0x106b, 0x0002, 1, {{ 0x20000, 0x0D }} },
	// Heathrow / Paddington: two ata-3 channels (what dingusppc emulates).
	{ 0x106b, 0x0010, 2, {{ 0x20000, 0x0D }, { 0x21000, 0x0E }} },
	{ 0x106b, 0x0017, 2, {{ 0x20000, 0x0D }, { 0x21000, 0x0E }} },
	// KeyLargo (Power Mac G4 etc.): the internal Ultra-ATA/66 bus is the OF
	// "ata-4" cell at mac-io offset 0x1f000 (ATA IRQ 0x13) -- this is where the
	// boot drive lives. The two legacy ata-3 cells (0x20000/0x21000) remain for
	// optical / media-bay devices.
	{ 0x106b, 0x0022, 1, {{ 0x1f000, 0x13 }} },
	// Pangea (iBook G3 "dual USB" = PowerBook4,1, iMac G3 summer 2001, eMac):
	// a KeyLargo-derived mac-io (PCI device 0x0025, model "AAPL,Keylargo").
	// Its OF device tree has a SINGLE ata cell, ata-4@1f000 (compatible
	// "keylargo-ata", reg offset 0x1f000, interrupt 0x13) carrying the internal
	// Ultra-ATA/66 hard disk - the same layout as KeyLargo. It has NO ata-3
	// cells, so publish only ata-4; probing the absent 0x20000/0x21000 cells
	// touches bogus mac-io registers and corrupts memory (confirmed on a G3).
	{ 0x106b, 0x0025, 1, {{ 0x1f000, 0x13 }} },
	{ 0, 0, 0, {} }
};


struct macio_ata_controller_info {
	device_node*				node;
	pci_device_module_info*		pci;
	pci_device*					device;
	macio_ata_supported_device*	supported;

	area_id						register_area;
	addr_t						register_base;	// virtual base of mac-io regs
};


struct macio_ata_channel_info {
	device_node*				node;
	ata_channel					ataChannel;
	macio_ata_controller_info*	controller;

	addr_t						channel_base;	// virtual, points at reg 0 (data)
	uint8						irq;
	uint8						channel_index;
	bool						lost;
};


static ata_for_controller_interface*	sATA;
static device_manager_info*				sDeviceManager;
static bool								sControllerRegistered = false;


static macio_ata_supported_device*
macio_ata_check_supported(uint16 vendorID, uint16 deviceID)
{
	for (macio_ata_supported_device* d = sSupportedDevices; d->vendor_id; d++) {
		if (d->vendor_id == vendorID && d->device_id == deviceID)
			return d;
	}
	return NULL;
}


// mac-io task-file register access: register N lives at N << 4 from the
// channel base, one byte wide. The registers are big-endian device memory.
static inline uint8
macio_read_reg(macio_ata_channel_info* channel, int reg)
{
	uint8 value = *(volatile uint8*)(channel->channel_base
		+ ((addr_t)reg << MACIO_ATA_REG_SHIFT));
	asm volatile("eieio" ::: "memory");
	return value;
}


static inline void
macio_write_reg(macio_ata_channel_info* channel, int reg, uint8 value)
{
	*(volatile uint8*)(channel->channel_base
		+ ((addr_t)reg << MACIO_ATA_REG_SHIFT)) = value;
	asm volatile("eieio" ::: "memory");
}


// #pragma mark - ata_controller_interface


static void
macio_ata_set_channel(void* cookie, ata_channel channel)
{
	macio_ata_channel_info* info = (macio_ata_channel_info*)cookie;
	info->ataChannel = channel;
}


static status_t
macio_ata_write_command_block_regs(void* cookie, ata_task_file* tf,
	ata_reg_mask mask)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel->lost)
		return B_ERROR;

	// tf->raw.r[i] is ATA register (i + 1) (register 0 is the data port)
	for (int i = 0; i < 7; i++) {
		// LBA48 high bytes are written first (same register, twice)
		if (((1 << (i + 7)) & mask) != 0)
			macio_write_reg(channel, i + 1, tf->raw.r[i + 7]);
		if (((1 << i) & mask) != 0)
			macio_write_reg(channel, i + 1, tf->raw.r[i]);
	}

	return B_OK;
}


static status_t
macio_ata_read_command_block_regs(void* cookie, ata_task_file* tf,
	ata_reg_mask mask)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel->lost)
		return B_ERROR;

	for (int i = 0; i < 7; i++) {
		if (((1 << i) & mask) != 0)
			tf->raw.r[i] = macio_read_reg(channel, i + 1);
	}

	return B_OK;
}


static uint8
macio_ata_get_altstatus(void* cookie)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel->lost)
		return 0x01;	// error bit
	return macio_read_reg(channel, MACIO_ATA_ALTSTATUS_REG);
}


static status_t
macio_ata_write_device_control(void* cookie, uint8 val)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel->lost)
		return B_ERROR;
	macio_write_reg(channel, MACIO_ATA_ALTSTATUS_REG, val);
	return B_OK;
}


// PIO data transfer through the 16-bit data register (reg 0, offset 0). The
// mac-io returns the two data bytes in on-disk order for a plain big-endian
// 16-bit access, so the resulting byte stream matches what the ATA stack
// expects (identical to an x86 in/out of the data port).
static status_t
macio_ata_write_pio(void* cookie, uint16* data, int count, bool force16Bit)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel->lost)
		return B_ERROR;

	volatile uint16* dataReg = (volatile uint16*)channel->channel_base;
	for (; count > 0; --count) {
		*dataReg = *(data++);
		asm volatile("eieio" ::: "memory");
	}

	return B_OK;
}


static status_t
macio_ata_read_pio(void* cookie, uint16* data, int count, bool force16Bit)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel->lost)
		return B_ERROR;

	volatile uint16* dataReg = (volatile uint16*)channel->channel_base;
	for (; count > 0; --count) {
		*(data++) = *dataReg;
		asm volatile("eieio" ::: "memory");
	}

	return B_OK;
}


static status_t
macio_ata_prepare_dma(void* cookie, const physical_entry* sgList,
	size_t sgListCount, bool toDevice)
{
	return B_NOT_SUPPORTED;
}


static status_t
macio_ata_start_dma(void* cookie)
{
	return B_NOT_SUPPORTED;
}


static status_t
macio_ata_finish_dma(void* cookie)
{
	return B_NOT_SUPPORTED;
}


// #pragma mark - interrupt handler


static int32
macio_ata_interrupt_handler(void* arg)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)arg;
	if (channel->ataChannel == NULL)
		return B_UNHANDLED_INTERRUPT;

	// Reading the status register acknowledges the device's INTRQ. IRQ 0x0D/
	// 0x0E are dedicated to a single ATA channel, so any interrupt here is
	// ours to handle.
	uint8 status = macio_read_reg(channel, 0x07);

	return sATA->interrupt_handler(channel->ataChannel, status);
}


// #pragma mark - channel driver


static status_t
macio_ata_init_channel(device_node* node, void** cookie)
{
	uint32 regOffset;
	uint8 irq;
	uint8 channelIndex;
	if (sDeviceManager->get_attr_uint32(node, MACIO_ATA_REG_OFFSET, &regOffset,
			false) != B_OK
		|| sDeviceManager->get_attr_uint8(node, MACIO_ATA_IRQ, &irq, false)
			!= B_OK
		|| sDeviceManager->get_attr_uint8(node, MACIO_ATA_CHANNEL_INDEX,
			&channelIndex, false) != B_OK) {
		return B_ERROR;
	}

	macio_ata_controller_info* controller;
	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, NULL, (void**)&controller);
	sDeviceManager->put_node(parent);

	macio_ata_channel_info* channel
		= new(std::nothrow) macio_ata_channel_info;
	if (channel == NULL)
		return B_NO_MEMORY;

	channel->node = node;
	channel->ataChannel = NULL;
	channel->controller = controller;
	channel->channel_base = controller->register_base + regOffset;
	channel->irq = irq;
	channel->channel_index = channelIndex;
	channel->lost = false;

	status_t status = install_io_interrupt_handler(irq,
		macio_ata_interrupt_handler, channel, 0);
	if (status != B_OK) {
		dprintf("macio_ata: failed to install IRQ %u handler: %s\n", irq,
			strerror(status));
		delete channel;
		return status;
	}

	// disable device interrupts until the ATA stack enables them
	macio_write_reg(channel, MACIO_ATA_ALTSTATUS_REG,
		ATA_DEVICE_CONTROL_BIT3 | ATA_DEVICE_CONTROL_DISABLE_INTS);

	dprintf("macio_ata: channel %u ready (regs %p, IRQ %u)\n", channelIndex,
		(void*)channel->channel_base, irq);

	*cookie = channel;
	return B_OK;
}


static void
macio_ata_uninit_channel(void* cookie)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	remove_io_interrupt_handler(channel->irq, macio_ata_interrupt_handler,
		channel);
	delete channel;
}


static void
macio_ata_channel_removed(void* cookie)
{
	macio_ata_channel_info* channel = (macio_ata_channel_info*)cookie;
	if (channel != NULL)
		channel->lost = true;
}


// #pragma mark - controller driver


static float
macio_ata_supports_device(device_node* parent)
{
	const char* bus;
	uint16 vendorID;
	uint16 deviceID;

	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID,
			&vendorID, false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_ID, &deviceID,
			false) != B_OK) {
		return -1.0f;
	}

	if (strcmp(bus, "pci") != 0
		|| macio_ata_check_supported(vendorID, deviceID) == NULL) {
		return 0.0f;
	}

	return 0.6f;
}


static status_t
macio_ata_register_device(device_node* parent)
{
	// On some machines (notably the iBook G3) the mac-io PCI device is
	// enumerated as several device nodes; registering our controller under each
	// of them would create a separate ATA channel - and thus a duplicate
	// /dev/disk/ata/N entry - for the one physical disk. There is only ever a
	// single mac-io ATA controller, so register it exactly once.
	if (sControllerRegistered)
		return B_OK;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "mac-io ATA controller" }},
		{ ATA_CONTROLLER_CONTROLLER_NAME_ITEM, B_STRING_TYPE,
			{ .string = "mac-io ATA" }},
		// The mac-io ATA cells we support (KeyLargo/Pangea ata-4 internal
		// Ultra-ATA, and the Heathrow channels dingus emulates) each carry a
		// SINGLE master device - there is no slave. Probing a non-existent
		// slave floods the log with endless ATAPI "timeout waiting for data
		// request" retries (seen on the iBook G3), so only probe the master.
		{ ATA_CONTROLLER_MAX_DEVICES_ITEM, B_UINT8_TYPE, { .ui8 = 1 }},
		{ ATA_CONTROLLER_CAN_DMA_ITEM, B_UINT8_TYPE, { .ui8 = 0 }},
		{}
	};

	status_t status = sDeviceManager->register_node(parent,
		MACIO_ATA_CONTROLLER_MODULE_NAME, attrs, NULL, NULL);
	if (status == B_OK)
		sControllerRegistered = true;
	return status;
}


static status_t
macio_ata_init_controller(device_node* node, void** cookie)
{
	macio_ata_controller_info* controller
		= new(std::nothrow) macio_ata_controller_info;
	if (controller == NULL)
		return B_NO_MEMORY;

	controller->node = node;
	controller->register_area = -1;

	device_node* parent = sDeviceManager->get_parent_node(node);
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&controller->pci, (void**)&controller->device);
	sDeviceManager->put_node(parent);
	if (status != B_OK) {
		delete controller;
		return status;
	}

	pci_info pciInfo;
	controller->pci->get_pci_info(controller->device, &pciInfo);

	controller->supported = macio_ata_check_supported(pciInfo.vendor_id,
		pciInfo.device_id);
	if (controller->supported == NULL) {
		delete controller;
		return B_ERROR;
	}

	// make sure memory space decoding is enabled on the mac-io device
	uint16 pcicmd = controller->pci->read_pci_config(controller->device,
		PCI_command, 2);
	if ((pcicmd & PCI_command_memory) == 0) {
		controller->pci->write_pci_config(controller->device, PCI_command, 2,
			pcicmd | PCI_command_memory);
	}

	// map the whole mac-io register space (uncached device memory)
	phys_addr_t physicalBase = pciInfo.u.h0.base_registers[0];
	size_t size = pciInfo.u.h0.base_register_sizes[0];
	if (size < MACIO_ATA_IDE1_OFFSET + 0x1000)
		size = MACIO_ATA_IDE1_OFFSET + 0x1000;

	void* virtualBase = NULL;
	controller->register_area = map_physical_memory("mac-io ata registers",
		physicalBase, size, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (controller->register_area < 0) {
		status = controller->register_area;
		delete controller;
		return status;
	}
	controller->register_base = (addr_t)virtualBase;

	dprintf("macio_ata: controller at mac-io %#" B_PRIxPHYSADDR " (%u channel"
		"%s)\n", physicalBase, controller->supported->channel_count,
		controller->supported->channel_count == 1 ? "" : "s");

	*cookie = controller;
	return B_OK;
}


static void
macio_ata_uninit_controller(void* cookie)
{
	macio_ata_controller_info* controller
		= (macio_ata_controller_info*)cookie;
	if (controller->register_area >= 0)
		delete_area(controller->register_area);
	delete controller;
}


static status_t
macio_ata_publish_channel(device_node* controllerNode, uint32 regOffset,
	uint8 irq, uint8 index)
{
	char prettyName[32];
	snprintf(prettyName, sizeof(prettyName), "mac-io ATA Channel %u", index);

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = prettyName }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = ATA_FOR_CONTROLLER_MODULE_NAME }},
		{ ATA_CONTROLLER_CAN_DMA_ITEM, B_UINT8_TYPE, { .ui8 = 0 }},
		{ MACIO_ATA_REG_OFFSET, B_UINT32_TYPE, { .ui32 = regOffset }},
		{ MACIO_ATA_IRQ, B_UINT8_TYPE, { .ui8 = irq }},
		{ MACIO_ATA_CHANNEL_INDEX, B_UINT8_TYPE, { .ui8 = index }},
		{}
	};

	return sDeviceManager->register_node(controllerNode,
		MACIO_ATA_CHANNEL_MODULE_NAME, attrs, NULL, NULL);
}


static status_t
macio_ata_register_child_devices(void* cookie)
{
	macio_ata_controller_info* controller
		= (macio_ata_controller_info*)cookie;

	for (uint8 i = 0; i < controller->supported->channel_count; i++) {
		const macio_ata_channel_def& def = controller->supported->channels[i];
		CHECK_RET(macio_ata_publish_channel(controller->node, def.reg_offset,
			def.irq, i));
	}

	return B_OK;
}


// #pragma mark - module definitions


static ata_controller_interface sChannelInterface = {
	{
		{
			MACIO_ATA_CHANNEL_MODULE_NAME,
			0,
			NULL
		},

		NULL,	// supports_device
		NULL,	// register_device
		macio_ata_init_channel,
		macio_ata_uninit_channel,
		NULL,	// register_child_devices
		NULL,	// rescan
		macio_ata_channel_removed,
	},

	macio_ata_set_channel,

	macio_ata_write_command_block_regs,
	macio_ata_read_command_block_regs,

	macio_ata_get_altstatus,
	macio_ata_write_device_control,

	macio_ata_write_pio,
	macio_ata_read_pio,

	macio_ata_prepare_dma,
	macio_ata_start_dma,
	macio_ata_finish_dma,
};


static driver_module_info sControllerInterface = {
	{
		MACIO_ATA_CONTROLLER_MODULE_NAME,
		0,
		NULL
	},

	macio_ata_supports_device,
	macio_ata_register_device,
	macio_ata_init_controller,
	macio_ata_uninit_controller,
	macio_ata_register_child_devices,
	NULL,	// rescan
	NULL,	// device_removed
};


module_dependency module_dependencies[] = {
	{ ATA_FOR_CONTROLLER_MODULE_NAME, (module_info**)&sATA },
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};

module_info* modules[] = {
	(module_info*)&sControllerInterface,
	(module_info*)&sChannelInterface,
	NULL
};
