/*
 * Copyright 2001-2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 */


/*!	Handles the system's cursor infrastructure */


#include "CursorManager.h"

#include "ServerCursor.h"
#include "ServerConfig.h"
#include "ServerTokenSpace.h"

#include <Autolock.h>
#include <Directory.h>
#include <String.h>
#include <IconUtils.h>

#include <agg_pixfmt_rgba.h>
#include <agg_blur.h>

#include <new>
#include <stdio.h>

#include "CursorData.cpp"
#include "CursorBitmaps.h"


CursorManager::CursorManager()
	:
	BLocker("CursorManager")
{
}


CursorManager::~CursorManager()
{
	ReleaseCursors();
}


void
CursorManager::InitializeCursors(float scale)
{
	if (scale < 1.0)
		scale = 1.0;

	const BPoint kHandHotspot(1, 1);
	const BPoint kResizeHotspot(8, 8);

	struct StandardCursor {
		ServerCursor*& member;
		BCursorID id;
		const uint8* data;
		uint32 dataLength;
		BPoint hotspot;
		const uint8* bitmap;

#define C(IDNAME, NAME, HOTSPOT) {fCursor##NAME, B_CURSOR_ID_##IDNAME, \
		kCursor##NAME, B_COUNT_OF(kCursor##NAME), HOTSPOT, NULL}
/* CB: pixel-grid bitmap art in place of the HVIF vector. Its hotspot differs
 * from the vector version's because it is derived from where the art actually
 * puts its point rather than imposed on it - forcing this set onto the old
 * hotspots threw 118 of Grab's 188 pixels off the canvas, because Haiku's Grab
 * hotspot is (1,1) for a pointer while this art is a centred open hand. */
#define CB(IDNAME, NAME, HOTSPOT) {fCursor##NAME, B_CURSOR_ID_##IDNAME, \
		kCursor##NAME, B_COUNT_OF(kCursor##NAME), HOTSPOT, kCursorBitmap##NAME}
	} standardCursors[] = {
		{fCursorNoCursor, B_CURSOR_ID_NO_CURSOR, NULL, 0, BPoint(0, 0), NULL},

		CB(SYSTEM_DEFAULT,	SystemDefault,	BPoint(2, 2)),
		C(CONTEXT_MENU,		ContextMenu,	kHandHotspot),
		CB(COPY,			Copy,			BPoint(3, 2)),
		C(CREATE_LINK,		CreateLink,		kHandHotspot),
		C(CROSS_HAIR,		CrossHair,		BPoint(10, 10)),
		CB(FOLLOW_LINK,		FollowLink,		BPoint(3, 3)),
		CB(GRAB,			Grab,			BPoint(11, 11)),
		C(GRABBING,			Grabbing,		kHandHotspot),
		CB(HELP,			Help,			BPoint(5, 3)),
		CB(I_BEAM,			IBeam,			BPoint(12, 10)),
		C(I_BEAM_HORIZONTAL, IBeamHorizontal, BPoint(8, 8)),
		C(MOVE,				Move,			kResizeHotspot),
		C(NOT_ALLOWED,		NotAllowed,		BPoint(8, 8)),
		CB(PROGRESS,		Progress,		BPoint(4, 3)),
		C(RESIZE_EAST,		ResizeEast,		kResizeHotspot),
		CB(RESIZE_EAST_WEST, ResizeEastWest, BPoint(10, 12)),
		C(RESIZE_NORTH,		ResizeNorth,	kResizeHotspot),
		C(RESIZE_NORTH_EAST, ResizeNorthEast, kResizeHotspot),
		CB(RESIZE_NORTH_EAST_SOUTH_WEST, ResizeNorthEastSouthWest, BPoint(12, 12)),
		CB(RESIZE_NORTH_SOUTH, ResizeNorthSouth, BPoint(12, 10)),
		C(RESIZE_NORTH_WEST, ResizeNorthWest, kResizeHotspot),
		CB(RESIZE_NORTH_WEST_SOUTH_EAST, ResizeNorthWestSouthEast, BPoint(12, 12)),
		C(RESIZE_SOUTH,		ResizeSouth,	kResizeHotspot),
		C(RESIZE_SOUTH_EAST, ResizeSouthEast, kResizeHotspot),
		C(RESIZE_SOUTH_WEST, ResizeSouthWest, kResizeHotspot),
		C(RESIZE_WEST,		ResizeWest,		kResizeHotspot),
		C(ZOOM_IN,			ZoomIn,			BPoint(6, 6)),
		C(ZOOM_OUT,			ZoomOut,		BPoint(6, 6))
	};
#undef C
#undef CB

	for (size_t i = 0; i < B_COUNT_OF(standardCursors); i++) {
		const StandardCursor& info = standardCursors[i];
		_InitCursor(info.member, info.id, info.data, info.dataLength, info.hotspot,
			scale, info.bitmap);
	}
}


void
CursorManager::ReleaseCursors()
{
	for (int32 i = 0; i < fCursorList.CountItems(); i++) {
		ServerCursor* cursor = ((ServerCursor*)fCursorList.ItemAtFast(i));
		cursor->fManager = NULL;
		fTokenSpace.RemoveToken(cursor->Token());
		cursor->ReleaseReference();
	}
	fCursorList.MakeEmpty();
}


ServerCursor*
CursorManager::CreateCursor(team_id clientTeam, const uint8* cursorData)
{
	if (!Lock())
		return NULL;

	ServerCursorReference cursor(_FindCursor(clientTeam, cursorData), false);

	if (!cursor) {
		cursor.SetTo(new (std::nothrow) ServerCursor(cursorData), true);
		if (cursor) {
			cursor->SetOwningTeam(clientTeam);
			if (AddCursor(cursor) < B_OK)
				cursor = NULL;
		}
	}

	Unlock();

	return cursor.Detach();
}


ServerCursor*
CursorManager::CreateCursor(team_id clientTeam, BRect r, color_space format,
	int32 flags, BPoint hotspot, int32 bytesPerRow)
{
	if (!Lock())
		return NULL;

	ServerCursor* cursor = new (std::nothrow) ServerCursor(r, format, flags,
		hotspot, bytesPerRow);
	if (cursor != NULL) {
		cursor->SetOwningTeam(clientTeam);
		if (AddCursor(cursor) < B_OK) {
			delete cursor;
			cursor = NULL;
		}
	}

	Unlock();

	return cursor;
}


/*!	\brief Registers a cursor with the manager.
	\param cursor ServerCursor object to register
	\return The token assigned to the cursor or B_ERROR if cursor is NULL
*/
int32
CursorManager::AddCursor(ServerCursor* cursor, int32 token)
{
	if (cursor == NULL)
		return B_BAD_VALUE;
	if (!Lock())
		return B_ERROR;

	ServerCursor* oldCursor = FindCursor(token);
	if (oldCursor != NULL) {
		fCursorList.ReplaceItem(fCursorList.IndexOf(oldCursor), cursor);
		oldCursor->ReleaseReference();
	} else {
		if (!fCursorList.AddItem(cursor)) {
			Unlock();
			return B_NO_MEMORY;
		}
	}

	if (token == -1)
		token = fTokenSpace.NewToken(kCursorToken, cursor);
	else
		fTokenSpace.SetToken(token, kCursorToken, cursor);

	cursor->fToken = token;
	cursor->AttachedToManager(this);

	Unlock();
	return token;
}


/*!	\brief Removes a cursor.

	If this was the last reference to this cursor, it will be deleted.
*/
void
CursorManager::RemoveCursor(ServerCursor* cursor)
{
	if (!Lock())
		return;

	_RemoveCursor(cursor);
	cursor->ReleaseReference();

	Unlock();
}


/*!	\brief Removes and deletes all of an application's cursors
	\param signature Signature to which the cursors belong
*/
void
CursorManager::DeleteCursors(team_id team)
{
	if (!Lock())
		return;

	for (int32 index = fCursorList.CountItems(); index-- > 0;) {
		ServerCursor* cursor = (ServerCursor*)fCursorList.ItemAtFast(index);
		if (cursor->OwningTeam() != team)
			continue;

		_RemoveCursor(cursor);
		cursor->ReleaseReference();
	}

	Unlock();
}


/*!	\brief Sets all the cursors from a specified CursorSet
	\param path Path to the cursor set

	All cursors in the set will be assigned. If the set does not specify a
	cursor for a particular cursor specifier, it will remain unchanged.
	This function will fail if passed a NULL path, an invalid path, or the
	path to a non-CursorSet file.
*/
void
CursorManager::SetCursorSet(const char* path)
{
	BAutolock locker (this);

	CursorSet cursorSet(NULL);

	if (!path || cursorSet.Load(path) != B_OK)
		return;

	_LoadCursor(fCursorSystemDefault, cursorSet, B_CURSOR_ID_SYSTEM_DEFAULT);
	_LoadCursor(fCursorContextMenu, cursorSet, B_CURSOR_ID_CONTEXT_MENU);
	_LoadCursor(fCursorCopy, cursorSet, B_CURSOR_ID_COPY);
	_LoadCursor(fCursorCreateLink, cursorSet, B_CURSOR_ID_CREATE_LINK);
	_LoadCursor(fCursorCrossHair, cursorSet, B_CURSOR_ID_CROSS_HAIR);
	_LoadCursor(fCursorFollowLink, cursorSet, B_CURSOR_ID_FOLLOW_LINK);
	_LoadCursor(fCursorGrab, cursorSet, B_CURSOR_ID_GRAB);
	_LoadCursor(fCursorGrabbing, cursorSet, B_CURSOR_ID_GRABBING);
	_LoadCursor(fCursorHelp, cursorSet, B_CURSOR_ID_HELP);
	_LoadCursor(fCursorIBeam, cursorSet, B_CURSOR_ID_I_BEAM);
	_LoadCursor(fCursorIBeamHorizontal, cursorSet,
		B_CURSOR_ID_I_BEAM_HORIZONTAL);
	_LoadCursor(fCursorMove, cursorSet, B_CURSOR_ID_MOVE);
	_LoadCursor(fCursorNotAllowed, cursorSet, B_CURSOR_ID_NOT_ALLOWED);
	_LoadCursor(fCursorProgress, cursorSet, B_CURSOR_ID_PROGRESS);
	_LoadCursor(fCursorResizeEast, cursorSet, B_CURSOR_ID_RESIZE_EAST);
	_LoadCursor(fCursorResizeEastWest, cursorSet,
		B_CURSOR_ID_RESIZE_EAST_WEST);
	_LoadCursor(fCursorResizeNorth, cursorSet, B_CURSOR_ID_RESIZE_NORTH);
	_LoadCursor(fCursorResizeNorthEast, cursorSet,
		B_CURSOR_ID_RESIZE_NORTH_EAST);
	_LoadCursor(fCursorResizeNorthEastSouthWest, cursorSet,
		B_CURSOR_ID_RESIZE_NORTH_EAST_SOUTH_WEST);
	_LoadCursor(fCursorResizeNorthSouth, cursorSet,
		B_CURSOR_ID_RESIZE_NORTH_SOUTH);
	_LoadCursor(fCursorResizeNorthWest, cursorSet,
		B_CURSOR_ID_RESIZE_NORTH_WEST);
	_LoadCursor(fCursorResizeNorthWestSouthEast, cursorSet,
		B_CURSOR_ID_RESIZE_NORTH_WEST_SOUTH_EAST);
	_LoadCursor(fCursorResizeSouth, cursorSet, B_CURSOR_ID_RESIZE_SOUTH);
	_LoadCursor(fCursorResizeSouthEast, cursorSet,
		B_CURSOR_ID_RESIZE_SOUTH_EAST);
	_LoadCursor(fCursorResizeSouthWest, cursorSet,
		B_CURSOR_ID_RESIZE_SOUTH_WEST);
	_LoadCursor(fCursorResizeWest, cursorSet, B_CURSOR_ID_RESIZE_WEST);
	_LoadCursor(fCursorZoomIn, cursorSet, B_CURSOR_ID_ZOOM_IN);
	_LoadCursor(fCursorZoomOut, cursorSet, B_CURSOR_ID_ZOOM_OUT);
}


/*!	\brief Acquire the cursor which is used for a particular system cursor
	\param which Which system cursor to get
	\return Pointer to the particular cursor used or NULL if which is
	invalid or the cursor has not been assigned
*/
ServerCursor*
CursorManager::GetCursor(BCursorID which)
{
	BAutolock locker(this);

	switch (which) {
		case B_CURSOR_ID_SYSTEM_DEFAULT:
			return fCursorSystemDefault;
		case B_CURSOR_ID_CONTEXT_MENU:
			return fCursorContextMenu;
		case B_CURSOR_ID_COPY:
			return fCursorCopy;
		case B_CURSOR_ID_CREATE_LINK:
			return fCursorCreateLink;
		case B_CURSOR_ID_CROSS_HAIR:
			return fCursorCrossHair;
		case B_CURSOR_ID_FOLLOW_LINK:
			return fCursorFollowLink;
		case B_CURSOR_ID_GRAB:
			return fCursorGrab;
		case B_CURSOR_ID_GRABBING:
			return fCursorGrabbing;
		case B_CURSOR_ID_HELP:
			return fCursorHelp;
		case B_CURSOR_ID_I_BEAM:
			return fCursorIBeam;
		case B_CURSOR_ID_I_BEAM_HORIZONTAL:
			return fCursorIBeamHorizontal;
		case B_CURSOR_ID_MOVE:
			return fCursorMove;
		case B_CURSOR_ID_NO_CURSOR:
			return fCursorNoCursor;
		case B_CURSOR_ID_NOT_ALLOWED:
			return fCursorNotAllowed;
		case B_CURSOR_ID_PROGRESS:
			return fCursorProgress;
		case B_CURSOR_ID_RESIZE_EAST:
			return fCursorResizeEast;
		case B_CURSOR_ID_RESIZE_EAST_WEST:
			return fCursorResizeEastWest;
		case B_CURSOR_ID_RESIZE_NORTH:
			return fCursorResizeNorth;
		case B_CURSOR_ID_RESIZE_NORTH_EAST:
			return fCursorResizeNorthEast;
		case B_CURSOR_ID_RESIZE_NORTH_EAST_SOUTH_WEST:
			return fCursorResizeNorthEastSouthWest;
		case B_CURSOR_ID_RESIZE_NORTH_SOUTH:
			return fCursorResizeNorthSouth;
		case B_CURSOR_ID_RESIZE_NORTH_WEST:
			return fCursorResizeNorthWest;
		case B_CURSOR_ID_RESIZE_NORTH_WEST_SOUTH_EAST:
			return fCursorResizeNorthWestSouthEast;
		case B_CURSOR_ID_RESIZE_SOUTH:
			return fCursorResizeSouth;
		case B_CURSOR_ID_RESIZE_SOUTH_EAST:
			return fCursorResizeSouthEast;
		case B_CURSOR_ID_RESIZE_SOUTH_WEST:
			return fCursorResizeSouthWest;
		case B_CURSOR_ID_RESIZE_WEST:
			return fCursorResizeWest;
		case B_CURSOR_ID_ZOOM_IN:
			return fCursorZoomIn;
		case B_CURSOR_ID_ZOOM_OUT:
			return fCursorZoomOut;

		default:
			return NULL;
	}
}


/*!	\brief Internal function which finds the cursor with a particular ID
	\param token ID of the cursor to find
	\return The cursor or NULL if not found
*/
ServerCursor*
CursorManager::FindCursor(int32 token)
{
	if (!Lock())
		return NULL;

	ServerCursor* cursor;
	if (fTokenSpace.GetToken(token, kCursorToken, (void**)&cursor) != B_OK)
		cursor = NULL;

	Unlock();

	return cursor;
}


BBitmap
CursorManager::_RenderVectorCursor(uint32 size, const uint8* vector,
	uint32 vectorSize, float shadowStrength)
{
	const uint32 flags = B_BITMAP_NO_SERVER_LINK;

	// The cursor HVIFs need to be rendered at an equivalent of 32x32,
	// with everything outside the 22x22 area discarded.
	const int32 renderRectSize = (int32)(size * (32.0f / 22.0f));

	BBitmap renderCursor(BRect(0, 0, renderRectSize - 1, renderRectSize - 1), flags, B_RGBA32);
	status_t status = BIconUtils::GetVectorIcon(vector, vectorSize, &renderCursor);
	if (status != B_OK) {
		BBitmap fallback(BRect(0, 0, 21, 21), B_BITMAP_NO_SERVER_LINK, B_RGBA32);
		fallback.SetBits(kCursorFallbackBits, 22 * 22 * 4, 0, B_RGBA32);
		return fallback;
	}

	const BRect rect(0, 0, size - 1, size - 1);
	BBitmap cursor(rect, flags, B_RGBA32);
	cursor.ImportBits(&renderCursor, B_ORIGIN, B_ORIGIN, rect.Size());

	BBitmap shadow(rect, flags, B_RGBA32);
	memset(shadow.Bits(), 0, shadow.BitsLength());

	{
		int32 offset = size / 32;
		if (offset == 0)
			offset = 1; // <32px cursors

		shadow.ImportBits(&cursor, BPoint(0, 0), BPoint(offset, offset),
			BSize(size - offset - 1, size - offset - 1));

		agg::rendering_buffer buffer((unsigned char*)shadow.Bits(),
			size, size, shadow.BytesPerRow());
		agg::pixfmt_rgba32 pixFmt(buffer);

		agg::recursive_blur<agg::rgba8, agg::recursive_blur_calc_rgba<> > blur;
		blur.blur(pixFmt, 1);

		for (int32 i = 0; i < shadow.BitsLength(); i += 4) {
			uint8* bits = (uint8*)shadow.Bits() + i;
			bits[0] = 0;
			bits[1] = 0;
			bits[2] = 0;
			bits[3] = (uint8)(bits[3] * shadowStrength);
		}
	}

	BBitmap composite(rect, flags, B_RGBA32);

	uint8* s = (uint8*)shadow.Bits();
	uint8* c = (uint8*)cursor.Bits();
	uint8* d = (uint8*)composite.Bits();
	for (uint32 y = 0; y < size; y++) {
		for (uint32 x = 0; x < size; x++) {
			uint8 a = (uint8)(c[3] + (255 - c[3]) * (s[3] / 255.0));
			d[3] = a;
			for (int32 i = 0; i < 3; ++i) {
				d[i] = ((s[i] * (255 - c[3]) + 255) >> 8) + c[i];

				// premultiply
				d[i] = (uint8)(d[i] * int32(a) / 255.0);
			}
			s += 4;
			c += 4;
			d += 4;
		}
	}

	return composite;
}


/*!	\brief Initializes a predefined system cursor.

	This method must only be called in the CursorManager's constructor,
	as it may throw exceptions.
*/
void
CursorManager::_InitCursor(ServerCursor*& cursorMember, BCursorID id,
	const uint8* vector, uint32 vectorSize, const BPoint& hotSpot, float scale,
	const uint8* bitmap)
{
	int32 cursorSize = (int32)(22 * scale);
	float shadow = 3 / 10.0;
	BPoint scaledHotspot((int32)(hotSpot.x * scale), (int32)(hotSpot.y * scale));

	// the bitmap path below falls through to the vector one on failure, so
	// this has to start from a known value rather than whatever the member held
	cursorMember = NULL;

	if (bitmap != NULL) {
		// Pixel-grid art. It must be scaled by an INTEGER factor with nearest
		// neighbour: the whole point of this art is that it has no partial
		// alpha, and any fractional resampling reintroduces exactly that.
		//
		// Capped at 2x because the hardware cursor is 64x64 and 22 * 3 = 66
		// would overflow it, dropping app_server back to a software cursor -
		// which is what caused cursor ghosting on accelerated window drags.
		int32 zoom = (int32)(scale + 0.5f);
		if (zoom < 1)
			zoom = 1;
		if (zoom > 2)
			zoom = 2;

		const int32 size = kCursorBitmapSize * zoom;
		uint8* scaled = new(std::nothrow) uint8[size * size * 4];
		if (scaled != NULL) {
			for (int32 y = 0; y < size; y++) {
				const uint8* src = bitmap
					+ (y / zoom) * kCursorBitmapSize * 4;
				uint8* dst = scaled + y * size * 4;
				for (int32 x = 0; x < size; x++) {
					const uint8* s = src + (x / zoom) * 4;
					dst[x * 4 + 0] = s[0];
					dst[x * 4 + 1] = s[1];
					dst[x * 4 + 2] = s[2];
					dst[x * 4 + 3] = s[3];
				}
			}

			cursorMember = new ServerCursor(scaled, size, size, B_RGBA32);
			delete[] scaled;
			scaledHotspot.Set((int32)hotSpot.x * zoom, (int32)hotSpot.y * zoom);
		}
	}

	// ★ The vector and no-cursor branches must stay bound to EACH OTHER. An
	// earlier version changed this to "if (cursorMember == NULL && vector !=
	// NULL)" and left the else where it was, so a successful bitmap cursor
	// fell into the else and was overwritten by the 1x1 transparent
	// no-cursor - every bitmap cursor invisible, every vector one fine.
	if (cursorMember == NULL) {
		if (vector != NULL) {
			BBitmap vectorBitmap = _RenderVectorCursor(cursorSize, vector,
				vectorSize, shadow);
			cursorMember = new ServerCursor((uint8*)vectorBitmap.Bits(),
				cursorSize, cursorSize, vectorBitmap.ColorSpace());
		} else {
			const unsigned char noCursor[] = {0x00, 0x00, 0x00, 0x00};
			cursorMember = new ServerCursor(noCursor, 1, 1, B_RGBA32);
		}
	}

	cursorMember->SetHotSpot(scaledHotspot);
	AddCursor(cursorMember, id);
}


void
CursorManager::_LoadCursor(ServerCursor*& cursorMember, const CursorSet& set,
	BCursorID id)
{
	ServerCursor* cursor;
	if (set.FindCursor(id, &cursor) == B_OK) {
		AddCursor(cursor, id);
		cursorMember = cursor;
	}
}


ServerCursor*
CursorManager::_FindCursor(team_id clientTeam, const uint8* cursorData)
{
	int32 count = fCursorList.CountItems();
	for (int32 i = 0; i < count; i++) {
		ServerCursor* cursor = (ServerCursor*)fCursorList.ItemAtFast(i);
		if (cursor->OwningTeam() == clientTeam
			&& cursor->CursorData()
			&& memcmp(cursor->CursorData(), cursorData, 68) == 0) {
			return cursor;
		}
	}
	return NULL;
}


void
CursorManager::_RemoveCursor(ServerCursor* cursor)
{
	fCursorList.RemoveItem(cursor);
	fTokenSpace.RemoveToken(cursor->fToken);
	cursor->fToken = -1;
}
