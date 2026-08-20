/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * A minimal read-only ISO9660 file system for the boot loader, enough to load
 * the kernel and packages from a live/installer CD. Names come from Rock Ridge
 * "NM" entries (genisoimage -R) so the lower-case Haiku paths survive; the
 * bare ISO9660 identifier is used as a fallback.
 */

#include <boot/partitions.h>
#include <boot/vfs.h>

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

//#define TRACE_ISO9660
#ifdef TRACE_ISO9660
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


namespace ISO9660 {


static const uint32 kBlockSize = 2048;
static const uint32 kVolumeDescriptorStart = 16;	// logical block of the PVD
static const size_t kMaxNameLength = 255;


static inline uint32
read32_lsb(const uint8* p)
{
	return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16)
		| ((uint32)p[3] << 24);
}


class Volume;


//! Extract the entry name: prefer a Rock Ridge "NM" name, else the ISO id.
static void
extract_name(const uint8* record, char* nameBuffer, size_t bufferSize)
{
	uint8 recordLength = record[0];
	uint8 idLength = record[32];

	// default: the ISO9660 file identifier (strip a ";version" suffix)
	size_t out = 0;
	for (uint8 i = 0; i < idLength && out < bufferSize - 1; i++) {
		char c = (char)record[33 + i];
		if (c == ';')
			break;
		nameBuffer[out++] = c;
	}
	nameBuffer[out] = '\0';

	// Rock Ridge: scan the system-use area for "NM" entries
	uint32 suaOffset = 33 + idLength + ((idLength & 1) == 0 ? 1 : 0);
	if (suaOffset >= recordLength)
		return;

	char rrName[kMaxNameLength + 1];
	size_t rrLen = 0;
	bool haveNM = false;
	uint32 p = suaOffset;
	while (p + 4 <= recordLength) {
		uint8 len = record[p + 2];
		if (len < 4 || p + len > recordLength)
			break;
		if (record[p] == 'N' && record[p + 1] == 'M') {
			// [p+4] = flags, name bytes follow at [p+5 .. p+len-1]
			for (uint32 i = p + 5; i < p + len && rrLen < kMaxNameLength; i++)
				rrName[rrLen++] = (char)record[i];
			haveNM = true;
		}
		p += len;
	}
	if (haveNM) {
		rrName[rrLen] = '\0';
		strlcpy(nameBuffer, rrName, bufferSize);
	}
}


class File : public ::Node {
public:
	File(Volume& volume, uint32 extent, uint32 size, const char* name);
	virtual ~File() {}

	virtual ssize_t ReadAt(void* cookie, off_t pos, void* buffer,
		size_t bufferSize);
	virtual ssize_t WriteAt(void* cookie, off_t pos, const void* buffer,
		size_t bufferSize) { return B_NOT_ALLOWED; }

	virtual status_t GetName(char* nameBuffer, size_t bufferSize) const;
	virtual int32 Type() const { return S_IFREG; }
	virtual off_t Size() const { return fSize; }
	virtual ino_t Inode() const { return fExtent; }

private:
	Volume&	fVolume;
	uint32	fExtent;
	uint32	fSize;
	char	fName[kMaxNameLength + 1];
};


class Directory : public ::Directory {
public:
	Directory(Volume& volume, uint32 extent, uint32 size, const char* name);
	virtual ~Directory() {}

	virtual status_t Open(void** _cookie, int mode);
	virtual status_t Close(void* cookie);

	virtual Node* LookupDontTraverse(const char* name);

	virtual status_t GetNextEntry(void* cookie, char* nameBuffer,
		size_t bufferSize);
	virtual status_t GetNextNode(void* cookie, Node** _node);
	virtual status_t Rewind(void* cookie);
	virtual bool IsEmpty();

	virtual status_t GetName(char* nameBuffer, size_t bufferSize) const;
	virtual int32 Type() const { return S_IFDIR; }
	virtual ino_t Inode() const { return fExtent; }

	virtual status_t CreateFile(const char* name, mode_t permissions,
		Node** _node) { return B_NOT_ALLOWED; }

private:
	// Reads the whole directory extent into a freshly allocated buffer.
	uint8*	_ReadContents();
	// Parses the record at *_pos, advancing it; returns false at the end.
	bool	_NextRecord(uint8* contents, uint32* _pos, const uint8** _record);

	Volume&	fVolume;
	uint32	fExtent;
	uint32	fSize;
	char	fName[kMaxNameLength + 1];
};


class Volume {
public:
	Volume(boot::Partition* partition);
	~Volume();

	status_t InitCheck() const { return fStatus; }
	::Directory* Root() { return fRoot; }

	ssize_t ReadBlocks(uint32 block, void* buffer, uint32 count)
	{
		return fPartition->ReadAt(NULL, (off_t)block * kBlockSize, buffer,
			(size_t)count * kBlockSize);
	}

private:
	boot::Partition*	fPartition;
	Directory*			fRoot;
	status_t			fStatus;
};


//	#pragma mark - Volume


Volume::Volume(boot::Partition* partition)
	:
	fPartition(partition),
	fRoot(NULL),
	fStatus(B_ERROR)
{
	uint8* descriptor = (uint8*)malloc(kBlockSize);
	if (descriptor == NULL)
		return;
	if (ReadBlocks(kVolumeDescriptorStart, descriptor, 1)
			!= (ssize_t)kBlockSize) {
		free(descriptor);
		return;
	}

	// primary volume descriptor: type 1, standard identifier "CD001"
	if (descriptor[0] != 0x01 || memcmp(descriptor + 1, "CD001", 5) != 0) {
		free(descriptor);
		return;
	}

	// the root directory record lives at offset 156 of the PVD
	const uint8* root = descriptor + 156;
	uint32 extent = read32_lsb(root + 2);
	uint32 size = read32_lsb(root + 10);
	free(descriptor);

	fRoot = new(std::nothrow) Directory(*this, extent, size, "");
	if (fRoot == NULL)
		return;

	fStatus = B_OK;
}


Volume::~Volume()
{
	if (fRoot != NULL)
		fRoot->Release();
}


//	#pragma mark - File


File::File(Volume& volume, uint32 extent, uint32 size, const char* name)
	:
	fVolume(volume),
	fExtent(extent),
	fSize(size)
{
	strlcpy(fName, name, sizeof(fName));
}


ssize_t
File::ReadAt(void* cookie, off_t pos, void* buffer, size_t bufferSize)
{
	if (pos < 0 || pos >= fSize)
		return 0;
	if (pos + (off_t)bufferSize > fSize)
		bufferSize = fSize - pos;
	if (bufferSize == 0)
		return 0;

	off_t start = (off_t)fExtent * kBlockSize + pos;
	// The extent is contiguous; read whole aligned blocks through a bounce
	// buffer for the head/tail, and directly for the aligned middle.
	uint8* out = (uint8*)buffer;
	size_t done = 0;
	uint8* block = (uint8*)malloc(kBlockSize);
	if (block == NULL)
		return B_NO_MEMORY;

	while (done < bufferSize) {
		off_t at = start + done;
		uint32 blockIndex = (uint32)(at / kBlockSize);
		uint32 inBlock = (uint32)(at % kBlockSize);
		size_t chunk = kBlockSize - inBlock;
		if (chunk > bufferSize - done)
			chunk = bufferSize - done;

		if (inBlock == 0 && chunk == kBlockSize) {
			// aligned full block: read directly
			if (fVolume.ReadBlocks(blockIndex, out + done, 1)
					!= (ssize_t)kBlockSize) {
				free(block);
				return B_ERROR;
			}
		} else {
			if (fVolume.ReadBlocks(blockIndex, block, 1)
					!= (ssize_t)kBlockSize) {
				free(block);
				return B_ERROR;
			}
			memcpy(out + done, block + inBlock, chunk);
		}
		done += chunk;
	}

	free(block);
	return done;
}


status_t
File::GetName(char* nameBuffer, size_t bufferSize) const
{
	strlcpy(nameBuffer, fName, bufferSize);
	return B_OK;
}


//	#pragma mark - Directory


Directory::Directory(Volume& volume, uint32 extent, uint32 size,
	const char* name)
	:
	fVolume(volume),
	fExtent(extent),
	fSize(size)
{
	strlcpy(fName, name, sizeof(fName));
}


uint8*
Directory::_ReadContents()
{
	uint32 blocks = (fSize + kBlockSize - 1) / kBlockSize;
	if (blocks == 0)
		return NULL;
	uint8* buffer = (uint8*)malloc((size_t)blocks * kBlockSize);
	if (buffer == NULL)
		return NULL;
	if (fVolume.ReadBlocks(fExtent, buffer, blocks)
			!= (ssize_t)((size_t)blocks * kBlockSize)) {
		free(buffer);
		return NULL;
	}
	return buffer;
}


bool
Directory::_NextRecord(uint8* contents, uint32* _pos, const uint8** _record)
{
	uint32 pos = *_pos;
	while (pos < fSize) {
		uint8 length = contents[pos];
		if (length == 0) {
			// no more records in this logical block; advance to the next
			uint32 next = (pos / kBlockSize + 1) * kBlockSize;
			if (next <= pos || next >= fSize)
				return false;
			pos = next;
			continue;
		}
		if (pos + length > fSize)
			return false;
		*_record = contents + pos;
		*_pos = pos + length;
		return true;
	}
	return false;
}


Node*
Directory::LookupDontTraverse(const char* name)
{
	if (strcmp(name, ".") == 0) {
		Acquire();
		return this;
	}

	uint8* contents = _ReadContents();
	if (contents == NULL)
		return NULL;

	Node* result = NULL;
	uint32 pos = 0;
	const uint8* record;
	while (_NextRecord(contents, &pos, &record)) {
		uint8 idLength = record[32];
		// skip the "." (0x00) and ".." (0x01) self/parent records
		if (idLength == 1 && (record[33] == 0 || record[33] == 1))
			continue;

		char entryName[kMaxNameLength + 1];
		extract_name(record, entryName, sizeof(entryName));
		if (strcmp(entryName, name) != 0)
			continue;

		uint32 extent = read32_lsb(record + 2);
		uint32 size = read32_lsb(record + 10);
		bool isDir = (record[25] & 0x02) != 0;

		if (isDir)
			result = new(std::nothrow) Directory(fVolume, extent, size, name);
		else
			result = new(std::nothrow) File(fVolume, extent, size, name);
		break;
	}

	free(contents);
	return result;
}


status_t
Directory::Open(void** _cookie, int mode)
{
	// cookie is the current byte offset into the directory data
	uint32* cookie = (uint32*)malloc(sizeof(uint32));
	if (cookie == NULL)
		return B_NO_MEMORY;
	*cookie = 0;
	*_cookie = cookie;
	Acquire();
	return B_OK;
}


status_t
Directory::Close(void* cookie)
{
	free(cookie);
	Release();
	return B_OK;
}


status_t
Directory::GetNextEntry(void* _cookie, char* nameBuffer, size_t bufferSize)
{
	uint32* cookie = (uint32*)_cookie;

	uint8* contents = _ReadContents();
	if (contents == NULL)
		return B_ENTRY_NOT_FOUND;

	status_t status = B_ENTRY_NOT_FOUND;
	uint32 pos = *cookie;
	const uint8* record;
	while (_NextRecord(contents, &pos, &record)) {
		uint8 idLength = record[32];
		if (idLength == 1 && (record[33] == 0 || record[33] == 1))
			continue;
		extract_name(record, nameBuffer, bufferSize);
		status = B_OK;
		break;
	}

	*cookie = pos;
	free(contents);
	return status;
}


status_t
Directory::GetNextNode(void* _cookie, Node** _node)
{
	uint32* cookie = (uint32*)_cookie;

	uint8* contents = _ReadContents();
	if (contents == NULL)
		return B_ENTRY_NOT_FOUND;

	status_t status = B_ENTRY_NOT_FOUND;
	uint32 pos = *cookie;
	const uint8* record;
	while (_NextRecord(contents, &pos, &record)) {
		uint8 idLength = record[32];
		if (idLength == 1 && (record[33] == 0 || record[33] == 1))
			continue;

		char entryName[kMaxNameLength + 1];
		extract_name(record, entryName, sizeof(entryName));
		uint32 extent = read32_lsb(record + 2);
		uint32 size = read32_lsb(record + 10);
		bool isDir = (record[25] & 0x02) != 0;

		if (isDir) {
			*_node = new(std::nothrow) Directory(fVolume, extent, size,
				entryName);
		} else {
			*_node = new(std::nothrow) File(fVolume, extent, size, entryName);
		}
		status = (*_node != NULL) ? B_OK : B_NO_MEMORY;
		break;
	}

	*cookie = pos;
	free(contents);
	return status;
}


status_t
Directory::Rewind(void* cookie)
{
	*(uint32*)cookie = 0;
	return B_OK;
}


bool
Directory::IsEmpty()
{
	uint8* contents = _ReadContents();
	if (contents == NULL)
		return true;

	bool empty = true;
	uint32 pos = 0;
	const uint8* record;
	while (_NextRecord(contents, &pos, &record)) {
		uint8 idLength = record[32];
		if (idLength == 1 && (record[33] == 0 || record[33] == 1))
			continue;
		empty = false;
		break;
	}

	free(contents);
	return empty;
}


status_t
Directory::GetName(char* nameBuffer, size_t bufferSize) const
{
	strlcpy(nameBuffer, fName, bufferSize);
	return B_OK;
}


}	// namespace ISO9660


//	#pragma mark - module interface


static float
iso9660_identify_file_system(boot::Partition* partition)
{
	ISO9660::Volume volume(partition);
	return volume.InitCheck() < B_OK ? 0 : 0.8f;
}


static status_t
iso9660_get_file_system(boot::Partition* partition, ::Directory** _root)
{
	ISO9660::Volume* volume = new(std::nothrow) ISO9660::Volume(partition);
	if (volume == NULL)
		return B_NO_MEMORY;

	if (volume->InitCheck() < B_OK) {
		delete volume;
		return B_ERROR;
	}

	::Directory* root = volume->Root();
	root->Acquire();
	*_root = root;
	return B_OK;
}


file_system_module_info gISO9660FileSystemModule = {
	"file_systems/iso9660/v1",
	"ISO9660 File System",
	iso9660_identify_file_system,
	iso9660_get_file_system
};
