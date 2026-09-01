# Tabby — instructions for coding agents

Tabby is a fork of Haiku for 32-bit PowerPC Power Macs (G3 and G4). Unlike
upstream Haiku, this fork accepts AI-assisted work, under the rules below.
Default branch: `powerpc`; changes arrive as GitHub pull requests.

## Ground rules

- **A human is accountable for every change.** Prepare commits and leave them
  for review. Never push to `powerpc`, force-push, merge, or open a PR unasked.
- **Disclose agent work** with the trailer already in use,
  `Co-Authored-By: <agent and model> <email>` (or `Assisted-by: <tool>:<model>`).
  Never add `Signed-off-by:` for the human.
- **State what was verified and what was not** in every commit message:
  "confirmed on a PowerBook G4 12in (PowerBook6,4)", "dingusppc only",
  "not run on hardware". Silence is not acceptable.
- **Never invent hardware behaviour.** If a register, timing or firmware
  property cannot be read from the machine, a datasheet or a reference driver
  (Linux, X.Org, OpenBSD/NetBSD macppc), say so and stop. The blank iMac G3
  screen came from invented CRT timings.
- **License is MIT.** New files: `Copyright 2026, The Tabby Maintainers.
  Distributed under the terms of the MIT License.` Name the source and license
  of anything ported; GPL code cannot enter MIT or kernel code; firmware blobs
  need written provenance.
- **The tree builds at every commit, in both build trees.** Commit only what
  the task needs; never `generated*` or `build/jam/UserBuildConfig`.

## Scope: PowerPC only

Target: PowerPC 750 (G3) and 74xx (G4), New World Macs, OpenFirmware 3,
big-endian, boot platform `openfirmware`, kernel arch `ppc`. Known to boot:
iMac G3, iBook G3 (PowerBook4,1), iBook G4, PowerBook G4 12in (PowerBook6,4),
Titanium PowerBook G4, Power Mac G4.

Code for other architectures is being removed, not maintained:

- If a task touches x86, x86_64, ARM, RISC-V, m68k, SPARC or MIPS-only code,
  delete it in its own commit or leave it alone. Never fix it.
- Candidates: `<arch>` subdirectories under `src/system/kernel/arch`,
  `src/system/libroot/{os,posix}/arch`, `src/system/{runtime_loader,glue}/arch`,
  `src/system/ldscripts`, `src/add-ons/kernel/cpu`, `headers/*/arch`,
  `src/kits/debug*/arch`, `build/jam/repositories`; the non-OpenFirmware
  `src/system/boot/platform/*`; drivers for hardware no Power Mac G3/G4 has.
- Keep everything `generic` or arch-neutral, the `ppc`/`openfirmware`
  directories, and all host-side code (`src/tools`, `build/scripts`,
  `configure`, host handling in `build/jam`): the build host is x86_64/arm64.
  `B_HOST_IS_LENDIAN` branches are correctness code and stay.
- One subsystem per commit: `git grep` the path, the `ARCH_*`/`__i386__`
  macros and every `SubInclude` first; build both trees after; list removed
  paths in the message. Upstream fixes come in by cherry-pick, never merge.

## Build

Needs Haiku's `buildtools` next to this tree, a case-sensitive filesystem and
the packages in `ReadMe.Compiling.md`. GCC enables AltiVec for `-mcpu=7400`
and AltiVec traps on a G3, so there are two build trees from one source tree:

```sh
mkdir generated.ppc && cd generated.ppc                       # G3 image
../configure --cross-tools-source ../../buildtools --build-cross-tools ppc \
	--distro-compatibility compatible -j8
jam -q -j8 @minimum-raw                                       # haiku-minimum.image

mkdir ../generated.ppc-g4 && cd ../generated.ppc-g4           # G4 image
../configure --distro-compatibility compatible \
	--cross-tools-prefix "$(realpath ../generated.ppc)/cross-tools-ppc/bin/powerpc-apple-haiku-"
printf 'HAIKU_PPC_CPU = 7400 ;\nHAIKU_PPC_TUNE = 7450 ;\n' >> build/BuildConfig
jam -q -j8 @minimum-raw
```

- Flags live in `build/jam/ArchitectureRules` (`HAIKU_PPC_CPU`, `_TUNE`,
  `_NOVEC`). Re-running `configure` rewrites `build/BuildConfig`; re-add the G4
  lines. The G3 image also runs on any G4 and is the fallback.
- **Kernel, boot loader and every kernel add-on stay at `-mcpu=750` in both
  trees.** The kernel saves AltiVec state so cannot use it; the loader runs
  before AltiVec is enabled. `-mtune` is reset with `-mcpu`; a stale G4 tune
  reorders MMIO and has broken drivers.
- `-Werror` is off for ppc. Read the build log for warnings in files you
  touched and fix them.
- Single targets: `jam -q kernel_ppc`, `haiku_loader.openfirmware`,
  `nvidia.accelerant`; `-a` forces a rebuild; from a source directory add
  `-sHAIKU_OUTPUT_DIR=<tree>`. Image contents: `build/jam/images/definitions/minimum`
  (`ppc @{ }@` blocks); version: `src/apps/aboutsystem/TabbyVersion.h`.
- Rough edge: `minimum` copies NetSurf from the absolute path
  `/work/netsurf-build/nspkg/apps`. Guard such steps on the path existing.

## Test before you build an image

An image build is the slowest step, so failures must happen earlier. Cheapest
first; a passing later step does not excuse a skipped earlier one.

1. **Compile touched targets in both trees.** Code that links at 750 has
   failed at 7400 before (`set_tabby_wallpaper` and `libsupc++`).
2. **Host-side tests, no image, no emulator.** `BuildPlatformTest` /
   `BuildPlatformMain` (`build/jam/TestsRules`) for anything that compiles on
   the host: byte-order helpers, PLL and mode-timing arithmetic, partition-map
   and on-disk parsing. Keep such logic in units with no kernel or MMIO
   dependency so tests can include them. BFS: `jam -q bfs_shell`, then its
   `checkfs` on a fresh image and on the same image after a boot.
3. **CppUnit target tests** (`UnitTest`/`UnitTestLib`, under `src/tests/`):
   `jam -q unittests`; the `test-raw` profile plus `src/tests/TestsBootscript`
   run them from an image.
4. **Emulator boot.** dingusppc is the test bench (emulated G3, Rage Pro,
   `output-device=scca` for the debug console on serial). QEMU `g3beige`/`mac99`
   run OpenBIOS, not Apple OF: smoke test only. Pass = serial log shows
   "Welcome to kernel debugger output" and no `kdebug>` (see
   `src/tests/qemu-boot-test`). No emulator has a G4 or AltiVec; a G4 image is
   hardware-only, and `HAIKU_PPC_NOVEC = 1` isolates AltiVec from G4 codegen.
   <!-- maintainer: paste the dingusppc command line and config here -->
5. **Real hardware.** Drivers, modesetting, PMU, sleep, WiFi and storage count
   as verified only on a named machine and device (`PowerBook6,4`, `10de:0329`).

Every new function with testable logic gets a test in the same commit; a bug
fix gets a regression test or one sentence on why the harness cannot. Split
driver code into testable logic and a thin MMIO layer. Never report a test you
did not run. Done means: both trees build, no new warnings, tests pass, the G3
image boots in the emulator without KDL, hardware status stated.

## PowerPC correctness, already paid for

- Registers and on-disk/wire formats have fixed byte order: use `ByteOrder.h`
  macros, never native stores into a framebuffer or descriptor. `B_RGB16` and
  `B_RGB32` are little-endian. A driver that "works on x86" dereferences MMIO
  raw; check every access.
- PowerPC does not order cache-inhibited accesses: index/data register pairs
  need `eieio` + `sync` between them (`ATI_IO_BARRIER`).
- AltiVec is G4 userland only. Verify by `powerpc-apple-haiku-objdump -d`, not
  by flags. Do not rely on the optimizer to remove a call; link what is needed.
- Apple cards carry FCode ROMs, not a BIOS: inherit the firmware's mode and
  clock, never pick timings from a VESA table.
- No 3D acceleration and a 500 MHz G3 floor: keep 2D on accelerant hooks,
  avoid per-pixel work in app_server, measure on hardware before and after.
- Most laptops have no serial port. Keep the boot-splash "Loading <driver>"
  line (`legacy_drivers.cpp`); rate-limit per-page or per-packet `dprintf`. A
  PMU machine may have no battery: return `B_DEV_NO_MEDIA`, never block.

## Style and commits

- Haiku coding guidelines: <https://www.haiku-os.org/development/coding-guidelines/>
  (tabs, 100 columns, `fMember`/`kConstant`/`sStatic`/`gGlobal`/`_Private`,
  `int32`/`status_t`/`NULL`). `python3 src/tools/checkstyle/checkstyle.py <files>`
  flags common violations. Comments say why; Tabby-specific build decisions in
  Jam files are marked with a leading ★.
- Subject `area: what changed` (`ppc`, `kernel/ppc`, `nvidia/ppc`, `ati`,
  `bfs`, `bwi`, `Tabby`, or the component); body: symptom, cause, fix,
  verification with hardware named. One logical change per commit.
- Issues are disabled on the repository; PRs target `powerpc`.

## Where the PowerPC code lives

`src/system/boot/platform/openfirmware/` (loader), `src/system/kernel/arch/ppc/`
(kernel), `src/add-ons/kernel/interrupt_controllers/openpic/`,
`src/add-ons/kernel/{busses/ata,drivers/audio}/macio/`,
`src/add-ons/kernel/drivers/input/{adb,usb_hid}/` (ADB/PMU, Geyser trackpad),
`src/add-ons/kernel/drivers/graphics/{nvidia,ati}/` + `src/add-ons/accelerants/{nvidia,ati}/`,
`src/add-ons/kernel/drivers/network/ether/gem/` (GMAC),
`src/add-ons/kernel/drivers/network/wlan/broadcom43xx/` (bwi, with
`src/libs/compat/freebsd_wlan/`), `src/add-ons/kernel/partitioning_systems/apple/`
+ `src/add-ons/disk_systems/apple/` (Apple Partition Map),
`src/bin/{makebootable/platform/openfirmware,tabby_install}/`, `src/libs/mesa/`,
`data/firmware/broadcom43xx/`, `data/wifi/`, `data/system/boot/first_login/`,
`docs/develop/kernel/arch/ppc/`.
