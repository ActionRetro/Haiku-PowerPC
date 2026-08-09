/*
 * Tabby ppc: userland disk-system add-on for the Apple Partition Map, so
 * DriveSetup/Installer can initialize + partition Apple maps (the kernel
 * partitioning_systems/apple module does the actual on-disk writing).
 * Distributed under the terms of the MIT License.
 */
#ifndef _APPLE_DISK_ADD_ON_H
#define _APPLE_DISK_ADD_ON_H

#include <DiskSystemAddOn.h>


class AppleDiskAddOn : public BDiskSystemAddOn {
public:
								AppleDiskAddOn();
	virtual						~AppleDiskAddOn();

	virtual	status_t			CreatePartitionHandle(
									BMutablePartition* partition,
									BPartitionHandle** handle);

	virtual	bool				CanInitialize(
									const BMutablePartition* partition);
	virtual	status_t			ValidateInitialize(
									const BMutablePartition* partition,
									BString* name, const char* parameters);
	virtual	status_t			Initialize(BMutablePartition* partition,
									const char* name, const char* parameters,
									BPartitionHandle** handle);
	virtual	status_t			GetTypeForContentType(const char* contentType,
									BString* type);
};


class ApplePartitionHandle : public BPartitionHandle {
public:
								ApplePartitionHandle(
									BMutablePartition* partition);
	virtual						~ApplePartitionHandle();

			status_t			Init();

	virtual	uint32				SupportedOperations(uint32 mask);
	virtual	uint32				SupportedChildOperations(
									const BMutablePartition* child,
									uint32 mask);

	virtual	status_t			GetNextSupportedType(
									const BMutablePartition* child,
									int32* cookie, BString* type);
	virtual	status_t			GetPartitioningInfo(BPartitioningInfo* info);

	virtual	status_t			ValidateCreateChild(off_t* offset,
									off_t* size, const char* type,
									BString* name, const char* parameters);
	virtual	status_t			CreateChild(off_t offset, off_t size,
									const char* type, const char* name,
									const char* parameters,
									BMutablePartition** child);
	virtual	status_t			DeleteChild(BMutablePartition* child);
};


#endif	// _APPLE_DISK_ADD_ON_H
