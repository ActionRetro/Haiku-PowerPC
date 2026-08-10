/*
 * Apple "Geyser"/"Fountain" trackpad (appletouch-style) protocol handler.
 * Distributed under the terms of the MIT license.
 */
#ifndef USB_APPLETOUCH_PROTOCOL_HANDLER_H
#define USB_APPLETOUCH_PROTOCOL_HANDLER_H

#include "ProtocolHandler.h"

class HIDCollection;
class HIDReport;


class AppleTouchProtocolHandler : public ProtocolHandler {
public:
							AppleTouchProtocolHandler(HIDReport &report);

	static	void			AddHandlers(HIDDevice &device,
								HIDCollection &collection,
								ProtocolHandler *&handlerList);

	virtual	status_t		Control(uint32 *cookie, uint32 op, void *buffer,
								size_t length);

private:
			status_t		_ReadReport(void *buffer, uint32 *cookie);

			HIDReport &		fReport;

			int32			fBaseX[16];
			int32			fBaseY[16];
			bool			fHaveBaseline;
			int32			fLastX;
			int32			fLastY;
			int32			fSmoothX;
			int32			fSmoothY;
			int32			fHistX[3];
			int32			fHistY[3];
			int32			fHistN;
			bool			fFingerDown;
			int32			fMoveHold;
			bool			fHaveLast;
			int32			fAccumX;
			int32			fAccumY;
			uint32			fLastButtons;
			int32			fIdleCount;
			int32			fStuckCount;
};

#endif // USB_APPLETOUCH_PROTOCOL_HANDLER_H
