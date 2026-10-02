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

Files and directories can be created, written, extended, truncated, renamed,
moved and deleted; attributes, times and the volume label can be changed.
Memory-mapped writes, the fast I/O write path and write-through work. Writing
can be turned off in the registry (`EnableWriteSupport`).

**Boots Windows NT 3.1.** FATNT has been run as the boot/system file system on
a real NT 3.1 SP0 install (x86, under QEMU): it mounts the FAT16 system volume,
passes `autochk`, loads the boot and system drivers, reaches the graphical
logon, Program Manager and File Manager, and backs the paging file - a complete
boot driven entirely by FATNT. Paging-file opens (`SL_OPEN_PAGING_FILE`) are
supported; the Memory Manager requires the paging file to be reasonably
contiguous, which is how Windows lays one out.

A FAT, read/write, boot-capable IFS must get a number of NT-3.1-specific details
right that only surface on the real kernel; the ones that mattered are called
out under *Windows NT 3.1 notes* below.

Three tools come with it: `fatfmt.exe` formats a volume, `fatchk.exe` checks
and repairs one, and (planned) `fatachk.exe` does the boot-time check.

## Features

- **FAT12, FAT16 and FAT32**, type chosen by cluster count per the Microsoft
  specification
  - 12-bit FAT entries handled across byte-pair boundaries
  - Second reserved entry's clean-shutdown bit (FAT16/32) set and cleared like
    Windows; a volume left dirty stays marked for the check tool
  - All FAT copies kept in step; the FAT32 FSInfo free count is maintained
  - 512 to 4096-byte sectors, every power-of-two cluster size to 32 KB,
    files up to 4 GB - 1
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
  - Change notification, byte-range locks, share access, delete-on-close
  - `FSCTL_IS_VOLUME_DIRTY`, `FSCTL_MARK_VOLUME_DIRTY`, flush of files and of
    the whole volume
  - Volume lock/unlock/dismount, media change verification, removable and
    hot-plug write-back
- **Single source tree**, built with `FAT_NT4` (and `FAT_CROSS` for the GCC
  cross build) into one `fatnt.sys` for NT 3.1 - 4.0 (x86). The same sources
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
- **bin\fatnt.reg** - service registration
- **fmt\ chk\** - the tools
- **test\** - user-mode test run on Linux

## Building

### Windows NT 3.1 - 4.0

Needs Visual C++ 4.x, the Windows NT 4.0 DDK and the free `ntifs.h`
(release 58) by Bo Branten, included unchanged as `src\NT\ntifs.h`.

1. Adjust `MSVCDIR` and `DDKDIR` at the top of `src\NT\build.bat`.
2. Run `build.bat` in `src\NT\`.

The same binary is meant for NT 3.1 through 4.0: it imports only kernel
functions NT 3.1 exports, and the 64-bit arithmetic helpers NT 3.1 lacks are
linked from `libcntpr.lib`. The subsystem version is 3.10 so the NT 3.1
loader accepts it.

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
which selects the NT 3.1 import profile (legacy resource functions, stdcall
`Io`/`Ob` calls, `ExAllocatePool`, no `FsRtlNotifyFull`/critical-region
exports) and shims the SEH keywords GCC-for-NT lacks (the `__finally`
cleanup and `__leave` unwinding are kept; only catching CPU exceptions is
dropped). `build/fatcrt.c` supplies the few `mem*` routines the code
generator emits that NT 3.1's ntoskrnl does not export. The resulting
`fatnt.sys` is subsystem native 3.10, image base `0x10000`, and imports
only functions NT 3.1 exports. The same script also builds `fatinst`, `fatfmt`
and `fatchk` as freestanding NT 3.1 console programs (subsystem 3.10, importing
only `KERNEL32`/`ADVAPI32`), and is built `-march=i486` so no P6 instruction is
emitted. The Visual C++ build (`src\NT\build.bat`) uses none of the `FAT_CROSS`
shims.

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

## Windows NT 3.1 notes

NT 3.1 is the oldest target and differs from every later NT in ways that only
show up once the driver runs on the real kernel. The ones that mattered:

- **486 code generation.** NT 3.1 runs on 80386/486 CPUs, so the driver must
  contain no P6+ instructions. The cross build uses `-march=i486`; without it
  the compiler emits `CMOV` and the kernel traps with an illegal instruction
  the first time such a path runs.
- **`ERESOURCE` size.** NT 3.1's `ERESOURCE` is larger than the structure later
  (NT 4.0 / WDK / mingw) headers describe, and `ExInitializeResource` fills the
  larger one. Every embedded `ERESOURCE` is padded (`FAT_ERESOURCE_PAD`) so the
  initialise call cannot run past its field into the next; the same applies to
  `FILE_LOCK`. Harmless on NT 3.5+.
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
fatfmt drive: [/FS:FAT|FAT32] [/V:label] [/A:size] [/Y]
fatchk drive: [/F] [/V]
```

`fatfmt` lays out the boot sector (and the FAT32 backup and FSInfo), the FATs
and an empty root directory, choosing the FAT type from the volume size unless
`/FS:` forces it. `fatchk` checks the boot sector, the reserved FAT entries and
clean-shutdown bit, every cluster chain (bad links, loops, over-long chains,
cross-links), directory sizes, lost clusters and the free count; `/F` repairs.

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
