/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sean Malseed, actionretro@pm.me
 *		Claude (Anthropic), paired via Claude Code
 *
 * macio_snd - hmulti_audio driver for the sound cells of the Apple "mac-io"
 * chips.
 *
 * Two generations of hardware, one driver:
 *
 *   davbus	Burgundy / Screamer, on the iMac G3 and its contemporaries. The
 *		cell is a codec register interface with two DBDMA channels bolted
 *		on, and the codec is reached through the cell itself.
 *   i2s	Tumbler / Snapper (TAS3004) / Onyx, on the G4 portables and
 *		later. The cell is a configurable serial bus; the codec lives on a
 *		separate Keywest i2c bus and its mute lines are mac-io GPIOs.
 *
 * Both move samples the same way: a DBDMA command ring streams big-endian
 * 16-bit stereo frames straight out of memory with no CPU involvement. What
 * differs is the plumbing that has to be correct before the first frame moves
 * - which is why the two paths differ only in prepare_transport() below.
 *
 * None of the addressing is discoverable from the kernel. Which cells exist,
 * where they sit inside mac-io, their interrupts, the codec's i2c address and
 * the GPIO assignments are all properties of the Open Firmware device tree,
 * and OF is not callable once the kernel is up. The boot loader reads the lot
 * and hands it over in kernel_args (see arch_audio_info.h); this driver
 * refuses to publish a device if that description is missing, rather than
 * guessing at register offsets.
 *
 * Endianness, verified against dingusppc and matching Linux's out_le32 use:
 * the DBDMA registers, the DBDMA command descriptors and the i2s cell
 * registers are all LITTLE-endian; the audio sample data on the wire is
 * BIG-endian int16, so native PowerPC stores of samples are already correct.
 * Descriptors little, samples big - classic Apple DBDMA.
 */

#include <KernelExport.h>
#include <Drivers.h>
#include <ByteOrder.h>
#include <driver_settings.h>
#include <stdlib.h>
#include <hmulti_audio.h>
#include <string.h>

#include <arch_audio_info.h>


extern "C" const ppc_audio_info* ppc_get_audio_info();


// ---------------------------------------------------------------------------
//	mac-io registers
// ---------------------------------------------------------------------------

// The mac-io register window. Everything this driver touches is an offset
// inside it: feature control at 0x3c, the GPIO bytes, the i2s cell and the
// DBDMA channels.
#define MACIO_WINDOW_SIZE		0x80000

// Feature control register 1 gates the i2s cells' power and clocks. Offsets
// and bits from Linux's asm/keylargo.h; KeyLargo, Pangea and Intrepid agree.
#define KEYLARGO_FCR1			0x3c
#define KL1_I2S0_CELL_ENABLE		0x00000400
#define KL1_I2S0_CLK_ENABLE_BIT		0x00001000
#define KL1_I2S0_ENABLE			0x00002000

// A GPIO is one byte: bit 2 makes the pin an output, bit 0 is the level we
// drive, bit 1 is the level read back.
#define KEYLARGO_GPIO_OUTPUT_ENABLE	0x04
#define KEYLARGO_GPIO_OUTPUT_DATA	0x01
#define KEYLARGO_GPIO_INPUT_DATA	0x02

// DBDMA channel registers (Apple's DBDMA, one 0x100 block per channel).
#define DBDMA_CH_CTRL			0x00
#define DBDMA_CH_STAT			0x04
#define DBDMA_CMD_PTR_LO		0x0c
#define DBDMA_RUN			0x8000
#define DBDMA_PAUSE			0x4000
#define DBDMA_FLUSH			0x2000
#define DBDMA_WAKE			0x1000
#define DBDMA_DEAD			0x0800
#define DBDMA_ACTIVE			0x0400

#define DBDMA_OUTPUT_MORE		0
#define DBDMA_OUTPUT_LAST		1
#define DBDMA_BR_ALWAYS			0x0c	// cmd_bits branch field = always
#define DBDMA_INTR_ALWAYS		0x30	// cmd_bits interrupt field = always

// Keywest i2c cell. The registers are logical indices spaced by the device
// tree's "AAPL,address-step" (16 bytes on every machine seen so far), and the
// bus number lives in the top nibble of the mode register.
#define KW_I2C_REG_MODE			0
#define KW_I2C_REG_CONTROL		1
#define KW_I2C_REG_STATUS		2
#define KW_I2C_REG_ISR			3
#define KW_I2C_REG_IER			4
#define KW_I2C_REG_ADDR			5
#define KW_I2C_REG_SUBADDR		6
#define KW_I2C_REG_DATA			7

#define KW_I2C_MODE_100KHZ		0x00
#define KW_I2C_MODE_50KHZ		0x01
// What Linux defaults to when the device tree does not say otherwise, and what
// this driver uses: a slave that will not answer at 100 kHz may answer here,
// and nothing we do is rate sensitive.
#define KW_I2C_MODE_25KHZ		0x02
#define KW_I2C_MODE_STANDARD		0x04
#define KW_I2C_MODE_STANDARDSUB		0x08
#define KW_I2C_MODE_CHANNEL_SHIFT	4

#define KW_I2C_CTL_XADDR		0x02
#define KW_I2C_CTL_STOP			0x04

#define KW_I2C_STAT_BUSY		0x01
#define KW_I2C_STAT_LAST_AAK		0x02

#define KW_I2C_IRQ_DATA			0x01
#define KW_I2C_IRQ_ADDR			0x02
#define KW_I2C_IRQ_STOP			0x04
#define KW_I2C_IRQ_MASK			0x0f

// TAS3004 ("Snapper") codec registers, from Linux's sound/aoa/codecs/tas.h.
#define TAS_REG_MCS			0x01	// 1 byte: serial port mode
#define TAS_REG_DRC			0x02	// 6 bytes: dynamic range control
#define TAS_REG_VOL			0x04	// 6 bytes: left, right
#define TAS_REG_TREBLE			0x05	// 1 byte
#define TAS_REG_BASS			0x06	// 1 byte
#define TAS_REG_LMIX			0x07	// 9 bytes: three inputs
#define TAS_REG_RMIX			0x08	// 9 bytes
#define TAS_REG_ACR			0x40	// 1 byte: analog control
#define TAS_REG_MCS2			0x43	// 1 byte

#define TAS_MCS_SCLK64			0x40
#define TAS_MCS_SPORT_MODE_I2S		0x20
#define TAS_MCS_SPORT_WL_16BIT		0x00
#define TAS_MCS_SPORT_WL_24BIT		0x03
#define TAS_ACR_ANALOG_PDOWN		0x01
// 0 dB, from the tables in Linux's tas-basstreble.h. Left at whatever they
// power up as, these two can cut the whole band.
#define TAS_TONE_FLAT			114
#define TAS_MCS2_ALLPASS		0x02	// bypass the biquad filters

// i2s cell registers. Each is 16 bytes apart; values from Linux's
// sound/aoa/soundbus/i2sbus/interface.h.
#define I2S_REG_INTR_CTL		0x00
#define I2S_PENDING_CLOCKS_STOPPED	(1 << 24)
#define I2S_REG_SERIAL_FORMAT		0x10
#define I2S_REG_FRAME_COUNT		0x40
#define I2S_REG_DATA_WORD_SIZES		0x60

#define I2S_SF_CLOCK_SOURCE_45MHZ	(1 << 30)
#define I2S_SF_MCLKDIV(div)		((((div) / 2 - 1) << 24) & 0x1f000000)
#define I2S_SF_SCLKDIV(div)		((((div) / 2 - 1) << 20) & 0x00f00000)
#define I2S_SF_SCLK_MASTER		(1 << 19)
#define I2S_SF_SERIAL_FORMAT_I2S_64X	(1 << 16)

#define I2S_DWS_NUM_CHANNELS_IN_SHIFT	24
#define I2S_DWS_NUM_CHANNELS_OUT_SHIFT	8
#define I2S_DWS_DATA_16BIT		0

// The three clock sources the cell can divide down. 45.1584 MHz is the one
// that divides evenly for the 44.1 kHz family, which is all we ask for.
#define I2S_CLOCK_45MHZ			45158400


typedef struct {
	uint16	req_count;
	uint8	cmd_bits;
	uint8	cmd_key;		// opcode = cmd_key >> 4
	uint32	address;
	uint32	cmd_arg;
	uint16	res_count;
	uint16	xfer_stat;
} dbdma_cmd;


// ---------------------------------------------------------------------------
//	audio format
// ---------------------------------------------------------------------------

#define SND_RATE			44100
#define SND_CHANNELS			2
#define SND_SAMPLE_SIZE			2		// signed 16-bit
#define SND_FRAME_SIZE			(SND_CHANNELS * SND_SAMPLE_SIZE)

// The TAS3004 and its relatives want a 256*fs master clock and a 64*fs bit
// clock, which is what the divisors below are computed for.
#define I2S_MCLK_PER_FRAME		256
#define I2S_SCLK_PER_FRAME		64

#define NUM_BUFFERS			2
#define FRAMES_PER_BUFFER		1024

#define MULTI_AUDIO_DEV_PATH		"audio/hmulti"
#define MULTI_AUDIO_BASE_ID		1024
#define MULTI_AUDIO_MASTER_ID		0


typedef struct {
	const ppc_audio_info*	info;
	bool			is_i2s;		// false = davbus (Burgundy/Screamer)

	area_id			reg_area;
	addr_t			macio_base;	// virtual base of the mac-io window
	uint32			i2c_step;	// bytes between Keywest register indices
	uint32			codec_channel;	// resolved by probing, not assumed
	uint8			codec_address;
	addr_t			channel_base;	// playback DBDMA channel
	addr_t			i2s_base;	// i2s cell (unused on davbus)

	// Playback DMA ring. User accessible: the media kit writes here.
	area_id			buffer_area;
	uint8*			buffer_base;
	phys_addr_t		buffer_phys;
	void*			buffers[NUM_BUFFERS];

	// DBDMA command list, kernel only.
	area_id			cmd_area;
	dbdma_cmd*		cmds;
	phys_addr_t		cmds_phys;

	uint32			num_buffers;
	uint32			buffer_frames;
	uint32			format;
	uint32			rate;

	sem_id			buffer_ready_sem;
	thread_id		pace_thread;
	volatile bool		running;
	spinlock		lock;
	uint32			buffer_cycle;
	uint32			frames_count;
	bigtime_t		real_time;
} device_t;


int32 api_version = B_CUR_DRIVER_API_VERSION;

static device_t sDevice;
static bool sSelfTest;
static bool sI2SEnabled;
static bool sTrace;
static bool sCodecEnabled;
static bool sScanI2C;
// Whether the cell reported anything at all during the last address probe.
// "Nobody acknowledged" and "the controller never moved" look identical from
// the outside and mean completely different things.
static bool sLastProbeSawInterrupt;
// 24-bit codec gain, unity if the registers are 4.20 fixed point. Overridable
// from the settings file, because that scaling is the one thing here that
// cannot be verified: the TAS3004 is write-only.
static uint32 sCodecGain = 0x100000;

// The mixer, in the units the multi_audio API speaks: dB per channel, plus a
// mute. Zero is unity, which is where the codec comes up.
static float sMixGain[2] = { 0.0f, 0.0f };
static bool sMixMuted = false;
static bool sCodecAllPass;
static bool sInvertMute;


// ---------------------------------------------------------------------------
//	register access
//
//	mac-io is a little-endian island on a big-endian machine: every register
//	here is byte-swapped, and the write has to have landed before the next one
//	is issued, hence the eieio.
// ---------------------------------------------------------------------------

static inline uint32
macio_read32(uint32 offset)
{
	uint32 value = *(volatile uint32*)(sDevice.macio_base + offset);
	__asm__ volatile("eieio" ::: "memory");
	return B_LENDIAN_TO_HOST_INT32(value);
}


static inline void
macio_write32(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sDevice.macio_base + offset)
		= B_HOST_TO_LENDIAN_INT32(value);
	__asm__ volatile("eieio" ::: "memory");
}


static inline uint8
macio_read8(uint32 offset)
{
	uint8 value = *(volatile uint8*)(sDevice.macio_base + offset);
	__asm__ volatile("eieio" ::: "memory");
	return value;
}


static inline void
macio_write8(uint32 offset, uint8 value)
{
	*(volatile uint8*)(sDevice.macio_base + offset) = value;
	__asm__ volatile("eieio" ::: "memory");
}


static inline void
dbdma_write(uint32 reg, uint32 value)
{
	*(volatile uint32*)(sDevice.channel_base + reg)
		= B_HOST_TO_LENDIAN_INT32(value);
	__asm__ volatile("eieio" ::: "memory");
}


static inline uint32
dbdma_read(uint32 reg)
{
	uint32 value = *(volatile uint32*)(sDevice.channel_base + reg);
	__asm__ volatile("eieio" ::: "memory");
	return B_LENDIAN_TO_HOST_INT32(value);
}


static inline uint32
i2s_read(uint32 reg)
{
	uint32 value = *(volatile uint32*)(sDevice.i2s_base + reg);
	__asm__ volatile("eieio" ::: "memory");
	return B_LENDIAN_TO_HOST_INT32(value);
}


static inline void
i2s_write(uint32 reg, uint32 value)
{
	*(volatile uint32*)(sDevice.i2s_base + reg)
		= B_HOST_TO_LENDIAN_INT32(value);
	__asm__ volatile("eieio" ::: "memory");
}


// ---------------------------------------------------------------------------
//	hardware bring-up
// ---------------------------------------------------------------------------

/*!	Announce a hardware access before making it.

	Bringing up a cell that has never been touched on this machine can fault or
	wedge, and when it does the useful question is which access did it. So each
	step says what it is about to do, and forces the output on while it does -
	a diagnostic image is normally built quiet so the boot splash shows, which
	would otherwise leave exactly this sequence invisible.
*/
static void
trace_step(const char* what)
{
	if (!sTrace)
		return;
	bool wasEnabled = set_dprintf_enabled(true);
	dprintf("macio_snd: -> %s\n", what);
	set_dprintf_enabled(wasEnabled);
}


//! Set or clear bits in the mac-io feature control register that gates the
//	i2s cell's power and clocks.
static void
set_feature_bits(uint32 bits, bool on)
{
	uint32 value = macio_read32(KEYLARGO_FCR1);
	macio_write32(KEYLARGO_FCR1, on ? (value | bits) : (value & ~bits));
	snooze(2000);
}


// ---------------------------------------------------------------------------
//	The codec, over the Keywest i2c bus
//
//	Polled rather than interrupt driven: this runs a handful of times at
//	start-up and never again, so a state machine hung off an interrupt would be
//	all cost and no benefit.
// ---------------------------------------------------------------------------

static inline uint8
kw_read(uint32 index)
{
	return macio_read8(sDevice.info->i2c_offset + (index * sDevice.i2c_step));
}


static inline void
kw_write(uint32 index, uint8 value)
{
	macio_write8(sDevice.info->i2c_offset + (index * sDevice.i2c_step), value);
}


/*!	Does anything answer at \a address on \a channel?

	An address-only transaction: the cell puts the address on the wire, we look
	at whether it was acknowledged, and we stop before any data changes hands.
	Safe to point at an address that turns out to be something else entirely.
*/
static bool
i2c_probe_address(uint32 channel, uint8 address)
{
	for (int i = 0; i < 50; i++) {
		if ((kw_read(KW_I2C_REG_STATUS) & KW_I2C_STAT_BUSY) == 0)
			break;
		snooze(1000);
	}

	kw_write(KW_I2C_REG_ISR, kw_read(KW_I2C_REG_ISR));
	kw_write(KW_I2C_REG_MODE, (channel << KW_I2C_MODE_CHANNEL_SHIFT)
		| KW_I2C_MODE_STANDARD | KW_I2C_MODE_25KHZ);
	kw_write(KW_I2C_REG_STATUS, 0);
	kw_write(KW_I2C_REG_IER, 0);
	kw_write(KW_I2C_REG_ADDR, address & 0xfe);
	kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_XADDR);

	bool acknowledged = false;
	sLastProbeSawInterrupt = false;
	for (int guard = 0; guard < 200; guard++) {
		uint8 pending = kw_read(KW_I2C_REG_ISR);
		if (pending == 0) {
			snooze(200);
			continue;
		}
		sLastProbeSawInterrupt = true;
		if ((pending & KW_I2C_IRQ_ADDR) != 0) {
			acknowledged = (kw_read(KW_I2C_REG_STATUS)
				& KW_I2C_STAT_LAST_AAK) != 0;
			kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_STOP);
			kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_ADDR);
		} else if ((pending & KW_I2C_IRQ_STOP) != 0) {
			kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_STOP);
			break;
		} else
			kw_write(KW_I2C_REG_ISR, pending);
	}
	return acknowledged;
}


/*!	Find the codec for real, rather than trusting one reading of the device
	tree.

	Two things about `deq@6a` are genuinely ambiguous: whether Apple's unit
	address is the 8-bit write byte (so the 7-bit address is 0x35) or the
	7-bit address itself (so the byte on the wire is 0xd4), and which of the
	cell's buses the part hangs off. Both are cheap to settle by asking, and
	an address probe cannot disturb whatever is really there.
*/
static bool
find_codec(void)
{
	uint32 fromTree = sDevice.info->codec_i2c_addr;
	uint8 candidates[2] = { (uint8)(fromTree & 0xfe),
		(uint8)((fromTree << 1) & 0xfe) };

	// The device tree's own channel first, then the others.
	for (uint32 i = 0; i < 5; i++) {
		uint32 channel = i == 0 ? sDevice.info->i2c_channel : i - 1;
		if (i > 0 && channel == sDevice.info->i2c_channel)
			continue;
		for (uint32 c = 0; c < 2; c++) {
			if (c == 1 && candidates[1] == candidates[0])
				continue;
			if (!i2c_probe_address(channel, candidates[c]))
				continue;
			sDevice.codec_channel = channel;
			sDevice.codec_address = candidates[c];
			dprintf("macio_snd: codec answers at i2c %#x on channel %"
				B_PRIu32 " (device tree said %#" B_PRIx32 " channel %"
				B_PRIu32 ")\n", candidates[c], channel, fromTree,
				sDevice.info->i2c_channel);
			return true;
		}
	}
	return false;
}


/*!	Are we looking at the i2c cell at all?

	A scan that finds nothing on any bus says far more than a wrong address
	would: a working bus almost always has something on it. The likeliest way
	to see silence everywhere is to be reading and writing the wrong addresses
	entirely - the register spacing comes from "AAPL,address-step" and is 16
	bytes on every machine seen so far, but "so far" is doing a lot of work in
	that sentence.

	So: write a known value to the mode register at each plausible spacing and
	see which one reads back. Every candidate keeps the highest register inside
	the cell's own 0x1000 window, so nothing outside it is touched, and the
	mode register configures the i2c cell and nothing else.
*/
static void
probe_i2c_layout(void)
{
	static const uint32 kCandidateSteps[] = { 0x10, 0x100, 0x04, 0x01 };
	uint32 base = sDevice.info->i2c_offset;

	dprintf("macio_snd: i2c cell at +%#" B_PRIx32 ", device tree step %#"
		B_PRIx32 ", using %#" B_PRIx32 "\n", base,
		sDevice.info->i2c_address_step, sDevice.i2c_step);

	for (uint32 i = 0; i < B_COUNT_OF(kCandidateSteps); i++) {
		uint32 step = kCandidateSteps[i];
		macio_write8(base + KW_I2C_REG_MODE * step, KW_I2C_MODE_STANDARDSUB);
		snooze(1000);
		dprintf("macio_snd: i2c step %#" B_PRIx32 ": mode reads %#x (wrote %#x)"
			", status %#x, isr %#x\n", step,
			macio_read8(base + KW_I2C_REG_MODE * step), KW_I2C_MODE_STANDARDSUB,
			macio_read8(base + KW_I2C_REG_STATUS * step),
			macio_read8(base + KW_I2C_REG_ISR * step));
	}

	// The raw window, so a spacing nobody guessed still shows itself as a
	// pattern of non-zero bytes.
	for (uint32 row = 0; row < 0x80; row += 0x20) {
		char line[3 * 0x20 + 4];
		size_t used = 0;
		for (uint32 i = 0; i < 0x20; i++) {
			used += snprintf(line + used, sizeof(line) - used, " %02x",
				macio_read8(base + row + i));
		}
		dprintf("macio_snd: i2c +%02" B_PRIx32 ":%s\n", row, line);
	}
}


/*!	One transaction, narrated.

	The scan can only say "nobody answered", which is the same answer for a bus
	with nothing on it, a controller that is not really clocking the wire, and
	a transaction that goes wrong in a way the state machine quietly absorbs.
	This runs a single address and prints what the controller actually did at
	each step, which is the difference between those three.
*/
static void
trace_i2c_transaction(uint32 channel, uint8 address)
{
	kw_write(KW_I2C_REG_ISR, kw_read(KW_I2C_REG_ISR));
	kw_write(KW_I2C_REG_MODE, (channel << KW_I2C_MODE_CHANNEL_SHIFT)
		| KW_I2C_MODE_STANDARD | KW_I2C_MODE_25KHZ);
	kw_write(KW_I2C_REG_STATUS, 0);
	kw_write(KW_I2C_REG_IER, 0);
	kw_write(KW_I2C_REG_ADDR, address & 0xfe);

	dprintf("macio_snd: i2c trace %#x on channel %" B_PRIu32
		": before start, status %#x isr %#x\n", address, channel,
		kw_read(KW_I2C_REG_STATUS), kw_read(KW_I2C_REG_ISR));

	kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_XADDR);

	for (int step = 0; step < 12; step++) {
		snooze(2000);
		uint8 isr = kw_read(KW_I2C_REG_ISR);
		uint8 status = kw_read(KW_I2C_REG_STATUS);
		dprintf("macio_snd:   step %d: isr %#x status %#x control %#x\n",
			step, isr, status, kw_read(KW_I2C_REG_CONTROL));
		if (isr != 0) {
			kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_STOP);
			kw_write(KW_I2C_REG_ISR, isr);
		}
	}
}


//! Every address that answers, on every bus of the cell. Diagnostic only.
static void
scan_i2c(void)
{
	for (uint32 channel = 0; channel < 4; channel++) {
		char found[160];
		size_t used = 0;
		bool anyInterrupt = false;
		found[0] = '\0';
		for (uint32 address = 2; address < 0xff; address += 2) {
			bool acknowledged = i2c_probe_address(channel, (uint8)address);
			anyInterrupt |= sLastProbeSawInterrupt;
			if (!acknowledged)
				continue;
			if (used < sizeof(found) - 8) {
				used += snprintf(found + used, sizeof(found) - used, " %#lx",
					(unsigned long)address);
			}
		}
		dprintf("macio_snd: i2c channel %" B_PRIu32 ":%s%s\n", channel,
			found[0] != '\0' ? found : " nothing answered",
			anyInterrupt ? "" : " (and the controller never responded)");
	}
}


/*!	Write \a length bytes to one register of the codec.

	"Standard sub" mode is the cell doing the whole transaction for us: it
	sends the device address and the register number, then hands us one
	interrupt flag per byte. We poll those flags rather than take the
	interrupt.
*/
static status_t
codec_write(uint8 subaddress, const uint8* data, size_t length)
{
	for (int i = 0; i < 100; i++) {
		if ((kw_read(KW_I2C_REG_STATUS) & KW_I2C_STAT_BUSY) == 0)
			break;
		snooze(1000);
	}

	kw_write(KW_I2C_REG_ISR, kw_read(KW_I2C_REG_ISR));
	kw_write(KW_I2C_REG_MODE,
		(sDevice.codec_channel << KW_I2C_MODE_CHANNEL_SHIFT)
			| KW_I2C_MODE_STANDARDSUB | KW_I2C_MODE_25KHZ);
	kw_write(KW_I2C_REG_STATUS, 0);
	kw_write(KW_I2C_REG_IER, 0);
	kw_write(KW_I2C_REG_ADDR, sDevice.codec_address);
	kw_write(KW_I2C_REG_SUBADDR, subaddress);
	kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_XADDR);

	size_t sent = 0;
	for (int guard = 0; guard < 2000; guard++) {
		uint8 pending = kw_read(KW_I2C_REG_ISR);
		if (pending == 0) {
			snooze(200);
			continue;
		}

		if ((pending & KW_I2C_IRQ_ADDR) != 0) {
			if ((kw_read(KW_I2C_REG_STATUS) & KW_I2C_STAT_LAST_AAK) == 0) {
				// Nobody at that address.
				kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_STOP);
				kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_MASK);
				return B_DEV_INVALID_IOCTL;	// not acknowledged
			}
			kw_write(KW_I2C_REG_DATA, data[sent++]);
			kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_ADDR);
		} else if ((pending & KW_I2C_IRQ_DATA) != 0) {
			// ★ Check the acknowledge on every byte, not just the address.
			// Without this a transfer whose data phase is being refused
			// reports success, and "codec initialised" becomes a claim about
			// the first byte only.
			if ((kw_read(KW_I2C_REG_STATUS) & KW_I2C_STAT_LAST_AAK) == 0) {
				kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_STOP);
				kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_MASK);
				dprintf("macio_snd: codec refused byte %d of register %#x\n",
					(int)sent, subaddress);
				return B_DEV_INVALID_IOCTL;
			}
			if (sent < length)
				kw_write(KW_I2C_REG_DATA, data[sent++]);
			else
				kw_write(KW_I2C_REG_CONTROL, KW_I2C_CTL_STOP);
			kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_DATA);
		} else if ((pending & KW_I2C_IRQ_STOP) != 0) {
			kw_write(KW_I2C_REG_ISR, KW_I2C_IRQ_STOP);
			return B_OK;
		} else
			kw_write(KW_I2C_REG_ISR, pending);
	}
	return B_TIMED_OUT;
}


static void set_audio_gpio(const char* name, bool asserted);


/*!	Pulse the codec's reset line.

	This is why nothing ever acknowledged on the i2c bus: the part is held in
	reset out of Open Firmware and cannot answer until released. Linux's
	tas_reset_init() does the same pulse before its first write, with the
	amplifiers muted around it so the codec coming up does not thump the
	speaker.

	The line is an ordinary mac-io GPIO named "hw-reset" in the device tree -
	no platform-function bytecode involved, despite the codec node carrying a
	"platform-do-tas-codec-ref" property (that one is a cross-reference binding
	the codec to the sound node, not an operation).
*/
static void
reset_codec(void)
{
	set_audio_gpio("hw-reset", false);
	snooze(5000);
	set_audio_gpio("hw-reset", true);
	snooze(20000);
	set_audio_gpio("hw-reset", false);
	snooze(10000);
}


/*!	Bring the TAS3004 up far enough to pass audio.

	Open Firmware plays the startup chime through this codec, but it does not
	leave it in a state we can use - the DMA ring can be looping perfectly and
	the machine still silent, which is exactly what happened here. Four things
	matter: the serial port has to agree with how the i2s cell is clocking it,
	the analog section has to be powered, and the volume and mixer have to be
	somewhere other than zero.
*/
/*	The TAS3004 gain table, generated the way Linux generates it:

		hwvalue = 1048576.0 * exp(0.057564628 * dB * 2)

	Entry 0 is silence; entries 1 to 177 run from -70.0 dB to +18.0 dB in half
	decibel steps, so entry 141 is unity. The MIXER registers take these values
	as they stand and the VOLUME register takes them shifted down by four -
	that is the whole reason this table is here rather than a multiply.
*/
#define TAS_GAIN_SILENT		0
#define TAS_GAIN_UNITY		141
#define TAS_GAIN_MAX		177

static const uint32 kTasGainTable[] = {
	0x000000, 0x00014b, 0x00015f, 0x000174, 0x00018a, 0x0001a1,
	0x0001ba, 0x0001d4, 0x0001f0, 0x00020d, 0x00022c, 0x00024d,
	0x000270, 0x000295, 0x0002bc, 0x0002e6, 0x000312, 0x000340,
	0x000372, 0x0003a6, 0x0003dd, 0x000418, 0x000456, 0x000498,
	0x0004de, 0x000528, 0x000576, 0x0005c9, 0x000620, 0x00067d,
	0x0006e0, 0x000748, 0x0007b7, 0x00082c, 0x0008a8, 0x00092b,
	0x0009b6, 0x000a49, 0x000ae5, 0x000b8b, 0x000c3a, 0x000cf3,
	0x000db8, 0x000e88, 0x000f64, 0x00104e, 0x001145, 0x00124b,
	0x001361, 0x001487, 0x0015be, 0x001708, 0x001865, 0x0019d8,
	0x001b60, 0x001cff, 0x001eb7, 0x002089, 0x002276, 0x002481,
	0x0026ab, 0x0028f5, 0x002b63, 0x002df5, 0x0030ae, 0x003390,
	0x00369e, 0x0039db, 0x003d49, 0x0040ea, 0x0044c3, 0x0048d6,
	0x004d27, 0x0051b9, 0x005691, 0x005bb2, 0x006121, 0x0066e3,
	0x006cfb, 0x007370, 0x007a48, 0x008186, 0x008933, 0x009154,
	0x0099f1, 0x00a310, 0x00acba, 0x00b6f6, 0x00c1cd, 0x00cd49,
	0x00d973, 0x00e655, 0x00f3fb, 0x010270, 0x0111c0, 0x0121f9,
	0x013328, 0x01455b, 0x0158a2, 0x016d0e, 0x0182af, 0x019999,
	0x01b1de, 0x01cb94, 0x01e6cf, 0x0203a7, 0x022235, 0x024293,
	0x0264db, 0x02892c, 0x02afa3, 0x02d862, 0x03038a, 0x033142,
	0x0361af, 0x0394fa, 0x03cb50, 0x0404de, 0x0441d5, 0x048268,
	0x04c6d0, 0x050f44, 0x055c04, 0x05ad50, 0x06036e, 0x065ea5,
	0x06bf44, 0x07259d, 0x079207, 0x0804dc, 0x087e80, 0x08ff59,
	0x0987d5, 0x0a1866, 0x0ab189, 0x0b53be, 0x0bff91, 0x0cb591,
	0x0d765a, 0x0e4290, 0x0f1adf, 0x100000, 0x10f2b4, 0x11f3c9,
	0x13041a, 0x14248e, 0x15561a, 0x1699c0, 0x17f094, 0x195bb8,
	0x1adc61, 0x1c73d5, 0x1e236d, 0x1fec98, 0x21d0d9, 0x23d1cd,
	0x25f125, 0x2830af, 0x2a9254, 0x2d1818, 0x2fc420, 0x3298b0,
	0x35982f, 0x38c528, 0x3c224c, 0x3fb278, 0x4378b0, 0x477829,
	0x4bb446, 0x5030a1, 0x54f106, 0x59f980, 0x5f4e52, 0x64f403,
	0x6aef5e, 0x714575, 0x77fbaa, 0x7f17af,
};


//! Half-decibel step for a gain in dB, clamped to the table.
static uint32
tas_gain_for_db(float decibels)
{
	int32 step = (int32)(decibels * 2.0f + (decibels < 0 ? -0.5f : 0.5f));
	int32 index = step + TAS_GAIN_UNITY;
	if (index < 1)
		index = 1;
	if (index > TAS_GAIN_MAX)
		index = TAS_GAIN_MAX;
	return kTasGainTable[index];
}


/*!	Push the current mixer setting into the codec's volume register.

	Mute is the table's silent entry rather than a separate control, which is
	what Linux does too - the mute GPIOs turn out not to gate what comes out of
	this machine's speaker, so the codec has to do it.
*/
static void
apply_codec_volume(void)
{
	if (!sDevice.is_i2s || sDevice.codec_address == 0)
		return;

	uint8 volume[6];
	for (int channel = 0; channel < 2; channel++) {
		uint32 value = sMixMuted
			? kTasGainTable[TAS_GAIN_SILENT] : tas_gain_for_db(sMixGain[channel]);
		volume[channel * 3 + 0] = (uint8)(value >> 20);
		volume[channel * 3 + 1] = (uint8)(value >> 12);
		volume[channel * 3 + 2] = (uint8)(value >> 4);
	}
	codec_write(TAS_REG_VOL, volume, sizeof(volume));
}


static void
init_codec(void)
{
	trace_step("release the codec from reset");
	reset_codec();

	if (sScanI2C) {
		probe_i2c_layout();
		trace_i2c_transaction(sDevice.info->i2c_channel,
			(uint8)(sDevice.info->codec_i2c_addr & 0xfe));
		scan_i2c();
	}

	if (!find_codec()) {
		dprintf("macio_snd: nothing answered for the codec (device tree said "
			"%#" B_PRIx32 " channel %" B_PRIu32 ")\n",
			sDevice.info->codec_i2c_addr, sDevice.info->i2c_channel);
		return;
	}

	/*	I2S framing, 64x bit clock, and 24-bit words - which is what Linux
		programs whatever the sample format is. The cell puts our 16-bit
		samples at the top of the same slot either way, so the codec latches
		them correctly, and the extra bits it also latches are the zeros the
		cell shifts out behind them.
	*/
	uint8 value = TAS_MCS_SCLK64 | TAS_MCS_SPORT_MODE_I2S
		| TAS_MCS_SPORT_WL_24BIT;
	status_t status = codec_write(TAS_REG_MCS, &value, 1);
	if (status != B_OK) {
		dprintf("macio_snd: codec at %#x stopped answering: %s\n",
			sDevice.codec_address, strerror(status));
		return;
	}

	// Analog section held down while the rest is programmed and brought up at
	// the end - the order Linux uses.
	value = TAS_ACR_ANALOG_PDOWN;
	status |= codec_write(TAS_REG_ACR, &value, 1);

	// The biquad filters sit in the signal path unless bypassed. They should
	// come out of a hardware reset passing everything through, but if they do
	// not, they pass nothing at all - so the bypass is available as a switch.
	value = sCodecAllPass ? TAS_MCS2_ALLPASS : 0;
	status |= codec_write(TAS_REG_MCS2, &value, 1);

	// Dynamic range compression off. Bytes from tas3004_set_drc(); with
	// compression disabled the third (range) byte does not matter. Left at
	// whatever it powers up as, this can squash the output to nothing.
	static const uint8 kCompressionOff[6]
		= { 0x51, 0x02, 0x00, 0xb0, 0x60, 0xa0 };
	status |= codec_write(TAS_REG_DRC, kCompressionOff, sizeof(kCompressionOff));

	// Treble and bass flat. The last thing Linux's tas_reset_init does that
	// this did not, and a tone control sitting at full cut is silence.
	value = TAS_TONE_FLAT;
	status |= codec_write(TAS_REG_TREBLE, &value, 1);
	value = TAS_TONE_FLAT;
	status |= codec_write(TAS_REG_BASS, &value, 1);

	/*	★ The volume register and the mixer registers are on DIFFERENT
		SCALES, and this driver was writing the same bytes to both.

		Linux keeps one gain table, generated by

			hwvalue = 1048576.0 * exp(0.057564628 * dB * 2)

		so unity is 0x100000 and the table ends at +18 dB = 0x7f0000. It
		writes that value raw into the mixer registers, but shifted DOWN BY
		FOUR into the volume register - "the two tables are similar enough
		when we shift the mixer table down by 4 bits". So unity means
		0x100000 in the mixer and 0x010000 in the volume.

		Writing the mixer value into the volume register asks for sixteen
		times unity - 24 dB past the top of the table. It also explains why
		sweeping the gain never changed anything: 0x100000, 0x400000,
		0x800000 and 0xffffff are all outside the range the part accepts.

		`codec_gain` is in MIXER scale, so 0x100000 is 0 dB.
	*/
	uint8 gain[3] = { (uint8)(sCodecGain >> 16), (uint8)(sCodecGain >> 8),
		(uint8)sCodecGain };

	// The volume register comes from the mixer, so that a fresh codec and a
	// codec that has just been handed a slider position end up in the same
	// place. `codec_gain` sets the MIXER registers below, which are the input
	// trims and stay at unity.
	apply_codec_volume();

	// Three inputs per side. Which one carries the serial stream is not
	// something this driver can check, so give all three the same gain: the
	// others have nothing feeding them.
	uint8 mixer[9];
	for (int i = 0; i < 3; i++)
		memcpy(mixer + 3 * i, gain, 3);
	status |= codec_write(TAS_REG_LMIX, mixer, sizeof(mixer));
	status |= codec_write(TAS_REG_RMIX, mixer, sizeof(mixer));

	// Analog section up.
	value = 0;
	status |= codec_write(TAS_REG_ACR, &value, 1);

	dprintf("macio_snd: codec tas3004 %s at i2c %#x channel %" B_PRIu32
		", gain %#" B_PRIx32 " (volume %02x %02x %02x, mixer %02x %02x %02x)"
		"%s\n",
		status == B_OK ? "initialised" : "PARTLY initialised",
		sDevice.codec_address, sDevice.codec_channel, sCodecGain,
		(uint8)(tas_gain_for_db(sMixGain[0]) >> 20),
		(uint8)(tas_gain_for_db(sMixGain[0]) >> 12),
		(uint8)(tas_gain_for_db(sMixGain[0]) >> 4),
		gain[0], gain[1], gain[2],
		sCodecAllPass ? ", biquads bypassed" : "");
}


//! Drive one of the audio GPIOs named by the device tree. Absent names are
//	ignored - which machine has which line is exactly what varies.
static void
set_audio_gpio(const char* name, bool asserted)
{
	const ppc_audio_info* info = sDevice.info;
	for (uint32 i = 0; i < info->gpio_count; i++) {
		if (strcmp(info->gpios[i].name, name) != 0)
			continue;

		// active_state says which level means "asserted"; anything else is
		// the released level. `mute_invert` flips the whole convention,
		// because a mute line driven the wrong way is indistinguishable from
		// everything else being right and the machine still silent.
		bool wanted = sInvertMute ? !asserted : asserted;
		uint8 level = (info->gpios[i].active_state != 0) == wanted ? 1 : 0;
		uint8 written = KEYLARGO_GPIO_OUTPUT_ENABLE
			| (level != 0 ? KEYLARGO_GPIO_OUTPUT_DATA : 0);
		macio_write8(info->gpios[i].offset, written);

		/*	★ And read it straight back.

			Driving both mute lines both ways made no difference to what came
			out of the speaker, and that has two readings: the amplifier is
			not behind these lines, or these writes are not landing at all and
			the amplifiers have been sitting wherever Open Firmware left them.
			The read-back separates them - bit 1 is the level actually on the
			pin, so if it does not follow what we wrote, we are writing to the
			wrong byte.
		*/
		uint8 back = macio_read8(info->gpios[i].offset);
		dprintf("macio_snd: gpio %s -> %s (+%#" B_PRIx32 " wrote %#x, reads "
			"%#x, pin %d)\n", name, asserted ? "asserted" : "released",
			info->gpios[i].offset, written, back,
			(back & KEYLARGO_GPIO_INPUT_DATA) != 0 ? 1 : 0);
		return;
	}
}


/*!	Put the serial transport into a state where a DBDMA run will actually
	produce a bit clock and data.

	On davbus there is nothing to do: the cell is always live, and on the
	machines that have it Open Firmware has already set the codec up (its own
	boot chime went through it).

	On i2s the cell has to be clocked and told the frame format first. The
	divisors follow from the codec's requirements - 256*fs master clock,
	64*fs bit clock - against the 45.1584 MHz source, which is the one that
	divides evenly for the 44.1 kHz family.
*/
static void
prepare_transport(void)
{
	if (!sDevice.is_i2s)
		return;

	trace_step("read feature control");
	uint32 entryFCR1 = macio_read32(KEYLARGO_FCR1);
	trace_step("power the i2s cell");
	set_feature_bits(KL1_I2S0_ENABLE | KL1_I2S0_CELL_ENABLE, true);

	/*	★ The format registers may only be written with the bit clock STOPPED.
		Programming them while the cell is clocked is what Linux's i2sbus goes
		out of its way to avoid - it gates the clock, waits for the cell to
		report CLOCKS_STOPPED, writes, and only then starts it again. Writing
		them live wedges the cell on this hardware.
	*/
	trace_step("stop the bit clock");
	set_feature_bits(KL1_I2S0_CLK_ENABLE_BIT, false);

	trace_step("wait for clocks stopped");
	for (int i = 0; i < 20; i++) {
		if ((i2s_read(I2S_REG_INTR_CTL) & I2S_PENDING_CLOCKS_STOPPED) != 0)
			break;
		snooze(5000);
	}

	uint32 mclkDivisor = I2S_CLOCK_45MHZ / (SND_RATE * I2S_MCLK_PER_FRAME);
	uint32 sclkDivisor = I2S_MCLK_PER_FRAME / I2S_SCLK_PER_FRAME;

	uint32 serialFormat = I2S_SF_CLOCK_SOURCE_45MHZ
		| I2S_SF_MCLKDIV(mclkDivisor)
		| I2S_SF_SCLKDIV(sclkDivisor)
		| I2S_SF_SCLK_MASTER
		| I2S_SF_SERIAL_FORMAT_I2S_64X;

	uint32 wordSizes = (SND_CHANNELS << I2S_DWS_NUM_CHANNELS_OUT_SHIFT)
		| (SND_CHANNELS << I2S_DWS_NUM_CHANNELS_IN_SHIFT)
		| I2S_DWS_DATA_16BIT;

	trace_step("write the serial format");
	i2s_write(I2S_REG_SERIAL_FORMAT, serialFormat);
	trace_step("write the word sizes");
	i2s_write(I2S_REG_DATA_WORD_SIZES, wordSizes);

	trace_step("start the bit clock");
	set_feature_bits(KL1_I2S0_CLK_ENABLE_BIT, true);

	trace_step("read the cell back");
	dprintf("macio_snd: fcr1 %#" B_PRIx32 " -> %#" B_PRIx32 ", serial format %#"
		B_PRIx32 " (read back %#" B_PRIx32 "), word sizes %#" B_PRIx32
		" (read back %#" B_PRIx32 ")\n", entryFCR1,
		macio_read32(KEYLARGO_FCR1), serialFormat,
		i2s_read(I2S_REG_SERIAL_FORMAT), wordSizes,
		i2s_read(I2S_REG_DATA_WORD_SIZES));

	// Unmute whatever the machine calls its amplifier and headphone lines.
	// The names come from the device tree's "audio-gpio" property.
	// Muted across the codec's reset and configuration, so it cannot thump the
	// speaker on the way up.
	trace_step("mute while the codec comes up");
	set_audio_gpio("amp-mute", true);
	set_audio_gpio("headphone-mute", true);

	if (sCodecEnabled && sDevice.info->codec_i2c_addr != 0) {
		trace_step("initialise the codec over i2c");
		init_codec();
	}

	trace_step("unmute the amplifier");
	set_audio_gpio("amp-mute", false);
	trace_step("unmute the headphones");
	set_audio_gpio("headphone-mute", false);
	set_audio_gpio("lineout-mute", false);
	trace_step("cell ready");
}


static status_t
map_registers(void)
{
	if (sDevice.reg_area >= 0)
		return B_OK;

	const ppc_audio_info* info = sDevice.info;

	// One mapping for the whole mac-io window: the pieces this driver needs
	// (feature control at 0x3c, the GPIOs, the i2s cell and the DBDMA
	// channels) are scattered across it, and they are all just offsets.
	void* registers = NULL;
	sDevice.reg_area = map_physical_memory("macio sound registers",
		info->macio_phys, MACIO_WINDOW_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &registers);
	if (sDevice.reg_area < 0)
		return sDevice.reg_area;

	sDevice.macio_base = (addr_t)registers;
	sDevice.i2c_step = info->i2c_address_step != 0
		? info->i2c_address_step : 0x10;
	sDevice.channel_base = sDevice.macio_base + info->tx_dbdma_offset;
	sDevice.i2s_base = sDevice.macio_base + info->i2s_offset;
	return B_OK;
}


// ---------------------------------------------------------------------------
//	DMA buffers and the command ring
// ---------------------------------------------------------------------------

static void
free_buffers(void)
{
	if (sDevice.cmd_area >= 0) {
		delete_area(sDevice.cmd_area);
		sDevice.cmd_area = -1;
	}
	if (sDevice.buffer_area >= 0) {
		delete_area(sDevice.buffer_area);
		sDevice.buffer_area = -1;
	}
}


static status_t
create_buffers(uint32 numBuffers, uint32 bufferFrames)
{
	free_buffers();

	uint32 bufferBytes = bufferFrames * SND_FRAME_SIZE;
	uint32 allocation = (bufferBytes * numBuffers + B_PAGE_SIZE - 1)
		& ~(B_PAGE_SIZE - 1);

	// Contiguous, so one DBDMA data run covers a whole buffer, and user
	// readable/writable so the media_addon_server can fill it directly.
	void* base = NULL;
	sDevice.buffer_area = create_area("macio_snd buffers", &base,
		B_ANY_KERNEL_ADDRESS, allocation, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (sDevice.buffer_area < 0)
		return sDevice.buffer_area;

	physical_entry entry;
	get_memory_map(base, allocation, &entry, 1);
	sDevice.buffer_base = (uint8*)base;
	sDevice.buffer_phys = entry.address;
	memset(base, 0, allocation);
	for (uint32 i = 0; i < numBuffers; i++)
		sDevice.buffers[i] = (uint8*)base + i * bufferBytes;

	void* commands = NULL;
	sDevice.cmd_area = create_area("macio_snd dbdma commands", &commands,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (sDevice.cmd_area < 0) {
		free_buffers();
		return sDevice.cmd_area;
	}
	get_memory_map(commands, B_PAGE_SIZE, &entry, 1);
	sDevice.cmds = (dbdma_cmd*)commands;
	sDevice.cmds_phys = entry.address;

	sDevice.num_buffers = numBuffers;
	sDevice.buffer_frames = bufferFrames;
	return B_OK;
}


/*!	Build a command ring that plays buffer 0, 1, ... and branches back to 0
	forever. The commands are contiguous, so a non-branching one falls through
	to its successor; only the last needs an explicit branch.
*/
static void
build_dbdma_ring(void)
{
	uint32 bufferBytes = sDevice.buffer_frames * SND_FRAME_SIZE;
	for (uint32 i = 0; i < sDevice.num_buffers; i++) {
		dbdma_cmd* command = &sDevice.cmds[i];
		memset(command, 0, sizeof(*command));
		command->req_count = B_HOST_TO_LENDIAN_INT16(bufferBytes);
		command->cmd_key = DBDMA_OUTPUT_MORE << 4;
		command->address = B_HOST_TO_LENDIAN_INT32(
			(uint32)(sDevice.buffer_phys + i * bufferBytes));
		if (i == sDevice.num_buffers - 1) {
			command->cmd_bits = DBDMA_BR_ALWAYS;
			command->cmd_arg = B_HOST_TO_LENDIAN_INT32(
				(uint32)sDevice.cmds_phys);
		}
	}
	__asm__ volatile("sync; eieio" ::: "memory");
}


// ---------------------------------------------------------------------------
//	transport
// ---------------------------------------------------------------------------

static int32
pace_thread(void* argument)
{
	// Pace B_MULTI_BUFFER_EXCHANGE off the clock rather than off a per-buffer
	// DBDMA interrupt. The ring runs at exactly the sample rate, so the two
	// stay in step; moving to the real interrupt is a refinement, not a
	// prerequisite, and the emulator's completion interrupt is a level it
	// never deasserts between buffers.
	bigtime_t period = (bigtime_t)sDevice.buffer_frames * 1000000LL
		/ sDevice.rate;
	while (sDevice.running) {
		cpu_status state = disable_interrupts();
		acquire_spinlock(&sDevice.lock);
		sDevice.real_time = system_time();
		sDevice.frames_count += sDevice.buffer_frames;
		sDevice.buffer_cycle = (sDevice.buffer_cycle + 1) % sDevice.num_buffers;
		release_spinlock(&sDevice.lock);
		restore_interrupts(state);

		release_sem_etc(sDevice.buffer_ready_sem, 1, B_DO_NOT_RESCHEDULE);
		snooze(period);
	}
	return 0;
}


static status_t
start_hardware(void)
{
	if (sDevice.running)
		return B_OK;

	prepare_transport();
	build_dbdma_ring();

	// Stop the channel, point it at the command list, then run. The control
	// register takes (mask << 16) | value, so each write says exactly which
	// bits it means.
	dbdma_write(DBDMA_CH_CTRL, 0xffff0000);
	dbdma_write(DBDMA_CMD_PTR_LO, (uint32)sDevice.cmds_phys);
	dbdma_write(DBDMA_CH_CTRL, (DBDMA_RUN << 16) | DBDMA_RUN);

	sDevice.buffer_cycle = 0;
	sDevice.frames_count = 0;
	sDevice.real_time = system_time();
	sDevice.running = true;
	sDevice.pace_thread = spawn_kernel_thread(pace_thread, "macio_snd pacer",
		B_REAL_TIME_PRIORITY, NULL);
	if (sDevice.pace_thread < 0) {
		sDevice.running = false;
		return sDevice.pace_thread;
	}
	resume_thread(sDevice.pace_thread);

	// RUN|ACTIVE here means the channel really is fetching commands. It is the
	// one cheap check that separates "we asked for playback" from "playback is
	// happening", so it is worth a line in the log every time.
	dprintf("macio_snd: playback started (%" B_PRIu32 " x %" B_PRIu32
		" frames), channel status %#" B_PRIx32 "\n", sDevice.num_buffers,
		sDevice.buffer_frames, dbdma_read(DBDMA_CH_STAT));
	return B_OK;
}


static void
stop_hardware(void)
{
	if (!sDevice.running)
		return;

	sDevice.running = false;
	if (sDevice.pace_thread >= 0) {
		status_t exitValue;
		wait_for_thread(sDevice.pace_thread, &exitValue);
		sDevice.pace_thread = -1;
	}
	dbdma_write(DBDMA_CH_CTRL, DBDMA_RUN << 16);

	if (sDevice.is_i2s) {
		set_audio_gpio("amp-mute", true);
		set_audio_gpio("headphone-mute", true);
	}
}


/*!	Play a tone straight from the driver, with no media stack involved.

	Sound has a long chain - driver, hmulti_audio add-on, media_server, mixer,
	an application - and a silent machine looks the same at every link. This
	cuts the chain to its first link: fill a buffer with a square wave, start
	the DBDMA ring, and report the channel status. A tone means the transport
	is right and any remaining silence is above the driver; no tone means stop
	looking at the media stack.

	Enabled with `selftest true` in the driver settings, which is a text file
	on the machine - so switching it on costs an edit and a reboot rather than
	a rebuild and a reflash.

	The buffers are deliberately never freed in this mode: the ring loops
	forever by design, and devfs unloads a driver as soon as nobody holds it
	open, which would otherwise hand the DMA engine freed pages.
*/
static void
run_self_test(void)
{
	if (create_buffers(NUM_BUFFERS, FRAMES_PER_BUFFER) != B_OK) {
		dprintf("macio_snd: self test could not allocate buffers\n");
		return;
	}

	// A 441 Hz square wave: 100 frames per cycle at 44100, and a modest
	// amplitude because this may come out of a laptop speaker at whatever
	// volume the codec happens to be set to.
	const int16 amplitude = 14000;
	const uint32 halfCycle = SND_RATE / 441 / 2;
	int16* samples = (int16*)sDevice.buffer_base;
	uint32 totalFrames = NUM_BUFFERS * FRAMES_PER_BUFFER;
	for (uint32 frame = 0; frame < totalFrames; frame++) {
		int16 value = (frame / halfCycle) % 2 == 0 ? amplitude : -amplitude;
		samples[frame * SND_CHANNELS + 0] = value;
		samples[frame * SND_CHANNELS + 1] = value;
	}
	__asm__ volatile("sync; eieio" ::: "memory");

	prepare_transport();
	trace_step("build the dma ring");
	build_dbdma_ring();

	trace_step("stop the dma channel");
	dbdma_write(DBDMA_CH_CTRL, 0xffff0000);
	trace_step("point it at the ring");
	dbdma_write(DBDMA_CMD_PTR_LO, (uint32)sDevice.cmds_phys);
	trace_step("start the dma channel");
	dbdma_write(DBDMA_CH_CTRL, (DBDMA_RUN << 16) | DBDMA_RUN);

	/*	★ Is the cell actually clocking?

		Everything so far has been inference: the DMA reports RUN and ACTIVE,
		the format registers read back, so the digital side "must" be running.
		The i2s cell counts frames as it shifts them out, and that counter is
		the one thing that can say so directly. Two reads, 100 ms apart:

		  advancing by roughly 4410  -> the cell is clocking at 44.1 kHz and
		                                consuming our samples, so the silence
		                                is in the codec or after it
		  not moving                 -> nothing is being shifted out at all,
		                                and the codec is irrelevant
	*/
	snooze(50000);
	uint32 firstStatus = dbdma_read(DBDMA_CH_STAT);
	uint32 firstCommand = dbdma_read(DBDMA_CMD_PTR_LO);
	uint32 firstFrames = sDevice.is_i2s ? i2s_read(I2S_REG_FRAME_COUNT) : 0;
	snooze(100000);
	uint32 secondStatus = dbdma_read(DBDMA_CH_STAT);
	uint32 secondFrames = sDevice.is_i2s ? i2s_read(I2S_REG_FRAME_COUNT) : 0;

	dprintf("macio_snd: SELF TEST running, channel status %#" B_PRIx32
		" then %#" B_PRIx32 " (expect bit 15 RUN and bit 10 ACTIVE)\n",
		firstStatus, secondStatus);
	if (sDevice.is_i2s) {
		dprintf("macio_snd: i2s frame count %" B_PRIu32 " -> %" B_PRIu32
			" in 100ms (%" B_PRId32 " frames; 44100 Hz would be about 4410)\n",
			firstFrames, secondFrames, (int32)(secondFrames - firstFrames));
	}

	/*	★ And is it shifting OUR samples, or just clocking?

		A cell with nothing arriving from memory still counts frames - it
		shifts out silence. So the frame counter alone cannot tell a working
		path from an idle one. The DMA descriptors can: res_count is what is
		left of each buffer, xfer_stat is how the engine finished with it, and
		the command pointer says which command it is on. If those move,
		samples are genuinely being fetched out of our buffer.
	*/
	dprintf("macio_snd: dma command pointer %#" B_PRIx32 " then %#" B_PRIx32
		"\n", firstCommand, dbdma_read(DBDMA_CMD_PTR_LO));
	for (uint32 i = 0; i < sDevice.num_buffers; i++) {
		dprintf("macio_snd: dma buffer %" B_PRIu32 ": %u of %u bytes left, "
			"status %#x\n", i,
			B_LENDIAN_TO_HOST_INT16(sDevice.cmds[i].res_count),
			B_LENDIAN_TO_HOST_INT16(sDevice.cmds[i].req_count),
			B_LENDIAN_TO_HOST_INT16(sDevice.cmds[i].xfer_stat));
	}

	/*	★ And then drive the mute lines both ways.

		The device tree says which level mutes ("audio-gpio-active-state"),
		and a line driven backwards is indistinguishable from everything else
		being right and the machine still silent. That was a settings switch,
		which costs a boot, a text edit on the machine and another boot - and
		the one attempt at it ended with the editor freezing the machine.

		The tone is already looping, so the test can simply hold each polarity
		for a few seconds and let the ear decide. Phase 1 is what the device
		tree says; phase 2 is the opposite. Whichever phase makes a sound is
		the answer, and if neither does the mute lines are not the problem.
	*/
	if (sDevice.is_i2s) {
		for (int phase = 0; phase < 2; phase++) {
			bool asserted = phase != 0;
			dprintf("macio_snd: SELF TEST mute phase %d of 2 - lines %s "
				"(%s), listening for 4 seconds\n", phase + 1,
				asserted ? "asserted" : "released",
				asserted ? "the opposite of the device tree"
					: "what the device tree calls unmuted");
			set_audio_gpio("amp-mute", asserted);
			set_audio_gpio("headphone-mute", asserted);
			snooze(4000000);
		}

		// Leave them the way the device tree says they belong.
		set_audio_gpio("amp-mute", false);
		set_audio_gpio("headphone-mute", false);
	}
}


// ---------------------------------------------------------------------------
//	hmulti_audio protocol
// ---------------------------------------------------------------------------

static multi_channel_info sChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
};


static status_t
get_description(multi_description* data)
{
	multi_description description;
	if (user_memcpy(&description, data, sizeof(multi_description)) != B_OK)
		return B_BAD_ADDRESS;

	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	snprintf(description.friendly_name, sizeof(description.friendly_name),
		"%s (mac-io %s)",
		sDevice.info->codec[0] != '\0' ? sDevice.info->codec : "Audio",
		sDevice.is_i2s ? "i2s" : "davbus");
	strcpy(description.vendor_info, "Haiku/PowerPC");

	description.output_channel_count = 2;
	description.input_channel_count = 0;
	description.output_bus_channel_count = 2;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;

	description.output_rates = B_SR_44100;
	description.input_rates = 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 30000;
	strcpy(description.control_panel, "");

	if (user_memcpy(data, &description, sizeof(multi_description)) != B_OK)
		return B_BAD_ADDRESS;

	if ((size_t)description.request_channel_count >= B_COUNT_OF(sChannels)) {
		if (user_memcpy(data->channels, &sChannels, sizeof(sChannels)) != B_OK)
			return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
get_enabled_channels(multi_channel_enable* data)
{
	B_SET_CHANNEL(data->enable_bits, 0, true);
	B_SET_CHANNEL(data->enable_bits, 1, true);
	return B_OK;
}


static status_t
get_global_format(multi_format_info* data)
{
	data->output_latency = 0;
	data->input_latency = 0;
	data->timecode_kind = 0;
	data->output.format = sDevice.format;
	data->output.rate = sDevice.rate;
	data->input.format = 0;
	data->input.rate = 0;
	return B_OK;
}


static status_t
set_global_format(multi_format_info* data)
{
	// 16-bit 44100 is all the hardware is set up for; accept the request and
	// keep reporting what we actually do.
	sDevice.format = B_FMT_16BIT;
	sDevice.rate = B_SR_44100;
	return B_OK;
}


#define MIX_ID_GROUP	(MULTI_AUDIO_BASE_ID)
#define MIX_ID_MUTE	(MULTI_AUDIO_BASE_ID + 1)
#define MIX_ID_GAIN_L	(MULTI_AUDIO_BASE_ID + 2)
#define MIX_ID_GAIN_R	(MULTI_AUDIO_BASE_ID + 3)


/*!	Describe the mixer.

	This used to publish one empty group and nothing in it, which is exactly
	what a volume slider with nothing to move looks like from user space. The
	group now holds a mute and a linked stereo gain covering the codec's whole
	range - -70 dB to +18 dB in half decibel steps, which is the table Linux
	uses, not a range invented here.

	On davbus there is no register to put a gain in, so that hardware keeps the
	bare group rather than being given a slider that does nothing.
*/
static status_t
list_mix_controls(multi_mix_control_info* data)
{
	multi_mix_control* controls = data->controls;
	int32 capacity = data->control_count;
	int32 count = 0;

	if (capacity < 1)
		return B_OK;

	controls[count].id = MIX_ID_GROUP;
	controls[count].parent = 0;
	controls[count].flags = B_MULTI_MIX_GROUP;
	controls[count].master = MULTI_AUDIO_MASTER_ID;
	controls[count].string = S_null;
	strcpy(controls[count].name, "Playback");
	count++;

	if (sDevice.is_i2s && sDevice.codec_address != 0 && capacity >= 4) {
		controls[count].id = MIX_ID_MUTE;
		controls[count].parent = MIX_ID_GROUP;
		controls[count].flags = B_MULTI_MIX_ENABLE;
		controls[count].master = MULTI_AUDIO_MASTER_ID;
		controls[count].string = S_MUTE;
		controls[count].name[0] = '\0';
		count++;

		for (int channel = 0; channel < 2; channel++) {
			controls[count].id = channel == 0 ? MIX_ID_GAIN_L : MIX_ID_GAIN_R;
			controls[count].parent = MIX_ID_GROUP;
			controls[count].flags = B_MULTI_MIX_GAIN;
			// The right channel is slaved to the left, so one slider moves
			// both unless the user unlinks them.
			controls[count].master = channel == 0
				? MULTI_AUDIO_MASTER_ID : MIX_ID_GAIN_L;
			controls[count].string = S_GAIN;
			controls[count].name[0] = '\0';
			controls[count].gain.min_gain = -70.0f;
			controls[count].gain.max_gain = 18.0f;
			controls[count].gain.granularity = 0.5f;
			count++;
		}
	}

	data->control_count = count;
	return B_OK;
}


static status_t
get_mix(multi_mix_value_info* data)
{
	for (int32 i = 0; i < data->item_count; i++) {
		switch (data->values[i].id) {
			case MIX_ID_MUTE:
				// The API's "enable" is the opposite of a mute: set means
				// audible.
				data->values[i].enable = !sMixMuted;
				break;
			case MIX_ID_GAIN_L:
				data->values[i].gain = sMixGain[0];
				break;
			case MIX_ID_GAIN_R:
				data->values[i].gain = sMixGain[1];
				break;
			default:
				break;
		}
	}
	return B_OK;
}


static status_t
set_mix(multi_mix_value_info* data)
{
	bool changed = false;
	for (int32 i = 0; i < data->item_count; i++) {
		switch (data->values[i].id) {
			case MIX_ID_MUTE:
				sMixMuted = !data->values[i].enable;
				changed = true;
				break;
			case MIX_ID_GAIN_L:
				sMixGain[0] = data->values[i].gain;
				changed = true;
				break;
			case MIX_ID_GAIN_R:
				sMixGain[1] = data->values[i].gain;
				changed = true;
				break;
			default:
				break;
		}
	}

	if (changed)
		apply_codec_volume();
	return B_OK;
}


static status_t
get_buffers(multi_buffer_list* data)
{
	if (data->request_playback_buffers != NUM_BUFFERS)
		data->request_playback_buffers = NUM_BUFFERS;
	if (data->request_playback_buffer_size == 0)
		data->request_playback_buffer_size = FRAMES_PER_BUFFER;
	// One DBDMA data run per buffer, so a buffer has to fit in the 16-bit
	// req_count field.
	if (data->request_playback_buffer_size * SND_FRAME_SIZE > 0xf000)
		data->request_playback_buffer_size = 0xf000 / SND_FRAME_SIZE;

	data->flags = 0;

	status_t result = create_buffers(data->request_playback_buffers,
		data->request_playback_buffer_size);
	if (result != B_OK) {
		dprintf("macio_snd: create_buffers failed: %#" B_PRIx32 "\n", result);
		return result;
	}

	data->return_playback_buffers = sDevice.num_buffers;
	data->return_playback_channels = data->request_playback_channels;
	data->return_playback_buffer_size = sDevice.buffer_frames;

	for (int32 buffer = 0; buffer < data->return_playback_buffers; buffer++) {
		for (int32 channel = 0;
				channel < data->return_playback_channels; channel++) {
			data->playback_buffers[buffer][channel].base
				= (char*)sDevice.buffers[buffer] + SND_SAMPLE_SIZE * channel;
			data->playback_buffers[buffer][channel].stride
				= SND_SAMPLE_SIZE * data->return_playback_channels;
		}
	}

	data->return_record_buffers = 0;
	data->return_record_channels = 0;
	data->return_record_buffer_size = 0;
	return B_OK;
}


static status_t
buffer_exchange(multi_buffer_info* info)
{
	multi_buffer_info bufferInfo;
	if (user_memcpy(&bufferInfo, info, sizeof(multi_buffer_info)) != B_OK)
		return B_BAD_ADDRESS;

	if (!sDevice.running) {
		status_t result = start_hardware();
		if (result != B_OK)
			return result;
	}

	status_t result = acquire_sem(sDevice.buffer_ready_sem);
	if (result != B_OK)
		return result;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sDevice.lock);
	bufferInfo.playback_buffer_cycle = sDevice.buffer_cycle;
	bufferInfo.played_real_time = sDevice.real_time;
	bufferInfo.played_frames_count = sDevice.frames_count;
	release_spinlock(&sDevice.lock);
	restore_interrupts(state);

	if (user_memcpy(info, &bufferInfo, sizeof(multi_buffer_info)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
multi_audio_control(uint32 op, void* argument)
{
	switch (op) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description((multi_description*)argument);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels((multi_channel_enable*)argument);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format((multi_format_info*)argument);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format((multi_format_info*)argument);
		case B_MULTI_LIST_MIX_CONTROLS:
			return list_mix_controls((multi_mix_control_info*)argument);
		case B_MULTI_GET_MIX:
			return get_mix((multi_mix_value_info*)argument);
		case B_MULTI_SET_MIX:
			return set_mix((multi_mix_value_info*)argument);
		case B_MULTI_GET_BUFFERS:
			return get_buffers((multi_buffer_list*)argument);
		case B_MULTI_BUFFER_EXCHANGE:
			return buffer_exchange((multi_buffer_info*)argument);
		case B_MULTI_BUFFER_FORCE_STOP:
			stop_hardware();
			return B_OK;
	}
	return B_BAD_VALUE;
}


// ---------------------------------------------------------------------------
//	driver hooks
// ---------------------------------------------------------------------------

status_t init_driver(void);


status_t
init_hardware(void)
{
	const ppc_audio_info* info = ppc_get_audio_info();
	if (info == NULL || info->valid == 0 || info->macio_phys == 0) {
		// Not an error worth shouting about on a machine that has no mac-io
		// sound cell at all - but say it once, because "no audio device" and
		// "the loader did not find the audio device" look identical from user
		// space and have completely different fixes.
		dprintf("macio_snd: no mac-io audio cell in the device tree\n");
		return B_ERROR;
	}

	// The davbus cells sit at a fixed mac-io offset with a codec that answers
	// to the cell itself; anything else here is an i2s bus, which needs its
	// clocks and frame format programmed before it will carry samples.
	bool isI2S = strcmp(info->codec, "burgundy") != 0
		&& strcmp(info->codec, "screamer") != 0
		&& strcmp(info->codec, "awacs") != 0;

	// Report the hardware whether or not we go on to drive it. On a machine
	// this driver has never run on, this description IS the deliverable.
	dprintf("macio_snd: %s codec \"%s\", mac-io %#" B_PRIx32 ", cell +%#"
		B_PRIx32 " size %#" B_PRIx32 ", playback dma +%#" B_PRIx32 " irq %"
		B_PRIu32 ", capture dma +%#" B_PRIx32 " irq %" B_PRIu32 "\n",
		isI2S ? "i2s" : "davbus", info->codec, info->macio_phys,
		info->i2s_offset, info->i2s_size, info->tx_dbdma_offset, info->tx_irq,
		info->rx_dbdma_offset, info->rx_irq);
	dprintf("macio_snd: i2c +%#" B_PRIx32 " codec address %#" B_PRIx32
		", layout %" B_PRIu32 ", device %" B_PRIu32 ", %" B_PRIu32 " gpios\n",
		info->i2c_offset, info->codec_i2c_addr, info->layout_id,
		info->device_id, info->gpio_count);
	for (uint32 i = 0; i < info->gpio_count; i++) {
		dprintf("macio_snd: gpio \"%s\" +%#" B_PRIx32 " active %" B_PRIu32
			" irq %" B_PRIu32 "\n", info->gpios[i].name, info->gpios[i].offset,
			info->gpios[i].active_state, info->gpios[i].irq);
	}

	void* settings = load_driver_settings("macio_snd");
	if (settings != NULL) {
		sI2SEnabled = get_driver_boolean_parameter(settings, "i2s", false,
			true);
		sSelfTest = get_driver_boolean_parameter(settings, "selftest", false,
			true);
		sTrace = get_driver_boolean_parameter(settings, "trace", false, true);
		sCodecEnabled = get_driver_boolean_parameter(settings, "codec", false,
			true);
		sScanI2C = get_driver_boolean_parameter(settings, "i2cscan", false,
			true);
		const char* gain = get_driver_parameter(settings, "codec_gain", NULL,
			NULL);
		if (gain != NULL)
			sCodecGain = strtoul(gain, NULL, 0) & 0xffffff;
		sCodecAllPass = get_driver_boolean_parameter(settings, "codec_allpass",
			false, true);
		sInvertMute = get_driver_boolean_parameter(settings, "mute_invert",
			false, true);
		unload_driver_settings(settings);
	}

	/*	★ An i2s machine gets no device published until someone has checked
		those offsets against that machine's device tree.

		The register offsets above are derived by the boot loader, and the
		derivation has only ever been verified on a davbus machine. Publishing
		the device is not passive: the media stack connects to whatever output
		it finds, which calls through to a DBDMA start and an i2s cell
		configuration - so an offset that is out by one level of the device
		tree would have this driver writing into some unrelated mac-io cell,
		as a side effect of the desktop coming up. Nothing about the failure
		would point here.

		So the first boot on new hardware only ever reads and reports. Turn
		`i2s true` on in the driver settings once the description above has
		been checked.
	*/
	if (isI2S && !sI2SEnabled) {
		dprintf("macio_snd: i2s hardware, not enabled - reporting only. Set "
			"`i2s true` in ~/config/settings/kernel/drivers/macio_snd once "
			"the offsets above have been checked against the device tree.\n");
		return B_ERROR;
	}

	/*	The self test owns the hardware outright: it leaves a DMA ring looping
		forever, so a media stack that also claimed the channel would be
		fighting it for the same registers. Publishing no device keeps the two
		experiments separate - which matters, because the whole point of the
		self test is to be the ONLY thing touching the hardware.
	*/
	if (sSelfTest) {
		if (init_driver() == B_OK)
			run_self_test();
		return B_ERROR;
	}
	return B_OK;
}


status_t
init_driver(void)
{
	sDevice.info = ppc_get_audio_info();
	sDevice.reg_area = -1;
	sDevice.buffer_area = -1;
	sDevice.cmd_area = -1;
	sDevice.pace_thread = -1;
	sDevice.running = false;
	sDevice.format = B_FMT_16BIT;
	sDevice.rate = B_SR_44100;

	sDevice.is_i2s = strcmp(sDevice.info->codec, "burgundy") != 0
		&& strcmp(sDevice.info->codec, "screamer") != 0
		&& strcmp(sDevice.info->codec, "awacs") != 0;

	B_INITIALIZE_SPINLOCK(&sDevice.lock);
	sDevice.buffer_ready_sem = create_sem(0, "macio_snd buffer ready");
	if (sDevice.buffer_ready_sem < 0)
		return sDevice.buffer_ready_sem;

	status_t result = map_registers();
	if (result != B_OK) {
		dprintf("macio_snd: could not map mac-io at %#" B_PRIx32 ": %#"
			B_PRIx32 "\n", sDevice.info->macio_phys, result);
		return result;
	}

	void* settings = load_driver_settings("macio_snd");
	if (settings != NULL) {
		sSelfTest = get_driver_boolean_parameter(settings, "selftest", false,
			true);
		unload_driver_settings(settings);
	}
	return B_OK;
}


void
uninit_driver(void)
{
	if (sSelfTest) {
		// Leave the ring running and the pages allocated; see run_self_test().
		return;
	}
	stop_hardware();
	free_buffers();
	if (sDevice.reg_area >= 0)
		delete_area(sDevice.reg_area);
	if (sDevice.buffer_ready_sem >= 0)
		delete_sem(sDevice.buffer_ready_sem);
}


static status_t
snd_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = &sDevice;
	return B_OK;
}


static status_t
snd_close(void* cookie)
{
	stop_hardware();
	return B_OK;
}


static status_t
snd_free(void* cookie)
{
	return B_OK;
}


static status_t
snd_control(void* cookie, uint32 op, void* argument, size_t length)
{
	return multi_audio_control(op, argument);
}


static status_t
snd_read(void* cookie, off_t position, void* data, size_t* numBytes)
{
	*numBytes = 0;
	return B_IO_ERROR;
}


static status_t
snd_write(void* cookie, off_t position, const void* data, size_t* numBytes)
{
	*numBytes = 0;
	return B_IO_ERROR;
}


static device_hooks sHooks = {
	snd_open,
	snd_close,
	snd_free,
	snd_control,
	snd_read,
	snd_write,
	NULL,
	NULL,
	NULL,
	NULL
};


const char**
publish_devices(void)
{
	static const char* names[] = {
		MULTI_AUDIO_DEV_PATH "/macio_snd/0",
		NULL
	};
	return names;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
