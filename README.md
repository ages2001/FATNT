# FATNT - FAT12/16/32 File System Driver for Windows NT 3.1 - 4.0

An installable file system (IFS) driver that mounts FAT12, FAT16 and FAT32
volumes read/write on Windows NT 3.1, 3.5, 3.51 and 4.0. Built on the same
design as [EXFATNT](https://github.com/ages2001/EXFATNT): one source tree,
metadata cached through the Cache Manager, a run-list allocation layer. It is a
from-scratch, single-source, auditable FAT implementation that can serve as a
boot file system all the way down to NT 3.1.

> **Windows 2000 and later already include FAT support** (their own
> `fastfat.sys`), so FATNT is not needed there - use Microsoft's in-box driver.
> FATNT's reason to exist is NT 3.1 - 4.0. The sources are kept portable and
> will build for 2000/XP with the WDK, but that is optional and only of interest
> if you specifically want to audit or replace the in-box FAT driver.

## Status

**Version 1.3** - read/write, boot and paging file on Windows NT 3.1 - 4.0,
FAT32 with drive letters on all of them.
See [changelog.txt](changelog.txt) for what changed.

Files and directories can be created, written, extended, truncated, renamed,
moved and deleted; attributes, times and the volume label can be changed.
Memory-mapped writes, the fast I/O write path and write-through work. Writing
can be turned off in the registry (`EnableWriteSupport`).

**Boots Windows NT 3.1, 3.51 and 4.0.** The one `fatnt.sys` has been run as the
boot/system file system, standing in for `Fastfat.sys`, on real installs of each
(x86, under QEMU): NT 3.1 SP0, NT 3.51 SP5 and NT 4.0 SP6. On each it mounts the
FAT16 system volume, passes the boot-time check, loads the boot and system
drivers, reaches the graphical logon and desktop, and serves ordinary file
read/write from a command prompt - a complete boot driven entirely by FATNT.
**A paging file can live on a FAT volume.** The Memory Manager creates and sizes
`pagefile.sys` there, grows it under memory pressure, and an existing paging
file is opened as it is, never truncated - see *Windows NT 3.1 - 4.0 notes*.

A FAT, read/write, boot-capable IFS must get a number of version-specific
details right that only surface on the real kernel; the ones that mattered are
called out under *Windows NT 3.1 - 4.0 notes* below.

Three programs come with it: `fatinst.exe` installs the driver, `fatfmt.exe`
formats a volume and `fatchk.exe` checks and repairs one. There is no
boot-time (`autochk`-style) check yet.

## Features

- **FAT12, FAT16 and FAT32**, type chosen by cluster count per the Microsoft
  specification
  - 12-bit FAT entries handled across byte-pair boundaries
  - Second reserved entry's clean-shutdown bit (FAT16/32) set and cleared like
    Windows; a volume left dirty stays marked for the check tool
  - All FAT copies kept in step; the FAT32 FSInfo free count is maintained
  - 512 to 4096-byte sectors, every power-of-two cluster size from 512 bytes
    to 64 KB (a 4 GB FAT16 volume uses 64 KB clusters), files up to 4 GB - 1
- **8.3 and long (VFAT) names**
  - Long-name entry sets read and written, with checksums and `~N` numeric
    tails generated for lossy or long names
  - Lower-case 8.3 names stored with the NT case bits (no long entry needed)
  - Files open by either their long or short name
- **A full IFS, not a converter**
  - Cache Manager integration for file data and metadata: cached reads and
    writes, fast I/O, memory-mapped files and executables, lazy writing
  - FAT keeps no valid-data length on disk, so clusters allocated past the
    last byte written are zeroed before the larger size is recorded - no
    stale data shows through after a remount
  - Non-cached I/O and direct volume (DASD) access
  - Directory listing with wildcards; subdirectories report `.` and `..`,
    the root does not
  - Set information: basic, disposition, rename/move (with replace),
    allocation, end of file
  - Byte-range locks, share access, delete-on-close (directory change
    notification: live on a single-version build; on the universal NT 3.1 - 4.0
    binary the request is held pending rather than delivered - see the notes)
  - `FSCTL_IS_VOLUME_DIRTY`, `FSCTL_MARK_VOLUME_DIRTY`, flush of files and of
    the whole volume
  - Volume lock/unlock/dismount, media change verification, removable and
    hot-plug write-back; a volume formatted over by another file system while
    files were still open on it is let go, so the new file system mounts
  - Paging files: created, sized and grown by the Memory Manager; a paging file
    in use cannot be deleted or renamed
  - **Drive letters for FAT32 partitions on NT 3.1 - 4.0.** Those systems give
    letters only to partition types `01`, `04`, `06` and `07`; FATNT gives the
    FAT32 types `0B` and `0C` and the LBA FAT16 type `0E` letters itself when
    their boot sector is FAT (see *FAT32 partitions on NT 3.1 - 4.0*)
- **Single source tree**, built with `FAT_NT4` (and `FAT_CROSS` for the GCC
  cross build) into **one** `fatnt.sys` that loads and boots on NT 3.1, 3.5,
  3.51 and 4.0 (x86) - no per-version binary. It binds the executive-resource
  API to the running kernel at load time (see *Windows NT 3.1 - 4.0 notes*), so
  the same image uses each NT's own native resource functions. The same sources
  still compile with the WDK for 2000/XP, but that is optional (see the note at
  the top).

## Architecture

```
            Application
                 v
          I/O Manager ----------------- Cache Manager / Memory Manager
                 v                              ^ paging I/O
          fatnt.sys  <- This driver ------------'
                 v
      Disk class driver (disk.sys) -> ScsiPort / AHCINT / ATAPI ...
```

Each directory and the FAT are cached through one internal stream. On
FAT12/16 the root directory is a fixed region addressed linearly; on FAT32
it is an ordinary cluster chain. File data is cached in the file's own
section. Requests run synchronously in the caller's thread; a close that
arrives while the volume is busy is finished by a worker thread.

### Key components

- **src\fat.h** - target switch (`FAT_NT4`), VCB/FCB/CCB, prototypes
- **src\fatdisk.h** - BPB and directory entry on-disk structures
- **src\fatsup.c** - BPB validation and geometry, 8.3 + long name handling,
  directory entry decode/encode, DOS timestamps (no kernel calls; shared with
  the tools and the test harness)
- **src\fatinit.c** - `DriverEntry`, registration, settings
- **src\fatres.c** - run-time binding of the executive-resource API to the
  running kernel (Lite on NT 3.51+, legacy on NT 3.1)
- **src\fatdisp.c** - IRP dispatch, exception handling, media-change retry
- **src\fatfsctl.c** - mount (BPB parse, FAT type, root setup, dirty/free/label),
  verify, lock/unlock/dismount, PnP
- **src\fatcreat.c** - open, create, overwrite, supersede, open target directory
- **src\fatdir.c** - directory scan (8.3 + LFN assembly), lookup, listing
- **src\fatdirw.c** - entry sets: create, update, remove, move, directory growth,
  `.`/`..`, the label
- **src\fatread.c / fatwrite.c** - cached, non-cached, paging and DASD I/O;
  valid-data-length zero-fill
- **src\fatsetin.c** - set information: times, attributes, delete, rename,
  allocation, end of file
- **src\fatinfo.c / fatvol.c** - file and volume information, the label
- **src\fatflush.c** - flushing files, the FAT and FSInfo; the clean-shutdown
  bit; shutdown
- **src\fatclose.c** - cleanup, close, dismount, volume teardown
- **src\fatalloc.c** - cluster chains, run lists, free scan, 12/16/32-bit FAT
  entries, FAT mirroring, FSInfo
- **src\fatio.c** - metadata stream mapping, disk requests
- **src\fatstruc.c** - FCB/CCB management, internal streams, names, time conversion
- **src\fatfast.c** - fast I/O and Cache Manager callbacks
- **src\fatmisc.c** - device control pass-through, byte-range locks
- **src\fatletr.c** - drive letters for `0B`/`0C`/`0E` partitions on NT 3.1 - 4.0
- **bin\fatnt.reg** - service registration
- **fmt\ chk\** - the tools
- **test\** - user-mode test run on Linux

## Building

### Windows NT 3.1 - 4.0

Needs Visual C++ 4.x, the Windows NT 4.0 DDK and the free `ntifs.h`
(release 58) by Bo Branten, included unchanged as `src\NT\ntifs.h`.

1. Adjust `MSVCDIR` and `DDKDIR` at the top of `src\NT\build.bat`.
2. Run `build.bat` in `src\NT\`.

The same binary is meant for NT 3.1 through 4.0. It is built with `FAT_NT4`
and `FAT_NT31`: the latter selects the universal profile, so the
executive-resource API is bound to the running kernel at load time
(`fatres.c`) and the one image loads on NT 3.1 (legacy resources) through 4.0
(Lite). The 64-bit arithmetic helpers NT 3.1 lacks are linked from
`libcntpr.lib`; the subsystem version is 3.10 so the NT 3.1 loader accepts it;
and `link -release` writes the PE checksum the NT 3.51+ loader validates.

### The tools

`fatfmt`, `fatchk` and `fatinst` build with Visual C++ 4.x
(`fmt\NT\build.bat`, `chk\NT\build.bat`, `inst\NT\build.bat`), linked with the
static C runtime (`-ML`) so they need no `msvcrt`/`crtdll` at run time on NT
3.1. `fatchk` shares `src\fatsup.c`, compiled with `FAT_USERMODE`. They are
also built by the cross script below (freestanding, with `build\toolcrt.c` /
`build\nocrt.c` as a tiny built-in C runtime), and prebuilt copies are in
`bin\` (see *Prebuilt binaries*).

### Cross build (GCC/clang, no Visual C++)

`sh build/build_cross.sh` produces a native NT 3.1 `fatnt.sys` on a Linux
host using the mingw-w64 DDK headers and import libraries, for building and
boot-testing without Visual C++. It compiles the sources with `-DFAT_CROSS`,
which selects the universal import profile (resource API bound at run time,
stdcall `Io`/`Ob` calls, `ExAllocatePool`, no `FsRtlNotifyFull`/critical-region
exports) and shims the SEH keywords GCC-for-NT lacks (the `__finally`
cleanup and `__leave` unwinding are kept; only catching CPU exceptions is
dropped). `build/fatcrt.c` supplies the few `mem*` routines the code
generator emits that NT 3.1's ntoskrnl does not export. After linking, the
script **strips the image and writes a correct PE checksum**
(`build/pechecksum.py`) - the NT 3.51+/2000-era OS loader validates both and
rejects a driver that fails them as "missing or corrupt" (see the notes below).
The resulting `fatnt.sys` is subsystem native 3.10, image base `0x10000`, and
imports only functions present on every target. The same script also builds
`fatinst`, `fatfmt` and `fatchk` as freestanding NT 3.1 console programs
(subsystem 3.10, importing only `KERNEL32`/`ADVAPI32`), and is built
`-march=i486` so no P6 instruction is emitted. The Visual C++ build
(`src\NT\build.bat`) uses none of the `FAT_CROSS` shims.

## Prebuilt binaries

`bin\` holds ready-to-use binaries (all native subsystem 3.10, 486-safe, and
importing only DLLs present on NT 3.1), so no build step is needed to install:

| File | What it is |
|---|---|
| `fatnt.sys`   | the file system driver |
| `fatinst.exe` | the installer (see *Installing*) |
| `fatfmt.exe`  | format a FAT volume |
| `fatchk.exe`  | check / repair a FAT volume |
| `fatnt.reg`   | service values for a manual `/DISABLE`-style install |

## Windows NT 3.1 - 4.0 notes

One binary spans four NT releases whose kernels differ in ways that only show up
once the driver runs for real. The ones that mattered:

- **Resource API bound at run time.** The executive-resource functions split
  across versions with no statically-importable common set: NT 3.1 exports the
  legacy names (`ExInitializeResource`, `ExAcquireResourceExclusive`/`Shared`, ...)
  over the *old* `ERESOURCE` layout and has no Lite forms; NT 3.51+ export the
  Lite names over the *new* layout and dropped `ExAcquireResourceShared`
  entirely. A static import the running kernel does not export makes the loader
  reject the driver as "missing or corrupt", and routing the Lite-era code
  through NT 3.51's legacy compatibility path mismatches the structure and
  bugchecks (0xA) inside the kernel's resource wait. So `fatnt.sys` imports none
  of them: `DriverEntry` finds ntoskrnl in memory and resolves each function by
  name - the Lite form on NT 3.51+, the legacy form on NT 3.1 (`src\fatres.c`).
  Each NT then runs its own native, structurally-matched resource code, and true
  shared (reader) acquisition is preserved everywhere.
- **Stripped image with a valid PE checksum.** The NT 3.51+ loader (and the
  Windows 2000-era NTLDR a disk may boot with) verifies a boot driver's PE
  checksum and expects the file to match the image the headers describe; NT 3.1's
  loader does neither. A GNU `ld` driver carries a ~30 KB COFF symbol table past
  the last section and (depending on toolchain) a stale checksum, which NT 3.1
  tolerated but NT 3.51 rejected as "missing or corrupt". The cross build
  therefore strips the binary and writes the checksum as its last step.
- **486 code generation.** NT 3.1 runs on 80386/486 CPUs, so the driver must
  contain no P6+ instructions. The cross build uses `-march=i486`; without it
  the compiler emits `CMOV` and the kernel traps with an illegal instruction
  the first time such a path runs.
- **`ERESOURCE` size.** NT 3.1's `ERESOURCE` is larger than the structure later
  (NT 4.0 / WDK / mingw) headers describe, and the initialise call fills the
  larger one. Every embedded `ERESOURCE` is padded (`FAT_ERESOURCE_PAD`) so it
  cannot run past its field into the next; the same applies to `FILE_LOCK`.
  Harmless on NT 3.5+.
- **The Memory Manager sizes the paging file.** A paging file in use must not be
  deleted or renamed, but Mm *does* set its size (`FileEndOfFileInformation`,
  `FileAllocationInformation`) when it sets the paging file up and when it grows
  it. The driver must allow those on the paging-file FCB; refusing them makes Mm
  report "no properly sized paging file" and run with almost no virtual memory,
  which also starves everything else (directory listings fail for lack of pool,
  the shell crawls). Only delete and rename are refused; truncation at open is
  prevented separately by forcing `FILE_OPEN` for paging opens.
- **No synchronous zero-fill for the paging file.** FAT has no valid-data
  length on disk, so ordinary files get the clusters past their last written
  byte zeroed before a larger size is recorded. Doing that for the paging file
  meant a huge zero-fill inside the modified page writer when Mm grew the file
  under memory pressure, and the system froze. A paging file is treated as
  valid up to its full size: the Memory Manager never relies on the file
  system to zero it.
- **Resizing a memory-mapped file.** A file mapped by the Memory Manager but
  without a cache map must not get a temporary cache map just to report a new
  size: with the volume and the file held, `CcInitializeCacheMap` waits for the
  old cache map's teardown, whose flush needs the file - a deadlock that showed
  up on Windows 2000 as a boot hang at "Preparing network connections...". A
  shrink purges the pages past the new end instead, which needs no cache map.
  The lazy writer's valid-data-length report is accepted even when it comes
  through a file object whose handle is already closed.
- **Directory change notification.** The `FsRtlNotifyFull` package is NT 3.51+
  only, and NT 3.1's older notify package needs a sync object NT 3.1 does not
  export an initialiser for, so the one universal binary does not hand notify
  requests to FsRtl. A notify request must still never be completed on the spot:
  that makes the caller (File Manager, the shell) treat it as "something
  changed", re-read and re-arm instantly, and spin - pinning the CPU and
  freezing the shell. The driver instead holds each `IRP_MN_NOTIFY_CHANGE_DIRECTORY`
  pending on the volume's notify list (with a cancel routine) and completes it
  at handle cleanup. No live auto-refresh (press refresh to re-read), but no
  spin or freeze. A Visual C++ build pinned to NT 3.51/4.0 (without `FAT_NT31`)
  uses the real `FsRtlNotifyFull` package and delivers live notification.
- **Dispatch table size.** `DRIVER_OBJECT.MajorFunction[]` is sized by the
  running kernel's `IRP_MJ_MAXIMUM_FUNCTION`, which on NT 3.1 is smaller than
  the headers' value, so `DriverEntry` fills it only up to `IRP_MJ_CLEANUP`.
- **Metadata streams.** Each metadata stream (the FAT, the fixed root, each
  directory) is a stream file on FATNT's own volume device, so its cached and
  paging I/O comes back to FATNT to be mapped from stream offset to disk LBO.
- **Dismount of the live boot volume.** When a raw (DASD) writer such as
  `autochk` unlocks the system volume, FATNT drops its cached metadata; if the
  volume still has open handles (it always does while running) it re-reads in
  place instead of a full dismount, which NT 3.1 cannot remount cleanly.
- **The file system device is `\Fat`.** NT 3.1's FAT recognizer (`Fat_Rec`)
  checks for a file system device of that name before it asks for the FAT
  driver to be loaded; with any other name it kept asking, and the first FAT
  volume to appear after boot (a freshly formatted one, say) hung the system.
  If `\Fat` is taken (Fastfat still loaded), FATNT uses `\FatNt`.
- **The boot volume is loaded by file name** - the loader loads `Fastfat.sys`,
  which is why the installer's default is to stand in for that file (see
  *Why "replace" is the default*).

## Installing

FATNT is for **Windows NT 3.1, 3.5, 3.51 and 4.0**. Windows 2000 and later
already ship a FAT driver, so FATNT is not needed there - the same sources do
build with the WDK for 2000/XP (x86 and x64), but installing it is optional and
only makes sense if you specifically want to replace the in-box `fastfat`.

Put `fatnt.sys` next to `fatinst.exe` and run the installer:

```
fatinst            install (default): back up Fastfat.sys to Fastfat.sav and
                   put FATNT in its place, so FATNT is the FAT driver for every
                   FAT volume, the boot volume included
fatinst /DISABLE   NT 3.5+ alternative that overwrites no file: install FATNT as
                   a separate boot-start service and disable the Fastfat service
fatinst /DISABLE /SYSTEM    with /DISABLE: system-start (data volumes only)
fatinst /DISABLE /KEEPFAT   with /DISABLE: leave the Fastfat service enabled
fatinst /U         uninstall (restore Fastfat.sav; undo a /DISABLE install)
```

### Why "replace" is the default

The NT 3.1 OS loader loads the boot volume's file system driver by the fixed
file name `Fastfat.sys`, *before* the service database is read; and on NT 3.5+
the `Fastfat` service loads that same file. So the one method that hands the
boot volume to FATNT **uniformly across NT 3.1 - 4.0** is to stand in for
`Fastfat.sys`. The default install backs up the original to `Fastfat.sav` (once)
and copies FATNT over `Fastfat.sys`; FATNT then loads under the existing
`Fastfat` service, and its `EnableWriteSupport` setting lives on that service
key. `fatinst /U` copies `Fastfat.sav` back. This is the only file that is
touched, and it is preserved and reversible.

`/DISABLE` is the earlier, non-destructive method: FATNT is installed as its own
boot-start `fatnt` service and the `Fastfat` service is set to `Start = 4`
(disabled), overwriting nothing. It works on **NT 3.5 and later** (where the
loader honours the disabled service), but **not for the NT 3.1 boot volume**
(the loader still loads the `Fastfat.sys` file regardless), so on NT 3.1 use it
only for non-boot FAT volumes. `bin\fatnt.reg` holds the same service values for
a manual `regedit /s` of the `/DISABLE` method.

`fatinst` links against no C runtime (it imports only `ADVAPI32`/`KERNEL32`), so
it needs no `msvcrt`/`crtdll` on NT 3.1, and a plain restart applies it.

> Offline install note: tools like `chntpw`/`hivex` cannot edit an NT 3.1
> `SYSTEM` hive (its pre-NT 3.5 cell format is not supported); the default
> replace method only copies files, so it works offline too, while the
> `/DISABLE` registry method should be run inside NT on 3.1. On NT 3.5 and
> later the hive is the standard format and can be edited offline.

### FAT32 partitions on NT 3.1 - 4.0

NT 3.1 - 4.0 predate FAT32: they give drive letters only to partition types
`01`, `04`, `06` and `07`, and their Disk Administrator lists FAT32 (`0B`,
`0C`) and LBA FAT16 (`0E`) partitions as **Unknown**. FATNT reads the boot
sector of every such partition at startup and gives the FAT ones the next free
drive letters (remembered in the registry, so a partition keeps its letter);
FATNT mounts them like any other FAT volume. Disk Administrator still shows
them as Unknown - it judges by the partition type alone - and cannot assign
those letters.

If Disk Administrator should show the volume as FAT and NT itself should give
it its letter, give the partition type `06`: `fatfmt X: /T:06 /TYPEONLY`
(no format; applies after a restart). FATNT and Windows 2000 and later read a
FAT32 volume on a type `06` partition fine; DOS and Windows 9x would misread
it, so do this only on disks that stay with NT.

### Settings

With the default (replace) install the setting lives on the `Fastfat` service
key; with `/DISABLE` it is on the `fatnt` service key
(`HKLM\SYSTEM\CurrentControlSet\Services\...`):

| Value | Type | Meaning |
|---|---|---|
| `EnableWriteSupport` | `REG_DWORD` | `1` (or missing): read/write. `0`: every FAT volume is mounted read-only. |

Read at each mount, so a change applies to volumes mounted afterwards.

## Tools

```
fatfmt drive: [/FS:FAT|FAT32] [/V:label] [/A:size] [/T:xx] [/Y]
fatfmt drive: /T:xx /TYPEONLY
fatchk drive: [/F] [/V]
```

`fatfmt` lays out the boot sector (and the FAT32 backup and FSInfo), the FATs
and an empty root directory, choosing the FAT type from the volume size unless
`/FS:` forces it. `/A:` takes a cluster size in bytes or as `4K`, `64K` and so
on; FAT16 clusters go up to 64 KB, so a 4 GB partition formats as FAT16 the
way NT's own `FORMAT` does it. `fatfmt` also writes the hidden-sectors field
and gives the partition the type that matches the new FAT (`01`, `04`, `06`;
`0B`/`0C` for FAT32 on Windows 2000 and later - NT 3.1 - 4.0 only give drive
letters to the older types, so there a FAT32 partition keeps its type and a
FAT12/FAT16 volume moves to `01`/`04`/`06`). `/T:xx` sets a type of your
choice instead, `/TYPEONLY` changes only the type. `fatfmt` locks and
dismounts the volume before writing, so a mounted volume is formatted cleanly
and remounted fresh. Both tools run on NT 3.1 through 2000. `fatchk` checks the boot sector, the reserved FAT entries and
clean-shutdown bit, every cluster chain (bad links, loops, over-long chains,
cross-links), directory sizes, lost clusters and the free count; `/F` repairs.
A mounted volume is locked first (which writes out everything cached); if files
are open on it, `fatchk` checks what is on the disk so far and `/F` is refused.

## Testing

`python3 test/runtests.py` (needs gcc, dosfstools and mtools) builds the driver
sources together with a small fake NT kernel (`test\kern.c`, `test\ntifs.h`)
and runs them in user mode on Linux against real FAT12, FAT16 and FAT32 images:
mount, 8.3 and long-name reads, directory listing, create/write/read of large
files, sparse extension with zero-fill verified across a remount, delete, then
a remount and an `fsck.fat` consistency check of the result. This complements,
but does not replace, testing on real Windows.

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).

`src\NT\ntifs.h` is the free `ntifs.h` (release 58) by Bo Branten, GPL v2 or
later, included unchanged.
