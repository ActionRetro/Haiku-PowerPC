/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sean Malseed, actionretro@pm.me
 *		Claude (Anthropic), paired via Claude Code
 *
 * OpenFirmware PCI host-bridge controller for PowerPC Macs.
 *
 * The Haiku PCI bus manager on ppc enumerates the bus itself (fBusEnumeration
 * = true), but that needs a pci_controller host bridge registered via
 * PCI::AddController() to provide config-space access. On x86 the host bridge
 * comes from ACPI; on arm/riscv from FDT. PowerPC Macs describe their host
 * bridge in the OpenFirmware device tree instead, and there was no ppc host
 * bridge driver at all - so device_manager saw an empty device tree and
 * nothing (disk, interrupt controller, ...) could ever be driven.
 *
 * This driver attaches to the device_manager root and provides config-space
 * access. The first target is the MPC106 "Grackle" host bridge used by the
 * iMac G3 and beige Power Mac G3: its config mechanism is the classic indirect
 * CONFIG_ADDR/CONFIG_DATA pair, memory-mapped at architecturally-fixed
 * physical addresses (0xFEC00000 / 0xFEE00000) and accessed little-endian (the
 * Grackle runs in PCI little-endian mode).
 *
 * NOTE: this driver deliberately makes no OpenFirmware client calls. The OF
 * client interface is not reliably callable this late in kernel boot - walking
 * the device tree at runtime jumps into OF ROM code the kernel MMU doesn't map
 * and faults. Everything here therefore relies on the fixed Grackle register
 * layout. Reading the host bridge's address windows from OF (for non-Grackle
 * bridges, or for PCI resource allocation) will require capturing that
 * information in the boot loader, where OF is fully alive, and passing it
 * through kernel_args.
 */

#include <new>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <AutoDeleterOS.h>
#include <bus/PCI.h>
#include <device_manager.h>


#define OF_PCI_DRIVER_MODULE_NAME	"busses/pci/openfirmware/driver_v1"

#define CHECK_RET(err) { status_t _err = (err); if (_err < B_OK) return _err; }


// Grackle (MPC106) indirect config registers. These physical addresses are
// architecturally fixed for the MPC106 and match the OpenFirmware device tree.
#define GRACKLE_CONFIG_ADDR		0xFEC00000
#define GRACKLE_CONFIG_DATA		0xFEE00000
#define GRACKLE_REGS_BASE		0xFEC00000
#define GRACKLE_REGS_SIZE		0x00300000	// covers ADDR, DATA and int-ack

// Grackle "Address Map B" host-side windows (CPU physical -> PCI). These are
// the standard MPC106 windows; they let the PCI stack understand where the
// already-assigned device BARs live.
#define GRACKLE_MMIO_HOST_BASE	0x80000000
#define GRACKLE_MMIO_PCI_BASE	0x80000000
#define GRACKLE_MMIO_SIZE		0x7D000000	// 0x80000000 .. 0xFCFFFFFF
#define GRACKLE_IO_HOST_BASE	0xFE000000
#define GRACKLE_IO_PCI_BASE		0x00000000
#define GRACKLE_IO_SIZE			0x00400000

// Host bridge types (must match arch_kernel_args.h).
#define PCI_HOST_BRIDGE_GRACKLE		0
#define PCI_HOST_BRIDGE_UNINORTH	1

extern "C" void ppc_get_pci_host_bridge(uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData);
extern "C" uint32 ppc_get_pci_host_bridge_count();
extern "C" status_t ppc_get_pci_host_bridge_at(uint32 index, uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData);
extern "C" uint32 ppc_get_gmac_irq();
extern "C" uint32 ppc_get_cardbus_irq();
extern "C" uint32 ppc_get_airport_irq();
extern "C" uint32 ppc_get_cardbus_mem_base();


device_manager_info* gDeviceManager;
pci_module_info* gPCI;


// The CardBus bring-up runs in the boot bridge's InitController, which happens
// too early in boot for its dprintf output to survive in the kernel syslog ring
// buffer. Accumulate the probe's output here so a later (captured) bridge can
// replay it into the log. See ProbeCardBus() / InitController().
static char sCardBusLog[8192];
static size_t sCardBusLogLen = 0;

static void
cb_log(const char* fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	dprintf("%s", line);
	size_t n = strlen(line);
	if (sCardBusLogLen + n + 1 < sizeof(sCardBusLog)) {
		memcpy(sCardBusLog + sCardBusLogLen, line, n);
		sCardBusLogLen += n;
		sCardBusLog[sCardBusLogLen] = '\0';
	}
}


class OpenFirmwarePCIController {
public:
	static float SupportsDevice(device_node* parent);
	static status_t RegisterDevice(device_node* parent);
	static status_t InitDriver(device_node* node,
		OpenFirmwarePCIController*& outDriver);
	void UninitDriver();

	status_t ReadConfig(uint8 bus, uint8 device, uint8 function,
		uint16 offset, uint8 size, uint32& value);
	status_t WriteConfig(uint8 bus, uint8 device, uint8 function,
		uint16 offset, uint8 size, uint32 value);

	status_t GetMaxBusDevices(int32& count);
	status_t GetRange(uint32 index, pci_resource_range* range);

	status_t ReadIrq(uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8& irq);
	status_t WriteIrq(uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 irq);

	status_t Finalize();

private:
	status_t InitController();
	void ProbeCardBus();

	inline void SetConfigAddress(uint8 bus, uint8 device, uint8 function,
		uint16 offset);

private:
	device_node*	fNode = NULL;

	AreaDeleter		fRegsArea;
	addr_t			fConfigAddr = 0;
	addr_t			fConfigData = 0;
	uint32			fHostBridgeType = PCI_HOST_BRIDGE_GRACKLE;
	uint32			fBridgeIndex = 0;
	addr_t			fConfigDataMask = 0x03;

	pci_resource_range	fRanges[4];
	uint32			fRangeCount = 0;
};


// #pragma mark - discovery / driver lifecycle


float
OpenFirmwarePCIController::SupportsDevice(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return -1.0f;
	}

	// Attach to the machine root. This driver only builds for ppc Open
	// Firmware machines, which always have a PCI host bridge.
	if (strcmp(bus, "root") != 0)
		return 0.0f;

	return 0.8f;
}


status_t
OpenFirmwarePCIController::RegisterDevice(device_node* parent)
{
	// A Power Mac G4 has several UniNorth PCI host bridges (the boot disk,
	// the built-in Ethernet, and FireWire can each live on a different one).
	// Register one controller node per bridge so each becomes its own PCI
	// domain and every bus gets enumerated.
	uint32 count = ppc_get_pci_host_bridge_count();
	if (count == 0)
		count = 1;

	status_t lastError = B_OK;
	for (uint32 i = 0; i < count; i++) {
		device_attr attrs[] = {
			{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
				{ .string = "OpenFirmware PCI Host Bridge" } },
			{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
				{ .string = "bus_managers/pci/root/driver_v1" } },
			{ "of_pci/bridge_index", B_UINT32_TYPE, { .ui32 = i } },
			{}
		};

		status_t error = gDeviceManager->register_node(parent,
			OF_PCI_DRIVER_MODULE_NAME, attrs, NULL, NULL);
		if (error != B_OK)
			lastError = error;
	}

	return lastError;
}


status_t
OpenFirmwarePCIController::InitDriver(device_node* node,
	OpenFirmwarePCIController*& outDriver)
{
	ObjectDeleter<OpenFirmwarePCIController> driver(
		new(std::nothrow) OpenFirmwarePCIController);
	if (!driver.IsSet())
		return B_NO_MEMORY;

	driver->fNode = node;

	CHECK_RET(driver->InitController());

	outDriver = driver.Detach();
	return B_OK;
}


void
OpenFirmwarePCIController::UninitDriver()
{
	delete this;
}



// The GMAC Ethernet clock is gated by the UniNorth (not KeyLargo) clock
// control register. Until it is running the GMAC's PCI config space reads
// all-ones and the bus scan cannot see it. Enable it before the Ethernet
// bridge (the UniNorth bus at config 0xf4800000) is enumerated. The UniNorth
// control registers live at physical 0xf8000000 on these Power Mac G4s; the
// clock control register is at +0x20 and is accessed little-endian. We only
// OR in the GMAC bit (read-modify-write), so other clocks are untouched.
// This mirrors Linux's core99_gmac_enable().
#define UNINORTH_PHYS_BASE		0xf8000000
#define UNI_N_CLOCK_CNTL		0x20
#define UNI_N_CLOCK_CNTL_GMAC		0x02
#define ETHERNET_BRIDGE_CONFIG_ADDR	0xf4800000
#define AIRPORT_BRIDGE_CONFIG_ADDR	0xf2800000

static void
enable_gmac_clock()
{
	void* regs = NULL;
	area_id area = map_physical_memory("uni-n clock cntl",
		UNINORTH_PHYS_BASE, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (area < B_OK) {
		dprintf("of_pci: GMAC clock enable: cannot map uni-n (%#lx)\n",
			(addr_t)area);
		return;
	}

	volatile uint32* clockCntl
		= (volatile uint32*)((addr_t)regs + UNI_N_CLOCK_CNTL);
	// UniNorth CONTROL registers are big-endian/native (unlike its PCI config
	// space, which is little-endian). Access this register natively.
	uint32 before = B_BENDIAN_TO_HOST_INT32(*clockCntl);
	*clockCntl = B_HOST_TO_BENDIAN_INT32(before | UNI_N_CLOCK_CNTL_GMAC);
	asm volatile("eieio" ::: "memory");
	(void)*clockCntl;
	asm volatile("eieio" ::: "memory");
	spin(20);
	uint32 after = B_BENDIAN_TO_HOST_INT32(*clockCntl);
	dprintf("of_pci: GMAC clock enable: UNI_N_CLOCK_CNTL %#08x -> %#08x\n",
		(unsigned)before, (unsigned)after);
	// Give the cell time to come out of clock-gating before it is probed.
	spin(3000);

	delete_area(area);
}


status_t
OpenFirmwarePCIController::InitController()
{
	// Which host bridge is this node? (Set by RegisterDevice.)
	uint32 index = 0;
	gDeviceManager->get_attr_uint32(fNode, "of_pci/bridge_index", &index,
		false);
	fBridgeIndex = index;

	uint32 type = PCI_HOST_BRIDGE_GRACKLE;
	phys_addr_t configAddrPhys = GRACKLE_CONFIG_ADDR;
	phys_addr_t configDataPhys = GRACKLE_CONFIG_DATA;
	if (ppc_get_pci_host_bridge_at(index, &type, &configAddrPhys,
			&configDataPhys) != B_OK) {
		ppc_get_pci_host_bridge(&type, &configAddrPhys, &configDataPhys);
	}
	fHostBridgeType = type;
	// UniNorth CONFIG_DATA is an 8-byte window (offset & 0x07); Grackle a
	// 4-byte one. The boot bridge (index 0) is deliberately kept on the
	// 4-byte access that every prior boot used: widening it there makes
	// odd-offset registers (header_type, BAR1/3/5) of the mac-io/ATA devices
	// read "correctly" and re-triggers a pre-existing KDiskDeviceManager
	// scan recursion during boot-volume mount. gem only needs correct access
	// on the Ethernet bridge, so apply the wider window to the non-boot
	// bridges only.
	fConfigDataMask = (type == PCI_HOST_BRIDGE_UNINORTH && fBridgeIndex != 0)
		? 0x07 : 0x03;

	// Map a window covering both config registers (uncached device memory).
	phys_addr_t lo = configAddrPhys < configDataPhys
		? configAddrPhys : configDataPhys;
	phys_addr_t hi = configAddrPhys > configDataPhys
		? configAddrPhys : configDataPhys;
	phys_addr_t regsBase = lo & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	phys_addr_t regsEnd = (hi + sizeof(uint32) + B_PAGE_SIZE - 1)
		& ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* regs = NULL;
	fRegsArea.SetTo(map_physical_memory("PCI host bridge config",
		regsBase, regsEnd - regsBase,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs));
	CHECK_RET(fRegsArea.Get());

	fConfigAddr = (addr_t)regs + (addr_t)(configAddrPhys - regsBase);
	fConfigData = (addr_t)regs + (addr_t)(configDataPhys - regsBase);

	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH) {
		// UniNorth host-side windows. Each bridge's registers sit at its OF
		// "reg" base; the config regs are base+0x800000, so recover the base.
		// The near I/O and MMIO windows are relative to that base; the main
		// 32-bit MMIO window is 1:1, so device BARs (incl. mac-io) are
		// CPU-addressable. (BARs are mapped directly by their physical value,
		// so these ranges only need to be good enough for resource lookup.)
		phys_addr_t base = configAddrPhys - 0x800000;

		pci_resource_range& mmio = fRanges[fRangeCount++];
		mmio = {};
		mmio.type = B_IO_MEMORY;
		mmio.address_type = PCI_address_type_32;
		mmio.host_address = 0x80000000;
		mmio.pci_address = 0x80000000;
		// 0x80000000..0xafffffff, forwarded 1:1 by UniNorth. Must extend past
		// the old 256MB (which ended at 0x8fffffff) to cover the CardBus card
		// memory window at 0x90000000: pci_ram_address() translates a device
		// BAR's PCI address to a host address via these ranges, and an address
		// just outside the range translated to 0 -- so a CardBus card's BAR
		// (0x90000000) came back as host address 0 and its driver mapped
		// nothing (master-abort). The socket regs at 0xa0000000 are also now
		// covered (they were only reachable before via a direct phys mapping).
		mmio.size = 0x30000000;

		pci_resource_range& mmio2 = fRanges[fRangeCount++];
		mmio2 = {};
		mmio2.type = B_IO_MEMORY;
		mmio2.address_type = PCI_address_type_32;
		mmio2.host_address = base + 0x01000000;
		mmio2.pci_address = base + 0x01000000;
		mmio2.size = 0x01000000;

		pci_resource_range& io = fRanges[fRangeCount++];
		io = {};
		io.type = B_IO_PORT;
		io.host_address = base;
		io.pci_address = 0x00000000;
		io.size = 0x00800000;
	} else {
		pci_resource_range& mmio = fRanges[fRangeCount++];
		mmio = {};
		mmio.type = B_IO_MEMORY;
		mmio.address_type = PCI_address_type_32;
		mmio.host_address = GRACKLE_MMIO_HOST_BASE;
		mmio.pci_address = GRACKLE_MMIO_PCI_BASE;
		mmio.size = GRACKLE_MMIO_SIZE;

		pci_resource_range& io = fRanges[fRangeCount++];
		io = {};
		io.type = B_IO_PORT;
		io.host_address = GRACKLE_IO_HOST_BASE;
		io.pci_address = GRACKLE_IO_PCI_BASE;
		io.size = GRACKLE_IO_SIZE;
	}

	dprintf("of_pci: %s host bridge %u ready (config %#lx/%#lx)\n",
		fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH ? "UniNorth" : "Grackle",
		(unsigned)fBridgeIndex, (addr_t)configAddrPhys, (addr_t)configDataPhys);

	// The GMAC Ethernet cell hangs off this bridge but is clock-gated until
	// enabled; do it now, before the PCI stack enumerates this domain.
	// The built-in AirPort hangs off the pci@f2000000 UniNorth bridge. The
	// loader already powered its radio via Apple OF (enable-cardslot /
	// init-cardslot-radio, see arch/ppc/mmu.cpp); here we force it to D0,
	// enable its memory decode, and program its (loader-resolved) interrupt
	// line before the PCI stack enumerates this domain, so bwi can map and
	// interrupt-drive it.
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH
			&& (addr_t)configAddrPhys == AIRPORT_BRIDGE_CONFIG_ADDR) {
		// The AirPort (BCM4306) is device 0x12 on this bridge. Powering the
		// KeyLargo slot (above) makes its config space fully answer, but the
		// radio itself comes up in a low PCI power state: until it is forced
		// to D0 via its PM capability, the command register's memory-enable
		// will not stick and any BAR access master-aborts (machine check).
		// Mirror the CardBus bring-up: walk the PM cap, force D0, then turn on
		// memory + bus-master decoding so the BAR responds when bwi probes.
		const uint8 apDev = 0x12;
		uint32 apVendor = 0;
		ReadConfig(0, apDev, 0, 0x00, 4, apVendor);
		if ((apVendor & 0xffff) == 0x14e4) {
			// Force the radio to full power (D0) via its PM capability, then
			// turn on memory + bus-master decoding so its BAR responds when bwi
			// probes. Config access to this device uses the correct 8-byte
			// CONFIG_DATA window (see ReadConfig/WriteConfig), so the command
			// register writes actually land now.
			uint32 capPtrReg = 0;
			ReadConfig(0, apDev, 0, 0x34, 1, capPtrReg);
			uint8 cap = capPtrReg & 0xfc;
			int capGuard = 0;
			while (cap != 0 && capGuard++ < 16) {
				uint32 capId = 0, capNext = 0;
				ReadConfig(0, apDev, 0, cap, 1, capId);
				ReadConfig(0, apDev, 0, cap + 1, 1, capNext);
				if ((capId & 0xff) == 0x01) {
					uint32 pmcsr = 0;
					ReadConfig(0, apDev, 0, cap + 4, 2, pmcsr);
					if ((pmcsr & 0x3) != 0) {
						WriteConfig(0, apDev, 0, cap + 4, 2, pmcsr & ~0x3u);
						spin(50000);
					}
					break;
				}
				cap = capNext & 0xfc;
			}
			uint32 cmd = 0;
			ReadConfig(0, apDev, 0, 0x04, 2, cmd);
			WriteConfig(0, apDev, 0, 0x04, 2,
				cmd | PCI_command_io | PCI_command_memory | PCI_command_master);
			spin(20000);
			uint32 cmdBack = 0, apBar0 = 0;
			ReadConfig(0, apDev, 0, 0x04, 2, cmdBack);
			ReadConfig(0, apDev, 0, 0x10, 4, apBar0);
			dprintf("of_pci: airport enabled: cmd %#x -> %#x (mem-enable %s), "
				"BAR0=%#x\n", (unsigned)cmd, (unsigned)cmdBack,
				(cmdBack & PCI_command_memory) ? "on" : "OFF",
				(unsigned)apBar0);
			// The radio's PCI interrupt_line is unrouted (reads 0xff); the
			// loader resolved its real OpenPIC input from the OF interrupt-map.
			// Program it so bwi's bus_alloc_resource(SYS_RES_IRQ) works.
			uint32 apIRQ = ppc_get_airport_irq();
			if (apIRQ != 0 && apIRQ < 0xff) {
				WriteConfig(0, apDev, 0, 0x3c, 1, apIRQ);
				dprintf("of_pci: airport irq_line set to %u\n", (unsigned)apIRQ);
			}
		}
	}

	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH
			&& (addr_t)configAddrPhys == ETHERNET_BRIDGE_CONFIG_ADDR) {
		enable_gmac_clock();

		// The GMAC's config interrupt_line register is unrouted (reads 0xff)
		// on these Macs; the boot loader resolved its real OpenPIC input from
		// the OF interrupt-map. Program it so the network driver's
		// bus_alloc_resource(SYS_RES_IRQ) finds a usable vector. (ethernet@f
		// is device 15 on this bridge.)
		uint32 gmacIRQ = ppc_get_gmac_irq();
		if (gmacIRQ != 0 && gmacIRQ < 0xff)
			WriteConfig(0, 15, 0, 0x3c, 1, gmacIRQ);
	}

	// If this bridge hosts a CardBus (PC Card) socket, bring it up so an
	// inserted CardBus card (e.g. a Sonnet Aria Extreme WiFi card) can be
	// enumerated. Open Firmware leaves the socket powered off.
	ProbeCardBus();

	// idx 0 (the boot bridge) hosts the CardBus but logs too early to be
	// captured; replay the accumulated probe log from a later bridge.
	// Replay from EVERY later bridge (not just the first): the boot bridge
	// (idx 0, which hosts the CardBus) logs too early to be captured, and its
	// init order relative to the other bridges varies, so a once-only replay can
	// fire before idx 0 has populated the buffer. Firing from each non-zero
	// bridge guarantees a captured copy once idx 0 has run.
	if (fBridgeIndex != 0 && sCardBusLogLen > 0) {
		dprintf("of_pci: cardbus-probe REPLAY (via bridge idx %u):\n%s",
			(unsigned)fBridgeIndex, sCardBusLog);
	}

	return B_OK;
}


// #pragma mark - CardBus (TI PCI1410) socket bring-up


// CardBus/Yenta socket registers, memory-mapped at the bridge's BAR0. They are
// little-endian (PCI memory space), so byte-swap on this big-endian host.
#define CB_SOCKET_EVENT		0x00
#define CB_SOCKET_MASK		0x04
#define CB_SOCKET_STATE		0x08
#define CB_SOCKET_FORCE		0x0c
#define CB_SOCKET_CONTROL	0x10
#define CB_SOCKET_POWER		0x20

// CB_SOCKET_STATE (Socket Present State) bits - Linux yenta convention
#define CB_SS_CARDSTS		0x00000001
#define CB_SS_CD1		0x00000002	// CDETECT1 (0 = pin grounded/made)
#define CB_SS_CD2		0x00000004	// CDETECT2 (0 = pin grounded/made)
#define CB_SS_CD		0x00000006	// both detects: card seated when == 0
#define CB_SS_PWRCYCLE		0x00000008	// power is applied
#define CB_SS_16BIT		0x00000010	// 16-bit PC Card present
#define CB_SS_CB		0x00000020	// CardBus (32-bit) card present
#define CB_SS_IREQ		0x00000040
#define CB_SS_NOTACARD		0x00000080	// unrecognized card / no valid card
#define CB_SS_DATALOST		0x00000100	// data lost
#define CB_SS_BADVCC		0x00000200	// bad Vcc request
#define CB_SS_5VCARD		0x00000400	// card wants 5V
#define CB_SS_3VCARD		0x00000800	// card wants 3.3V
#define CB_SS_5VSOCKET		0x10000000
#define CB_SS_3VSOCKET		0x20000000

// CB_SOCKET_CONTROL bits
#define CB_SC_VPP_MASK		0x00000007
#define CB_SC_VPP_OFF		0x00000000
#define CB_SC_VPP_5V		0x00000002
#define CB_SC_VPP_3V		0x00000003
#define CB_SC_VCC_MASK		0x00000070
#define CB_SC_VCC_OFF		0x00000000
#define CB_SC_VCC_5V		0x00000020
#define CB_SC_VCC_3V		0x00000030

// CardBus bridge (PCI header type 2) config registers
#define CB_PRIMARY_BUS		0x18	// PCI (primary) bus number
#define CB_CARDBUS_BUS		0x19	// CardBus (secondary) bus number
#define CB_SUBORDINATE_BUS	0x1a	// subordinate bus number
#define CB_LATENCY_TIMER	0x1b
#define CB_BRIDGE_CONTROL	0x3e
#define CB_BCR_CRST		0x0040	// CardBus card reset (1 = asserted)
#define CB_BCR_INTR_EXCA	0x0080	// 1 = route card ints to ExCA/ISA IRQ
#define CB_BCR_WRITE_POST	0x0400	// 1 = enable memory write posting to card

// The secondary bus number we assign to the CardBus socket. The UniNorth
// bridges on these Macs carry their devices directly on bus 0 with no
// PCI-to-PCI bridges, so bus 1 is free for the card behind the CardBus bridge.
#define CB_SECONDARY_BUS	1
// Memory window the CardBus bridge forwards to the card (from the OF
// cardbus node `ranges`: 0x90000000, 256MB). The card's BAR0 lives here.
#define CB_MEM_WINDOW_BASE	0x90000000


static inline uint32
cb_read32(addr_t base, uint32 off)
{
	uint32 v = *(volatile uint32*)(base + off);
	asm volatile("eieio" ::: "memory");
	return B_LENDIAN_TO_HOST_INT32(v);
}


static inline void
cb_write32(addr_t base, uint32 off, uint32 value)
{
	*(volatile uint32*)(base + off) = B_HOST_TO_LENDIAN_INT32(value);
	asm volatile("eieio" ::: "memory");
}


void
OpenFirmwarePCIController::ProbeCardBus()
{
	// CardBus only exists behind the UniNorth bridges here.
	if (fHostBridgeType != PCI_HOST_BRIDGE_UNINORTH)
		return;

	// Find a CardBus bridge (class 0x0607) on this domain's bus 0. Log every
	// bus-0 device so we can see the topology on machines where the CardBus
	// controller sits at a different device/bus.
	int cbDev = -1;
	uint32 vendorDevice = 0;
	for (int dev = 11; dev < 32; dev++) {
		uint32 vd = 0;
		ReadConfig(0, dev, 0, 0x00, 4, vd);
		if ((vd & 0xffff) == 0xffff)
			continue;
		uint32 classReg = 0;
		ReadConfig(0, dev, 0, 0x08, 4, classReg);
		cb_log("of_pci: cardbus-probe idx %u dev %d %04x:%04x class %02x/%02x\n",
			(unsigned)fBridgeIndex, dev, (unsigned)(vd & 0xffff),
			(unsigned)(vd >> 16), (unsigned)((classReg >> 24) & 0xff),
			(unsigned)((classReg >> 16) & 0xff));
		if (((classReg >> 24) & 0xff) == 0x06
			&& ((classReg >> 16) & 0xff) == 0x07) {
			cbDev = dev;
			vendorDevice = vd;
			break;
		}
	}
	if (cbDev < 0) {
		cb_log("of_pci: cardbus-probe idx %u: no CardBus bridge on bus 0\n",
			(unsigned)fBridgeIndex);
		return;
	}

	// The bridge's odd-dword config registers (command @0x04, MemBase0 @0x1c,
	// bridge-control/reset @0x3e) need the correct 8-byte CONFIG_DATA window.
	// The boot bridge is otherwise kept on the narrow 4-byte window; widen it
	// here only for the CardBus bridge (dev >= 11) and the card's own bus, so
	// the mac-io/ATA disk-scan quirk is not re-triggered.
	addr_t savedMask = fConfigDataMask;
	fConfigDataMask = 0x07;

	// Default the CardBus bus number to 0 so the PCI enumerator never recurses
	// a stale/bogus value if we bail before assigning it below.
	WriteConfig(0, cbDev, 0, CB_CARDBUS_BUS, 1, 0);
	WriteConfig(0, cbDev, 0, CB_SUBORDINATE_BUS, 1, 0);

	// Map the socket registers (bridge BAR0).
	uint32 bar0 = 0;
	ReadConfig(0, cbDev, 0, 0x10, 4, bar0);
	phys_addr_t socketPhys = bar0 & ~(phys_addr_t)0xf;
	if (socketPhys == 0) {
		fConfigDataMask = savedMask;
		return;
	}
	void* socketRegs = NULL;
	area_id socketArea = map_physical_memory("cardbus socket", socketPhys,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &socketRegs);
	if (socketArea < B_OK) {
		fConfigDataMask = savedMask;
		return;
	}
	addr_t sock = (addr_t)socketRegs;

	// Force the controller to full power (D0) via its PM capability, and make
	// sure its decoders are on, before trusting card detection.
	uint32 capPtrReg = 0;
	ReadConfig(0, cbDev, 0, 0x34, 1, capPtrReg);
	uint8 cap = capPtrReg & 0xfc;
	int capGuard = 0;
	while (cap != 0 && capGuard++ < 16) {
		uint32 capId = 0, capNext = 0;
		ReadConfig(0, cbDev, 0, cap, 1, capId);
		ReadConfig(0, cbDev, 0, cap + 1, 1, capNext);
		if ((capId & 0xff) == 0x01) {
			uint32 pmcsr = 0;
			ReadConfig(0, cbDev, 0, cap + 4, 2, pmcsr);
			if ((pmcsr & 0x3) != 0) {
				WriteConfig(0, cbDev, 0, cap + 4, 2, pmcsr & ~0x3u);
				spin(50000);
			}
			break;
		}
		cap = capNext & 0xfc;
	}
	uint32 cmdReg = 0;
	ReadConfig(0, cbDev, 0, 0x04, 2, cmdReg);
	WriteConfig(0, cbDev, 0, 0x04, 2,
		cmdReg | PCI_command_io | PCI_command_memory | PCI_command_master);
	spin(20000);

	// TI CardBus controller init (mirrors Linux yenta ti12xx_override): keep the
	// CardBus clock running (KEEPCLK) and enable memory burst in the System
	// Control register (0x80). Without a running socket clock the controller's
	// card-detect / voltage-sense logic is frozen -- it misreads a card-detect
	// pin and rejects Vcc (BADVCC), which is why a cold Haiku boot cannot see a
	// card that OS X sees fine. Also route card interrupts to PCI (Device
	// Control 0x92 interrupt-mode = all-PCI).
	// System Control (0x80): memory burst, keep the CardBus clock, and mark the
	// socket active. (Real TI113X bit values: MRBURSTUP|DN=0xc000, KEEPCLK=0x02,
	// SOCACTIVE=0x2000.)
	#define TI_SCR_MRBURST		0x0000c000
	#define TI_SCR_KEEPCLK		0x00000002
	#define TI_SCR_SOCACTIVE	0x00002000
	uint32 sysctrl = 0;
	ReadConfig(0, cbDev, 0, 0x80, 4, sysctrl);
	WriteConfig(0, cbDev, 0, 0x80, 4,
		sysctrl | TI_SCR_MRBURST | TI_SCR_KEEPCLK | TI_SCR_SOCACTIVE);
	// Device Control (0x92): route card interrupts to PCI (IMODE = 0x06); leave
	// the voltage-sense force bits at their power-on values (forcing them jams
	// the sense).
	uint32 devctrl = 0;
	ReadConfig(0, cbDev, 0, 0x92, 1, devctrl);
	WriteConfig(0, cbDev, 0, 0x92, 1, (devctrl & ~0x06u) | 0x06u);
	spin(50000);
	uint32 sysctrlBack = 0;
	ReadConfig(0, cbDev, 0, 0x80, 4, sysctrlBack);
	cb_log("of_pci: CardBus TI init: SysCtrl %#x -> %#x\n",
		(unsigned)sysctrl, (unsigned)sysctrlBack);

	// Interrogate the socket (Linux yenta_sock_init / yenta_interrogate): zero
	// the ExCA global + general control, then force a Card Voltage-Sense test so
	// the controller RE-EVALUATES card presence and voltage. On a cold power-on
	// (OF never inits CardBus) the detect/voltage bits are stale -> NOTACARD and
	// Vcc requests fail with BADVCC. CB_CVSTEST is the missing step that makes
	// detection work (it is why OS X sees the card and our minimal init did not).
	#define CB_SOCKET_FORCE_REG	0x0c
	#define CB_CVSTEST		0x00004000
	#define CB_CDMASK		0x00000006
	#define I365_GBLCTL		0x1e
	#define I365_GENCTL		0x16
	*(volatile uint8*)(sock + 0x800 + I365_GBLCTL) = 0;
	*(volatile uint8*)(sock + 0x800 + I365_GENCTL) = 0;
	asm volatile("eieio" ::: "memory");
	cb_write32(sock, CB_SOCKET_MASK, CB_CDMASK);

	// Poll the interrogate: re-issue the Card Voltage-Sense test until the
	// controller reports a card type (CardBus/16-bit) or we time out (~4s).
	// Cold-inserted card-detect can take several passes to latch.
	uint32 state = 0;
	for (int i = 0; i < 25; i++) {
		cb_write32(sock, CB_SOCKET_EVENT, 0xffffffff);
		cb_write32(sock, CB_SOCKET_FORCE_REG, CB_CVSTEST);
		spin(150000);
		state = cb_read32(sock, CB_SOCKET_STATE);
		if ((state & (CB_SS_CB | CB_SS_16BIT)) != 0)
			break;
	}

	// Cross-check card presence via the ExCA (16-bit) interface status byte at
	// BAR0 + 0x800 + 0x01 (card fully present when both detect bits 0x0c set).
	uint8 excaStatus = *(volatile uint8*)(sock + 0x801);
	bool excaPresent = ((excaStatus & 0x0c) == 0x0c);
	cb_log("of_pci: CardBus (TI %04x) BAR0 %#lx: state %#x, ExCA status %#x "
		"(%s)\n", (unsigned)(vendorDevice >> 16), (addr_t)socketPhys,
		(unsigned)state, (unsigned)excaStatus,
		excaPresent ? "ExCA:card-present" : "ExCA:absent");

	// Data-gathering: dump the TI controller's config registers (incl. the
	// MFUNC routing at 0x8c) and the ExCA base registers, to find the
	// board-specific pin routing OS X programs that makes card-detect work.
	uint32 r80 = 0, r84 = 0, r88 = 0, r8c = 0, r90 = 0;
	ReadConfig(0, cbDev, 0, 0x80, 4, r80);
	ReadConfig(0, cbDev, 0, 0x84, 4, r84);
	ReadConfig(0, cbDev, 0, 0x88, 4, r88);
	ReadConfig(0, cbDev, 0, 0x8c, 4, r8c);
	ReadConfig(0, cbDev, 0, 0x90, 4, r90);
	cb_log("of_pci: CardBus TIcfg 80=%#x 84=%#x 88=%#x 8c=%#x 90=%#x\n",
		(unsigned)r80, (unsigned)r84, (unsigned)r88, (unsigned)r8c,
		(unsigned)r90);
	cb_log("of_pci: CardBus ExCA 00=%#x 01=%#x 02=%#x 03=%#x 05=%#x 06=%#x\n",
		(unsigned)*(volatile uint8*)(sock + 0x800),
		(unsigned)*(volatile uint8*)(sock + 0x801),
		(unsigned)*(volatile uint8*)(sock + 0x802),
		(unsigned)*(volatile uint8*)(sock + 0x803),
		(unsigned)*(volatile uint8*)(sock + 0x805),
		(unsigned)*(volatile uint8*)(sock + 0x806));

	// The CardBus present-state register has proven unreliable here (OS X sees
	// the card at the same physical seat), so do NOT trust its card-type bits.
	// Clear stale events and attempt a 3.3V bring-up regardless; the card's
	// config-space response on the secondary bus is the ground truth. 3.3V is
	// the correct and safe voltage for a CardBus card (and harmless if empty).
	cb_write32(sock, CB_SOCKET_EVENT, 0xffffffff);
	uint32 control = CB_SC_VCC_3V | CB_SC_VPP_3V;
	cb_write32(sock, CB_SOCKET_CONTROL, control);
	spin(300000);	// let Vcc ramp and settle
	uint32 pstate = cb_read32(sock, CB_SOCKET_STATE);
	cb_log("of_pci: CardBus powered (control %#x): state %#x%s%s\n",
		(unsigned)control, (unsigned)pstate,
		(pstate & CB_SS_PWRCYCLE) ? " power-on" : " no-power",
		(pstate & CB_SS_BADVCC) ? " BAD-VCC" : "");

	// Assign bus numbers so the bridge forwards config cycles to the card, and
	// route card interrupts to PCI INTA (not the ExCA/ISA IRQ).
	WriteConfig(0, cbDev, 0, CB_PRIMARY_BUS, 1, 0);
	WriteConfig(0, cbDev, 0, CB_CARDBUS_BUS, 1, CB_SECONDARY_BUS);
	WriteConfig(0, cbDev, 0, CB_SUBORDINATE_BUS, 1, CB_SECONDARY_BUS);
	WriteConfig(0, cbDev, 0, CB_LATENCY_TIMER, 1, 0x20);

	// Pulse CardBus reset: assert, settle, de-assert.
	uint32 bridgeControl = 0;
	ReadConfig(0, cbDev, 0, CB_BRIDGE_CONTROL, 2, bridgeControl);
	WriteConfig(0, cbDev, 0, CB_BRIDGE_CONTROL, 2,
		(bridgeControl | CB_BCR_CRST) & ~CB_BCR_INTR_EXCA);
	spin(20000);
	WriteConfig(0, cbDev, 0, CB_BRIDGE_CONTROL, 2,
		bridgeControl & ~(CB_BCR_CRST | CB_BCR_INTR_EXCA | CB_BCR_WRITE_POST));
	spin(100000);

	uint32 command = 0;
	ReadConfig(0, cbDev, 0, 0x04, 2, command);
	WriteConfig(0, cbDev, 0, 0x04, 2,
		command | PCI_command_io | PCI_command_memory | PCI_command_master);

	// Probe the card now sitting on the secondary bus.
	uint32 cardId = 0;
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x00, 4, cardId);
	if ((cardId & 0xffff) == 0xffff) {
		cb_log("of_pci: CardBus: powered but card did NOT respond on bus %d\n",
			CB_SECONDARY_BUS);
		// No card responded; power the socket down and make sure the enumerator
		// won't recurse a bogus bus.
		cb_write32(sock, CB_SOCKET_CONTROL, 0);
		WriteConfig(0, cbDev, 0, CB_CARDBUS_BUS, 1, 0);
		WriteConfig(0, cbDev, 0, CB_SUBORDINATE_BUS, 1, 0);
		fConfigDataMask = savedMask;
		delete_area(socketArea);
		return;
	}

	uint32 cardClass = 0;
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x08, 4, cardClass);

	// Give the card a memory BAR inside the bridge's forwarding window and
	// enable it, so the PCI stack and the driver can reach its registers. Use
	// the memory-window base the loader captured from the OF `ranges` (the
	// address the host bridge actually forwards); fall back to the compiled
	// default only if unavailable.
	uint32 loaderMemBase = ppc_get_cardbus_mem_base();
	// The socket registers (bridge BAR0, socketPhys) are readable, so the host
	// bridge definitely forwards that region. Place the card window just above
	// them; if this works, 0x90000000 simply wasn't in this machine's forwarded
	// MMIO range. Prefer the loader-captured OF base when available.
	// Use the OF-designated memory-window base (guaranteed forwarded by the host
	// bridge); fall back to just above the socket regs.
	uint32 cardMemBase = (loaderMemBase != 0)
		? loaderMemBase : ((uint32)socketPhys + 0x00100000);
	cb_log("of_pci: cardbus mem: loader=%#x socket=%#lx using=%#x\n",
		(unsigned)loaderMemBase, (addr_t)socketPhys, (unsigned)cardMemBase);
	WriteConfig(CB_SECONDARY_BUS, 0, 0, 0x10, 4, 0xffffffff);
	uint32 barSizeMask = 0;
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x10, 4, barSizeMask);
	uint32 barSize = (barSizeMask == 0 || barSizeMask == 0xffffffff)
		? 0 : (~(barSizeMask & ~0xfu) + 1);
	WriteConfig(CB_SECONDARY_BUS, 0, 0, 0x10, 4, cardMemBase);
	WriteConfig(0, cbDev, 0, 0x1c, 4, cardMemBase);
	WriteConfig(0, cbDev, 0, 0x20, 4, cardMemBase + 0x000ff000);
	// Disable the bridge's unused windows (Mem1 @0x24/0x28, IO0 @0x2c/0x30,
	// IO1 @0x34/0x38) by making base > limit, so stale values can't interfere.
	WriteConfig(0, cbDev, 0, 0x24, 4, 0xfffff000);
	WriteConfig(0, cbDev, 0, 0x28, 4, 0x00000000);
	WriteConfig(0, cbDev, 0, 0x2c, 4, 0xfffff000);
	WriteConfig(0, cbDev, 0, 0x30, 4, 0x00000000);
	WriteConfig(0, cbDev, 0, 0x34, 4, 0xfffff000);
	WriteConfig(0, cbDev, 0, 0x38, 4, 0x00000000);
	uint32 cardCmd = 0;
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x04, 2, cardCmd);
	// Enable memory decode only; the driver enables bus mastering when it
	// initializes the card (avoids stray DMA from an uninitialized chip).
	WriteConfig(CB_SECONDARY_BUS, 0, 0, 0x04, 2, cardCmd | PCI_command_memory);

	cb_log("of_pci: *** CardBus card up on bus %d: vendor %04x device %04x "
		"class %02x/%02x, BAR0 %#x (size %#x) ***\n", CB_SECONDARY_BUS,
		(unsigned)(cardId & 0xffff), (unsigned)(cardId >> 16),
		(unsigned)((cardClass >> 24) & 0xff), (unsigned)((cardClass >> 16) & 0xff),
		(unsigned)cardMemBase, (unsigned)barSize);

	// Interrupt routing: the card's INTA is routed through the CardBus bridge,
	// so it shares the bridge's IRQ. OpenFirmware programs the bridge's
	// interrupt_line from its interrupt-map; copy it to the card so the driver's
	// bus_alloc_resource(SYS_RES_IRQ) finds a usable vector (mirrors the gem fix).
	uint32 brIntLine = 0, brIntPin = 0, cardIntLine = 0, cardIntPin = 0;
	ReadConfig(0, cbDev, 0, 0x3c, 1, brIntLine);
	ReadConfig(0, cbDev, 0, 0x3d, 1, brIntPin);
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x3c, 1, cardIntLine);
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x3d, 1, cardIntPin);
	cb_log("of_pci: CardBus IRQ: bridge intline %#x pin %#x, card intline %#x "
		"pin %#x\n", (unsigned)brIntLine, (unsigned)brIntPin,
		(unsigned)cardIntLine, (unsigned)cardIntPin);
	// The bridge's interrupt_line is unrouted (0xff) on these Macs; use the
	// OpenPIC input the loader resolved from the OF interrupt-map, and program
	// it into BOTH the bridge and the card (the card's INTA routes through the
	// bridge) so the driver's bus_alloc_resource(SYS_RES_IRQ) succeeds.
	uint32 cardbusIRQ = ppc_get_cardbus_irq();
	if (cardbusIRQ != 0 && cardbusIRQ < 0xff) {
		WriteConfig(0, cbDev, 0, 0x3c, 1, cardbusIRQ);
		WriteConfig(CB_SECONDARY_BUS, 0, 0, 0x3c, 1, cardbusIRQ);
		cb_log("of_pci: CardBus: programmed IRQ %u into bridge + card\n",
			(unsigned)cardbusIRQ);
	} else {
		cb_log("of_pci: CardBus: no loader-resolved IRQ (got %#x)\n",
			(unsigned)cardbusIRQ);
	}

	// Read back the bridge memory-window + card-BAR config to verify the card's
	// registers are being forwarded (config reads only - safe, no bus hang).
	uint32 wBase = 0, wLimit = 0, wBrCmd = 0, wBrCtl = 0, wCardBar = 0, wCardCmd = 0;
	ReadConfig(0, cbDev, 0, 0x1c, 4, wBase);
	ReadConfig(0, cbDev, 0, 0x20, 4, wLimit);
	ReadConfig(0, cbDev, 0, 0x04, 2, wBrCmd);
	ReadConfig(0, cbDev, 0, 0x3e, 2, wBrCtl);
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x10, 4, wCardBar);
	ReadConfig(CB_SECONDARY_BUS, 0, 0, 0x04, 2, wCardCmd);
	cb_log("of_pci: CardBus window: MemBase0 %#x MemLimit0 %#x brCmd %#x "
		"brCtl %#x | card BAR0 %#x cmd %#x\n", (unsigned)wBase, (unsigned)wLimit,
		(unsigned)wBrCmd, (unsigned)wBrCtl, (unsigned)wCardBar, (unsigned)wCardCmd);

	fConfigDataMask = savedMask;
	delete_area(socketArea);
}


// #pragma mark - config space access (Grackle indirect)


void
OpenFirmwarePCIController::SetConfigAddress(uint8 bus, uint8 device,
	uint8 function, uint16 offset)
{
	uint32 address;
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH) {
		// UniNorth: bus 0 uses a 1-hot IDSEL in the high bits; other buses
		// use type-1 config (low bit set).
		if (bus == 0) {
			address = (1u << device) | ((uint32)function << 8)
				| (offset & 0xFC);
		} else {
			address = ((uint32)bus << 16) | ((uint32)device << 11)
				| ((uint32)function << 8) | (offset & 0xFC) | 1;
		}
	} else {
		address = 0x80000000 | ((uint32)bus << 16)
			| ((uint32)device << 11) | ((uint32)function << 8)
			| (offset & 0xFC);
	}

	// Both bridges run PCI little-endian: a byte-reversed store latches the
	// natural CONFIG_ADDR value (equivalent to PowerPC out_le32).
	*(volatile uint32*)fConfigAddr = B_HOST_TO_LENDIAN_INT32(address);
	asm volatile("eieio" ::: "memory");
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH) {
		// UniNorth returns garbage unless the address register is read back.
		(void)*(volatile uint32*)fConfigAddr;
		asm volatile("eieio" ::: "memory");
	}
}


status_t
OpenFirmwarePCIController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus == 0
			&& device < 11) {
		value = 0xffffffff;
		return B_OK;
	}
	SetConfigAddress(bus, device, function, offset);

	// UniNorth exposes CONFIG_DATA as an 8-byte window: the register byte is
	// at cfg_data + (offset & 0x07), NOT (offset & 0x03). Getting this wrong
	// makes every odd dword (command @0x04, BAR1 @0x14, ...) alias the wrong
	// location - which is why the GMAC command register could not be written.
	// Secondary buses (e.g. the CardBus card behind the TI1410) always need
	// the correct 8-byte window; only the boot bridge's own bus 0 is pinned
	// narrow for the disk-scan quirk.
	addr_t effMask = fConfigDataMask;
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus != 0)
		effMask = 0x07;
	// The built-in AirPort (dev 0x12) on the boot bridge needs the CORRECT
	// UniNorth 8-byte CONFIG_DATA window: its odd-dword registers (command
	// @0x04, latency @0x0c) live at cfg_data + (offset & 0x07), not & 0x03.
	// The boot bridge is otherwise pinned narrow so mac-io/ATA header-type
	// reads stay "wrong" and do not re-trigger a KDiskDeviceManager scan
	// recursion; the radio is not a disk device, so the correct window is safe
	// here and is what lets its command register (memory-decode enable, and
	// bwi_pci_attach->pci_enable_io) actually be written.
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus == 0 && device == 0x12)
		effMask = 0x07;
	addr_t data = fConfigData + (offset & effMask);
	switch (size) {
		case 1:
			value = *(volatile uint8*)data;
			break;
		case 2:
			value = B_LENDIAN_TO_HOST_INT16(*(volatile uint16*)data);
			break;
		case 4:
			value = B_LENDIAN_TO_HOST_INT32(*(volatile uint32*)data);
			break;
		default:
			return B_BAD_VALUE;
	}
	asm volatile("eieio" ::: "memory");

	return B_OK;
}


status_t
OpenFirmwarePCIController::WriteConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32 value)
{
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus == 0
			&& device < 11) {
		return B_OK;
	}
	if (size != 1 && size != 2 && size != 4)
		return B_BAD_VALUE;

	// The UniNorth host bridge only reliably accepts 32-bit writes to its
	// CONFIG_DATA register; a narrower store to a byte lane does not stick
	// (this is why enabling the GMAC's command-register memory bit silently
	// failed). Perform sub-dword writes as a 32-bit read-modify-write.
	// See ReadConfig: the register dword lives at cfg_data + (offset & mask)
	// where mask is 0x07 on UniNorth. The bridge only reliably accepts 32-bit
	// CONFIG_DATA stores, so sub-dword writes are done as a read-modify-write
	// of the containing 32-bit word.
	addr_t effMask = fConfigDataMask;
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus != 0)
		effMask = 0x07;
	// AirPort (dev 0x12): correct 8-byte window so command-register writes land
	// (see ReadConfig). Without this, io/mem-enable writes hit the wrong lane
	// and are dropped, and bwi cannot map the radio.
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus == 0 && device == 0x12)
		effMask = 0x07;
	addr_t dwordAddr = fConfigData + (offset & effMask & ~(addr_t)3);
	if (size == 4) {
		SetConfigAddress(bus, device, function, offset);
		*(volatile uint32*)dwordAddr = B_HOST_TO_LENDIAN_INT32(value);
		asm volatile("eieio" ::: "memory");
		return B_OK;
	}

	SetConfigAddress(bus, device, function, offset);
	uint32 dword = B_LENDIAN_TO_HOST_INT32(*(volatile uint32*)dwordAddr);
	uint32 shift = (uint32)(offset & 3) * 8;
	uint32 mask = ((size == 1) ? 0xffu : 0xffffu) << shift;
	dword = (dword & ~mask) | ((value << shift) & mask);
	SetConfigAddress(bus, device, function, offset);
	*(volatile uint32*)dwordAddr = B_HOST_TO_LENDIAN_INT32(dword);
	asm volatile("eieio" ::: "memory");

	return B_OK;
}


// #pragma mark - controller queries


status_t
OpenFirmwarePCIController::GetMaxBusDevices(int32& count)
{
	count = 32;
	return B_OK;
}


status_t
OpenFirmwarePCIController::GetRange(uint32 index, pci_resource_range* range)
{
	if (index >= fRangeCount)
		return B_BAD_INDEX;

	*range = fRanges[index];
	return B_OK;
}


status_t
OpenFirmwarePCIController::ReadIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8& irq)
{
	// PCI interrupt routing is resolved through the OpenFirmware
	// "interrupt-map" and the mac-io interrupt controller, not here.
	return B_UNSUPPORTED;
}


status_t
OpenFirmwarePCIController::WriteIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8 irq)
{
	return B_UNSUPPORTED;
}


status_t
OpenFirmwarePCIController::Finalize()
{
	return B_OK;
}


// #pragma mark - module


static pci_controller_module_info sControllerModuleInfo = {
	.info = {
		.info = {
			.name = OF_PCI_DRIVER_MODULE_NAME,
		},
		.supports_device = OpenFirmwarePCIController::SupportsDevice,
		.register_device = OpenFirmwarePCIController::RegisterDevice,
		.init_driver = [](device_node* node, void** driverCookie) {
			return OpenFirmwarePCIController::InitDriver(node,
				*(OpenFirmwarePCIController**)driverCookie);
		},
		.uninit_driver = [](void* driverCookie) {
			static_cast<OpenFirmwarePCIController*>(driverCookie)
				->UninitDriver();
		},
	},
	.read_pci_config = [](void* cookie, uint8 bus, uint8 device,
		uint8 function, uint16 offset, uint8 size, uint32* value) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->ReadConfig(bus, device, function, offset, size, *value);
	},
	.write_pci_config = [](void* cookie, uint8 bus, uint8 device,
		uint8 function, uint16 offset, uint8 size, uint32 value) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->WriteConfig(bus, device, function, offset, size, value);
	},
	.get_max_bus_devices = [](void* cookie, int32* count) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->GetMaxBusDevices(*count);
	},
	.read_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8* irq) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->ReadIrq(bus, device, function, pin, *irq);
	},
	.write_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 irq) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->WriteIrq(bus, device, function, pin, irq);
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->GetRange(index, range);
	},
	.finalize = [](void* cookie) {
		return static_cast<OpenFirmwarePCIController*>(cookie)->Finalize();
	},
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{ B_PCI_MODULE_NAME, (module_info**)&gPCI },
	{}
};

module_info* modules[] = {
	(module_info*)&sControllerModuleInfo,
	NULL
};
