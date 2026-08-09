/*
** Copyright 2003-2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include "apple.h"

#include <ddm_modules.h>
#include <disk_device_types.h>
#include <KernelExport.h>
#ifdef _BOOT_MODE
#	include <boot/partitions.h>
#else
#	include <DiskDeviceTypes.h>
#endif
#include <util/kernel_cpp.h>

#include <unistd.h>
#include <string.h>


#define TRACE_APPLE 0
#if TRACE_APPLE
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif

#define APPLE_PARTITION_MODULE_NAME "partitioning_systems/apple/v1"

static const char *kApplePartitionTypes[] = {
	"partition_map",	// the partition map itself
	"Driver",			// contains a device driver
	"Driver43",			// the SCSI 4.3 manager
	"MFS",				// Macintosh File System
	"HFS",				// Hierarchical File System (HFS/HFS+)
	"Unix_SVR2",		// UFS
	"PRODOS",
	"Free",				// unused partition
	"Scratch",			// empty partition
	"Driver_ATA",		// the device driver for an ATA device
	"Driver_ATAPI",		// the device driver for an ATAPI device
	"Driver43_CD",		// an SCSI CD-ROM driver suitable for booting
	"FWDriver",			// a FireWire driver for the device
	"Void",				// dummy partition map entry (used to align entries for CD-ROM)
	"Patches",
	NULL
};
#if 0
static const char *kOtherPartitionTypes[] = {
	"Be_BFS",			// Be's BFS (not specified endian)
};
#endif

static status_t
get_next_partition(int fd, apple_driver_descriptor &descriptor, uint32 &cookie,
	apple_partition_map &partition)
{
	uint32 block = cookie;

	// find first partition map if this is the first call,
	// or else, just load the next block
	do {
		ssize_t bytesRead = read_pos(fd, (off_t)block * descriptor.BlockSize(),
					(void *)&partition, sizeof(apple_partition_map));
		if (bytesRead < (ssize_t)sizeof(apple_partition_map))
			return B_ERROR;

		block++;
	} while (cookie == 0 && block < 64 && !partition.HasValidSignature());

	if (!partition.HasValidSignature()) {
		if (cookie)
			return B_ENTRY_NOT_FOUND;

		// we searched for the first partition map entry and failed
		return B_ERROR;
	}

	// the first partition map entry must be of type Apple_partition_map
	if (!cookie && (strncmp(partition.type, "Apple_", 6)
		|| strcmp(partition.type + 6, kApplePartitionTypes[0])))
		return B_ERROR;

	// ToDo: warn about unknown types?

	cookie = block;
	return B_OK;
}


//	#pragma mark -
//	Apple public module interface


static status_t
apple_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;
	}

	return B_ERROR;
}


static float
apple_identify_partition(int fd, partition_data *partition, void **_cookie)
{
	struct apple_driver_descriptor *descriptor;
	uint8 buffer[512];

	if (read_pos(fd, 0, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer))
		return B_ERROR;

	descriptor = (apple_driver_descriptor *)buffer;

	TRACE(("apple: read first chunk (signature = %x)\n", descriptor->signature));

	if (!descriptor->HasValidSignature())
		return B_ERROR;

	TRACE(("apple: valid partition descriptor!\n"));

	// ToDo: Should probably call get_next_partition() once to know if there
	//		are any partitions on this disk

	// copy the relevant part of the first block
	descriptor = new apple_driver_descriptor();
	memcpy(descriptor, buffer, sizeof(apple_driver_descriptor));

	*_cookie = (void *)descriptor;

	// ToDo: reevaluate the priority with ISO-9660 and others in mind
	//		(for CD-ROM only, as far as I can tell)
	return 0.5f;
}


static status_t
apple_scan_partition(int fd, partition_data *partition, void *_cookie)
{
	TRACE(("apple_scan_partition(cookie = %p)\n", _cookie));

	apple_driver_descriptor &descriptor = *(apple_driver_descriptor *)_cookie;

	partition->status = B_PARTITION_VALID;
	partition->flags |= B_PARTITION_PARTITIONING_SYSTEM;
#ifdef _BOOT_MODE
	partition->flags |= B_PARTITION_READ_ONLY;
#endif
	partition->content_size = (off_t)descriptor.BlockSize() * descriptor.BlockCount();

	// scan all children

	apple_partition_map partitionMap;
	uint32 index = 0, cookie = 0;
	status_t status;

	while ((status = get_next_partition(fd, descriptor, cookie, partitionMap)) == B_OK) {
		TRACE(("apple: found partition: name = \"%s\", type = \"%s\"\n",
			partitionMap.name, partitionMap.type));

		if (partitionMap.Start(descriptor) + partitionMap.Size(descriptor) > (uint64)partition->size) {
			TRACE(("apple: child partition exceeds existing space (%lld bytes)\n",
				partitionMap.Size(descriptor)));
			continue;
		}

#ifndef _BOOT_MODE
		// Skip the map's own self-entry: it describes the partition map, not a
		// usable partition. Registering it as a framework child de-syncs the disk
		// device manager's shadow-tree reconciliation on DriveSetup/Installer
		// commits, which made CommitModifications fail with B_BAD_VALUE. The write
		// code (apple_create_child) derives its map block from read_map(), not the
		// framework child count, so skipping this child is safe.
		{
			char typeBuffer[33];
			memcpy(typeBuffer, partitionMap.type, 32);
			typeBuffer[32] = '\0';
			if (strcmp(typeBuffer, "Apple_partition_map") == 0)
				continue;
		}
#endif

		partition_data *child = create_child_partition(partition->id, index++,
			partition->offset + partitionMap.Start(descriptor),
			partitionMap.Size(descriptor), -1);
		if (child == NULL) {
			TRACE(("apple: Creating child at index %ld failed\n", index - 1));
			return B_ERROR;
		}

		child->block_size = partition->block_size;

#ifndef _BOOT_MODE
		// expose the Apple partition type/name (fields are not necessarily
		// null-terminated on disk). makebootable locates the HFS loader
		// partition by its type, and DriveSetup shows these to the user.
		char buffer[33];
		memcpy(buffer, partitionMap.type, 32);
		buffer[32] = '\0';
		child->type = strdup(buffer);
		memcpy(buffer, partitionMap.name, 32);
		buffer[32] = '\0';
		if (buffer[0] != '\0')
			child->name = strdup(buffer);
#endif
	}

	if (status == B_ENTRY_NOT_FOUND)
		return B_OK;

	return status;
}


static void
apple_free_identify_partition_cookie(partition_data *partition, void *_cookie)
{
	delete (apple_driver_descriptor *)_cookie;
}


#ifndef _BOOT_MODE
// write support (apple_write_support.cpp)
extern "C" uint32 apple_get_supported_operations(partition_data* partition,
	uint32 mask);
extern "C" uint32 apple_get_supported_child_operations(partition_data* partition,
	partition_data* child, uint32 mask);
extern "C" bool apple_supports_initializing_child(partition_data* partition,
	const char* system);
extern "C" bool apple_is_sub_system_for(partition_data* partition);
extern "C" bool apple_validate_set_type(partition_data* partition,
	const char* type);
extern "C" bool apple_validate_initialize(partition_data* partition, char* name,
	const char* parameters);
extern "C" bool apple_validate_create_child(partition_data* partition,
	off_t* start, off_t* size, const char* type, const char* name,
	const char* parameters, int32* index);
extern "C" status_t apple_get_partitionable_spaces(partition_data* partition,
	partitionable_space_data* buffer, int32 count, int32* actualCount);
extern "C" status_t apple_get_next_supported_type(partition_data* partition,
	int32* cookie, char* type);
extern "C" status_t apple_get_type_for_content_type(const char* contentType,
	char* type);
extern "C" status_t apple_initialize(int fd, partition_id partition,
	const char* name, const char* parameters, off_t partitionSize,
	disk_job_id job);
extern "C" status_t apple_create_child(int fd, partition_id partition,
	off_t offset, off_t size, const char* type, const char* name,
	const char* parameters, disk_job_id job, partition_id* childID);
extern "C" status_t apple_delete_child(int fd, partition_id partition,
	partition_id child, disk_job_id job);
#endif // !_BOOT_MODE


#ifndef _BOOT_MODE
static partition_module_info sApplePartitionModule = {
#else
partition_module_info gApplePartitionModule = {
#endif
	{
		APPLE_PARTITION_MODULE_NAME,
		0,
		apple_std_ops
	},
	"apple",							// short_name
	APPLE_PARTITION_NAME,				// pretty_name
#ifdef _BOOT_MODE
	0,									// flags
#else
	// flags - the disk device manager reads these directly to decide which
	// operations to offer (KPartitioningSystem::LoadModule -> SetFlags), so
	// they MUST be set even though apple_get_supported_operations also reports
	// them; without them DriveSetup/Installer can't initialize or partition an
	// Apple map.
	B_DISK_SYSTEM_SUPPORTS_INITIALIZING
		| B_DISK_SYSTEM_SUPPORTS_CREATING_CHILD
		| B_DISK_SYSTEM_SUPPORTS_DELETING_CHILD
		| B_DISK_SYSTEM_SUPPORTS_SETTING_TYPE,	// flags
#endif

	// scanning
	apple_identify_partition,			// identify_partition
	apple_scan_partition,				// scan_partition
	apple_free_identify_partition_cookie,	// free_identify_partition_cookie
	NULL,								// free_partition_cookie
	NULL,								// free_partition_content_cookie

#ifdef _BOOT_MODE
};
#else
	// querying
	apple_get_supported_operations,		// get_supported_operations
	apple_get_supported_child_operations,	// get_supported_child_operations
	apple_supports_initializing_child,	// supports_initializing_child
	apple_is_sub_system_for,			// is_sub_system_for
	NULL,								// validate_resize
	NULL,								// validate_resize_child
	NULL,								// validate_move
	NULL,								// validate_move_child
	NULL,								// validate_set_name
	NULL,								// validate_set_content_name
	apple_validate_set_type,			// validate_set_type
	NULL,								// validate_set_parameters
	NULL,								// validate_set_content_parameters
	apple_validate_initialize,			// validate_initialize
	apple_validate_create_child,		// validate_create_child
	apple_get_partitionable_spaces,		// get_partitionable_spaces
	apple_get_next_supported_type,		// get_next_supported_type
	apple_get_type_for_content_type,	// get_type_for_content_type

	// shadow partition modification
	NULL,								// shadow_changed

	// writing
	NULL,								// repair
	NULL,								// resize
	NULL,								// resize_child
	NULL,								// move
	NULL,								// move_child
	NULL,								// set_name
	NULL,								// set_content_name
	NULL,								// set_type
	NULL,								// set_parameters
	NULL,								// set_content_parameters
	apple_initialize,					// initialize
	NULL,								// uninitialize
	apple_create_child,					// create_child
	apple_delete_child,					// delete_child
};
#endif

#ifndef _BOOT_MODE
partition_module_info *modules[] = {
	&sApplePartitionModule,
	NULL
};
#endif
