/*
 * VIA-CUDA / VIA-PMU ADB input driver for PowerPC Macs (and dingusppc).
 *
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * The Cuda MCU is an ADB/I2C/RTC/power microcontroller attached to a 6522 VIA
 * inside the mac-io chip. The host talks to Cuda by shifting bytes through the
 * VIA shift register (SR) with a three-wire handshake (TIP/BYTEACK/TREQ) in the
 * VIA data-B register. Cuda auto-polls the ADB bus and hands us keyboard
 * (address 2) and mouse (address 3) reports, which we turn into Haiku
 * raw_key_info / mouse_movement events on /dev/input/{keyboard,mouse}/adb/0.
 *
 * Authors:
 *		Sean Malseed, actionretro@pm.me
 *		Claude (Anthropic), paired via Claude Code
 */

#include <KernelExport.h>
#include <Drivers.h>
#include <OS.h>

#include <string.h>
#include <stdio.h>

#include <keyboard_mouse_driver.h>
#include "device/power_managment.h"
#include <PCI.h>
#include <module.h>


#define INFO(x...)		dprintf("adb: " x)
//#define TRACE(x...)	dprintf("adb: " x)
#define TRACE(x...)		do {} while (0)


int32 api_version = B_CUR_DRIVER_API_VERSION;

// VIA register indices; byte offset of register N is (N << 9) (regs 0x200 apart).
enum {
	VIA_B    = 0x00, VIA_DIRB = 0x02, VIA_SR = 0x0A, VIA_ACR = 0x0B,
	VIA_PCR  = 0x0C, VIA_IFR = 0x0D, VIA_IER = 0x0E,
};

// Cuda handshake bits in VIA_B (active low: 0 = asserted).
#define CUDA_TIP		0x20	// transaction in progress (host -> Cuda)
#define CUDA_BYTEACK	0x10	// byte acknowledge (host -> Cuda)
#define CUDA_TREQ		0x08	// Cuda requests a transaction (Cuda -> host)

#define VIA_IF_SR		0x04	// shift-register interrupt flag
#define VIA_IER_SET		0x80	// bit7=1 in an IER write sets the given bits

#define VIA_ACR_SR_MASK	0x1c
#define VIA_ACR_SR_OUT	0x1c	// shift out under Cuda's clock
#define VIA_ACR_SR_IN	0x0c	// shift in  under Cuda's clock

#define CUDA_PKT_ADB	0x00
#define CUDA_PKT_PSEUDO	0x01
#define CUDA_START_STOP_AUTOPOLL	0x01

// dingusppc / Grackle mac-io physical base; Cuda VIA is at + 0x16000.
// TODO: discover from OpenFirmware / mac-io PCI BAR (UniNorth uses 0x80000000).
#define MACIO_PHYS_BASE		0x80800000
#define VIA_OFFSET			0x16000
#define VIA_CUDA_IRQ		0x12

// --- VIA-PMU (new-world PowerBooks: Pismo = UniNorth + KeyLargo) ---
// Same 6522 VIA register layout as Cuda (regs 0x200 apart) but its own
// handshake + packet protocol. Handshake bits in VIA_B (active low): TACK is an
// input (PMU->host), TREQ an output (host->PMU); there is no TIP. Protocol from
// Linux drivers/macintosh/via-pmu.c.
#define PMU_TACK		0x08	// transfer acknowledge (PMU -> host, input)
#define PMU_TREQ		0x10	// transfer request     (host -> PMU, output)

#define VIA_SR_INT		0x04	// shift-register interrupt (IFR/IER)
#define VIA_CB1_INT		0x10	// CB1 transition = PMU has data (IFR/IER)

#define PMU_ADB_CMD		0x20	// "send an ADB packet" command
#define PMU_ADB_POLL_OFF	0x21	// disable ADB autopoll
#define PMU_INT_ACK		0x78	// "read interrupt/autopoll data" command
#define PMU_INT_ADB		0x10	// data[0] bit: reply/autopoll is ADB
#define PMU_INT_ADB_AUTO	0x04	// data[0] bit: unsolicited autopoll
#define PMU_INT_TICK		0x80	// data[0] bit: 1-second tick
#define PMU_SET_INTR_MASK	0x70	// set which PMU interrupts are delivered
#define PMU_SYSTEM_READY	0xdf	// tell the PMU the OS is up (KeyLargo)
#define PMU_BATTERY_STATE	0x6b	// legacy battery-state query
#define PMU_SMART_BATTERY_STATE	0x6f	// smart-battery query (PowerBook G4)
// KeyLargo PMU interrupt mask (matches Linux): PCEJECT|SNDBRT|ADB|ENV|TICK.
#define PMU_INTR_MASK_VALUE	0xdc	// Linux KeyLargo: PCEJECT|SNDBRT|ADB|ENV|TICK

#define PCI_VENDOR_APPLE	0x106b	// KeyLargo-family mac-io vendor
// via-pmu is at mac-io + 0x16000 and raises mac-io interrupt 0x19 (Pismo OF
// device tree: reg 0x16000/0x2000, interrupts 0x19).
#define PMU_VIA_OFFSET		0x16000
#define PMU_IRQ				0x19
// KeyLargo signals "PMU has data" on extint-gpio1 (IRQ 0x2f), NOT the VIA CB1.
// The gpio block is at mac-io + 0x50; extint-gpio1's level byte is at + 0x59,
// bit 0x02 (active low: 0 = PMU asserting). (Pismo OF: extint-gpio1 interrupts
// 0x2f, compatible keywest-gpio1.)
#define PMU_GPIO1_IRQ		0x2f
#define KEYLARGO_EXTINT_GPIO1	0x59
#define KEYLARGO_GPIO_LEVEL	0x02

// Map ADB keycodes (register-0 talk data) to Haiku keycodes (as used by the
// system keymap). Best-effort for the main keys; unmapped keys pass 0. ADB
// keycodes are 7-bit (bit7 = key-up). Index = ADB keycode.
static const uint8 kAdbToHaiku[128] = {
	/* 0x00 */ 0x3c,0x3d,0x3e,0x3f,0x41,0x40,0x4c,0x4d, // a s d f h g z x
	/* 0x08 */ 0x4e,0x4f,0x00,0x50,0x27,0x28,0x29,0x2a, // c v (§) b q w e r
	/* 0x10 */ 0x2c,0x2b,0x12,0x13,0x14,0x15,0x17,0x16, // y t 1 2 3 4 6 5
	/* 0x18 */ 0x1d,0x1a,0x18,0x1c,0x19,0x1b,0x32,0x2f, // = 9 7 - 8 0 ] o
	/* 0x20 */ 0x2d,0x31,0x2e,0x30,0x47,0x44,0x42,0x46, // u [ i p enter l j '
	/* 0x28 */ 0x43,0x45,0x33,0x53,0x55,0x51,0x52,0x54, // k ; \\ , / n m .
	/* 0x30 */ 0x26,0x5e,0x11,0x1e,0x00,0x01,0x5c,0x5d, // tab space ` bksp - esc lctrl lcmd
	/* 0x38 */ 0x4b,0x3b,0x66,0x00,0x56,0x67,0x60,0x00, // lshift caps lopt - rshift ropt rctrl -
	/* 0x40 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // (keypad, unmapped)
	/* 0x48 */ 0x00,0x00,0x00,0x00,0x5b,0x00,0x00,0x00, // kp-enter @ 0x4c
	/* 0x50 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // (keypad, unmapped)
	/* 0x58 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // (keypad, unmapped)
	/* 0x60 */ 0x06,0x07,0x08,0x04,0x09,0x0a,0x00,0x0c, // F5 F6 F7 F3 F8 F9 - F11
	/* 0x68 */ 0x00,0x0e,0x00,0x0f,0x00,0x0b,0x00,0x0d, // - F13 - F14 - F10 - F12
	/* 0x70 */ 0x00,0x10,0x1f,0x20,0x21,0x34,0x05,0x35, // - F15 help home pgup del F4 end
	/* 0x78 */ 0x03,0x36,0x02,0x61,0x63,0x62,0x57,0x00, // F2 pgdn F1 left right down up -
};


static area_id sRegisterArea = -1;
static addr_t sVIABase = 0;
static addr_t sGpioExt1 = 0;	// extint-gpio1 level byte

// Which transport this machine uses (chosen in init_hardware by host bridge).
static bool sIsPMU = false;

// PMU transfer state machine (mirrors Linux via-pmu.c pmu_state).
enum {
	PMU_IDLE = 0, PMU_SENDING, PMU_INTACK, PMU_READING, PMU_READING_INTR
};
static volatile int sPmuState = PMU_IDLE;
static uint8 sPmuReq[16];		// outgoing request bytes (data[0] = command)
static int sPmuReqBytes = 0;
static bool sPmuReqActive = false;
static int sPmuIndex = 0;
static int sPmuLen = -1;
static int sPmuSendLen = -1;	// wire send-length rule for the current cmd
static int sPmuReplyLen = 0;	// expected reply length (0 none, -1 length-prefixed)
static uint8 sPmuReply[32];	// command reply (discarded)
static volatile int sPmuLastLen = 0;	// byte count of the last command reply
static uint8* sPmuReplyPtr = NULL;
static uint8 sPmuIntr[32];		// interrupt/autopoll reply
static volatile bool sPmuDidInput = false;
static volatile bool sPmuCB1 = false;

// Cuda receive-transaction assembly.
static uint8 sReplyBuffer[16];
static int sReplyLength = 0;
static bool sReading = false;

// Keyboard event ring + blocking read.
#define KB_QUEUE_SIZE	64
static raw_key_info sKeyQueue[KB_QUEUE_SIZE];
static int sKeyHead = 0, sKeyTail = 0;
static sem_id sKeySem = -1;
static spinlock sKeyLock = B_SPINLOCK_INITIALIZER;

// Mouse event ring + blocking read.
#define MS_QUEUE_SIZE	256
static mouse_movement sMouseQueue[MS_QUEUE_SIZE];
static int sMouseHead = 0, sMouseTail = 0;
static sem_id sMouseSem = -1;
static spinlock sMouseLock = B_SPINLOCK_INITIALIZER;
static uint32 sMouseButtons = 0;

// Haiku mouse button bits (from InterfaceDefs.h; not pulled into kernel land).
#define ADB_PRIMARY_BUTTON		0x01
#define ADB_SECONDARY_BUTTON	0x02

// Double-click tracking. The input_server expects the driver to fill in the
// click count (matching the USB/PS2 mouse drivers); app_server only treats a
// press as a double-click when clicks == 2. fClickSpeed default 250 ms.
static uint32 sLastButtons = 0;
static int32 sClickCount = 0;
static bigtime_t sLastClickTime = 0;
static bigtime_t sClickSpeed = 250000;


static inline uint8
via_read(int reg)
{
	uint8 value = *(volatile uint8*)(sVIABase + (reg << 9));
	asm volatile("eieio" ::: "memory");
	return value;
}


static inline void
via_write(int reg, uint8 value)
{
	*(volatile uint8*)(sVIABase + (reg << 9)) = value;
	asm volatile("eieio" ::: "memory");
}


static void
queue_key(uint8 adbCode)
{
	raw_key_info info;
	info.timestamp = system_time();
	info.keycode = kAdbToHaiku[adbCode & 0x7f];
	info.is_keydown = (adbCode & 0x80) == 0;
	if (info.keycode == 0)
		return;

	// Called from both the ADB interrupt handler (interrupts already off) and
	// from driver init via pmu_drain_pending() (a normal thread, interrupts
	// ON). acquire_spinlock() requires interrupts disabled, so save/restore
	// them here to be safe in either context.
	cpu_status former = disable_interrupts();
	acquire_spinlock(&sKeyLock);
	int next = (sKeyHead + 1) % KB_QUEUE_SIZE;
	if (next != sKeyTail) {
		sKeyQueue[sKeyHead] = info;
		sKeyHead = next;
		release_spinlock(&sKeyLock);
		restore_interrupts(former);
		release_sem_etc(sKeySem, 1, B_DO_NOT_RESCHEDULE);
	} else {
		release_spinlock(&sKeyLock);
		restore_interrupts(former);
	}
}


static void
queue_mouse(uint32 buttons, int dx, int dy)
{
	sMouseButtons = buttons;

	// Derive the click count on each fresh press, like MouseProtocolHandler.
	bigtime_t now = system_time();
	int32 clicks = 0;
	if (buttons != 0) {
		if (sLastButtons == 0) {
			if (sLastClickTime + sClickSpeed > now)
				sClickCount++;
			else
				sClickCount = 1;
		}
		sLastClickTime = now;
		clicks = sClickCount;
	}
	sLastButtons = buttons;

	mouse_movement m;
	memset(&m, 0, sizeof(m));
	m.buttons = buttons;
	m.xdelta = dx;
	// ADB/SDL accumulate Y positive-down; Haiku mouse deltas are positive-up
	// (BeOS convention - the USB HID driver negates Y the same way).
	m.ydelta = -dy;
	m.clicks = clicks;
	m.timestamp = now;

	// See queue_key(): safe for both interrupt and thread context.
	cpu_status former = disable_interrupts();
	acquire_spinlock(&sMouseLock);
	int next = (sMouseHead + 1) % MS_QUEUE_SIZE;
	if (next != sMouseTail) {
		sMouseQueue[sMouseHead] = m;
		sMouseHead = next;
		release_spinlock(&sMouseLock);
		restore_interrupts(former);
		release_sem_etc(sMouseSem, 1, B_DO_NOT_RESCHEDULE);
	} else {
		release_spinlock(&sMouseLock);
		restore_interrupts(former);
	}
}


static void
adb_process_reply(uint8* data, int length)
{
	TRACE("reply len %d: %02x %02x %02x %02x\n", length, data[0],
		length > 1 ? data[1] : 0, length > 2 ? data[2] : 0,
		length > 3 ? data[3] : 0);

	if (length < 3 || data[0] != CUDA_PKT_ADB)
		return;

	// dingusppc Cuda ADB response framing (viacuda.cpp response_header):
	//   data[0] = CUDA_PKT_ADB (0x00)
	//   data[1] = status flags  (ADB_STAT_AUTOPOLL | RESPONSE | ...)
	//   data[2] = the ADB command byte (bits7-4 = device address, low = reg)
	//   data[3..] = the device's register-0 payload
	uint8 address = (data[2] >> 4) & 0x0f;
	if (address == 2 && length >= 5) {
		// Keyboard reg 0: two 7-bit ADB keycodes, bit7 = up. 0xff = no key.
		TRACE("KEY %02x %02x\n", data[3], data[4]);
		if (data[3] != 0xff)
			queue_key(data[3]);
		if (data[4] != 0xff)
			queue_key(data[4]);
	} else if (address == 3 && length >= 5) {
		// Mouse reg 0 (standard 2-byte protocol, dingusppc adbmouse.cpp):
		//   data[3]: bit7 = ~button0 (left/primary), bits0-6 = signed Y
		//   data[4]: bit7 = ~button1 (right/secondary), bits0-6 = signed X
		// A cleared button bit means pressed; the second button was being
		// masked off before, so right-click never reached the desktop.
		uint32 buttons = 0;
		if ((data[3] & 0x80) == 0)
			buttons |= ADB_PRIMARY_BUTTON;
		if ((data[4] & 0x80) == 0)
			buttons |= ADB_SECONDARY_BUTTON;
		int dy = (int)(data[3] & 0x7f); if (dy & 0x40) dy -= 0x80;
		int dx = (int)(data[4] & 0x7f); if (dx & 0x40) dx -= 0x80;
		TRACE("MOUSE btn %#lx dx %d dy %d\n", buttons, dx, dy);
		queue_mouse(buttons, dx, dy);
	}
}


static int32
adb_interrupt(void* arg)
{
	uint8 ifr = via_read(VIA_IFR);
	if ((ifr & VIA_IF_SR) == 0)
		return B_UNHANDLED_INTERRUPT;

	via_write(VIA_IFR, VIA_IF_SR);
	uint8 portB = via_read(VIA_B);

	if (!sReading) {
		// An incoming transaction only begins when Cuda asserts TREQ (active
		// low, so bit clear = data available). Cuda also raises an SR
		// interrupt ~61us AFTER we end a transaction (its "idle acknowledge",
		// see ViaCuda::write) - with TREQ negated. If we treated that (or any
		// other spurious SR int) as a new transaction we would assert TIP,
		// read the stale last SR byte as a bogus 1-byte packet, end, and
		// thereby trigger the next idle-ack: an endless storm. Ignore any SR
		// int that is not an actual TREQ request.
		if ((portB & CUDA_TREQ) != 0)
			return B_HANDLED_INTERRUPT;

		sReading = true;
		sReplyLength = 0;
		via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK)
			| VIA_ACR_SR_IN);
		(void)via_read(VIA_SR);
		via_write(VIA_B, (portB & ~CUDA_TIP) | CUDA_BYTEACK);
		return B_HANDLED_INTERRUPT;
	}

	uint8 byte = via_read(VIA_SR);
	if (sReplyLength < (int)sizeof(sReplyBuffer))
		sReplyBuffer[sReplyLength++] = byte;

	if ((portB & CUDA_TREQ) != 0) {
		via_write(VIA_B, portB | CUDA_TIP | CUDA_BYTEACK);
		sReading = false;
		adb_process_reply(sReplyBuffer, sReplyLength);
		return B_INVOKE_SCHEDULER;
	}

	via_write(VIA_B, portB ^ CUDA_BYTEACK);
	return B_HANDLED_INTERRUPT;
}


static void
cuda_send_polled(const uint8* data, int length)
{
	for (int i = 0; i < 100000; i++) {
		if ((via_read(VIA_B) & (CUDA_TIP | CUDA_TREQ)) == (CUDA_TIP | CUDA_TREQ))
			break;
	}
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_OUT);

	uint8 portB = via_read(VIA_B);
	for (int i = 0; i < length; i++) {
		via_write(VIA_SR, data[i]);
		if (i == 0) {
			portB &= ~CUDA_TIP;
			via_write(VIA_B, portB);
		}
		portB ^= CUDA_BYTEACK;
		via_write(VIA_B, portB);
		for (int j = 0; j < 100000; j++) {
			if ((via_read(VIA_IFR) & VIA_IF_SR) != 0)
				break;
		}
		via_write(VIA_IFR, VIA_IF_SR);
	}
	via_write(VIA_B, portB | CUDA_TIP | CUDA_BYTEACK);
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN);
}


// #pragma mark - VIA-PMU transport


static inline bool
pmu_wait_for_ack(void)
{
	// Wait until TACK is negated (high). Bounded so a wedged PMU cannot hang
	// the kernel.
	for (int i = 0; i < 32000; i++) {
		if ((via_read(VIA_B) & PMU_TACK) != 0)
			return true;
		spin(10);
	}
	return false;
}


static inline void
pmu_send_byte(uint8 value)
{
	via_write(VIA_ACR, via_read(VIA_ACR) | VIA_ACR_SR_OUT);	// shift out
	via_write(VIA_SR, value);
	via_write(VIA_B, via_read(VIA_B) & ~PMU_TREQ);			// assert TREQ
}


static inline void
pmu_recv_byte(void)
{
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN);
	(void)via_read(VIA_SR);									// prime the shift-in
	via_write(VIA_B, via_read(VIA_B) & ~PMU_TREQ);			// assert TREQ
}


static inline void
pmu_sr_off(void)
{
	// Disable the shift register. In external-clock SR mode CB1 is the shift
	// clock and cannot also generate the "PMU has data" (CB1) interrupt; with
	// the SR disabled at idle, CB1 is free to signal an incoming transfer.
	via_write(VIA_ACR, via_read(VIA_ACR) & ~VIA_ACR_SR_MASK);
}


static void
pmu_start(void)
{
	if (!sPmuReqActive || sPmuState != PMU_IDLE)
		return;
	sPmuState = PMU_SENDING;
	sPmuIndex = 1;
	sPmuLen = sPmuSendLen;		// -1 = length byte follows; >=0 = that many bytes
	pmu_wait_for_ack();
	pmu_send_byte(sPmuReq[0]);
}


// Turn a completed ADB autopoll packet into Haiku input events. The PMU frames
// autopoll data as [adb_command, reg0_byte0, reg0_byte1, ...]; the ADB device
// address is the high nibble of the command byte (2 = keyboard, 3 = mouse).
static void
pmu_process_adb(uint8* buf, int length)
{
	if (length < 3)
		return;
	uint8 address = (buf[0] >> 4) & 0x0f;
	if (address == 2) {
		if (buf[1] != 0xff)
			queue_key(buf[1]);
		if (buf[2] != 0xff)
			queue_key(buf[2]);
		sPmuDidInput = true;
	} else if (address == 3) {
		// Trackpad in default mode = standard 2-byte ADB mouse:
		//   buf[1]: bit7 = ~button,  bits0-6 = signed Y
		//   buf[2]: bit7 = ~button2, bits0-6 = signed X
		uint32 buttons = 0;
		if ((buf[1] & 0x80) == 0)
			buttons |= ADB_PRIMARY_BUTTON;
		if ((buf[2] & 0x80) == 0)
			buttons |= ADB_SECONDARY_BUTTON;
		int dy = (int)(buf[1] & 0x7f); if (dy & 0x40) dy -= 0x80;
		int dx = (int)(buf[2] & 0x7f); if (dx & 0x40) dx -= 0x80;
		queue_mouse(buttons, dx, dy);
		sPmuDidInput = true;
	}
}


static void
pmu_handle_data(uint8* data, int length)
{
	if (length < 1)
		return;
	if ((data[0] & PMU_INT_ADB) != 0 && (data[0] & PMU_INT_ADB_AUTO) != 0)
		pmu_process_adb(data + 1, length - 1);
	// Non-ADB PMU interrupts (battery, tick, environment, ...) are ignored.
}


// Per-byte shift-register interrupt: advances the send/receive state machine.
static void
pmu_sr_intr(void)
{
	if ((via_read(VIA_B) & PMU_TREQ) != 0)
		return;								// spurious: TREQ not asserted
	for (int i = 0; i < 32000 && (via_read(VIA_B) & PMU_TACK) != 0; i++)
		spin(1);							// wait for TACK asserted (low)

	uint8 bite = 0;
	if (sPmuState == PMU_READING || sPmuState == PMU_READING_INTR)
		bite = via_read(VIA_SR);

	via_write(VIA_B, via_read(VIA_B) | PMU_TREQ);	// negate TREQ
	pmu_wait_for_ack();

	switch (sPmuState) {
		case PMU_SENDING:
			if (sPmuLen < 0) {
				sPmuLen = sPmuReqBytes - 1;			// length byte after command
				pmu_send_byte((uint8)sPmuLen);
				break;
			}
			if (sPmuIndex <= sPmuLen) {
				pmu_send_byte(sPmuReq[sPmuIndex++]);
				break;
			}
			if (sPmuReplyLen == 0) {
				sPmuReqActive = false;
				sPmuState = PMU_IDLE;
				pmu_sr_off();
			} else {
				sPmuState = PMU_READING;			// read the command reply
				sPmuIndex = 0;
				sPmuLen = (sPmuReplyLen < 0) ? -1 : sPmuReplyLen;
				sPmuReplyPtr = sPmuReply;
				pmu_recv_byte();
			}
			break;

		case PMU_INTACK:
			sPmuIndex = 0;
			sPmuLen = -1;
			sPmuState = PMU_READING_INTR;
			sPmuReplyPtr = sPmuIntr;
			pmu_recv_byte();
			break;

		case PMU_READING:
		case PMU_READING_INTR:
			if (sPmuLen == -1) {
				sPmuLen = bite;						// first byte = length
				if (sPmuLen > 32)
					sPmuLen = 32;
			} else if (sPmuIndex < 32) {
				sPmuReplyPtr[sPmuIndex++] = bite;
			}
			if (sPmuIndex < sPmuLen) {
				pmu_recv_byte();
				break;
			}
			if (sPmuState == PMU_READING_INTR)
				pmu_handle_data(sPmuIntr, sPmuIndex);
			else {
				sPmuLastLen = sPmuIndex;			// bytes now in sPmuReply
				sPmuReqActive = false;				// command reply consumed
			}
			sPmuState = PMU_IDLE;
			pmu_sr_off();
			break;

		default:
			break;
	}
}


static int32
adb_pmu_interrupt(void* arg)
{
	bool handled = false;
	for (int guard = 0; guard < 1000; guard++) {
		uint8 intr = via_read(VIA_IFR) & (VIA_SR_INT | VIA_CB1_INT);
		if (intr == 0)
			break;
		via_write(VIA_IFR, intr);				// acknowledge
		handled = true;
		if ((intr & VIA_CB1_INT) != 0) {
			sPmuCB1 = true;						// PMU has data to send us
		}
		if ((intr & VIA_SR_INT) != 0)
			pmu_sr_intr();
	}

	// If the PMU signalled data (CB1) and the bus is idle, pull it by sending
	// the interrupt-acknowledge command; the reply arrives via SR interrupts.
	if (sPmuState == PMU_IDLE && sPmuCB1) {
		sPmuCB1 = false;
		sPmuState = PMU_INTACK;
		pmu_wait_for_ack();
		pmu_send_byte(PMU_INT_ACK);
	} else if (sPmuState == PMU_IDLE && sPmuReqActive) {
		pmu_start();
	}

	if (sPmuDidInput) {
		sPmuDidInput = false;
		return B_INVOKE_SCHEDULER;
	}
	return handled ? B_HANDLED_INTERRUPT : B_UNHANDLED_INTERRUPT;
}


static bool
pmu_queue_wait(const uint8* data, int nbytes, int sendLen, int replyLen)
{
	for (int i = 0; i < nbytes && i < (int)sizeof(sPmuReq); i++)
		sPmuReq[i] = data[i];
	sPmuReqBytes = nbytes;
	sPmuSendLen = sendLen;
	sPmuReplyLen = replyLen;
	sPmuReqActive = true;

	// Only transmit once the bus is CLEAR: the state machine idle, no CB1, and
	// the PMU not asserting extint-gpio1 (pending autopoll/reply data, active
	// low). Sending a command while the PMU has data pending desyncs the
	// protocol and powers the machine off - the battery poll thread hit this
	// during the fork-heavy first-login storm. Wait with interrupts enabled
	// between checks (so the gpio1/SR handlers drain any pending data), then
	// start under a brief interrupt disable so check-and-send is atomic.
	for (int i = 0; i < 40000 && sPmuReqActive; i++) {
		cpu_status st = disable_interrupts();
		uint8 gpio = 0xff;
		if (sGpioExt1 != 0) {
			gpio = *(volatile uint8*)sGpioExt1;
			asm volatile("eieio" ::: "memory");
		}
		bool clear = sPmuState == PMU_IDLE && !sPmuCB1
			&& (gpio & KEYLARGO_GPIO_LEVEL) != 0;
		if (clear) {
			pmu_start();
			restore_interrupts(st);
			break;
		}
		restore_interrupts(st);
		spin(20);
	}

	for (int i = 0; i < 200000 && sPmuReqActive; i++)
		spin(10);
	return !sPmuReqActive;
}


// --- PMU battery monitor ---------------------------------------------------
// The smart-battery query (0x6f, battery arg 2) returns a length-prefixed
// record; scan for its "0a 05" anchor (length 10, format 5) and parse:
//   [flags][charge:2][max charge:2][current:2 signed][voltage:2]  (BE mAh/mA/mV)
// flags bit0 = AC present. A poll thread refreshes a cache every ~5 s and the
// /dev/power/pmu_battery device serves it to the PowerStatus applet via the
// same ioctl protocol as acpi_battery, so no app changes are needed.
static thread_id sBatteryThread = -1;
static bool sBatteryStop = false;

static volatile bool sBatValid = false;
static volatile uint8 sBatFlags = 0;
static volatile uint32 sBatCharge = 0;		// mAh
static volatile uint32 sBatMax = 0;			// mAh (full-charge capacity)
static volatile int32 sBatCurrent = 0;		// signed mA (negative = discharging)
static volatile uint32 sBatVoltage = 0;		// mV

static bool
pmu_read_battery(void)
{
	uint8 req[2] = { PMU_SMART_BATTERY_STATE, 2 };
	if (!pmu_queue_wait(req, 2, 1, -1))
		return false;
	int n = sPmuLastLen;
	if (n > 32)
		n = 32;
	// The synchronous reader occasionally captures leading 0x55 SR filler, so
	// scan for the record anchor rather than assuming a fixed offset.
	for (int i = 0; i + 10 < n; i++) {
		if (sPmuReply[i] != 0x0a || sPmuReply[i + 1] != 0x05)
			continue;
		const uint8* r = &sPmuReply[i + 1];	// r[0]=format, r[1]=flags, ...
		sBatFlags = r[1];
		sBatCharge = ((uint32)r[2] << 8) | r[3];
		sBatMax = ((uint32)r[4] << 8) | r[5];
		sBatCurrent = (int32)(int16)(((uint16)r[6] << 8) | r[7]);
		sBatVoltage = ((uint32)r[8] << 8) | r[9];
		sBatValid = true;
		return true;
	}
	return false;
}

static int32
battery_poll_thread(void*)
{
	snooze(3000000);
	while (!sBatteryStop) {
		pmu_read_battery();
		for (int i = 0; i < 50 && !sBatteryStop; i++)
			snooze(100000);		// ~5 s between polls
	}
	return 0;
}


// PowerStatus opens every /dev/power device and issues IDENTIFY_DEVICE; a
// battery identifies itself with kMagicACPIBatteryID. We answer that and the
// battery-info ioctls from the cached PMU record.
static status_t
battery_open(const char* name, uint32 flags, void** cookie)
{
	(void)name; (void)flags;
	*cookie = NULL;
	return B_OK;
}

static status_t
battery_close(void* cookie)
{
	(void)cookie;
	return B_OK;
}

static status_t
battery_free(void* cookie)
{
	(void)cookie;
	return B_OK;
}

static status_t
battery_read(void* cookie, off_t pos, void* buffer, size_t* length)
{
	(void)cookie; (void)pos; (void)buffer;
	*length = 0;
	return B_OK;
}

static status_t
battery_control(void* cookie, uint32 op, void* arg, size_t length)
{
	(void)cookie; (void)length;
	switch (op) {
		case IDENTIFY_DEVICE:
		{
			uint32 magic = kMagicACPIBatteryID;
			return user_memcpy(arg, &magic, sizeof(magic));
		}
		case GET_BATTERY_INFO:
		{
			// Return only the cached record. This ioctl runs on arbitrary
			// threads (the Deskbar replicant polls it often); it must NEVER
			// issue a PMU command, or it races the poll thread over the shared
			// PMU request buffer and desyncs the PMU -> the machine powers off.
			// The poll thread is the sole PMU battery reader.
			acpi_battery_info info;
			memset(&info, 0, sizeof(info));
			bool ac = (sBatFlags & 0x01) != 0;
			int32 current = sBatCurrent;
			if (!ac || current < -20)
				info.state = BATTERY_DISCHARGING;
			else if (sBatMax > 0 && sBatCharge + sBatMax / 50 < sBatMax)
				info.state = BATTERY_CHARGING;
			else
				info.state = BATTERY_NOT_CHARGING;
			if (sBatMax > 0 && sBatCharge * 20 < sBatMax)
				info.state |= BATTERY_CRITICAL_STATE;
			info.current_rate = (current < 0) ? (uint32)(-current)
				: (uint32)current;
			info.capacity = sBatCharge;
			info.voltage = sBatVoltage;
			return user_memcpy(arg, &info, sizeof(info));
		}
		case GET_EXTENDED_BATTERY_INFO:
		{
			acpi_extended_battery_info ext;
			memset(&ext, 0, sizeof(ext));
			ext.power_unit = ACPI_BATTERY_UNIT_MA;
			ext.design_capacity = sBatMax;
			ext.last_full_charge = sBatMax;
			ext.design_voltage = sBatVoltage;
			strlcpy(ext.model_number, "PMU", sizeof(ext.model_number));
			strlcpy(ext.type, "LION", sizeof(ext.type));
			return user_memcpy(arg, &ext, sizeof(ext));
		}
		case WATCH_BATTERY:
		case STOP_WATCHING_BATTERY:
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}

static device_hooks sBatteryHooks = {
	battery_open, battery_close, battery_free, battery_control,
	battery_read, NULL, NULL, NULL, NULL, NULL
};


// Wait for one shift-register byte to complete (polled), then clear the flag.
static inline bool
pmu_poll_sr(void)
{
	for (int i = 0; i < 8000; i++) {
		if ((via_read(VIA_IFR) & VIA_SR_INT) != 0) {
			via_write(VIA_IFR, VIA_SR_INT);
			return true;
		}
		spin(2);
	}
	return false;
}


static inline void
pmu_wait_tack_low(void)
{
	for (int i = 0; i < 8000 && (via_read(VIA_B) & PMU_TACK) != 0; i++)
		spin(2);
}


// Read the PMU's pending interrupt/autopoll data SYNCHRONOUSLY (polled), with
// the VIA SR interrupt masked so its handler can't steal bytes. Returns the
// reply length in buf. Sending PMU_INT_ACK makes the PMU release extint-gpio1,
// so the caller's level-triggered IRQ does not storm.
static int
pmu_read_pending(uint8* buf)
{
	via_write(VIA_IER, VIA_SR_INT);			// mask the VIA SR interrupt
	int len = -1, idx = 0;
	pmu_wait_for_ack();
	pmu_send_byte(PMU_INT_ACK);
	if (pmu_poll_sr()) {
		pmu_wait_tack_low();
		via_write(VIA_B, via_read(VIA_B) | PMU_TREQ);
		pmu_wait_for_ack();
		pmu_recv_byte();					// prime the first inbound byte
		while (pmu_poll_sr()) {
			pmu_wait_tack_low();
			uint8 bite = via_read(VIA_SR);
			via_write(VIA_B, via_read(VIA_B) | PMU_TREQ);
			pmu_wait_for_ack();
			if (len == -1) {
				len = bite;
				if (len > 32) len = 32;
				if (len <= 0) break;
			} else if (idx < 32)
				buf[idx++] = bite;
			if (len >= 0 && idx >= len)
				break;
			pmu_recv_byte();				// prime the next byte
		}
	}
	pmu_sr_off();
	via_write(VIA_IER, VIA_IER_SET | VIA_SR_INT);	// re-enable SR interrupt
	return idx;
}


// Drain any interrupts the PMU already has queued (from firmware/boot) so normal
// operation starts clean. Bounded.
static void
pmu_drain_pending(void)
{
	for (int i = 0; i < 32; i++) {
		uint8 level = *(volatile uint8*)sGpioExt1;
		asm volatile("eieio" ::: "memory");
		if ((level & KEYLARGO_GPIO_LEVEL) != 0)
			break;						// line negated: nothing pending
		uint8 buf[32];
		int idx = pmu_read_pending(buf);
		if (idx <= 0)
			break;
		pmu_handle_data(buf, idx);
	}
}


// extint-gpio1 (level, active low) => the PMU has data; read + dispatch it.
static int32
adb_gpio1_interrupt(void* arg)
{
	uint8 level = *(volatile uint8*)sGpioExt1;
	asm volatile("eieio" ::: "memory");
	if ((level & KEYLARGO_GPIO_LEVEL) != 0)
		return B_UNHANDLED_INTERRUPT;		// line not asserted

	uint8 buf[32];
	int idx = pmu_read_pending(buf);
	if (idx > 0) {
		pmu_handle_data(buf, idx);
		if (sPmuDidInput) {
			sPmuDidInput = false;
			return B_INVOKE_SCHEDULER;
		}
	}
	return B_HANDLED_INTERRUPT;
}


static void
pmu_init_via(void)
{
	via_write(VIA_B, via_read(VIA_B) | PMU_TREQ);			// TREQ idle (negated)
	via_write(VIA_DIRB,
		(via_read(VIA_DIRB) | PMU_TREQ) & ~PMU_TACK);		// TREQ out, TACK in
	via_write(VIA_IER, 0x7f);								// disable all VIA ints
	via_write(VIA_IFR, 0x7f);								// clear all flags
}


// Find the KeyLargo-family mac-io on PCI and return its BAR0 (physical base of
// the mac-io register block) and size.
static status_t
pmu_find_macio_base(phys_addr_t* outBase, size_t* outSize)
{
	pci_module_info* pci = NULL;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return B_ERROR;

	pci_info info;
	status_t result = B_ENTRY_NOT_FOUND;
	for (long i = 0; pci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id != PCI_VENDOR_APPLE)
			continue;
		// KeyLargo (0x0022), Pangea (0x0025), Intrepid (0x003e) = PMU-era.
		if (info.device_id == 0x0022 || info.device_id == 0x0025
				|| info.device_id == 0x003e) {
			*outBase = info.u.h0.base_registers[0];
			*outSize = info.u.h0.base_register_sizes[0];
			result = B_OK;
			break;
		}
	}
	put_module(B_PCI_MODULE_NAME);
	return result;
}


// #pragma mark - device hooks


static status_t
adb_open(const char* name, uint32 flags, void** cookie)
{
	TRACE("open %s\n", name);
	*cookie = NULL;
	return B_OK;
}


static status_t
adb_close(void* cookie)
{
	return B_OK;
}


static status_t
adb_free(void* cookie)
{
	return B_OK;
}


static status_t
adb_read(void* cookie, off_t pos, void* buffer, size_t* length)
{
	*length = 0;
	return B_OK;
}


static status_t
adb_write(void* cookie, off_t pos, const void* buffer, size_t* length)
{
	*length = 0;
	return B_OK;
}


static status_t
keyboard_control(void* cookie, uint32 op, void* arg, size_t length)
{
	switch (op) {
		case KB_READ:
		{
			if (acquire_sem(sKeySem) != B_OK)
				return B_ERROR;
			raw_key_info info;
			cpu_status state = disable_interrupts();
			acquire_spinlock(&sKeyLock);
			info = sKeyQueue[sKeyTail];
			sKeyTail = (sKeyTail + 1) % KB_QUEUE_SIZE;
			release_spinlock(&sKeyLock);
			restore_interrupts(state);
			return user_memcpy(arg, &info, sizeof(raw_key_info));
		}
		case KB_GET_KEYBOARD_ID:
		{
			uint16 id = 0x83;	// generic extended keyboard
			return user_memcpy(arg, &id, sizeof(id));
		}
		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
		case KB_SET_CONTROL_ALT_DEL_TIMEOUT:
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
mouse_control(void* cookie, uint32 op, void* arg, size_t length)
{
	switch (op) {
		case MS_READ:
		{
			if (acquire_sem(sMouseSem) != B_OK)
				return B_ERROR;
			mouse_movement m;
			cpu_status state = disable_interrupts();
			acquire_spinlock(&sMouseLock);
			m = sMouseQueue[sMouseTail];
			sMouseTail = (sMouseTail + 1) % MS_QUEUE_SIZE;
			release_spinlock(&sMouseLock);
			restore_interrupts(state);
			return user_memcpy(arg, &m, sizeof(mouse_movement));
		}
		case MS_NUM_EVENTS:
		{
			int32 count;
			get_sem_count(sMouseSem, &count);
			return count;
		}
		case MS_SET_CLICKSPEED:
			return user_memcpy(&sClickSpeed, arg, sizeof(bigtime_t));
		case MS_SET_TYPE:
		case MS_SET_MAP:
		case MS_SET_ACCEL:
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static device_hooks sKeyboardHooks = {
	adb_open, adb_close, adb_free, keyboard_control,
	adb_read, adb_write, NULL, NULL, NULL, NULL
};

static device_hooks sMouseHooks = {
	adb_open, adb_close, adb_free, mouse_control,
	adb_read, adb_write, NULL, NULL, NULL, NULL
};


// #pragma mark - driver hooks


// Provided by the kernel (arch/ppc/arch_platform.cpp); also used by the
// openfirmware PCI bus manager. type: 0 = Grackle (MPC106), 1 = UniNorth.
extern "C" void ppc_get_pci_host_bridge(uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData);


status_t
init_hardware(void)
{
	// This driver talks to the VIA-CUDA on the Paddington/Grackle mac-io that
	// dingusppc emulates, at the hardcoded MACIO_PHYS_BASE. Real Power Mac G4s
	// use a UniNorth host bridge with a KeyLargo mac-io at a different address;
	// poking MACIO_PHYS_BASE there hits nonexistent device space and machine-
	// checks. Only attach on a Grackle host bridge (dingusppc / beige G3);
	// on UniNorth, refuse so init_driver() never runs. Input on those machines
	// comes via USB, not ADB.
	uint32 hostBridgeType = 1;
	phys_addr_t configAddress = 0;
	phys_addr_t configData = 0;
	ppc_get_pci_host_bridge(&hostBridgeType, &configAddress, &configData);
	// Grackle (dingusppc / old-world beige): input is ADB via VIA-CUDA at a
	// fixed mac-io address. UniNorth (new-world, e.g. the PowerBook Pismo):
	// input is ADB via the VIA-PMU on the KeyLargo mac-io (found on PCI).
	sIsPMU = (hostBridgeType != 0);
	return B_OK;
}


static status_t
init_driver_cuda(void)
{
	void* virtualBase = NULL;
	sRegisterArea = map_physical_memory("via-cuda registers",
		MACIO_PHYS_BASE + VIA_OFFSET, B_PAGE_SIZE * 16,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (sRegisterArea < 0)
		return sRegisterArea;
	sVIABase = (addr_t)virtualBase;

	sKeySem = create_sem(0, "adb keyboard");
	sMouseSem = create_sem(0, "adb mouse");
	if (sKeySem < 0 || sMouseSem < 0) {
		delete_area(sRegisterArea);
		return B_NO_MEMORY;
	}

	// Cuda init: SR shift-in under external clock, idle handshake lines,
	// clear + enable the SR interrupt.
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN);
	via_write(VIA_B, via_read(VIA_B) | CUDA_TIP | CUDA_BYTEACK);
	(void)via_read(VIA_SR);
	via_write(VIA_IFR, 0x7f);

	// Install the handler BEFORE unmasking the SR interrupt in the VIA, so a
	// pending/immediate interrupt can never dispatch into an empty vector.
	install_io_interrupt_handler(VIA_CUDA_IRQ, adb_interrupt, NULL, 0);

	via_write(VIA_IER, VIA_IER_SET | VIA_IF_SR);

	uint8 autopoll[] = { CUDA_PKT_PSEUDO, CUDA_START_STOP_AUTOPOLL, 0x01 };
	cuda_send_polled(autopoll, sizeof(autopoll));

	INFO("VIA-CUDA up (phys %#x), autopoll on\n", MACIO_PHYS_BASE + VIA_OFFSET);
	return B_OK;
}


static status_t
init_driver_pmu(void)
{
	phys_addr_t physBase = 0;
	size_t physSize = 0;
	status_t status = pmu_find_macio_base(&physBase, &physSize);
	if (status != B_OK) {
		dprintf("adb: no KeyLargo mac-io found; VIA-PMU ADB not attaching\n");
		return status;
	}
	if (physSize < PMU_VIA_OFFSET + 0x2000)
		physSize = PMU_VIA_OFFSET + 0x2000;

	void* virtualBase = NULL;
	sRegisterArea = map_physical_memory("via-pmu registers", physBase, physSize,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (sRegisterArea < 0)
		return sRegisterArea;
	sVIABase = (addr_t)virtualBase + PMU_VIA_OFFSET;
	sGpioExt1 = (addr_t)virtualBase + KEYLARGO_EXTINT_GPIO1;

	sKeySem = create_sem(0, "adb keyboard");
	sMouseSem = create_sem(0, "adb mouse");
	if (sKeySem < 0 || sMouseSem < 0) {
		delete_area(sRegisterArea);
		sRegisterArea = -1;
		return B_NO_MEMORY;
	}

	pmu_init_via();

	// Install the handler BEFORE unmasking VIA interrupts.
	install_io_interrupt_handler(PMU_IRQ, adb_pmu_interrupt, NULL, 0);
	via_write(VIA_IER, VIA_IER_SET | VIA_SR_INT | VIA_CB1_INT);

	// Follow Linux's KeyLargo PMU init so it enters a stable, OS-managed state
	// KEY FINDING (lid-open power-off): the machine powers off on the first PMU
	// command sent AFTER SYSTEM_READY (cmd df), whatever that command is - in
	// every ordering. SYSTEM_READY returns a reply; we were not reading it, so
	// the PMU kept trying to hand it back and the next command we transmitted
	// collided with that pending reply and desynced the protocol -> power off.
	// TEST: drop SYSTEM_READY entirely (we do not reconfigure autopoll either -
	// PMU_ADB_CMD is also fatal lid-open - so we just read OpenFirmware's already
	// running autopoll via the gpio1 handler).
	bool pollOffOk = false, pollOk = false, readyOk = false;
	// 1) drain the firmware-era autopoll backlog before we hook the handler,
	pmu_drain_pending();
	// 2) install the "PMU has data" handler (extint-gpio1) to read autopoll data,
	install_io_interrupt_handler(PMU_GPIO1_IRQ, adb_gpio1_interrupt, NULL, 0);
	// 3) ensure ADB (+ tick/environment) interrupts are delivered (mask last).
	uint8 maskCmd[2] = { PMU_SET_INTR_MASK, PMU_INTR_MASK_VALUE };
	bool maskOk = pmu_queue_wait(maskCmd, 2, 1, 0);

	INFO("VIA-PMU up (mac-io %#" B_PRIxPHYSADDR " + %#x, irq %#x/gpio %#x), polloff=%d mask=%d ready=%d poll=%d\n",
		physBase, PMU_VIA_OFFSET, PMU_IRQ, PMU_GPIO1_IRQ, pollOffOk, maskOk, readyOk, pollOk);
	sBatteryStop = false;
	sBatteryThread = spawn_kernel_thread(battery_poll_thread,
		"pmu battery diag", B_LOW_PRIORITY, NULL);
	if (sBatteryThread >= 0)
		resume_thread(sBatteryThread);
	return B_OK;
}


status_t
init_driver(void)
{
	return sIsPMU ? init_driver_pmu() : init_driver_cuda();
}


void
uninit_driver(void)
{
	if (sRegisterArea >= 0) {
		if (sIsPMU) {
			sBatteryStop = true;
			if (sBatteryThread >= 0) {
				status_t r;
				wait_for_thread(sBatteryThread, &r);
				sBatteryThread = -1;
			}
			remove_io_interrupt_handler(PMU_IRQ, adb_pmu_interrupt, NULL);
			remove_io_interrupt_handler(PMU_GPIO1_IRQ, adb_gpio1_interrupt, NULL);
		} else
			remove_io_interrupt_handler(VIA_CUDA_IRQ, adb_interrupt, NULL);
		delete_area(sRegisterArea);
		sRegisterArea = -1;
	}
	if (sKeySem >= 0) delete_sem(sKeySem);
	if (sMouseSem >= 0) delete_sem(sMouseSem);
}


const char**
publish_devices(void)
{
	static const char* devicesPMU[] = {
		"input/keyboard/adb/0",
		"input/mouse/adb/0",
		"power/pmu_battery/0",
		NULL
	};
	static const char* devicesBase[] = {
		"input/keyboard/adb/0",
		"input/mouse/adb/0",
		NULL
	};
	return sIsPMU ? devicesPMU : devicesBase;
}


device_hooks*
find_device(const char* name)
{
	if (strstr(name, "pmu_battery") != NULL)
		return &sBatteryHooks;
	if (strstr(name, "keyboard") != NULL)
		return &sKeyboardHooks;
	if (strstr(name, "mouse") != NULL)
		return &sMouseHooks;
	return NULL;
}
