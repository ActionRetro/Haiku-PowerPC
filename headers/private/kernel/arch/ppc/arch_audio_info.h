/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_ARCH_PPC_AUDIO_INFO_H
#define KERNEL_ARCH_PPC_AUDIO_INFO_H

#include <SupportDefs.h>


#define PPC_AUDIO_MAX_GPIOS	8

/*!	Everything the sound hardware description of an Apple mac-io machine that
	only Open Firmware knows. The boot loader fills this in (see
	probe_audio_device_tree() in the openfirmware loader's mmu.cpp) because the
	kernel cannot call OF, and the offsets differ from machine to machine:
	Burgundy/Screamer machines expose a "davbus" cell, Tumbler/Snapper and
	later an "i2s" bus with an "i2s-a" cell, and the codec hangs off a Keywest
	i2c bus rather than being addressable directly.

	All offsets are relative to \a macio_phys, the CPU physical base of the
	mac-io register window.
*/
typedef struct ppc_audio_info {
	uint32	valid;			/* 0 = no audio cell found in the device tree */
	uint32	macio_phys;		/* CPU physical base of the mac-io registers */

	uint32	i2s_offset;		/* serial-bus (i2s or davbus) control registers */
	uint32	i2s_size;
	uint32	tx_dbdma_offset;	/* playback DBDMA channel */
	uint32	rx_dbdma_offset;	/* capture DBDMA channel */

	uint32	i2s_irq;		/* interrupt controller inputs, 0 = none */
	uint32	tx_irq;
	uint32	rx_irq;

	uint32	layout_id;		/* "layout-id" of the sound node, 0 = none */
	uint32	device_id;		/* "device-id" of the sound node */

	uint32	i2c_offset;		/* Keywest i2c cell carrying the codec */
	uint32	i2c_address_step;	/* "AAPL,address-step": register spacing */
	uint32	i2c_channel;		/* which bus of the cell the codec is on */
	uint32	codec_i2c_addr;		/* the codec's address on that bus */
	char	codec[32];		/* its "compatible", e.g. "tas3004" */

	/* Amplifier mute, headphone mute/detect and codec reset are GPIOs, named
	   by the device tree's "audio-gpio" property. active_state is the level
	   that means asserted. */
	struct {
		char	name[24];
		uint32	offset;
		uint32	active_state;
		uint32	irq;
	} gpios[PPC_AUDIO_MAX_GPIOS];
	uint32	gpio_count;
} ppc_audio_info;

#endif	/* KERNEL_ARCH_PPC_AUDIO_INFO_H */
