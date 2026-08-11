/*
 * Copyright 2006, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <arch_platform.h>

#include <new>

#include <KernelExport.h>

#include <arch/generic/debug_uart.h>
#include <boot/kernel_args.h>
#include <platform/openfirmware/openfirmware.h>
#include <real_time_clock.h>
#include <util/kernel_cpp.h>


void *gFDT;
static PPCPlatform *sPPCPlatform;


PPCPlatform::PPCPlatform(ppc_platform_type platformType)
	: fPlatformType(platformType)
{
}


PPCPlatform::~PPCPlatform()
{
}


PPCPlatform *
PPCPlatform::Default()
{
	return sPPCPlatform;
}


// #pragma mark - Open Firmware


namespace BPrivate {

class PPCOpenFirmware : public PPCPlatform {
public:
	PPCOpenFirmware();
	virtual ~PPCOpenFirmware();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual	void SetHardwareRTC(uint64 seconds);
	virtual	uint32 GetHardwareRTC();

	virtual	void ShutDown(bool reboot);

private:
	int	fInput;
	int	fOutput;
	int	fRTC;
};

}	// namespace BPrivate


using BPrivate::PPCOpenFirmware;


// OF debugger commands


static int
debug_command_of_exit(int argc, char **argv)
{
	of_exit();
	kprintf("of_exit() failed!\n");
	return 0;
}


static int
debug_command_of_enter(int argc, char **argv)
{
	of_call_client_function("enter", 0, 0);
	return 0;
}


PPCOpenFirmware::PPCOpenFirmware()
	: PPCPlatform(PPC_PLATFORM_OPEN_FIRMWARE),
	  fInput(-1),
	  fOutput(-1),
	  fRTC(-1)
{
}


PPCOpenFirmware::~PPCOpenFirmware()
{
}


status_t
PPCOpenFirmware::Init(struct kernel_args *kernelArgs)
{
	return of_init(
		(intptr_t(*)(void*))kernelArgs->platform_args.openfirmware_entry);
}


status_t
PPCOpenFirmware::InitSerialDebug(struct kernel_args *kernelArgs)
{
	if (of_getprop(gChosen, "stdin", &fInput, sizeof(int)) == OF_FAILED)
		return B_ERROR;
	// Always take OpenFirmware's stdout for the kernel debug console, even when
	// a framebuffer is enabled. When the firmware's output-device is a serial
	// line (our dingusppc/QEMU setup uses output-device=scca) this yields
	// kernel dprintf/panic/KDL over serial *and* a usable framebuffer for
	// app_server at the same time. The old code only took stdout when the
	// framebuffer was disabled, assuming stdout is always the screen - which is
	// untrue whenever output-device points at a UART, and is exactly why a boot
	// splash / framebuffer used to have to be disabled to get any serial log.
	if (of_getprop(gChosen, "stdout", &fOutput, sizeof(int)) == OF_FAILED)
		return B_ERROR;

	return B_OK;
}


status_t
PPCOpenFirmware::InitPostVM(struct kernel_args *kernelArgs)
{
	add_debugger_command("of_exit", &debug_command_of_exit,
		"Exit to the Open Firmware prompt. No way to get back into the OS!");
	add_debugger_command("of_enter", &debug_command_of_enter,
		"Enter a subordinate Open Firmware interpreter. Quitting it returns "
		"to KDL.");

	return B_OK;
}


// InitRTC
status_t
PPCOpenFirmware::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	// open RTC
	fRTC = of_open(kernelArgs->platform_args.rtc_path);
	if (fRTC == OF_FAILED) {
		dprintf("PPCOpenFirmware::InitRTC(): Failed open RTC device!\n");
		return B_ERROR;
	}

	return B_OK;
}


char
PPCOpenFirmware::SerialDebugGetChar()
{
	int key;
	if (of_interpret("key", 0, 1, &key) == OF_FAILED)
		return 0;
	return (char)key;
}


void
PPCOpenFirmware::SerialDebugPutChar(char c)
{
	if (fOutput == -1)
		return;

	if (c == '\n')
		of_write(fOutput, "\r\n", 2);
	else
		of_write(fOutput, &c, 1);
}


void
PPCOpenFirmware::SetHardwareRTC(uint64 seconds)
{
	struct tm t;
	rtc_secs_to_tm(seconds, &t);

	t.tm_year += RTC_EPOCH_BASE_YEAR;
	t.tm_mon++;

	if (of_call_method(fRTC, "set-time", 6, 0, t.tm_year, t.tm_mon, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec) == OF_FAILED) {
		dprintf("PPCOpenFirmware::SetHardwareRTC(): Failed to set RTC!\n");
	}
}


uint32
PPCOpenFirmware::GetHardwareRTC()
{
	struct tm t;
	if (of_call_method(fRTC, "get-time", 0, 6, &t.tm_year, &t.tm_mon,
			&t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == OF_FAILED) {
		dprintf("PPCOpenFirmware::GetHardwareRTC(): Failed to get RTC!\n");
		return 0;
	}

	t.tm_year -= RTC_EPOCH_BASE_YEAR;
	t.tm_mon--;

	return rtc_tm_to_secs(&t);
}


void
PPCOpenFirmware::ShutDown(bool reboot)
{
	if (reboot) {
		of_interpret("reset-all", 0, 0);
	} else {
		// not standardized, so it might fail
		of_interpret("shut-down", 0, 0);
	}
}


// #pragma mark - U-Boot + FDT


namespace BPrivate {

class PPCUBoot : public PPCPlatform {
public:
	PPCUBoot();
	virtual ~PPCUBoot();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual	void SetHardwareRTC(uint64 seconds);
	virtual	uint32 GetHardwareRTC();

	virtual	void ShutDown(bool reboot);

private:
	int	fInput;
	int	fOutput;
	int	fRTC;
	DebugUART *fDebugUART;
};

}	// namespace BPrivate

using BPrivate::PPCUBoot;


PPCUBoot::PPCUBoot()
	: PPCPlatform(PPC_PLATFORM_U_BOOT),
	  fInput(-1),
	  fOutput(-1),
	  fRTC(-1),
	  fDebugUART(NULL)
{
}


PPCUBoot::~PPCUBoot()
{
}


status_t
PPCUBoot::Init(struct kernel_args *kernelArgs)
{
	gFDT = kernelArgs->platform_args.fdt;
	// XXX: do we error out if no FDT?
	return B_OK;
}


status_t
PPCUBoot::InitSerialDebug(struct kernel_args *kernelArgs)
{
	// TODO: get relevant debug uart from fdt
	//fDebugUART = debug_uart_from_fdt(gFDT);
	if (fDebugUART == NULL)
		return B_ERROR;
	return B_OK;
}


status_t
PPCUBoot::InitPostVM(struct kernel_args *kernelArgs)
{
	return B_ERROR;
}


status_t
PPCUBoot::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	return B_ERROR;
}


char
PPCUBoot::SerialDebugGetChar()
{
	if (fDebugUART)
		return fDebugUART->GetChar(false);
	return 0;
}


void
PPCUBoot::SerialDebugPutChar(char c)
{
	if (fDebugUART)
		fDebugUART->PutChar(c);
}


void
PPCUBoot::SetHardwareRTC(uint64 seconds)
{
}


uint32
PPCUBoot::GetHardwareRTC()
{
	return 0;
}


void
PPCUBoot::ShutDown(bool reboot)
{
}


// # pragma mark -


#define PLATFORM_BUFFER_SIZE MAX(sizeof(PPCOpenFirmware),sizeof(PPCUBoot))
// static buffer for constructing the actual PPCPlatform
static char *sPPCPlatformBuffer[PLATFORM_BUFFER_SIZE];

// PCI host bridge info, captured by the boot loader (see arch_kernel_args).
// Legacy single-bridge fields mirror bridge 0 (the boot bridge).
static uint32 sPCIHostBridgeType = 0;
static phys_addr_t sPCIConfigAddress = 0xfec00000;
static phys_addr_t sPCIConfigData = 0xfee00000;

// All PCI host bridges (a Power Mac G4 has several UniNorth buses).
static uint32 sPCIHostBridgeCount = 0;
static struct {
	uint32		type;
	phys_addr_t	configAddress;
	phys_addr_t	configData;
} sPCIHostBridges[MAX_PCI_HOST_BRIDGES];

static uint32 sGmacIRQ = 0;
static uint32 sCardBusIRQ = 0;
static uint32 sCardBusMemBase = 0;
static uint32 sKauaiAtaIRQ = 0;
static uint32 sAirportIRQ = 0;
static uint8 sGmacMAC[6] = { 0, 0, 0, 0, 0, 0 };
static bool sGmacMACValid = false;


extern "C" void
ppc_get_pci_host_bridge(uint32* type, phys_addr_t* configAddress,
	phys_addr_t* configData)
{
	*type = sPCIHostBridgeType;
	*configAddress = sPCIConfigAddress;
	*configData = sPCIConfigData;
}

extern "C" uint32
ppc_get_pci_host_bridge_count()
{
	return sPCIHostBridgeCount;
}

extern "C" status_t
ppc_get_pci_host_bridge_at(uint32 index, uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData)
{
	if (index >= sPCIHostBridgeCount)
		return B_BAD_INDEX;
	*type = sPCIHostBridges[index].type;
	*configAddress = sPCIHostBridges[index].configAddress;
	*configData = sPCIHostBridges[index].configData;
	return B_OK;
}

extern "C" uint32
ppc_get_gmac_irq()
{
	return sGmacIRQ;
}


extern "C" uint32
ppc_get_cardbus_irq()
{
	return sCardBusIRQ;
}


extern "C" uint32
ppc_get_airport_irq()
{
	return sAirportIRQ;
}


extern "C" uint32
ppc_get_cardbus_mem_base()
{
	return sCardBusMemBase;
}

extern "C" uint32
ppc_get_kauai_ata_irq()
{
	return sKauaiAtaIRQ;
}

extern "C" bool
ppc_get_gmac_mac(uint8* address)
{
	if (!sGmacMACValid)
		return false;
	for (int i = 0; i < 6; i++)
		address[i] = sGmacMAC[i];
	return true;
}


status_t
arch_platform_init(struct kernel_args *kernelArgs)
{
	// only OpenFirmware supported for now
	switch (kernelArgs->arch_args.platform) {
		case PPC_PLATFORM_OPEN_FIRMWARE:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCOpenFirmware;
			break;
		case PPC_PLATFORM_U_BOOT:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCUBoot;
			break;
		default:
			return B_ERROR;
	}

	sPCIHostBridgeType = kernelArgs->arch_args.pci_host_bridge_type;
	sPCIConfigAddress = kernelArgs->arch_args.pci_config_address;
	sPCIConfigData = kernelArgs->arch_args.pci_config_data;
	sGmacIRQ = kernelArgs->arch_args.gmac_irq;
	sCardBusIRQ = kernelArgs->arch_args.cardbus_irq;
	sCardBusMemBase = kernelArgs->arch_args.cardbus_mem_base;
	sKauaiAtaIRQ = kernelArgs->arch_args.kauai_ata_irq;
	sAirportIRQ = kernelArgs->arch_args.airport_irq;
	sGmacMACValid = kernelArgs->arch_args.gmac_mac_valid != 0;
	for (int i = 0; i < 6; i++)
		sGmacMAC[i] = kernelArgs->arch_args.gmac_mac[i];

	uint32 bridgeCount = kernelArgs->arch_args.pci_host_bridge_count;
	if (bridgeCount == 0 || bridgeCount > MAX_PCI_HOST_BRIDGES) {
		// Old loader (or none captured): synthesize bridge 0 from the
		// legacy fields so single-bridge machines keep working.
		sPCIHostBridgeCount = 1;
		sPCIHostBridges[0].type = sPCIHostBridgeType;
		sPCIHostBridges[0].configAddress = sPCIConfigAddress;
		sPCIHostBridges[0].configData = sPCIConfigData;
	} else {
		sPCIHostBridgeCount = bridgeCount;
		for (uint32 i = 0; i < bridgeCount; i++) {
			sPCIHostBridges[i].type
				= kernelArgs->arch_args.pci_host_bridges[i].type;
			sPCIHostBridges[i].configAddress
				= kernelArgs->arch_args.pci_host_bridges[i].config_address;
			sPCIHostBridges[i].configData
				= kernelArgs->arch_args.pci_host_bridges[i].config_data;
		}
	}

	return sPPCPlatform->Init(kernelArgs);
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	return sPPCPlatform->InitPostVM(kernelArgs);
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	return B_OK;
}
