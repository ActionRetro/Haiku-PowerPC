/*
 * Tabby ppc: userland Apple Partition Map disk-system add-on.
 * Distributed under the terms of the MIT License.
 */

#include "AppleDiskAddOn.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <DiskDeviceTypes.h>	// kPartitionTypeBFS
#include <MutablePartition.h>
#include <PartitioningInfo.h>

#include <AutoDeleter.h>

#include <disk_device_types.h>	// APPLE_PARTITION_NAME


using std::nothrow;


// The Apple map reserves the first 64 blocks (block 0 = driver descriptor,
// blocks 1..63 = partition map entries). User partitions start at block 64,
// matching the kernel partitioning_systems/apple write support.
static const uint32 kFirstPartitionBlock = 64;

static const char* const kSupportedChildTypes[] = {
	"Apple_HFS",				// loader partition
	"Haiku_BFS",				// system partition
	NULL
};


static inline off_t
block_align(off_t value, uint32 blockSize)
{
	return value / blockSize * blockSize;
}


static inline off_t
block_align_up(off_t value, uint32 blockSize)
{
	return (value + blockSize - 1) / blockSize * blockSize;
}


// #pragma mark - AppleDiskAddOn


AppleDiskAddOn::AppleDiskAddOn()
	:
	BDiskSystemAddOn(APPLE_PARTITION_NAME)
{
}


AppleDiskAddOn::~AppleDiskAddOn()
{
}


status_t
AppleDiskAddOn::CreatePartitionHandle(BMutablePartition* partition,
	BPartitionHandle** _handle)
{
	ApplePartitionHandle* handle = new(nothrow) ApplePartitionHandle(partition);
	if (handle == NULL)
		return B_NO_MEMORY;

	status_t error = handle->Init();
	if (error != B_OK) {
		delete handle;
		return error;
	}

	*_handle = handle;
	return B_OK;
}


bool
AppleDiskAddOn::CanInitialize(const BMutablePartition* partition)
{
	// need at least the map plus one usable block
	return partition->Size()
		> (off_t)(kFirstPartitionBlock + 1) * partition->BlockSize();
}


status_t
AppleDiskAddOn::ValidateInitialize(const BMutablePartition* partition,
	BString* name, const char* parameters)
{
	if (!CanInitialize(partition)
		|| (parameters != NULL && parameters[0] != '\0'))
		return B_BAD_VALUE;

	// the Apple map itself has no content name
	if (name != NULL)
		name->Truncate(0);

	return B_OK;
}


status_t
AppleDiskAddOn::Initialize(BMutablePartition* partition, const char* name,
	const char* parameters, BPartitionHandle** _handle)
{
	if (!CanInitialize(partition)
		|| (name != NULL && name[0] != '\0')
		|| (parameters != NULL && parameters[0] != '\0'))
		return B_BAD_VALUE;

	ApplePartitionHandle* handle = new(nothrow) ApplePartitionHandle(partition);
	if (handle == NULL)
		return B_NO_MEMORY;
	ObjectDeleter<ApplePartitionHandle> handleDeleter(handle);

	status_t error = partition->SetContentType(Name());
	if (error != B_OK)
		return error;

	partition->SetContentName(NULL);
	partition->SetContentParameters(NULL);
	partition->SetContentSize(
		block_align(partition->Size(), partition->BlockSize()));
	partition->Changed(B_PARTITION_CHANGED_INITIALIZATION);

	*_handle = handleDeleter.Detach();
	return B_OK;
}


status_t
AppleDiskAddOn::GetTypeForContentType(const char* contentType, BString* type)
{
	if (contentType == NULL || type == NULL)
		return B_BAD_VALUE;

	if (strcmp(contentType, kPartitionTypeBFS) == 0) {
		type->SetTo("Haiku_BFS");
		return B_OK;
	}
	if (strcmp(contentType, "Apple_HFS") == 0) {
		type->SetTo("Apple_HFS");
		return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
}


// #pragma mark - ApplePartitionHandle


ApplePartitionHandle::ApplePartitionHandle(BMutablePartition* partition)
	:
	BPartitionHandle(partition)
{
}


ApplePartitionHandle::~ApplePartitionHandle()
{
}


status_t
ApplePartitionHandle::Init()
{
	// The Apple map is a flat list of children; BMutablePartition already
	// tracks them, so there is no extra bookkeeping to build here.
	return B_OK;
}


uint32
ApplePartitionHandle::SupportedOperations(uint32 mask)
{
	uint32 flags = B_DISK_SYSTEM_SUPPORTS_INITIALIZING;

	if ((mask & B_DISK_SYSTEM_SUPPORTS_CREATING_CHILD) != 0) {
		BPartitioningInfo info;
		if (GetPartitioningInfo(&info) == B_OK
			&& info.CountPartitionableSpaces() > 0) {
			flags |= B_DISK_SYSTEM_SUPPORTS_CREATING_CHILD;
		}
	}

	return flags;
}


uint32
ApplePartitionHandle::SupportedChildOperations(const BMutablePartition* child,
	uint32 mask)
{
	return B_DISK_SYSTEM_SUPPORTS_DELETING_CHILD
		| B_DISK_SYSTEM_SUPPORTS_SETTING_TYPE;
}


status_t
ApplePartitionHandle::GetNextSupportedType(const BMutablePartition* child,
	int32* cookie, BString* type)
{
	int32 index = *cookie;
	if (index < 0 || kSupportedChildTypes[index] == NULL)
		return B_ENTRY_NOT_FOUND;

	type->SetTo(kSupportedChildTypes[index]);
	*cookie = index + 1;
	return B_OK;
}


status_t
ApplePartitionHandle::GetPartitioningInfo(BPartitioningInfo* info)
{
	BMutablePartition* partition = Partition();
	uint32 blockSize = partition->BlockSize();
	off_t firstUsable = (off_t)kFirstPartitionBlock * blockSize;
	off_t diskEnd = block_align(partition->Size(), blockSize);

	status_t error = info->SetTo(firstUsable, diskEnd - firstUsable);
	if (error != B_OK)
		return error;

	// exclude existing children
	int32 count = partition->CountChildren();
	for (int32 i = 0; i < count; i++) {
		BMutablePartition* c = partition->ChildAt(i);
		if (c == NULL || c->Offset() < firstUsable)
			continue;
		error = info->ExcludeOccupiedSpace(c->Offset(), c->Size());
		if (error != B_OK)
			return error;
	}

	return B_OK;
}


status_t
ApplePartitionHandle::ValidateCreateChild(off_t* _offset, off_t* _size,
	const char* type, BString* name, const char* parameters)
{
	if (_offset == NULL || _size == NULL || type == NULL)
		return B_BAD_VALUE;

	if (parameters != NULL && parameters[0] != '\0')
		return B_BAD_VALUE;

	// Apple partitions carry no content name at the map level
	if (name != NULL)
		name->Truncate(0);

	BMutablePartition* partition = Partition();
	uint32 blockSize = partition->BlockSize();

	// Append after the end of the last existing child (mirrors the kernel).
	off_t usedEnd = (off_t)kFirstPartitionBlock * blockSize;
	int32 count = partition->CountChildren();
	for (int32 i = 0; i < count; i++) {
		BMutablePartition* c = partition->ChildAt(i);
		if (c == NULL || c->Offset() < usedEnd)
			continue;
		off_t childEnd = c->Offset() + c->Size();
		if (childEnd > usedEnd)
			usedEnd = childEnd;
	}

	off_t start = *_offset;
	if (start < usedEnd)
		start = usedEnd;
	start = block_align_up(start, blockSize);

	off_t diskEnd = block_align(partition->Size(), blockSize);
	if (start >= diskEnd)
		return B_BAD_VALUE;

	off_t maxSize = diskEnd - start;
	off_t size = *_size;
	if (size <= 0 || size > maxSize)
		size = maxSize;
	size = block_align(size, blockSize);
	if (size == 0)
		return B_BAD_VALUE;

	*_offset = start;
	*_size = size;
	return B_OK;
}


status_t
ApplePartitionHandle::CreateChild(off_t offset, off_t size, const char* type,
	const char* name, const char* parameters, BMutablePartition** _child)
{
	if (type == NULL)
		return B_BAD_VALUE;
	if (parameters != NULL && parameters[0] != '\0')
		return B_BAD_VALUE;

	BMutablePartition* partition = Partition();
	uint32 blockSize = partition->BlockSize();

	// offset/size must be block aligned
	if (offset != block_align(offset, blockSize)
		|| size != block_align(size, blockSize))
		return B_BAD_VALUE;

	// must fit within a free space
	BPartitioningInfo info;
	status_t error = GetPartitioningInfo(&info);
	if (error != B_OK)
		return error;

	bool foundSpace = false;
	off_t end = offset + size;
	int32 spacesCount = info.CountPartitionableSpaces();
	for (int32 i = 0; i < spacesCount; i++) {
		off_t spaceOffset, spaceSize;
		if (info.GetPartitionableSpaceAt(i, &spaceOffset, &spaceSize) != B_OK)
			continue;
		if (offset >= spaceOffset && end <= spaceOffset + spaceSize) {
			foundSpace = true;
			break;
		}
	}
	if (!foundSpace)
		return B_BAD_VALUE;

	// child index in the framework == number of existing children (append)
	int32 index = partition->CountChildren();
	BMutablePartition* child;
	// The framework's commit path (KPartitioningSystem::CreateChild) rejects a
	// NULL parameters string with B_BAD_VALUE, so store an empty string.
	error = partition->CreateChild(index, type, name,
		parameters != NULL ? parameters : "", &child);
	if (error != B_OK)
		return error;

	child->SetOffset(offset);
	child->SetSize(size);
	child->SetBlockSize(blockSize);

	if (_child != NULL)
		*_child = child;
	return B_OK;
}


status_t
ApplePartitionHandle::DeleteChild(BMutablePartition* child)
{
	if (child == NULL)
		return B_BAD_VALUE;

	BMutablePartition* partition = Partition();
	return partition->DeleteChild(child);
}


// #pragma mark -


status_t
get_disk_system_add_ons(BList* addOns)
{
	AppleDiskAddOn* addOn = new(nothrow) AppleDiskAddOn;
	if (addOn == NULL)
		return B_NO_MEMORY;

	if (!addOns->AddItem(addOn)) {
		delete addOn;
		return B_NO_MEMORY;
	}

	return B_OK;
}
