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
	fHaveLast(false),
	fAccumX(0),
	fAccumY(0),
	fLastButtons(0)
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

	// Signal = increase of each sensor over its (drifting) baseline.
	int32 xsum = 0, ysum = 0;
	int64 xacc = 0, yacc = 0;
	for (int i = 0; i < 16; i++) {
		int32 sx = xcur[i] - fBaseX[i];
		if (sx < 0)
			sx = 0;
		xsum += sx;
		xacc += (int64)sx * i;

		int32 sy = ycur[i] - fBaseY[i];
		if (sy < 0)
			sy = 0;
		ysum += sy;
		yacc += (int64)sy * i;
	}

	const int32 kFingerThreshold = 24;
	int32 dx = 0, dy = 0;
	bool finger = (xsum > kFingerThreshold && ysum > kFingerThreshold);
	if (finger) {
		// Weighted-centroid absolute position at high (x64) resolution.
		int32 posX = (int32)(xacc * 64 / xsum);
		int32 posY = (int32)(yacc * 86 / ysum);	// 2x: only ~8 active Y sensors
		if (!fHaveLast) {
			fLastX = posX;
			fLastY = posY;
			fHaveLast = true;
		} else {
			int32 rawDX = posX - fLastX;
			int32 rawDY = posY - fLastY;
			// Sticky dead-zone: only advance the reference position on a
			// genuine move (delta beyond the zone), so a resting finger's
			// sensor wobble never leaks into the pointer. Much wider while the
			// button is engaged so the pointer holds still during a click.
			int32 dz = (button != 0 || fLastButtons != 0) ? 48 : 12;
			if (rawDX <= dz && rawDX >= -dz)
				rawDX = 0;
			else
				fLastX = posX;
			if (rawDY <= dz && rawDY >= -dz)
				rawDY = 0;
			else
				fLastY = posY;
			// Sub-pixel accumulate + scale down (kSens) for fine, precise
			// control: no smoothing lag, and slow moves are not lost to
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
		fAccumX = 0;
		fAccumY = 0;
		// No finger: let the baseline follow slow sensor drift.
		for (int i = 0; i < 16; i++) {
			fBaseX[i] += (xcur[i] - fBaseX[i]) >> 3;
			fBaseY[i] += (ycur[i] - fBaseY[i]) >> 3;
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

