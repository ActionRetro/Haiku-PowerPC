/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * makebootable (OpenFirmware / PowerPC Macs)
 *
 * PowerPC Macs boot via Open Firmware, which loads the ELF boot loader
 * (haikuloader.elf) from a small HFS "loader" partition. There is no boot
 * sector to write (unlike x86); instead the target disk must carry a copy of
 * that HFS loader partition.
 *
 * Rather than generate a fresh HFS filesystem (Haiku has no HFS write support),
 * we copy a ready-made HFS loader image onto the target disk's HFS loader
 * partition (which the Installer/DriveSetup created via the Apple partitioning
 * add-on). The source is, in order of preference:
 *   1. the loader partition of the currently running system (installed->installed);
 *   2. the prebuilt loader blob shipped on the install medium
 *      (/boot/tabby-loader.hfs) - used when booting a live/installer CD, which
 *      is a CHRP disc with no Apple_HFS partition to clone.
 *
 * Usage: makebootable [ --dry-run ] <target-directory> ...
 *   <target-directory> is the mounted BFS system volume being installed to.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef HAIKU_TARGET_PLATFORM_HAIKU
#	include <Directory.h>
#	include <DiskDevice.h>
#	include <DiskDeviceRoster.h>
#	include <DiskDeviceVisitor.h>
#	include <Entry.h>
#	include <Partition.h>
#	include <Path.h>
#	include <Volume.h>
#endif


static const char* kCommandName = "makebootable";
static const char* kAppleTypeHFS = "Apple_HFS";
static const off_t kCopyBufferSize = 1024 * 1024;


static const char* kUsage =
"Usage: %s [ options ] <directory> ...\n"
"\n"
"Makes the disk holding each given (mounted) Haiku volume bootable on a\n"
"PowerPC Mac by copying the running system's Open Firmware HFS loader\n"
"partition onto the target disk's HFS loader partition.\n"
"\n"
"Options:\n"
"  -h, --help    - Print this help text and exit.\n"
"  --dry-run     - Do everything but actually write to the target.\n";


static void
print_usage_and_exit(bool error)
{
	fprintf(error ? stderr : stdout, kUsage, kCommandName);
	exit(error ? 1 : 0);
}


#ifdef HAIKU_TARGET_PLATFORM_HAIKU

//! Finds the first descendant partition whose (Apple) type matches.
class TypeFinder : public BDiskDeviceVisitor {
public:
	TypeFinder(const char* type)
		:
		fType(type)
	{
	}

	virtual bool Visit(BDiskDevice* device)
	{
		return _Check(device);
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		return _Check(partition);
	}

private:
	bool _Check(BPartition* partition)
	{
		const char* type = partition->Type();
		return type != NULL && strcmp(type, fType) == 0;
	}

	const char* fType;
};


//! Locates the Apple_HFS loader partition on the disk holding \a mountPoint.
static status_t
find_hfs_loader_partition(BDiskDeviceRoster& roster, const char* mountPoint,
	BDiskDevice& device, BPartition** _hfs)
{
	BPartition* mounted;
	status_t status = roster.FindPartitionByMountPoint(mountPoint, &device,
		&mounted);
	if (status != B_OK)
		return status;

	TypeFinder finder(kAppleTypeHFS);
	BPartition* hfs = device.VisitEachDescendant(&finder);
	if (hfs == NULL)
		return B_ENTRY_NOT_FOUND;

	*_hfs = hfs;
	return B_OK;
}


//! True if the first bytes of \a target already match \a source, i.e. the
//! loader is already written there (makes the tool safe to run repeatedly).
static bool
loader_already_present(const char* source, const char* target, off_t size)
{
	size_t check = 64 * 1024;
	if (size < (off_t)check)
		check = (size_t)size;
	if (check == 0)
		return false;

	int a = open(source, O_RDONLY);
	if (a < 0)
		return false;
	int b = open(target, O_RDONLY);
	if (b < 0) {
		close(a);
		return false;
	}

	uint8* bufA = (uint8*)malloc(check);
	uint8* bufB = (uint8*)malloc(check);
	bool same = false;
	if (bufA != NULL && bufB != NULL) {
		ssize_t ra = read(a, bufA, check);
		ssize_t rb = read(b, bufB, check);
		same = ra == (ssize_t)check && rb == (ssize_t)check
			&& memcmp(bufA, bufB, check) == 0;
	}
	free(bufA);
	free(bufB);
	close(a);
	close(b);
	return same;
}


//! Copies \a size bytes from the start of \a from onto \a to.
static status_t
clone_partition(const char* from, const char* to, off_t size, bool dryRun)
{
	if (dryRun) {
		printf("  (dry run) would copy %lld bytes\n    from %s\n    to   %s\n",
			(long long)size, from, to);
		return B_OK;
	}

	int in = open(from, O_RDONLY);
	if (in < 0) {
		fprintf(stderr, "Error: cannot open source %s: %s\n", from,
			strerror(errno));
		return errno;
	}
	int out = open(to, O_WRONLY);
	if (out < 0) {
		fprintf(stderr, "Error: cannot open target %s: %s\n", to,
			strerror(errno));
		close(in);
		return errno;
	}

	uint8* buffer = (uint8*)malloc(kCopyBufferSize);
	if (buffer == NULL) {
		close(in);
		close(out);
		return B_NO_MEMORY;
	}

	status_t result = B_OK;
	off_t remaining = size;
	while (remaining > 0) {
		size_t chunk = remaining < kCopyBufferSize
			? (size_t)remaining : (size_t)kCopyBufferSize;
		ssize_t read = pread(in, buffer, chunk, size - remaining);
		if (read <= 0) {
			result = read < 0 ? errno : B_IO_ERROR;
			fprintf(stderr, "Error: read from %s at offset %lld failed "
				"(got %ld): %s\n", from, (long long)(size - remaining),
				(long)read, strerror(result));
			break;
		}
		ssize_t written = pwrite(out, buffer, read, size - remaining);
		if (written != read) {
			result = written < 0 ? errno : B_IO_ERROR;
			fprintf(stderr, "Error: write to %s at offset %lld failed "
				"(wrote %ld of %ld): %s\n", to, (long long)(size - remaining),
				(long)written, (long)read, strerror(result));
			break;
		}
		remaining -= read;
	}

	free(buffer);
	fsync(out);
	close(out);
	close(in);
	return result;
}


static status_t
make_bootable(const char* directory, bool dryRun)
{
	// resolve the target directory to the mount point of its volume
	struct stat st;
	if (stat(directory, &st) != 0) {
		fprintf(stderr, "Error: cannot stat \"%s\": %s\n", directory,
			strerror(errno));
		return errno;
	}
	BDiskDeviceRoster roster;

	// The target may be given as a mounted volume's directory (the Installer
	// passes the install mount point) or as a raw disk device path (the Tabby
	// "Set up disk" step passes the disk directly, avoiding any mount).
	BDiskDevice targetDisk;
	BPartition* targetHFS = NULL;
	if (S_ISDIR(st.st_mode)) {
		BVolume volume(st.st_dev);
		BDirectory rootDir;
		BEntry rootEntry;
		BPath targetMountPoint;
		if (volume.GetRootDirectory(&rootDir) != B_OK
			|| rootDir.GetEntry(&rootEntry) != B_OK
			|| rootEntry.GetPath(&targetMountPoint) != B_OK) {
			fprintf(stderr, "Error: cannot determine mount point of \"%s\".\n",
				directory);
			return B_ERROR;
		}
		if (find_hfs_loader_partition(roster, targetMountPoint.Path(),
				targetDisk, &targetHFS) != B_OK) {
			fprintf(stderr, "Error: the target disk has no \"%s\" loader "
				"partition. Create a small (>= 16 MB) \"%s\" partition on the "
				"target disk before the Haiku partition, then try again.\n",
				kAppleTypeHFS, kAppleTypeHFS);
			return B_ENTRY_NOT_FOUND;
		}
	} else {
		if (roster.GetDeviceForPath(directory, &targetDisk) != B_OK) {
			fprintf(stderr, "Error: cannot open disk \"%s\".\n", directory);
			return B_ERROR;
		}
		TypeFinder finder(kAppleTypeHFS);
		targetHFS = targetDisk.VisitEachDescendant(&finder);
		if (targetHFS == NULL) {
			fprintf(stderr, "Error: the disk \"%s\" has no \"%s\" loader "
				"partition.\n", directory, kAppleTypeHFS);
			return B_ENTRY_NOT_FOUND;
		}
	}

	// Determine the loader source. Prefer the running system's own HFS loader
	// partition (installed->installed). On a live/installer CD there is no such
	// partition (the loader is just a file on a CHRP disc), so fall back to the
	// prebuilt HFS loader blob shipped on the medium.
	BPath sourcePath;
	off_t sourceSize = 0;
	BDiskDevice bootDisk;
	BPartition* sourceHFS;
	if (find_hfs_loader_partition(roster, "/boot", bootDisk, &sourceHFS)
			== B_OK) {
		if (sourceHFS->GetPath(&sourcePath) != B_OK) {
			fprintf(stderr, "Error: cannot resolve the source loader "
				"partition path.\n");
			return B_ERROR;
		}
		sourceSize = sourceHFS->Size();
	} else {
		static const char* const kLoaderBlob = "/boot/tabby-loader.hfs";
		struct stat blobStat;
		if (stat(kLoaderBlob, &blobStat) != 0) {
			fprintf(stderr, "Error: no HFS loader partition on the boot disk "
				"and no loader blob at %s.\n", kLoaderBlob);
			return B_ENTRY_NOT_FOUND;
		}
		sourcePath.SetTo(kLoaderBlob);
		sourceSize = blobStat.st_size;
	}

	BPath targetPath;
	if (targetHFS->GetPath(&targetPath) != B_OK) {
		fprintf(stderr, "Error: cannot resolve the target loader partition "
			"path.\n");
		return B_ERROR;
	}

	if (strcmp(sourcePath.Path(), targetPath.Path()) == 0) {
		// installing onto the disk we booted from; loader already present
		printf("Loader partition is the boot partition itself; nothing to "
			"do.\n");
		return B_OK;
	}

	if (targetHFS->Size() < sourceSize) {
		fprintf(stderr, "Error: target loader partition (%lld bytes) is "
			"smaller than the loader (%lld bytes).\n",
			(long long)targetHFS->Size(), (long long)sourceSize);
		return B_BAD_VALUE;
	}

	// Nothing to do if the loader is already there (e.g. it was written
	// when the disk was set up, and the Installer runs this again).
	if (!dryRun && loader_already_present(sourcePath.Path(),
			targetPath.Path(), sourceSize)) {
		printf("Loader already present on the target; nothing to do.\n");
		return B_OK;
	}

	printf("Installing Open Firmware loader: writing %lld bytes\n",
		(long long)sourceSize);
	printf("  from %s\n  to   %s\n", sourcePath.Path(), targetPath.Path());

	return clone_partition(sourcePath.Path(), targetPath.Path(), sourceSize,
		dryRun);
}

#endif // HAIKU_TARGET_PLATFORM_HAIKU


int
main(int argc, const char* const* argv)
{
#ifndef HAIKU_TARGET_PLATFORM_HAIKU
	// On PowerPC Macs, making a disk bootable means copying the Open Firmware
	// HFS loader partition, which only makes sense on a running Haiku system.
	// The host build is a no-op (the ppc image build does not run this tool).
	(void)argc;
	(void)argv;
	return 0;
#else
	bool dryRun = false;
	const char** directories = new const char*[argc];
	int count = 0;

	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
			print_usage_and_exit(false);
		else if (strcmp(arg, "--dry-run") == 0)
			dryRun = true;
		else if (arg[0] == '-')
			print_usage_and_exit(true);
		else
			directories[count++] = arg;
	}

	if (count == 0)
		print_usage_and_exit(true);

	for (int i = 0; i < count; i++) {
		status_t status = make_bootable(directories[i], dryRun);
		if (status != B_OK) {
			delete[] directories;
			return 1;
		}
	}

	delete[] directories;
	return 0;
#endif // HAIKU_TARGET_PLATFORM_HAIKU
}
