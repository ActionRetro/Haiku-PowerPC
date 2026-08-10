/*
 * Apple "Geyser"/"Fountain" trackpad protocol handler (appletouch-style).
 * Milestone 1: the device is switched to raw-sensor mode in HIDDevice; this
 * handler publishes an input/mouse device and (for now) just logs the raw
 * sensor reports so the decode can be written against real data.
 * Distributed under the terms of the MIT license.
 */

//!	Driver for USB Human Interface Devices.

#include "Driver.h"
#include "AppleTouchProtocolHandler.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDReport.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <keyboard_mouse_driver.h>


AppleTouchProtocolHandler::AppleTouchProtocolHandler(HIDReport &report)
	:
	ProtocolHandler(report.Device(), "input/mouse/" DEVICE_PATH_SUFFIX "/", 0),
	fReport(report),
	fHaveBaseline(false),
	fLastX(0),
	fLastY(0),
	fSmoothX(0),
	fSmoothY(0),
	fHistN(0),
	fFingerDown(false),
	fMoveHold(0),
	fHaveLast(false),
	fAccumX(0),
	fAccumY(0),
	fLastButtons(0),
	fIdleCount(0),
	fStuckCount(0)
{
	TRACE_ALWAYS("appletouch: Geyser trackpad handler, report size %lu\n",
		(unsigned long)report.ReportSize());
}


void
AppleTouchProtocolHandler::AddHandlers(HIDDevice &device,
	HIDCollection &collection, ProtocolHandler *&handlerList)
{
	(void)collection;
	if (!device.IsAppleTouch())
		return;

	// The Geyser trackpad exposes only a Physical top-level collection, so it
	// is not reachable via the Application-collection loop. Iterate the parsed
	// input reports directly and claim the large raw sensor report (81 bytes).
	HIDParser &parser = device.Parser();
	uint32 count = parser.CountReports(HID_REPORT_TYPE_INPUT);
	for (uint32 i = 0; i < count; i++) {
		HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, i);
		if (report == NULL || report->ReportSize() < 60)
			continue;

		ProtocolHandler *handler = new(std::nothrow)
			AppleTouchProtocolHandler(*report);
		if (handler == NULL) {
			TRACE_ALWAYS("appletouch: failed to allocate handler\n");
			continue;
		}

		handler->SetNextHandler(handlerList);
		handlerList = handler;
		TRACE_ALWAYS("appletouch: added trackpad handler (report %lu bytes)\n",
			(unsigned long)report->ReportSize());
		return;
	}
	TRACE_ALWAYS("appletouch: no suitable input report found\n");
}


status_t
AppleTouchProtocolHandler::Control(uint32 *cookie, uint32 op, void *buffer,
	size_t length)
{
	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			const char name[] = DEVICE_NAME " Trackpad";
			return IOGetDeviceName(name, buffer, length);
		}

		case MS_READ:
		{
			if (length < sizeof(mouse_movement))
				return B_BUFFER_OVERFLOW;

			while (true) {
				mouse_movement movement;
				status_t result = _ReadReport(&movement, cookie);
				if (result == B_INTERRUPTED)
					return result;
				if (result == B_BUSY)
					continue;
				if (result != B_OK)
					return result;

				if (!IS_USER_ADDRESS(buffer)
					|| user_memcpy(buffer, &movement, sizeof(movement))
						!= B_OK) {
					return B_BAD_ADDRESS;
				}
				return B_OK;
			}
		}

		case MS_NUM_EVENTS:
			if (fReport.Device()->IsRemoved())
				return B_DEV_NOT_READY;
			return 0;

		case MS_SET_CLICKSPEED:
			return B_OK;
	}

	return B_ERROR;
}


status_t
AppleTouchProtocolHandler::_ReadReport(void *buffer, uint32 *cookie)
{
	status_t result = fReport.WaitForReport(B_INFINITE_TIMEOUT);
	if (result != B_OK) {
		if (fReport.Device()->IsRemoved())
			return B_DEV_NOT_READY;
		if (result == B_INTERRUPTED)
			return result;
		if ((*cookie & PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED) != 0)
			return B_FILE_ERROR;
		return B_BUSY;
	}

	uint8 *data = fReport.CurrentReport();
	size_t size = fReport.ReportSize();

	// --- Milestone 2: decode the Geyser raw sensor report ---
	// Deinterleave the 16 X and 16 Y capacitive sensors (appletouch GEYSER1
	// layout; data[5*i] are skipped markers).
	int32 xcur[16];
	int32 ycur[16];
	if (data == NULL || size < 40) {
		fReport.DoneProcessing();
		memset(buffer, 0, sizeof(mouse_movement));
		return B_OK;
	}
	uint8 button = data[size - 1] & 0x01;
	for (int i = 0; i < 8; i++) {
		xcur[i]     = data[5 * i + 2];
		xcur[i + 8] = data[5 * i + 4];
		ycur[i]     = data[5 * i + 1];
		ycur[i + 8] = data[5 * i + 3];
	}

	if (!fHaveBaseline) {
		for (int i = 0; i < 16; i++) {
			fBaseX[i] = xcur[i];
			fBaseY[i] = ycur[i];
		}
		fHaveBaseline = true;
	}

	// Signal = increase of each sensor over its (drifting) baseline. The full
	// sums (xsumRaw/ysumRaw) drive finger detection and the baseline freeze; a
	// small per-sensor floor is applied ONLY to the weighted-centroid sums so
	// idle-sensor noise does not wander the reported position. (An earlier
	// attempt to spatially [1 2 1]/4-smooth the profile was removed: its integer
	// truncation drained ~40+ off the signal sum over 16 sensors x 4 passes,
	// collapsing finger detection to ~3% and making the pad feel unmovable.)
	int32 xsumRaw = 0, ysumRaw = 0;
	int32 xsum = 0, ysum = 0;
	int64 xacc = 0, yacc = 0;
	const int32 kSensorFloor = 5;
	for (int i = 0; i < 16; i++) {
		int32 sx = xcur[i] - fBaseX[i];
		if (sx < 0)
			sx = 0;
		xsumRaw += sx;
		if (sx >= kSensorFloor) {
			xsum += sx;
			xacc += (int64)sx * i;
		}
		int32 sy = ycur[i] - fBaseY[i];
		if (sy < 0)
			sy = 0;
		ysumRaw += sy;
		if (sy >= kSensorFloor) {
			ysum += sy;
			yacc += (int64)sy * i;
		}
	}

	// Finger detection with hysteresis: engage above the ON gate, hold until the
	// signal falls below the far-lower OFF gate. A single threshold made a light
	// or slightly-varying touch flicker in and out (a big part of the pad
	// feeling dead); hysteresis keeps it continuously detected.
	if (!fFingerDown) {
		if (xsumRaw > 20 && ysumRaw > 20)
			fFingerDown = true;
	} else {
		if (xsumRaw < 8 || ysumRaw < 8)
			fFingerDown = false;
	}

	// Recalibration timings (reports): fully re-snap the idle baseline this
	// often to follow slow thermal drift; and treat a "finger" held longer
	// than this as a stale-baseline phantom to be dropped. The pad streams
	// reports at ~90-125 Hz, so these are roughly 3 s and 15-20 s.
	const int32 kIdleRecal = 256;
	const int32 kStuckLimit = 1800;
	int32 dx = 0, dy = 0;
	bool finger = fFingerDown && xsum > 0 && ysum > 0;
	if (finger)
		fIdleCount = 0;
	// Stuck-baseline watchdog: a real finger never stays continuously down
	// for many seconds. If "finger down" persists far past any plausible
	// gesture it is almost certainly a stale baseline that slow thermal drift
	// has pushed above the detection gate - a phantom that makes the cursor
	// wander in small circles on its own after a few minutes. Force a full
	// baseline recapture and drop it (falling through to the no-finger reset
	// path) so the pad self-heals.
	if (finger && ++fStuckCount > kStuckLimit) {
		for (int i = 0; i < 16; i++) {
			fBaseX[i] = xcur[i];
			fBaseY[i] = ycur[i];
		}
		fFingerDown = false;
		finger = false;
		fStuckCount = 0;
	}
	if (finger) {
		// Weighted-centroid absolute position at high (x64) resolution.
		int32 posX = (int32)(xacc * 64 / xsum);
		int32 posY = (int32)(yacc * 86 / ysum);	// 2x: only ~8 active Y sensors
		// 3-sample median of the centroid. This rejects the single-report medium
		// outliers (jumps of ~20-100 that the coarse spike filter lets through)
		// which caused jitter both at rest and while moving - with NO lag on
		// real motion, since the median of a moving trend is the middle sample.
		// A median beats a low-pass here: it removes lone outliers outright
		// instead of averaging them in and adding onset lag.
		fHistX[2] = fHistX[1]; fHistX[1] = fHistX[0]; fHistX[0] = posX;
		fHistY[2] = fHistY[1]; fHistY[1] = fHistY[0]; fHistY[0] = posY;
		if (fHistN < 3)
			fHistN++;
		int32 medX = posX;
		int32 medY = posY;
		if (fHistN == 3) {
			int32 a = fHistX[0], b = fHistX[1], cc = fHistX[2];
			int32 lo = a < b ? a : b; lo = lo < cc ? lo : cc;
			int32 hi = a > b ? a : b; hi = hi > cc ? hi : cc;
			medX = a + b + cc - lo - hi;
			a = fHistY[0]; b = fHistY[1]; cc = fHistY[2];
			lo = a < b ? a : b; lo = lo < cc ? lo : cc;
			hi = a > b ? a : b; hi = hi > cc ? hi : cc;
			medY = a + b + cc - lo - hi;
		}
		if (!fHaveLast) {
			fSmoothX = medX;
			fSmoothY = medY;
			fLastX = medX;
			fLastY = medY;
			fHaveLast = true;
		} else {
			// Light one-pole low-pass (3/8 blend) on the median-filtered
			// centroid: smooths the small frame-to-frame "wiggle" left on the
			// motion path with only ~20 ms of lag (imperceptible). Safe now that
			// detection is solid - earlier smoothing felt laggy only because it
			// was compounding a broken detector. Measured: path jumbliness drops
			// ~2.7x and motion continuity rises, rest stays essentially still.
			fSmoothX += ((medX - fSmoothX) * 3) / 8;
			// Y is the noisier axis (only ~10 active sensors vs 16, and the
			// centroid is 2x-amplified by yfact to get correct vertical speed,
			// which amplifies its noise too), so smooth it a bit harder - this
			// removes the residual up/down wiggle seen on diagonal moves.
			fSmoothY += ((medY - fSmoothY) * 2) / 8;

			// Distance of the smoothed centroid from a STICKY reference. Inside
			// the dead-zone the reference stays put so a slow move's sub-
			// threshold creep ACCUMULATES and eventually registers (advancing
			// the reference each report instead discarded it, so slow moves
			// barely registered and the pad felt sluggish). The smoothing above
			// keeps a resting finger's wobble inside a small dead-zone, so the
			// steps stay small.
			int32 rawDX = fSmoothX - fLastX;
			int32 rawDY = fSmoothY - fLastY;
			// Spike rejection: a finger cannot teleport. The Fountain pad emits
			// occasional garbage centroids (weak-signal / landing / lift / palm)
			// - measured up to ~8 sensor-units in a single report - which the
			// old code turned into a ~64px pointer jump (the real "jitter").
			// Drop any report whose smoothed centroid jumped farther than a real
			// finger could and resync the smoother to the reference so the glitch
			// leaves no lingering offset.
			const int32 kMaxStep = 256;	// ~4 sensor units at x64; real fast
										// flicks stay well under this
			if (rawDX > kMaxStep || rawDX < -kMaxStep
				|| rawDY > kMaxStep || rawDY < -kMaxStep) {
				fSmoothX = fLastX;
				fSmoothY = fLastY;
				rawDX = 0;
				rawDY = 0;
			} else {
				// Adaptive dead-zone: wide while the finger rests (a still finger
				// stays put) but ~zero once it is actively moving, so motion is
				// reported every report instead of accumulating into visible
				// steps. A short hold keeps "moving mode" through brief pauses
				// mid-gesture. Measured on real gestures this lifts motion
				// smoothness from ~60% to ~75% continuous reports while keeping a
				// resting finger essentially still.
				int32 dz = (fMoveHold > 0) ? 1 : 6;
				if (button != 0 || fLastButtons != 0)
					dz += 6;	// steadier during a click / drag
				bool moved = false;
				if (rawDX > dz || rawDX < -dz) {
					fLastX = fSmoothX;
					moved = true;
				} else
					rawDX = 0;
				if (rawDY > dz || rawDY < -dz) {
					fLastY = fSmoothY;
					moved = true;
				} else
					rawDY = 0;
				if (moved)
					fMoveHold = 8;
				else if (fMoveHold > 0)
					fMoveHold--;
			}
			// Sub-pixel accumulate + scale down (kSens) for fine, precise
			// control: slow moves are not lost to
			// rounding (the fractional remainder carries over).
			const int32 kSens = 3;
			fAccumX += rawDX;
			fAccumY += rawDY;
			dx = fAccumX / kSens;
			dy = fAccumY / kSens;
			fAccumX -= dx * kSens;
			fAccumY -= dy * kSens;
		}
	} else {
		fHaveLast = false;
		fHistN = 0;
		fMoveHold = 0;
		fAccumX = 0;
		fAccumY = 0;
		fStuckCount = 0;
		// Baseline tracking while no finger is detected. Two mechanisms:
		// (1) When the pad is truly untouched (raw signal below the freeze
		//     gate) track the baseline slowly (>>6). Doing this only when the
		//     signal is genuinely low is what stops a light/marginal touch from
		//     being absorbed into the baseline (which erased the signal and made
		//     the finger undetectable).
		// (2) Periodically FULL-snap the baseline to the current raw reading
		//     every kIdleRecal reports of no detected finger. This is the cure
		//     for slow thermal/humidity drift: as the sensors warm up their
		//     resting value creeps into the 8..20 "no-finger-but-not-idle" band
		//     where the >>6 freeze above is inhibited, so without this the
		//     baseline froze forever and the drift eventually crossed the
		//     detection gate into a phantom wandering finger ("cursor drifts in
		//     little circles by itself after a few minutes"). Snapping during
		//     the frequent no-finger moments keeps the baseline honest.
		//     (appletouch does the same: memcpy(xy_old, xy_cur) on idlecount.)
		if (++fIdleCount >= kIdleRecal) {
			for (int i = 0; i < 16; i++) {
				fBaseX[i] = xcur[i];
				fBaseY[i] = ycur[i];
			}
			fIdleCount = 0;
		} else if (xsumRaw < 8 && ysumRaw < 8) {
			for (int i = 0; i < 16; i++) {
				fBaseX[i] += (xcur[i] - fBaseX[i]) >> 6;
				fBaseY[i] += (ycur[i] - fBaseY[i]) >> 6;
			}
		}
	}

	// Clamp per-report motion to something sane.
	if (dx > 64) dx = 64; else if (dx < -64) dx = -64;
	if (dy > 64) dy = 64; else if (dy < -64) dy = -64;

	fReport.DoneProcessing();

	mouse_movement *info = (mouse_movement *)buffer;
	memset(info, 0, sizeof(mouse_movement));
	info->buttons = button ? 1 : 0;
	info->xdelta = dx;
	info->ydelta = -dy;	// trackpad Y grows downward; Haiku ydelta is up-positive
	info->timestamp = system_time();
	if (button != 0 && fLastButtons == 0)
		info->clicks = 1;
	fLastButtons = button;
	return B_OK;
}

