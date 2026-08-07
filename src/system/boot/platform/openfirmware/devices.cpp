/*
 * Copyright 2003-2006, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2010, Andreas Färber <andreas.faerber@web.de>
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/vfs.h>
#include <boot/stdio.h>
#include <boot/stage2.h>
#include <boot/net/IP.h>
#include <boot/net/NetStack.h>
#include <boot/net/RemoteDisk.h>
#include <platform/openfirmware/devices.h>
#include <platform/openfirmware/openfirmware.h>
#include <util/kernel_cpp.h>

#include "Handle.h"
#include "machine.h"


#define ENABLE_ISCSI


char sBootPath[192];


status_t 
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	// print out the boot path (to be removed later?)

	int length = of_getprop(gChosen, "bootpath", sBootPath, sizeof(sBootPath));
	if (length <= 1)
		return B_ENTRY_NOT_FOUND;
	printf("boot path = \"%s\"\n", sBootPath);

	intptr_t node = of_finddevice(sBootPath);
	if (node != OF_FAILED) {
		char type[16];
		of_getprop(node, "device_type", type, sizeof(type));
		printf("boot type = %s\n", type);

		// If the boot device is a network device, we try to find a
		// "remote disk" at this point.
		if (strcmp(type, "network") == 0) {
			// init the net stack
			status_t error = net_stack_init();
			if (error != B_OK)
				return error;

			ip_addr_t bootAddress = 0;
			char* bootArgs = strrchr(sBootPath, ':');
			if (bootArgs != NULL) {
				bootArgs++;
				char* comma = strchr(bootArgs, ',');
				if (comma != NULL && comma - bootArgs > 0) {
					comma[0] = '\0';
					bootAddress = ip_parse_address(bootArgs);
					comma[0] = ',';
				}
			}
			if (bootAddress == 0) {
				intptr_t package = of_finddevice("/options");
				char defaultServerIP[16];
				int bytesRead = of_getprop(package, "default-server-ip",
					defaultServerIP, sizeof(defaultServerIP) - 1);
				if (bytesRead != OF_FAILED && bytesRead > 1) {
					defaultServerIP[bytesRead] = '\0';
					bootAddress = ip_parse_address(defaultServerIP);
				}
			}

			// init a native remote disk, if possible
			RemoteDisk *remoteDisk = RemoteDisk::FindAnyRemoteDisk();
			if (remoteDisk != NULL) {
				devicesList->Add(remoteDisk);
				return B_OK;
			}

			return B_ENTRY_NOT_FOUND;
		}

		if (strcmp("block", type) != 0) {
			printf("boot device is not a block device!\n");
			return B_ENTRY_NOT_FOUND;
		}
	} else
		printf("could not open boot path.\n");

	// "bootpath" is the path Open Firmware used to load this boot loader
	// itself: a device path plus a ":<boot-partition>,\haikuloader.elf"
	// argument. That boot-partition number is a CHRP/PReP bootstrap-only
	// pseudo partition used just to locate the loader image, not a
	// generically openable block device - re-opening it (with or without
	// the file part, with or without the ":0" disk-label-bypass argument
	// used below in platform_add_block_devices()) fails. Worse, on this
	// firmware a failed open attempt against the device leaves it in a
	// state where a *second* open of the same underlying device (e.g.
	// the one platform_add_block_devices() does moments later while
	// scanning for partitions) hangs indefinitely instead of failing.
	//
	// But we CAN open the plain device: strip the ":<partition>,\\file"
	// argument off and open it the same way platform_add_block_devices()
	// does below (with the Apple ":0" disk-label bypass). Adding the boot
	// disk here lets the loader boot straight from it, so it never has to
	// fall back to scanning *every* block device - that scan of_open()s each
	// device in turn and hangs indefinitely on some firmware when it reaches
	// an empty optical drive (e.g. the iBook G4's empty combo drive).
	char devicePath[192];
	strlcpy(devicePath, sBootPath, sizeof(devicePath));
	char *argument = strchr(devicePath, ':');
	if (argument != NULL)
		argument[0] = '\0';

	char openPath[200];
	strlcpy(openPath, devicePath, sizeof(openPath));
	if (gMachine & MACHINE_MAC)
		strlcat(openPath, ":0", sizeof(openPath));

	intptr_t handle = of_open(openPath);
	if (handle == OF_FAILED && (gMachine & MACHINE_MAC))
		handle = of_open(devicePath);
	if (handle == OF_FAILED) {
		// couldn't open the boot disk directly; defer to the full scan
		return B_ENTRY_NOT_FOUND;
	}

	Handle *device = new(nothrow) Handle(handle);
	if (device == NULL)
		return B_NO_MEMORY;
	printf("opened boot disk directly: %s\n", openPath);
	devicesList->Add(device);
	return B_OK;
}


status_t
platform_get_boot_partitions(struct stage2_args *args, Node *device,
	NodeList *list, NodeList *partitionList)
{
	// Offer every partition on the boot disk as a boot-partition candidate;
	// get_boot_file_system() then mounts each in turn and keeps the one that is
	// a valid Haiku boot volume. Taking only the first partition failed on an
	// Apple Partition Map disk, whose first entry is the partition map itself
	// (not a file system) - the loader then gave up and fell back to scanning
	// every device, which hangs on an empty optical drive on some firmware.
	NodeIterator iterator = list->GetIterator();
	boot::Partition *partition = NULL;
	status_t status = B_ENTRY_NOT_FOUND;
	while ((partition = (boot::Partition *)iterator.Next()) != NULL) {
		partitionList->Insert(partition);
		status = B_OK;
	}

	return status;
}


void
platform_cleanup_devices()
{
	net_stack_cleanup();
}


#define DUMPED_BLOCK_SIZE 16

void
dumpBlock(const char *buffer, int size, const char *prefix)
{
	int i;
	
	for (i = 0; i < size;) {
		int start = i;

		printf(prefix);
		for (; i < start+DUMPED_BLOCK_SIZE; i++) {
			if (!(i % 4))
				printf(" ");

			if (i >= size)
				printf("  ");
			else
				printf("%02x", *(unsigned char *)(buffer + i));
		}
		printf("  ");

		for (i = start; i < start + DUMPED_BLOCK_SIZE; i++) {
			if (i < size) {
				char c = buffer[i];

				if (c < 30)
					printf(".");
				else
					printf("%c", c);
			} else
				break;
		}
		printf("\n");
	}
}


status_t
platform_add_block_devices(stage2_args *args, NodeList *devicesList)
{
	// add all block devices to the list of possible boot devices

	intptr_t cookie = 0;
	char path[256];
	status_t status;
	while ((status = of_get_next_device(&cookie, 0, "block", path,
			sizeof(path))) == B_OK) {
		if (!strcmp(path, sBootPath)) {
			// don't add the boot device twice
			continue;
		}

		// Adjust the arguments passed to the open command so that
		// the disk-label package is by-passed - unfortunately,
		// this is implementation specific (and I found no docs
		// for the Apple OF disk-label usage, of course)

		// SUN's OpenBoot:
		//strcpy(path + strlen(path), ":nolabel");
		// Apple:
		char bypassPath[256];
		strlcpy(bypassPath, path, sizeof(bypassPath));
		if (gMachine & MACHINE_MAC)
			strlcat(bypassPath, ":0", sizeof(bypassPath));

		printf("\t%s\n", bypassPath);

		// Try the disk-label bypass path first (this is the original,
		// historical behavior). We deliberately do NOT retry with a
		// second open() call on ATAPI/CD-ROM media - a second open of
		// the same underlying device can hang outright on this
		// platform rather than cleanly failing, and this is true even
		// when the second attempt uses a different argument string (not
		// just an identical retry). For non-ATAPI disks, a second
		// open() using the plain path (letting the real disk-label
		// package parse the partition map) is safe and needed for at
		// least some disks where the bypass path fails to open at all.
		bool isAtapi = false;
		for (const char *scan = path; *scan != '\0'; scan++) {
			if (!strncmp(scan, "cdrom", 5)) {
				isAtapi = true;
				break;
			}
		}
		intptr_t handle = of_open(bypassPath);
		if (handle == OF_FAILED && (gMachine & MACHINE_MAC) && !isAtapi) {
			printf("\t\t(bypass path failed, trying plain path %s)\n", path);
			handle = of_open(path);
		}
		if (handle == OF_FAILED) {
			puts("\t\t(failed)");
			continue;
		}

		Handle *device = new(nothrow) Handle(handle);
		printf("\t\t(could open device, handle = %p, node = %p)\n",
			(void *)handle, device);

		devicesList->Add(device);
	}
	printf("\t(loop ended with %ld)\n", status);

	return B_OK;
}


status_t 
platform_register_boot_device(Node *device)
{
	disk_identifier disk;
	memset(&disk, 0, sizeof(disk_identifier));

	disk.bus_type = UNKNOWN_BUS;
	disk.device_type = UNKNOWN_DEVICE;
	disk.device.unknown.size = device->Size();

	// Handle::Size() does not report the real disk size yet (it returns a
	// placeholder), so we cannot compute meaningful check sums to uniquely
	// identify the disk. Mark them unused; the kernel then identifies the boot
	// partition by its offset (which is passed separately and is reliable).
	for (int32 i = 0; i < NUM_DISK_CHECK_SUMS; i++) {
		disk.device.unknown.check_sums[i].offset = -1;
		disk.device.unknown.check_sums[i].sum = 0;
	}

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE, &disk,
		sizeof(disk_identifier));

	return B_OK;
}		

