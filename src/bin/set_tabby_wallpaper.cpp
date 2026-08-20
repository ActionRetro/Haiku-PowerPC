/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * set_tabby_wallpaper - apply the default Tabby desktop background: the wordmark
 * drawn untiled in the bottom-right corner, scaled down from a 2x asset, with
 * padding, adapting to any screen resolution. Run by the first-login script on
 * both the live/installer and installed desktops.
 *
 * Usage: set_tabby_wallpaper <image-path>
 */

#include <stdio.h>
#include <string.h>

#include <FindDirectory.h>
#include <Message.h>
#include <Messenger.h>
#include <Node.h>
#include <Path.h>
#include <Point.h>
#include <TypeConstants.h>

#include <be_apps/Tracker/Background.h>


static const char* kTrackerSignature = "application/x-vnd.Be-TRAK";


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <image-path>\n", argv[0]);
		return 1;
	}
	const char* imagePath = argv[1];

	BPath desktop;
	if (find_directory(B_DESKTOP_DIRECTORY, &desktop) != B_OK) {
		fprintf(stderr, "could not find the desktop directory\n");
		return 1;
	}

	BNode node(desktop.Path());
	if (node.InitCheck() != B_OK) {
		fprintf(stderr, "could not open %s\n", desktop.Path());
		return 1;
	}

	// A single entry (index 0) shown across all workspaces, drawn untiled in
	// the bottom-right corner (scaled from a 2x asset) - see BackgroundImage.
	BMessage info;
	info.AddString(B_BACKGROUND_IMAGE, imagePath);
	info.AddInt32(B_BACKGROUND_WORKSPACES, (int32)0xffffffff);
	info.AddInt32(B_BACKGROUND_MODE, B_BACKGROUND_MODE_SCALED_BOTTOM_RIGHT);
	info.AddPoint(B_BACKGROUND_ORIGIN, BPoint(0, 0));

	ssize_t size = info.FlattenedSize();
	char* buffer = new char[size];
	if (info.Flatten(buffer, size) != B_OK) {
		delete[] buffer;
		fprintf(stderr, "could not flatten background info\n");
		return 1;
	}

	ssize_t written = node.WriteAttr(B_BACKGROUND_INFO, B_MESSAGE_TYPE, 0,
		buffer, size);
	delete[] buffer;
	if (written != size) {
		fprintf(stderr, "could not write the background attribute\n");
		return 1;
	}

	// Ask a running Tracker to reload the desktop background immediately.
	BMessenger tracker(kTrackerSignature);
	if (tracker.IsValid())
		tracker.SendMessage(new BMessage(B_RESTORE_BACKGROUND_IMAGE));

	return 0;
}
