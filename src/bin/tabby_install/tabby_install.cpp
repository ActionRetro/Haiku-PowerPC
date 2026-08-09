/*
 * tabby_install - headless installer/validation tool for the Tabby ppc live CD.
 *
 * Exercises the (previously untested) apple partition write-support and, in
 * later phases, BFS format + system copy + makebootable, so the install path
 * can be debugged from the serial log without driving the DriveSetup/Installer
 * GUI. Run automatically by a gated launch job on the test ISO.
 *
 * Phase A (this file): partition the target disk with an Apple Partition Map
 * containing a small Apple_HFS loader partition + a Haiku_BFS system partition,
 * then read the layout back to confirm.
 */

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskSystem.h>
#include <Partition.h>
#include <PartitioningInfo.h>
#include <String.h>

#include <stdio.h>
#include <string.h>

#define LOG(fmt, ...) do { printf("TABBY_INSTALL: " fmt "\n", ##__VA_ARGS__); \
	fflush(stdout); } while (0)

static const off_t kMB = 1024LL * 1024LL;
static const off_t kHFSSize = 16 * kMB;	// loader partition (generous)




static int
partition_disk(BDiskDevice& device, const char* kAPM)
{
	status_t st = device.PrepareModifications();
	if (st != B_OK) { LOG("PrepareModifications: %s", strerror(st)); return 1; }

	// Erase any existing partitions first. Re-running setup on a disk that is
	// already partitioned (or left in a corrupt/half-written state by a previous
	// failed run) must start from a clean slate; Initialize() alone does not
	// reliably remove pre-existing children.
	for (int32 i = device.CountChildren() - 1; i >= 0; i--) {
		BPartition* c = device.ChildAt(i);
		if (c == NULL)
			continue;
		if (device.CanDeleteChild(i)) {
			status_t del = device.DeleteChild(i);
			LOG("erase child %d: %s", (int)i,
				del == B_OK ? "deleted" : strerror(del));
		} else {
			LOG("erase child %d: not deletable", (int)i);
		}
	}

	// Initialize the whole device as an Apple Partition Map container.
	BString name("");
	st = device.ValidateInitialize(kAPM, &name, NULL);
	if (st != B_OK) { LOG("ValidateInitialize(APM): %s", strerror(st)); return 1; }
	st = device.Initialize(kAPM, name.String(), NULL);
	if (st != B_OK) { LOG("Initialize(APM): %s", strerror(st)); return 1; }
	LOG("APM container initialized");

	// --- Apple_HFS loader child ---
	BPartitioningInfo info;
	st = device.GetPartitioningInfo(&info);
	if (st != B_OK) { LOG("GetPartitioningInfo(1): %s", strerror(st)); return 1; }
	LOG("partitionable spaces after init: %d",
		(int)info.CountPartitionableSpaces());
	off_t offset = 0, size = 0;
	st = info.GetPartitionableSpaceAt(0, &offset, &size);
	if (st != B_OK) { LOG("GetPartitionableSpaceAt(0): %s", strerror(st)); return 1; }
	LOG("free space #0: offset=%lld size=%lld", (long long)offset, (long long)size);

	off_t hfsOffset = offset, hfsSize = kHFSSize;
	BString hfsName("loader");
	st = device.ValidateCreateChild(&hfsOffset, &hfsSize, "Apple_HFS", &hfsName,
		"");
	if (st != B_OK) { LOG("ValidateCreateChild(HFS): %s", strerror(st)); return 1; }
	LOG("HFS validated: offset=%lld size=%lld name='%s'",
		(long long)hfsOffset, (long long)hfsSize, hfsName.String());
	st = device.CreateChild(hfsOffset, hfsSize, "Apple_HFS", hfsName.String(),
		"");
	if (st != B_OK) { LOG("CreateChild(HFS): %s", strerror(st)); return 1; }
	LOG("Apple_HFS loader partition created");

	// --- Haiku_BFS system child (largest remaining space) ---
	st = device.GetPartitioningInfo(&info);
	if (st != B_OK) { LOG("GetPartitioningInfo(2): %s", strerror(st)); return 1; }
	int32 spaces = info.CountPartitionableSpaces();
	LOG("partitionable spaces after HFS: %d", (int)spaces);
	off_t bestOffset = 0, bestSize = 0;
	for (int32 i = 0; i < spaces; i++) {
		off_t o = 0, s = 0;
		if (info.GetPartitionableSpaceAt(i, &o, &s) == B_OK) {
			LOG("  space #%d: offset=%lld size=%lld", (int)i,
				(long long)o, (long long)s);
			if (s > bestSize) { bestSize = s; bestOffset = o; }
		}
	}
	if (bestSize <= 0) { LOG("no space left for BFS"); return 1; }

	off_t bfsOffset = bestOffset, bfsSize = bestSize;
	BString bfsName("Tabby");
	st = device.ValidateCreateChild(&bfsOffset, &bfsSize, "Haiku_BFS", &bfsName,
		"");
	if (st != B_OK) { LOG("ValidateCreateChild(BFS): %s", strerror(st)); return 1; }
	LOG("BFS validated: offset=%lld size=%lld name='%s'",
		(long long)bfsOffset, (long long)bfsSize, bfsName.String());
	st = device.CreateChild(bfsOffset, bfsSize, "Haiku_BFS", bfsName.String(),
		"");
	if (st != B_OK) { LOG("CreateChild(BFS): %s", strerror(st)); return 1; }
	LOG("Haiku_BFS system partition created");

	st = device.CommitModifications();
	if (st != B_OK) { LOG("CommitModifications: %s", strerror(st)); return 1; }
	LOG("=== PARTITIONING COMMITTED OK ===");
	return 0;
}


static int
format_bfs(BDiskDeviceRoster& roster, const char* targetPath)
{
	LOG("=== phase B: format Haiku_BFS ===");
	BDiskDevice device;
	status_t st = roster.GetDeviceForPath(targetPath, &device);
	if (st != B_OK) { LOG("GetDeviceForPath: %s", strerror(st)); return 1; }

	st = device.PrepareModifications();
	if (st != B_OK) { LOG("PrepareModifications: %s", strerror(st)); return 1; }

	// find the Haiku_BFS child in the (shadow) tree
	BPartition* bfs = NULL;
	int32 count = device.CountChildren();
	for (int32 i = 0; i < count; i++) {
		BPartition* c = device.ChildAt(i);
		if (c != NULL && c->Type() != NULL
				&& strcmp(c->Type(), "Haiku_BFS") == 0) {
			bfs = c;
			break;
		}
	}
	if (bfs == NULL) { LOG("no Haiku_BFS child found"); return 1; }
	LOG("Haiku_BFS child: offset=%lld size=%lld canInit=%d",
		(long long)bfs->Offset(), (long long)bfs->Size(),
		bfs->CanInitialize("Be File System"));

	BString name("Tabby");
	st = bfs->ValidateInitialize("Be File System", &name, NULL);
	if (st != B_OK) { LOG("ValidateInitialize(bfs): %s", strerror(st)); return 1; }
	LOG("bfs validated, name='%s'", name.String());

	st = bfs->Initialize("Be File System", name.String(), NULL);
	if (st != B_OK) { LOG("Initialize(bfs): %s", strerror(st)); return 1; }
	LOG("bfs Initialize ok");

	st = device.CommitModifications();
	if (st != B_OK) { LOG("CommitModifications(bfs): %s", strerror(st)); return 1; }
	LOG("=== BFS FORMAT COMMITTED OK ===");
	return 0;
}


int
main(int argc, char** argv)
{
	const char* targetPath = argc > 1 ? argv[1] : "/dev/disk/ata/0/master/raw";
	LOG("=== Tabby headless install (phase A: partition) -> %s ===", targetPath);

	BDiskDeviceRoster roster;

	// Enumerate registered disk systems; find the Apple partitioning system.
	BString appleName;
	{
		BDiskSystem ds;
		int32 cookie = 0;
		while (roster.GetNextDiskSystem(&ds) == B_OK) {
			LOG("disksystem: name='%s' short='%s' pretty='%s' part=%d init=%d "
				"createChild=%d", ds.Name(), ds.ShortName(), ds.PrettyName(),
				ds.IsPartitioningSystem(), ds.SupportsInitializing(),
				ds.SupportsCreatingChild());
			if (ds.IsPartitioningSystem()
					&& strcmp(ds.ShortName(), "apple") == 0)
				appleName = ds.PrettyName();
			cookie++;
		}
	}
	if (appleName.IsEmpty()) {
		LOG("!! Apple partitioning system not found among disk systems");
		return 1;
	}
	LOG("using apple disk system pretty name = '%s'", appleName.String());

	BDiskDevice device;
	status_t st = roster.GetDeviceForPath(targetPath, &device);
	if (st != B_OK) { LOG("GetDeviceForPath('%s'): %s", targetPath, strerror(st));
		return 1; }
	LOG("target device size=%lld bytes, read-only=%d",
		(long long)device.Size(), device.IsReadOnly());

	int rc = partition_disk(device, appleName.String());

	// Read the resulting layout back for confirmation.
	BDiskDevice check;
	if (roster.GetDeviceForPath(targetPath, &check) == B_OK) {
		LOG("--- resulting layout ---");
		LOG("device content type: %s", check.ContentType() ? check.ContentType()
			: "(none)");
		int32 count = check.CountChildren();
		LOG("children: %d", (int)count);
		for (int32 i = 0; i < count; i++) {
			BPartition* c = check.ChildAt(i);
			if (c == NULL) continue;
			BString cname = c->ContentName();
			if (cname.IsEmpty() && c->Name() != NULL)
				cname = c->Name();
			LOG("  child %d: type='%s' name='%s' offset=%lld size=%lld", (int)i,
				c->Type() ? c->Type() : "?", cname.String(),
				(long long)c->Offset(), (long long)c->Size());
		}
	}

	LOG("=== phase A %s ===", rc == 0 ? "SUCCESS" : "FAILED");

	if (rc == 0)
		rc = format_bfs(roster, targetPath);

	LOG("=== install %s ===", rc == 0 ? "SUCCESS" : "FAILED");
	return rc;
}
