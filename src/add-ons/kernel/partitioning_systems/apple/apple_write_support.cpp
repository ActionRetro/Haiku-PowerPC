/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Write support for the Apple partition map (APM), needed so the Installer /
 * DriveSetup can create an OpenFirmware-bootable layout on a target disk:
 *   block 0        : Driver Descriptor Record ('ER')
 *   blocks 1..63   : partition map entries ('PM'); entry 0 is the self entry
 *   block 64..     : partitions (small Apple_HFS loader partition + Haiku BFS)
 *
 * The on-disk map is a flat, contiguous array of 512-byte entries. The reader
 * (apple.cpp) terminates on the first block without a 'PM' signature, so the
 * block following the last entry must stay zeroed.
 */

#include "apple.h"

#include <ByteOrder.h>
#include <KernelExport.h>
#include <disk_device_manager.h>
#include <DiskDeviceTypes.h>
#include <ddm_modules.h>

#include <new>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "PartitionLocker.h"

#include <AutoDeleter.h>


//#define TRACE_APPLE_WRITE
#ifdef TRACE_APPLE_WRITE
#	define WTRACE(x) dprintf x
#else
#	define WTRACE(x) ;
#endif


// The Apple map lives in blocks 1..kMapReservedBlocks; partitions start after.
static const uint32 kMapSelfBlock = 1;
static const uint32 kMapReservedBlocks = 63;
static const uint32 kFirstPartitionBlock = kMapSelfBlock + kMapReservedBlocks;
	// == 64

// partition_status flags (see apple.h) for a freshly created data partition
static const uint32 kNewPartitionStatus = kPartitionIsValid | kPartitionIsAllocated
	| kPartitionIsReadable | kPartitionIsWriteable;
static const uint32 kMapPartitionStatus = kPartitionIsValid | kPartitionIsAllocated;


// Apple partition type strings we hand out. Content detection is filesystem
// based, so these are labels; we match the layout build_combined_disk.py
// produces (which boots on real Open Firmware).
static const char* kAppleTypeHFS = "Apple_HFS";
static const char* kAppleTypeBFS = "Haiku_BFS";
static const char* kAppleTypeMap = "Apple_partition_map";

static const char* const kSupportedChildTypes[] = {
	kAppleTypeBFS, kAppleTypeHFS, NULL
};


// #pragma mark - helpers


//! One in-memory partition map entry plus the block it lives at.
struct MapEntry {
	uint32				block;
	apple_partition_map	map;
};


static inline uint32
block_size_of(partition_data* partition)
{
	return partition->block_size != 0 ? partition->block_size : 512;
}


/*!	Reads every valid 'PM' entry from the map area into \a entries (up to
	\a maxEntries). Returns the number found (entry 0 is the self entry).
*/
static int32
read_map(int fd, uint32 blockSize, MapEntry* entries, int32 maxEntries)
{
	int32 count = 0;
	for (uint32 block = kMapSelfBlock;
			block < kMapSelfBlock + kMapReservedBlocks && count < maxEntries;
			block++) {
		apple_partition_map pm;
		if (read_pos(fd, (off_t)block * blockSize, &pm, sizeof(pm))
				< (ssize_t)sizeof(pm))
			break;
		if (!pm.HasValidSignature())
			break;
		entries[count].block = block;
		entries[count].map = pm;
		count++;
	}
	return count;
}


static status_t
write_map_entry(int fd, uint32 blockSize, uint32 block,
	const apple_partition_map& pm)
{
	// write a full, zero-padded block
	uint8 buffer[512];
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, &pm, sizeof(pm));
	if (write_pos(fd, (off_t)block * blockSize, buffer, blockSize)
			< (ssize_t)blockSize)
		return B_IO_ERROR;
	return B_OK;
}


static status_t
zero_block(int fd, uint32 blockSize, uint32 block)
{
	uint8 buffer[512];
	memset(buffer, 0, sizeof(buffer));
	if (write_pos(fd, (off_t)block * blockSize, buffer, blockSize)
			< (ssize_t)blockSize)
		return B_IO_ERROR;
	return B_OK;
}


// #pragma mark - querying


extern "C" uint32
apple_get_supported_operations(partition_data* partition, uint32 mask)
{
	uint32 flags = B_DISK_SYSTEM_SUPPORTS_INITIALIZING;

	// creating a child is possible if there is free space after the map
	if ((uint64)partition->size
			> (uint64)kFirstPartitionBlock * block_size_of(partition)) {
		flags |= B_DISK_SYSTEM_SUPPORTS_CREATING_CHILD;
	}

	return flags;
}


extern "C" uint32
apple_get_supported_child_operations(partition_data* partition,
	partition_data* child, uint32 mask)
{
	return B_DISK_SYSTEM_SUPPORTS_DELETING_CHILD
		| B_DISK_SYSTEM_SUPPORTS_SETTING_TYPE;
}


extern "C" bool
apple_supports_initializing_child(partition_data* partition, const char* system)
{
	// content initialization is handled by the filesystem add-ons, not here
	return false;
}


extern "C" bool
apple_is_sub_system_for(partition_data* partition)
{
	// the Apple map only ever lives directly on a whole disk device
	(void)partition;
	return false;
}


extern "C" status_t
apple_get_type_for_content_type(const char* contentType, char* type)
{
	if (contentType == NULL || type == NULL)
		return B_BAD_VALUE;

	if (strcmp(contentType, kPartitionTypeBFS) == 0)
		strcpy(type, kAppleTypeBFS);
	else if (strcmp(contentType, kPartitionTypeHFS) == 0
			|| strcmp(contentType, kPartitionTypeHFSPlus) == 0)
		strcpy(type, kAppleTypeHFS);
	else
		strcpy(type, kAppleTypeHFS);

	return B_OK;
}


extern "C" status_t
apple_get_next_supported_type(partition_data* partition, int32* cookie,
	char* type)
{
	int32 index = *cookie;
	if (index < 0 || kSupportedChildTypes[index] == NULL)
		return B_ENTRY_NOT_FOUND;

	strcpy(type, kSupportedChildTypes[index]);
	*cookie = index + 1;
	return B_OK;
}


// #pragma mark - validation


extern "C" bool
apple_validate_initialize(partition_data* partition, char* name,
	const char* parameters)
{
	if ((apple_get_supported_operations(partition, ~0)
			& B_DISK_SYSTEM_SUPPORTS_INITIALIZING) == 0)
		return false;

	// the partition map itself has no name; ignore any given one
	if (name != NULL)
		name[0] = '\0';
	return true;
}


extern "C" bool
apple_validate_create_child(partition_data* partition, off_t* start,
	off_t* size, const char* type, const char* name, const char* parameters,
	int32* index)
{
	if (partition == NULL || start == NULL || size == NULL || index == NULL)
		return false;

	uint32 blockSize = block_size_of(partition);
	off_t firstUsable = (off_t)kFirstPartitionBlock * blockSize;

	// find the end of the last existing child so new partitions append
	off_t usedEnd = firstUsable;
	for (int32 i = 0; i < partition->child_count; i++) {
		partition_data* child = get_child_partition(partition->id, i);
		if (child == NULL)
			continue;
		// skip the self map entry, which lives inside the reserved map area
		if (child->offset < firstUsable)
			continue;
		off_t childEnd = child->offset + child->size;
		if (childEnd > usedEnd)
			usedEnd = childEnd;
	}

	off_t requestedStart = *start;
	if (requestedStart < usedEnd)
		requestedStart = usedEnd;
	// block align
	requestedStart = (requestedStart + blockSize - 1) / blockSize * blockSize;

	if (requestedStart >= partition->offset + partition->size)
		return false;

	off_t maxSize = (partition->offset + partition->size) - requestedStart;
	off_t requestedSize = *size;
	if (requestedSize <= 0 || requestedSize > maxSize)
		requestedSize = maxSize;
	requestedSize = requestedSize / blockSize * blockSize;
	if (requestedSize == 0)
		return false;

	*start = requestedStart;
	*size = requestedSize;
	// child index in the framework == number of existing children
	*index = partition->child_count;
	return true;
}


extern "C" bool
apple_validate_set_type(partition_data* partition, const char* type)
{
	return type != NULL && strlen(type) < 32;
}


extern "C" status_t
apple_get_partitionable_spaces(partition_data* partition,
	partitionable_space_data* buffer, int32 count, int32* actualCount)
{
	uint32 blockSize = block_size_of(partition);
	off_t firstUsable = (off_t)kFirstPartitionBlock * blockSize;

	// determine the end of the last child partition
	off_t usedEnd = firstUsable;
	for (int32 i = 0; i < partition->child_count; i++) {
		partition_data* child = get_child_partition(partition->id, i);
		if (child == NULL || child->offset < firstUsable)
			continue;
		off_t childEnd = child->offset + child->size;
		if (childEnd > usedEnd)
			usedEnd = childEnd;
	}

	off_t diskEnd = partition->offset + partition->size;
	if (usedEnd >= diskEnd) {
		*actualCount = 0;
		return B_OK;
	}

	*actualCount = 1;
	if (count < 1)
		return B_BUFFER_OVERFLOW;

	buffer[0].offset = usedEnd;
	buffer[0].size = diskEnd - usedEnd;
	return B_OK;
}


// #pragma mark - writing


extern "C" status_t
apple_initialize(int fd, partition_id partitionID, const char* name,
	const char* parameters, off_t partitionSize, disk_job_id job)
{
	WTRACE(("apple: initialize\n"));
	if (fd < 0)
		return B_ERROR;

	PartitionWriteLocker locker(partitionID);
	if (!locker.IsLocked())
		return B_ERROR;

	partition_data* partition = get_partition(partitionID);
	if (partition == NULL)
		return B_BAD_VALUE;

	uint32 blockSize = block_size_of(partition);
	uint32 totalBlocks = (uint32)(partitionSize / blockSize);

	update_disk_device_job_progress(job, 0.0);

	// Driver Descriptor Record at block 0
	uint8 block[512];
	memset(block, 0, sizeof(block));
	apple_driver_descriptor* ddr = (apple_driver_descriptor*)block;
	ddr->signature = B_HOST_TO_BENDIAN_INT16((int16)kDriverDescriptorSignature);
	ddr->block_size = B_HOST_TO_BENDIAN_INT16((int16)blockSize);
	ddr->block_count = B_HOST_TO_BENDIAN_INT32((int32)totalBlocks);
	if (write_pos(fd, 0, block, blockSize) < (ssize_t)blockSize)
		return B_IO_ERROR;

	// clear the whole map area so the reader stops at the self entry
	for (uint32 b = kMapSelfBlock; b < kMapSelfBlock + kMapReservedBlocks; b++) {
		status_t status = zero_block(fd, blockSize, b);
		if (status != B_OK)
			return status;
	}

	// the self (map) entry at block 1
	apple_partition_map self;
	memset(&self, 0, sizeof(self));
	self.signature = B_HOST_TO_BENDIAN_INT16((int16)kPartitionMapSignature);
	self.map_block_count = B_HOST_TO_BENDIAN_INT32(1);
	self.start = B_HOST_TO_BENDIAN_INT32((int32)kMapSelfBlock);
	self.size = B_HOST_TO_BENDIAN_INT32((int32)kMapReservedBlocks);
	strlcpy(self.name, "Apple", sizeof(self.name));
	strlcpy(self.type, kAppleTypeMap, sizeof(self.type));
	self.status = B_HOST_TO_BENDIAN_INT32((int32)kMapPartitionStatus);
	status_t status = write_map_entry(fd, blockSize, kMapSelfBlock, self);
	if (status != B_OK)
		return status;

	status = scan_partition(partitionID);
	if (status != B_OK)
		return status;

	update_disk_device_job_progress(job, 1.0);
	partition_modified(partitionID);
	return B_OK;
}


extern "C" status_t
apple_create_child(int fd, partition_id partitionID, off_t offset, off_t size,
	const char* type, const char* name, const char* parameters,
	disk_job_id job, partition_id* childID)
{
	WTRACE(("apple: create_child\n"));
	if (fd < 0 || childID == NULL)
		return B_BAD_VALUE;

	PartitionWriteLocker locker(partitionID);
	if (!locker.IsLocked())
		return B_ERROR;

	partition_data* partition = get_partition(partitionID);
	if (partition == NULL)
		return B_BAD_VALUE;

	uint32 blockSize = block_size_of(partition);

	off_t validatedOffset = offset;
	off_t validatedSize = size;
	int32 index = 0;
	if (!apple_validate_create_child(partition, &validatedOffset, &validatedSize,
			type, name, parameters, &index))
		return B_BAD_VALUE;

	update_disk_device_job_progress(job, 0.0);

	// read the current map. The entries array is ~8.6KB (63 * sizeof(MapEntry)),
	// far too large for the kernel stack given the deep DDM commit call chain
	// (syscall -> CreateChild -> create_child_partition -> AddChild -> realloc);
	// keeping it on the stack overflowed the ppc kernel stack and corrupted
	// memory, producing nondeterministic hangs downstream. Allocate on the heap.
	MapEntry* entries = (MapEntry*)malloc(sizeof(MapEntry) * kMapReservedBlocks);
	if (entries == NULL)
		return B_NO_MEMORY;
	MemoryDeleter entriesDeleter(entries);
	int32 entryCount = read_map(fd, blockSize, entries, kMapReservedBlocks);
	if (entryCount <= 0)
		return B_BAD_DATA;
	if (entryCount >= (int32)kMapReservedBlocks)
		return B_DEVICE_FULL;

	uint32 newBlock = kMapSelfBlock + entryCount;

	// build the new entry
	apple_partition_map pm;
	memset(&pm, 0, sizeof(pm));
	pm.signature = B_HOST_TO_BENDIAN_INT16((int16)kPartitionMapSignature);
	pm.map_block_count = B_HOST_TO_BENDIAN_INT32(entryCount + 1);
	pm.start = B_HOST_TO_BENDIAN_INT32((int32)(validatedOffset / blockSize));
	pm.size = B_HOST_TO_BENDIAN_INT32((int32)(validatedSize / blockSize));
	if (name != NULL)
		strlcpy(pm.name, name, sizeof(pm.name));
	strlcpy(pm.type, type != NULL ? type : kAppleTypeBFS, sizeof(pm.type));
	pm.data_start = B_HOST_TO_BENDIAN_INT32(0);
	pm.data_size = B_HOST_TO_BENDIAN_INT32((int32)(validatedSize / blockSize));
	pm.status = B_HOST_TO_BENDIAN_INT32((int32)kNewPartitionStatus);

	status_t status = write_map_entry(fd, blockSize, newBlock, pm);
	if (status != B_OK)
		return status;

	// make sure the block after the new entry terminates the map
	if (newBlock + 1 < kMapSelfBlock + kMapReservedBlocks) {
		status = zero_block(fd, blockSize, newBlock + 1);
		if (status != B_OK)
			return status;
	}

	// bump map_block_count in all existing entries to include the new one
	for (int32 i = 0; i < entryCount; i++) {
		entries[i].map.map_block_count
			= B_HOST_TO_BENDIAN_INT32(entryCount + 1);
		status = write_map_entry(fd, blockSize, entries[i].block,
			entries[i].map);
		if (status != B_OK)
			return status;
	}

	// Register the child with the framework.
	// KNOWN ppc BLOCKER (2026-08-06): this create_child_partition() call
	// spinlock-deadlocks on ppc (frozen clock) inside KPartition::AddChild ->
	// PublishDevice, when run from a DiskDeviceJobQueue CreateChildJob. All the
	// disk writes above complete; releasing the write lock first does NOT help.
	// Boot-time partition publishing works, so it is job-context/ppc specific.
	partition_data* child = create_child_partition(partition->id, index,
		validatedOffset, validatedSize, *childID);
	if (child == NULL)
		return B_ERROR;

	child->block_size = blockSize;
	child->type = strdup(type != NULL ? type : kAppleTypeBFS);
	if (name != NULL)
		child->name = strdup(name);
	if (parameters != NULL)
		child->parameters = strdup(parameters);

	*childID = child->id;

	update_disk_device_job_progress(job, 1.0);
	partition_modified(partitionID);
	return B_OK;
}


extern "C" status_t
apple_delete_child(int fd, partition_id partitionID, partition_id childID,
	disk_job_id job)
{
	WTRACE(("apple: delete_child\n"));
	if (fd < 0)
		return B_ERROR;

	PartitionWriteLocker locker(partitionID);
	if (!locker.IsLocked())
		return B_ERROR;

	partition_data* partition = get_partition(partitionID);
	partition_data* child = get_partition(childID);
	if (partition == NULL || child == NULL)
		return B_BAD_VALUE;

	uint32 blockSize = block_size_of(partition);
	off_t firstUsable = (off_t)kFirstPartitionBlock * blockSize;

	update_disk_device_job_progress(job, 0.0);

	// read current map and drop the entry matching the child's start block
	MapEntry entries[kMapReservedBlocks];
	int32 entryCount = read_map(fd, blockSize, entries, kMapReservedBlocks);
	WTRACE(("apple: create_child read_map entryCount=%d\n", (int)entryCount));
	if (entryCount <= 0)
		return B_BAD_DATA;

	uint32 childStartBlock = (uint32)(child->offset / blockSize);

	MapEntry kept[kMapReservedBlocks];
	int32 keptCount = 0;
	for (int32 i = 0; i < entryCount; i++) {
		uint32 entryStart = B_BENDIAN_TO_HOST_INT32(entries[i].map.start);
		// always keep the self map entry (inside the reserved area)
		bool isSelf = entries[i].block == kMapSelfBlock
			|| (off_t)entryStart * blockSize < firstUsable;
		if (!isSelf && entryStart == childStartBlock)
			continue;
		kept[keptCount++] = entries[i];
	}

	if (keptCount == entryCount)
		return B_ENTRY_NOT_FOUND;

	// rewrite the map compactly from block 1, terminating with a zeroed block
	for (int32 i = 0; i < keptCount; i++) {
		kept[i].map.map_block_count = B_HOST_TO_BENDIAN_INT32(keptCount);
		status_t status = write_map_entry(fd, blockSize, kMapSelfBlock + i,
			kept[i].map);
		if (status != B_OK)
			return status;
	}
	if ((uint32)(kMapSelfBlock + keptCount)
			< kMapSelfBlock + kMapReservedBlocks) {
		status_t status = zero_block(fd, blockSize, kMapSelfBlock + keptCount);
		if (status != B_OK)
			return status;
	}

	if (!delete_partition(childID))
		return B_ERROR;

	update_disk_device_job_progress(job, 1.0);
	partition_modified(partitionID);
	return B_OK;
}
