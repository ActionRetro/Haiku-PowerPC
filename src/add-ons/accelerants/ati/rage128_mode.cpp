/*
	Haiku ATI video driver adapted from the X.org ATI driver.

	Copyright 1999, 2000 ATI Technologies Inc., Markham, Ontario,
						 Precision Insight, Inc., Cedar Park, Texas, and
						 VA Linux Systems Inc., Fremont, California.

	Copyright 2009 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac 2009
*/


#include "accelerant.h"
#include "rage128.h"

#include <unistd.h>


struct DisplayParams {
	// CRTC registers
	uint32	crtc_gen_cntl;
	uint32	crtc_h_total_disp;
	uint32	crtc_h_sync_strt_wid;
	uint32	crtc_v_total_disp;
	uint32	crtc_v_sync_strt_wid;
	uint32	crtc_pitch;

	// DDA register
	uint32	dda_config;
	uint32	dda_on_off;

	// Computed PLL values
	int		feedback_div;
	int		post_div;

	// PLL registers
	uint32	ppll_ref_div;
	uint32	ppll_div_3;
};



static inline int
DivideWithRounding(int n, int d)
{
	return (n + (d / 2)) / d;		// compute n/d with rounding
}


static int
MinimumBits(uint32 value)
{
	// Compute minimum number of bits required to contain a value (ie, log
	// base 2 of value).

	if (value == 0)
		return 1;

	int numBits = 0;

	while (value != 0) {
		value >>= 1;
		numBits++;
	}

	return numBits;
}


static bool
CalculateCrtcRegisters(const DisplayModeEx& mode, DisplayParams& params)
{
	// Define CRTC registers for requested video mode.
	// Return true if successful.

	const uint8 hSyncFudge[] = { 0x00, 0x12, 0x09, 0x09, 0x06, 0x05 };

	uint32 format;

	switch (mode.bitsPerPixel) {
	case 8:
		format = 2;
		break;
	case 15:
		format = 3;		// 555
		break;
	case 16:
		format = 4;		// 565
		break;
	case 32:
		format = 6;		// xRGB
		break;
	default:
		TRACE("Unsupported color depth: %d bits/pixel\n", mode.bitsPerPixel);
		return false;
	}

	params.crtc_gen_cntl = (R128_CRTC_EXT_DISP_EN
						   | R128_CRTC_EN
						   | (format << 8));

	params.crtc_h_total_disp = (((mode.timing.h_total / 8) - 1) & 0xffff)
							   | (((mode.timing.h_display / 8) - 1) << 16);

	int hSyncWidth = (mode.timing.h_sync_end - mode.timing.h_sync_start) / 8;
	if (hSyncWidth <= 0)
		hSyncWidth = 1;
	if (hSyncWidth > 0x3f)
		hSyncWidth = 0x3f;

	int hSyncStart = mode.timing.h_sync_start - 8 + hSyncFudge[format - 1];

	params.crtc_h_sync_strt_wid = (hSyncStart & 0xfff) | (hSyncWidth << 16)
		| ((mode.timing.flags & B_POSITIVE_HSYNC) ? 0 : R128_CRTC_H_SYNC_POL);

	params.crtc_v_total_disp = (((mode.timing.v_total - 1) & 0xffff)
		| ((mode.timing.v_display - 1) << 16));

	int vSyncWidth = mode.timing.v_sync_end - mode.timing.v_sync_start;
	if (vSyncWidth <= 0)
		vSyncWidth = 1;
	if (vSyncWidth > 0x1f)
		vSyncWidth = 0x1f;

	params.crtc_v_sync_strt_wid = ((mode.timing.v_sync_start - 1) & 0xfff)
		| (vSyncWidth << 16)
		| ((mode.timing.flags & B_POSITIVE_VSYNC) ? 0 : R128_CRTC_V_SYNC_POL);

	params.crtc_pitch = mode.timing.h_display / 8;

	return true;
}


static bool
CalculateDDARegisters(const DisplayModeEx& mode, DisplayParams& params)
{
	// Compute and write DDA registers for requested video mode.
	// Return true if successful.

	SharedInfo& si = *gInfo.sharedInfo;
	R128_RAMSpec& memSpec = si.r128MemSpec;
	R128_PLLParams& pll = si.r128PLLParams;

	int displayFifoWidth = 128;
	int displayFifoDepth = 32;
	int xClkFreq = pll.xclk;

	int vClkFreq = DivideWithRounding(pll.reference_freq * params.feedback_div,
		pll.reference_div * params.post_div);

#ifdef __POWERPC__
	// Use the clock actually in force, not the one we intended. On ppc the PLL
	// is deliberately left as OpenFirmware programmed it (its raster is the
	// only one this CRT will lock to), so timing the display FIFO from the
	// mode's divisors fetches data for the wrong fraction of each line and the
	// CRTC repeats what it has - measured at 39.789/62.375 = 0.638 of the
	// line, matching the duplication seen on the iMac exactly.
	//
	// PLL_DIV_SEL still holds OpenFirmware's choice (DIV_0 here) because the
	// write path that would set it to DIV_3 is skipped, so read the selector
	// rather than assume.
	{
		uint32 divSel = (INREG(R128_CLOCK_CNTL_INDEX) >> 8) & 0x3;
		uint32 refDiv = GetPLLReg(R128_PPLL_REF_DIV) & R128_PPLL_REF_DIV_MASK;
		// Only R128_PPLL_DIV_3 (index 0x07) is in the header; the four
		// dividers are 0x04..0x07, so DIV_0 is DIV_3 - 3.
		uint32 div = GetPLLReg((R128_PPLL_DIV_3 - 3) + divSel);
		uint32 fbDiv = div & 0x7ff;
		static const uint32 kPostDivs[8] = { 1, 2, 4, 8, 3, 0, 6, 12 };
		uint32 postDiv = kPostDivs[(div >> 16) & 0x7];

		if (refDiv != 0 && fbDiv != 0 && postDiv != 0) {
			int live = DivideWithRounding(pll.reference_freq * fbDiv,
				refDiv * postDiv);
			TRACE("ppc: DDA vClkFreq %d -> %d (live PLL: sel %u ref_div %u"
				" fb_div %u post_div %u)\n", vClkFreq, live, divSel, refDiv,
				fbDiv, postDiv);
			vClkFreq = live;
		} else {
			TRACE("ppc: live PLL divisors look wrong (sel %u ref %u fb %u"
				" post %u); keeping computed vClkFreq %d\n", divSel, refDiv,
				fbDiv, postDiv, vClkFreq);
		}
	}
#endif

	int bytesPerPixel = (mode.bitsPerPixel + 7) / 8;

	int xClksPerTransfer = DivideWithRounding(xClkFreq * displayFifoWidth,
		vClkFreq * bytesPerPixel * 8);

	int useablePrecision = MinimumBits(xClksPerTransfer) + 1;

	int xClksPerTransferPrecise = DivideWithRounding(
		(xClkFreq * displayFifoWidth) << (11 - useablePrecision),
		vClkFreq * bytesPerPixel * 8);

	int rOff = xClksPerTransferPrecise * (displayFifoDepth - 4);

	int rOn = (4 * memSpec.memBurstLen
		+ 3 * MAX(memSpec.rasToCasDelay - 2, 0)
		+ 2 * memSpec.rasPercentage
		+ memSpec.writeRecovery
		+ memSpec.casLatency
		+ memSpec.readToWriteDelay
		+ xClksPerTransfer) << (11 - useablePrecision);

	if (rOn + memSpec.loopLatency >= rOff) {
		TRACE("Error:  (rOn = %d) + (loopLatency = %d) >= (rOff = %d)\n",
			rOn, memSpec.loopLatency, rOff);
		return false;
	}

	params.dda_config = xClksPerTransferPrecise | (useablePrecision << 16)
			| (memSpec.loopLatency << 20);
	params.dda_on_off = (rOn << 16) | rOff;

	return true;
}


static bool
CalculatePLLRegisters(const DisplayModeEx& mode, DisplayParams& params)
{
	// Define PLL registers for requested video mode.

	struct Divider {
		int divider;
		int bitValue;
	};

	// The following data is from RAGE 128 VR/RAGE 128 GL Register Reference
	// Manual (Technical Reference Manual P/N RRG-G04100-C Rev. 0.04), page
	// 3-17 (PLL_DIV_[3:0]).

	const Divider postDividers[] = {
		{ 1, 0 },		// VCLK_SRC
		{ 2, 1 },		// VCLK_SRC/2
		{ 4, 2 },		// VCLK_SRC/4
		{ 8, 3 },		// VCLK_SRC/8
		{ 3, 4 },		// VCLK_SRC/3
						// bitValue = 5 is reserved
		{ 6, 6 },		// VCLK_SRC/6
		{ 12, 7 }		// VCLK_SRC/12
	};

	R128_PLLParams& pll = gInfo.sharedInfo->r128PLLParams;
	uint32 freq = mode.timing.pixel_clock / 10;

	if (freq > pll.max_pll_freq)
		freq = pll.max_pll_freq;
	if (freq * 12 < pll.min_pll_freq)
		freq = pll.min_pll_freq / 12;

	int bitValue = -1;
	uint32 output_freq;

	for (int j = 0; j < (int)B_COUNT_OF(postDividers); j++) {
		output_freq = postDividers[j].divider * freq;
		if (output_freq >= pll.min_pll_freq && output_freq <= pll.max_pll_freq) {
			params.feedback_div = DivideWithRounding(pll.reference_div * output_freq,
				pll.reference_freq);
			params.post_div = postDividers[j].divider;
			bitValue = postDividers[j].bitValue;
			break;
		}
	}

	if (bitValue < 0) {
		TRACE("CalculatePLLRegisters(), acceptable divider not found\n");
		return false;
	}

	params.ppll_ref_div = pll.reference_div;
	params.ppll_div_3 = (params.feedback_div | (bitValue << 16));

	return true;
}


static void
PLLWaitForReadUpdateComplete()
{
	// ppc bring-up: bounded. This waits on the PLL through an index/data
	// register pair, which is exactly the access pattern that needed an
	// explicit barrier on the NVIDIA port - so it is a plausible hang.
	for (int i = 0; i < 1000000; i++) {
		if (!(GetPLLReg(R128_PPLL_REF_DIV) & R128_PPLL_ATOMIC_UPDATE_R))
			break;
		if (i == 999999)
			TRACE("PLL atomic update TIMED OUT\n");
	}
}

static void
PLLWriteUpdate()
{
	PLLWaitForReadUpdateComplete();

	SetPLLReg(R128_PPLL_REF_DIV, R128_PPLL_ATOMIC_UPDATE_W, R128_PPLL_ATOMIC_UPDATE_W);
}


#ifdef __POWERPC__
#include <stdio.h>

// ppc bring-up bisect. Which register group blanks the iMac's CRT? The driver
// runs one stage per boot and rotates, so the whole bisect costs one flash and
// two reboots rather than three build-flash-readback rounds.
//
//   1  CRTC only          2  CRTC + DDA          3  everything
//
// In stages 1 and 2 the pixel clock stays as OpenFirmware left it, which puts
// 1024x768 at ~57.6 Hz - odd, but well inside a CRT's range.
static int
ppc_bisect_stage(void)
{
	static int sStage = -1;
	if (sStage >= 0)
		return sStage;

	// Default to the FULL modeset now that the bisect has done its job -
	// stages 1 and 2 showed a picture on the iMac and stage 3 did not, which
	// is what identified the PLL. The gates stay, still overridable through
	// /boot/home/ati_stage, because they cost nothing and have earned it.
	sStage = 3;
	FILE* f = fopen("/boot/home/ati_stage", "r");
	if (f != NULL) {
		if (fscanf(f, "%d", &sStage) != 1 || sStage < 1 || sStage > 3)
			sStage = 1;
		fclose(f);
	}

	// No longer rotates: the bisect is finished, so every boot runs the stage
	// asked for and repeats are directly comparable.
	TRACE("ppc: modeset stage %d (1=CRTC only, 2=+DDA, 3=full)\n", sStage);
	return sStage;
}
#endif	// __POWERPC__

static void
SetRegisters(DisplayParams& params,
	const DisplayModeEx& mode)
{
	// Write the common registers (most will be set to zero).
	//-------------------------------------------------------

	OUTREGM(R128_FP_GEN_CNTL, R128_FP_BLANK_DIS, R128_FP_BLANK_DIS);

	OUTREG(R128_OVR_CLR, 0);
	OUTREG(R128_OVR_WID_LEFT_RIGHT, 0);
	OUTREG(R128_OVR_WID_TOP_BOTTOM, 0);
	OUTREG(R128_OV0_SCALE_CNTL, 0);
	OUTREG(R128_MPP_TB_CONFIG, 0);
	OUTREG(R128_MPP_GP_CONFIG, 0);
	OUTREG(R128_SUBPIC_CNTL, 0);
	OUTREG(R128_VIPH_CONTROL, 0);
	OUTREG(R128_I2C_CNTL_1, 0);
	OUTREG(R128_GEN_INT_CNTL, 0);
	OUTREG(R128_CAP0_TRIG_CNTL, 0);
	OUTREG(R128_CAP1_TRIG_CNTL, 0);

	// If bursts are enabled, turn on discards and aborts.

	uint32 busCntl = INREG(R128_BUS_CNTL);
	if (busCntl & (R128_BUS_WRT_BURST | R128_BUS_READ_BURST)) {
		busCntl |= R128_BUS_RD_DISCARD_EN | R128_BUS_RD_ABORT_EN;
		OUTREG(R128_BUS_CNTL, busCntl);
	}

	// Write the DDA registers.
	//-------------------------

#ifdef __POWERPC__
	if (ppc_bisect_stage() >= 2) {
#endif
	OUTREG(R128_DDA_CONFIG, params.dda_config);
	OUTREG(R128_DDA_ON_OFF, params.dda_on_off);
#ifdef __POWERPC__
	} else
		TRACE("ppc bisect: DDA writes SKIPPED\n");
#endif

	// Write the CRTC registers.
	//--------------------------

	OUTREG(R128_CRTC_GEN_CNTL, params.crtc_gen_cntl);

	OUTREGM(R128_DAC_CNTL, R128_DAC_MASK_ALL | R128_DAC_8BIT_EN,
			~(R128_DAC_RANGE_CNTL | R128_DAC_BLANKING));

#ifdef __POWERPC__
	// KEEP OF RASTER. OpenFirmware drives this CRT at 800x600 with h_total
	// 1040 / v_total 632 and a 62.375 MHz dot clock - about 95 Hz. Our
	// computed 800x600@60 (1056/628, 39.8 MHz) is textbook, and blanks it.
	// Apple CRTs of this era ran 75/95/117 Hz and many will not lock at 60.
	//
	// So leave the raster alone and change only what a depth change requires.
	// app_server is already asking for 800x600, the size OF is displaying, so
	// if refresh is the problem the picture should simply appear.
	TRACE("ppc: KEEPING OF raster - CRTC timing and PLL not written\n");
	TRACE("ppc: (would have written h 0x%08x v 0x%08x)\n",
		params.crtc_h_total_disp, params.crtc_v_total_disp);
#else
	OUTREG(R128_CRTC_H_TOTAL_DISP, params.crtc_h_total_disp);
	OUTREG(R128_CRTC_H_SYNC_STRT_WID, params.crtc_h_sync_strt_wid);
	OUTREG(R128_CRTC_V_TOTAL_DISP, params.crtc_v_total_disp);
	OUTREG(R128_CRTC_V_SYNC_STRT_WID, params.crtc_v_sync_strt_wid);
#endif
	OUTREG(R128_CRTC_OFFSET, 0);
	OUTREG(R128_CRTC_OFFSET_CNTL, 0);
#ifdef __POWERPC__
	// pitch scaled by pixel size. h_display>>3 is depth-independent, which is
	// only right if CRTC_PITCH counts 8-PIXEL units. OF's 8bpp value cannot
	// tell the two readings apart; at 16bpp they differ by the exact factor of
	// two that the duplicated line implies.
	{
		uint32 bytesPerPixel = (mode.bitsPerPixel + 7) / 8;
		// NOT scaled. Cycle 14 tried x2 and got exactly 2x vertical
		// compression with unwritten VRAM below the image, so the field is
		// in 8-PIXEL units and the driver's h_display>>3 was right all along:
		// 100 -> 800 px -> 1600 bytes, which is app_server's bytes_per_row.
		TRACE("ppc: CRTC_PITCH %u (= %u bytes/line at %d bpp)\n",
			params.crtc_pitch, params.crtc_pitch * 8 * bytesPerPixel,
			mode.bitsPerPixel);
		OUTREG(R128_CRTC_PITCH, params.crtc_pitch);
	}
#else
	OUTREG(R128_CRTC_PITCH, params.crtc_pitch);
#endif

	// Write the PLL registers.
	//-------------------------

#ifdef __POWERPC__
	// KEEP OF RASTER: the pixel clock stays exactly as OpenFirmware set it.
	TRACE("ppc: PLL not written - keeping OF's pixel clock\n");
	snooze(50000);
	TRACE("VERIFY-END: CRTC_GEN_CNTL    0x%08x\n", INREG(R128_CRTC_GEN_CNTL));
	TRACE("VERIFY-END: CRTC_PITCH       0x%08x\n", INREG(R128_CRTC_PITCH));
	TRACE("VERIFY-END: CRTC_OFFSET      0x%08x\n", INREG(R128_CRTC_OFFSET));
	TRACE("VERIFY-END: CRTC_H_TOTAL_DISP 0x%08x  V 0x%08x\n",
		INREG(R128_CRTC_H_TOTAL_DISP), INREG(R128_CRTC_V_TOTAL_DISP));
	TRACE("VERIFY-END: DDA_CONFIG       0x%08x  ON_OFF 0x%08x\n",
		INREG(R128_DDA_CONFIG), INREG(R128_DDA_ON_OFF));
	return;
	if (ppc_bisect_stage() < 3) {
		TRACE("ppc bisect: PLL writes SKIPPED (keeping OpenFirmware's"
			" pixel clock)\n");
		return;
	}
#endif

	OUTREGM(R128_CLOCK_CNTL_INDEX, R128_PLL_DIV_SEL, R128_PLL_DIV_SEL);

	SetPLLReg(R128_VCLK_ECP_CNTL, R128_VCLK_SRC_SEL_CPUCLK, R128_VCLK_SRC_SEL_MASK);

	SetPLLReg(R128_PPLL_CNTL, 0xffffffff,
		R128_PPLL_RESET | R128_PPLL_ATOMIC_UPDATE_EN | R128_PPLL_VGA_ATOMIC_UPDATE_EN);

	PLLWaitForReadUpdateComplete();
	SetPLLReg(R128_PPLL_REF_DIV, params.ppll_ref_div, R128_PPLL_REF_DIV_MASK);
	PLLWriteUpdate();

	PLLWaitForReadUpdateComplete();
	SetPLLReg(R128_PPLL_DIV_3, params.ppll_div_3,
		R128_PPLL_FB3_DIV_MASK | R128_PPLL_POST3_DIV_MASK);
	PLLWriteUpdate();

	PLLWaitForReadUpdateComplete();
	SetPLLReg(R128_HTOTAL_CNTL, 0);
	PLLWriteUpdate();

	SetPLLReg(R128_PPLL_CNTL, 0, R128_PPLL_RESET
								 | R128_PPLL_SLEEP
								 | R128_PPLL_ATOMIC_UPDATE_EN
								 | R128_PPLL_VGA_ATOMIC_UPDATE_EN);

	snooze(5000);

	SetPLLReg(R128_VCLK_ECP_CNTL, R128_VCLK_SRC_SEL_PPLLCLK,
				R128_VCLK_SRC_SEL_MASK);

	// Switch the CRT output on. Measured on a real iMac G3: at the end of an
	// otherwise perfect modeset - PLL locked, reset released, VCLK on the
	// PLL, timing exact - CRTC_EXT_CNTL read 0x00200000, with CRT_ON (bit 15)
	// CLEAR. A CRTC programmed correctly into an output that is switched off
	// is a black screen, which is the symptom.
	//
	// Nothing in this driver has ever set that bit, because on x86 the VGA
	// BIOS sets it at POST and the driver inherits it. Apple cards carry an
	// OpenFirmware FCode ROM and no VGA BIOS, so nobody sets it here. The
	// reference drivers do not inherit it either - aty128fb and XFree86's r128
	// both program CRTC_EXT_CNTL explicitly.
	//
	// Masked so only this bit moves; the DPMS code owns the DIS bits.
	OUTREGM(R128_CRTC_EXT_CNTL, R128_CRTC_CRT_ON, R128_CRTC_CRT_ON);
	TRACE("VERIFY-END: CRT_ON set, CRTC_EXT_CNTL now 0x%08x\n",
		INREG(R128_CRTC_EXT_CNTL));

	// ---- the measurement that actually means something -----------------
	// Taken AFTER the reset release and the VCLK switch above. Cycle 10 read
	// these mid-sequence, where PPLL_RESET asserted and VCLK on CPUCLK are
	// both normal, and drew a false conclusion from it.
	snooze(50000);
	TRACE("VERIFY-END: PPLL_CNTL        0x%08x  (bit0 PPLL_RESET must be 0)\n",
		GetPLLReg(R128_PPLL_CNTL));
	TRACE("VERIFY-END: VCLK_ECP_CNTL    0x%08x  (src must be PPLL, OF had 3)\n",
		GetPLLReg(R128_VCLK_ECP_CNTL));
	TRACE("VERIFY-END: PPLL_REF_DIV     0x%08x\n",
		GetPLLReg(R128_PPLL_REF_DIV));
	TRACE("VERIFY-END: PPLL_DIV_3       0x%08x\n",
		GetPLLReg(R128_PPLL_DIV_3));
	TRACE("VERIFY-END: CRTC_GEN_CNTL    0x%08x\n", INREG(R128_CRTC_GEN_CNTL));
	TRACE("VERIFY-END: CRTC_EXT_CNTL    0x%08x\n", INREG(0x0054));
	TRACE("VERIFY-END: DAC_CNTL         0x%08x\n", INREG(R128_DAC_CNTL));
	TRACE("VERIFY-END: GEN_RESET_CNTL   0x%08x\n", INREG(0x00f0));
	TRACE("VERIFY-END: CRTC_H_TOTAL_DISP 0x%08x  V 0x%08x\n",
		INREG(R128_CRTC_H_TOTAL_DISP), INREG(R128_CRTC_V_TOTAL_DISP));
}



status_t
Rage128_SetDisplayMode(const DisplayModeEx& mode)
{
	// The code to actually configure the display.
	// All the error checking must be done in ProposeDisplayMode(),
	// and assume that the mode values we get here are acceptable.

	DisplayParams params;		// where computed parameters are saved

	if (gInfo.sharedInfo->displayType == MT_VGA) {
		// Chip is connected to a monitor via a VGA connector.

		if ( ! CalculateCrtcRegisters(mode, params))
			return B_BAD_VALUE;

		if ( ! CalculatePLLRegisters(mode, params))
			return B_BAD_VALUE;

		if ( ! CalculateDDARegisters(mode, params))
			return B_BAD_VALUE;

#if 0	// ppc DRY RUN off for cycle 10 (real modeset)	// ppc DRY RUN back ON
		// ppc bring-up DRY RUN. Cycle 2 blanked the iMac's CRT inside
		// SetRegisters() and took the whole hardware trip with it - no
		// picture, no clean shutdown, no syslog, no data at all. So compute
		// and REPORT everything, write nothing, and leave the CRTC exactly as
		// OpenFirmware set it. Returning here also skips the palette reload
		// and Rage128_EngineInit(), so nothing downstream can touch the
		// display either.
		//
		// app_server will believe the mode changed while the hardware did not,
		// so the picture will be GARBLED - wrong stride and depth. That is
		// intended: garbled survives to a clean shutdown and hands over the
		// log, blank does not.
		TRACE("DRY RUN (ppc): computed but NOT written -\n");
		TRACE("  requested %dx%d  %d bpp  pixel clock %d kHz\n",
			mode.timing.h_display, mode.timing.v_display, mode.bitsPerPixel,
			mode.timing.pixel_clock);
		TRACE("  crtc_gen_cntl        0x%08x\n", params.crtc_gen_cntl);
		TRACE("  crtc_h_total_disp    0x%08x\n", params.crtc_h_total_disp);
		TRACE("  crtc_h_sync_strt_wid 0x%08x\n", params.crtc_h_sync_strt_wid);
		TRACE("  crtc_v_total_disp    0x%08x\n", params.crtc_v_total_disp);
		TRACE("  crtc_v_sync_strt_wid 0x%08x\n", params.crtc_v_sync_strt_wid);
		TRACE("  crtc_pitch           0x%08x\n", params.crtc_pitch);
		TRACE("  dda_config           0x%08x\n", params.dda_config);
		TRACE("  dda_on_off           0x%08x\n", params.dda_on_off);
		TRACE("  ppll_ref_div         0x%08x\n", params.ppll_ref_div);
		TRACE("  ppll_div_3           0x%08x  (fb_div %d  post_code %d)\n",
			params.ppll_div_3, (int)(params.ppll_div_3 & 0x7ff),
			(int)((params.ppll_div_3 >> 16) & 0x7));
		TRACE("  feedback_div %d  post_div %d\n", params.feedback_div,
			params.post_div);
		TRACE("DRY RUN (ppc): CRTC left as OpenFirmware set it\n");
		return B_OK;
#endif

		SetRegisters(params, mode);

	} else {
		// Chip is connected to a laptop LCD monitor; or via a DVI interface.

		uint16 vesaMode = GetVesaModeNumber(display_mode(mode), mode.bitsPerPixel);
		if (vesaMode == 0)
			return B_BAD_VALUE;

		if (ioctl(gInfo.deviceFileDesc, ATI_SET_VESA_DISPLAY_MODE,
				&vesaMode, sizeof(vesaMode)) != B_OK)
			return B_ERROR;
	}

	Rage128_AdjustFrame(mode);

	// Initialize the palette so that color depths > 8 bits/pixel will display
	// the correct colors.

	// Select primary monitor and enable 8-bit color.
	OUTREGM(R128_DAC_CNTL, R128_DAC_8BIT_EN,
		R128_DAC_PALETTE_ACC_CTL | R128_DAC_8BIT_EN);
	OUTREG8(R128_PALETTE_INDEX, 0);		// set first color index

	for (int i = 0; i < 256; i++)
		OUTREG(R128_PALETTE_DATA, (i << 16) | (i << 8) | i );

#ifdef __POWERPC__
	// EngineInit skipped: nothing uses the 2D engine on ppc (see hooks.cpp),
	// and initialising it is itself a suspect for the VRAM corruption.
	TRACE("ppc: Rage128_EngineInit skipped - 2D acceleration is off\n");
#else
	Rage128_EngineInit(mode);
#endif

	return B_OK;
}



void
Rage128_AdjustFrame(const DisplayModeEx& mode)
{
	// Adjust start address in frame buffer.

	SharedInfo& si = *gInfo.sharedInfo;

	int address = (mode.v_display_start * mode.virtual_width
			+ mode.h_display_start) * ((mode.bitsPerPixel + 1) / 8);

	address &= ~0x07;
	address += si.frameBufferOffset;

	OUTREG(R128_CRTC_OFFSET, address);
	return;
}


void
Rage128_SetIndexedColors(uint count, uint8 first, uint8* colorData, uint32 flags)
{
	// Set the indexed color palette for 8-bit color depth mode.

	(void)flags;		// avoid compiler warning for unused arg

	if (gInfo.sharedInfo->displayMode.space != B_CMAP8)
		return ;

	// Select primary monitor and enable 8-bit color.
	OUTREGM(R128_DAC_CNTL, R128_DAC_8BIT_EN,
		R128_DAC_PALETTE_ACC_CTL | R128_DAC_8BIT_EN);
	OUTREG8(R128_PALETTE_INDEX, first);		// set first color index

	while (count--) {
		OUTREG(R128_PALETTE_DATA, ((colorData[0] << 16)	// red
								 | (colorData[1] << 8)	// green
								 |  colorData[2]));		// blue
		colorData += 3;
	}
}
