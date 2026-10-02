/*
 * FATNT - FAT12/16/32 file system driver for Windows NT 3.1, 3.5, 3.51,
 * 4.0, 2000 and XP (x86 and x64)
 *
 *   FAT_NT4   Windows NT 3.1 - 4.0 (NT4 DDK + free ntifs.h, see src\NT\build.bat)
 *   (none)    Windows 2000 and XP, x86 and amd64 (WDK 6001, src\2KXP)
 *
 * Built on the same design as EXFATNT: one FCB per object, metadata
 * (the FAT, the FAT12/16 root directory and every subdirectory) cached
 * through an internal stream each, files cached in their own section.
 */

#ifndef _FAT_H_
#define _FAT_H_

/*
 * FAT_CROSS: a GCC/clang cross build against the mingw-w64 DDK headers, for
 * producing and boot-testing an NT 3.1 binary without Visual C++. It implies
 * the NT 3.1 import profile (FAT_NT31) and the NT4-era code paths (FAT_NT4),
 * and shims the SEH keywords GCC/clang-for-NT does not provide: __finally
 * blocks still run and __leave still unwinds (cleanup is preserved), only the
 * catching of CPU exceptions is dropped. The real driver is built with
 * Visual C++ (src\NT\build.bat) and does not use any of this.
 */
#ifdef FAT_CROSS
#ifndef FAT_NT31
#define FAT_NT31
#endif
#ifndef FAT_NT4
#define FAT_NT4
#endif
#define __try
#define __finally               __fat_leave: __attribute__((unused));
#define __except(x)             if (0)
#define __leave                 goto __fat_leave
#ifndef AbnormalTermination
#define AbnormalTermination()   0
#endif
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)0)
#endif
#ifndef GetExceptionCode
#define GetExceptionCode()      0
#endif
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif
#ifndef EXCEPTION_CONTINUE_SEARCH
#define EXCEPTION_CONTINUE_SEARCH 0
#endif
#endif /* FAT_CROSS */

#include <ntifs.h>
#include <ntdddisk.h>

#include "fatdisk.h"

/* ------------------------------------------------------------------ */
/* NT 3.1 import profile                                               */
/* ------------------------------------------------------------------ */

/*
 * Windows NT 3.1 does not export the Lite executive-resource functions,
 * MmCanFileBeTruncated, or the FsRtlNotifyFull / NotifySync change-
 * notification family (all introduced in NT 3.5/4.0). Mapping the Lite
 * calls to their originals is exact; truncation always allowed is the safe
 * choice where the memory manager cannot answer; change notification is an
 * optional feature and is left out on NT 3.1. One binary then loads on NT
 * 3.1 through 4.0. (NT 3.51 and later export the Lite set, so a build
 * without FAT_NT31 uses it directly.)
 */
#ifdef FAT_NT31

/*
 * The mingw DDK aliases the legacy resource names to their Lite forms (the
 * only ones modern ntoskrnl exports). Undo that, declare the real NT 3.1
 * legacy exports, and route the Lite calls to them. (A Visual C++ NT 3.1
 * build needs the same remap but keeps the DDK's own prototypes.)
 */
#undef ExAcquireResourceExclusive
#undef ExAcquireResourceShared
#undef ExReleaseResourceForThread
#undef ExInitializeResource
#undef ExDeleteResource

__declspec(dllimport) BOOLEAN NTAPI ExAcquireResourceExclusive(PERESOURCE, BOOLEAN);
__declspec(dllimport) BOOLEAN NTAPI ExAcquireResourceShared(PERESOURCE, BOOLEAN);
__declspec(dllimport) VOID    NTAPI ExReleaseResourceForThread(PERESOURCE, ERESOURCE_THREAD);
__declspec(dllimport) VOID    NTAPI ExInitializeResource(PERESOURCE);
__declspec(dllimport) VOID    NTAPI ExDeleteResource(PERESOURCE);

#define ExAcquireResourceExclusiveLite  ExAcquireResourceExclusive
#define ExAcquireResourceSharedLite     ExAcquireResourceShared
#define ExReleaseResourceForThreadLite  ExReleaseResourceForThread
#define ExInitializeResourceLite        ExInitializeResource
#define ExDeleteResourceLite            ExDeleteResource

/*
 * NT 4.0 introduced the fast-call Iof/Obf forms; NT 3.1 has only the
 * stdcall originals. The DDK maps the plain names to the fast-call ones,
 * so undo that and declare the stdcall exports NT 3.1 actually has.
 */
#undef IoCallDriver
#undef IoCompleteRequest
#undef ObDereferenceObject
__declspec(dllimport) NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP);
__declspec(dllimport) VOID     NTAPI IoCompleteRequest(PIRP, CCHAR);
__declspec(dllimport) VOID     NTAPI ObDereferenceObject(PVOID);

#define MmCanFileBeTruncated(S, N)      TRUE
#define FsRtlNotifyInitializeSync(S)    ((void)0)
#define FsRtlNotifyUninitializeSync(S)  ((void)0)
#define FsRtlNotifyCleanup(a, b, c)     ((void)0)
#define FsRtlNotifyFullChangeDirectory(a,b,c,d,e,f,g,h,i,j)  ((void)0)
#define FsRtlNotifyFullReportChange(a,b,c,d,e,f,g,h,i)       ((void)0)
#endif /* FAT_NT31 */

/*
 * The mingw DDK IO_STACK_LOCATION already carries the file-system members,
 * so the FAT_NT4 extended-stack cast is a plain identity there.
 */
#ifdef FAT_CROSS
typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;

/* The DDK headers redefine these SEH intrinsics; re-assert the shim so no
   _abnormal_termination / exception-record references are emitted. */
#undef AbnormalTermination
#define AbnormalTermination()       0
#undef GetExceptionCode
#define GetExceptionCode()          0
#undef GetExceptionInformation
#define GetExceptionInformation()   ((PEXCEPTION_POINTERS)0)

/*
 * NT 3.1 does not export ExAllocatePoolWithTag, the Ke critical-region
 * calls, or the top-level-IRP / verify / requestor helpers (newer builds
 * access them inline through the thread). For the cross/boot-test build,
 * drop the pool tag, make the critical region a no-op pair, and keep the
 * top-level IRP in one process-wide slot (the test drives the volume from
 * a single thread). A Visual C++ NT 3.1 build would instead use the real
 * inline thread-field accessors.
 */
#define ExAllocatePoolWithTag(Type, Size, Tag)  ExAllocatePool((Type), (Size))
#define KeEnterCriticalRegion()                 ((void)0)
#define KeLeaveCriticalRegion()                 ((void)0)
extern PIRP FatXTopLevelIrp;
#define IoGetTopLevelIrp()          (FatXTopLevelIrp)
#define IoSetTopLevelIrp(I)         (FatXTopLevelIrp = (PIRP)(I))
#define IoGetRequestorProcess(Irp)  ((PEPROCESS)0)
#define IoGetDeviceToVerify(T)      ((PDEVICE_OBJECT)0)
#define IoSetDeviceToVerify(T, D)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

/*
 * The NT4 DDK IO_STACK_LOCATION lacks the file system members
 * (QueryDirectory, NotifyDirectory, FileSystemControl, LockControl);
 * the free ntifs.h provides them through EXTENDED_IO_STACK_LOCATION.
 */
#ifdef FAT_NT4
#define FatXSp(IrpSp)           ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))
#define FatMdlAddress(Mdl)      MmGetSystemAddressForMdl(Mdl)
#define FAT_STATUS_DISMOUNTED   STATUS_FILE_INVALID
#else
#define FatXSp(IrpSp)           (IrpSp)
#define FatMdlAddress(Mdl)      MmGetSystemAddressForMdlSafe((Mdl), NormalPagePriority)
#define FAT_STATUS_DISMOUNTED   STATUS_VOLUME_DISMOUNTED
#endif

/* Same as the W2K IoSkipCurrentIrpStackLocation, which NT4 lacks */
#define FatSkipStack(Irp) {                         \
    (Irp)->CurrentLocation++;                       \
    (Irp)->Tail.Overlay.CurrentStackLocation++;     \
}

#define FatCopyStackToNext(Irp) {                                           \
    PIO_STACK_LOCATION _Sp = IoGetCurrentIrpStackLocation(Irp);            \
    PIO_STACK_LOCATION _Next = IoGetNextIrpStackLocation(Irp);             \
    RtlCopyMemory(_Next, _Sp, FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine)); \
    _Next->Control = 0;                                                     \
}

/* ExReleaseResourceLite is not exported by every NT4 build */
#define FatRelease(Resource) \
    ExReleaseResourceForThreadLite((Resource), ExGetCurrentResourceThread())

#ifndef VPB_LOCKED
#define VPB_LOCKED                  0x00000002
#endif

/* File system IRP details some DDK headers leave out */
#ifndef IRP_MN_QUERY_DIRECTORY
#define IRP_MN_QUERY_DIRECTORY          0x01
#endif
#ifndef IRP_MN_NOTIFY_CHANGE_DIRECTORY
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#endif
#ifndef IRP_MN_USER_FS_REQUEST
#define IRP_MN_USER_FS_REQUEST          0x00
#endif
#ifndef IRP_MN_MOUNT_VOLUME
#define IRP_MN_MOUNT_VOLUME             0x01
#endif
#ifndef IRP_MN_VERIFY_VOLUME
#define IRP_MN_VERIFY_VOLUME            0x02
#endif
#ifndef SL_RESTART_SCAN
#define SL_RESTART_SCAN                 0x01
#endif
#ifndef SL_RETURN_SINGLE_ENTRY
#define SL_RETURN_SINGLE_ENTRY          0x02
#endif
#ifndef SL_INDEX_SPECIFIED
#define SL_INDEX_SPECIFIED              0x04
#endif
#ifndef SL_WATCH_TREE
#define SL_WATCH_TREE                   0x01
#endif
#ifndef SL_OPEN_PAGING_FILE
#define SL_OPEN_PAGING_FILE             0x02
#endif
#ifndef SL_OPEN_TARGET_DIRECTORY
#define SL_OPEN_TARGET_DIRECTORY        0x04
#endif
#ifndef SL_OVERRIDE_VERIFY_VOLUME
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#endif

#ifndef IO_REMOUNT
#define IO_REMOUNT                  0x00000001
#endif

#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME       0x00080000
#endif

#ifndef FSCTL_IS_VOLUME_DIRTY
#define FSCTL_IS_VOLUME_DIRTY       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY             0x00000001
#endif

#ifndef FILE_WRITE_TO_END_OF_FILE
#define FILE_WRITE_TO_END_OF_FILE   0xffffffff
#endif

#ifndef IOCTL_DISK_IS_WRITABLE
#define IOCTL_DISK_IS_WRITABLE      CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Status codes newer than some DDK headers */
#ifndef STATUS_USER_MAPPED_FILE
#define STATUS_USER_MAPPED_FILE     ((NTSTATUS)0xC0000243L)
#endif
#ifndef STATUS_CANNOT_MAKE
#define STATUS_CANNOT_MAKE          ((NTSTATUS)0xC000016BL)
#endif

/* Information classes newer than NT4, answered on both targets */
#define FAT_CLASS_ATTRIBUTE_TAG             35
#define FAT_CLASS_FS_FULL_SIZE              7

typedef struct _FAT_FILE_ATTRIBUTE_TAG_INFORMATION {
    ULONG       FileAttributes;
    ULONG       ReparseTag;
} FAT_FILE_ATTRIBUTE_TAG_INFORMATION, *PFAT_FILE_ATTRIBUTE_TAG_INFORMATION;

typedef struct _FAT_FILE_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG       SectorsPerAllocationUnit;
    ULONG       BytesPerSector;
} FAT_FILE_FS_FULL_SIZE_INFORMATION, *PFAT_FILE_FS_FULL_SIZE_INFORMATION;

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

#define FAT_PFX                 "[FATNT] "

#if DBG || defined(FAT_DEBUG)
#define FAT_DBG(args)           DbgPrint args
#else
#define FAT_DBG(args)
#endif

/*
 * Debug trace to the QEMU "debugcon" port (0xE9): readable bytes with no
 * kernel-debugger protocol. Boot QEMU with -debugcon file:/path. Compile
 * with -DFAT_DBGPORT to enable; a no-op otherwise.
 */
#ifdef FAT_DBGPORT
static __inline void FatDbgCh(unsigned char c)
{ unsigned short port = 0x00E9;
  __asm__ __volatile__ ("outb %b0, %w1" : : "a"(c), "d"(port)); }
static __inline void FatDbgStr(const char *s)
{ while (*s) FatDbgCh((unsigned char)*s++); }
static __inline void FatDbgHex(unsigned long v)
{ int i; char h[9]; const char *d="0123456789abcdef";
  for (i=7;i>=0;i--){ h[i]=d[v&0xF]; v>>=4; } h[8]=0; FatDbgStr(h); }
static __inline void FatDbgUni(const void *us)
{ const unsigned short *p; unsigned short len, i;
  len = ((const unsigned short *)us)[0];         /* UNICODE_STRING.Length */
  p   = *(const unsigned short * const *)((const char *)us + 4); /* .Buffer */
  if (!p) { FatDbgStr("(null)"); return; }
  for (i=0;i<len/2 && i<120;i++) FatDbgCh((unsigned char)(p[i]<128?p[i]:'?')); }
#define DBGP(s)     FatDbgStr(s)
#define DBGX(v)     FatDbgHex((unsigned long)(v))
#define DBGU(u)     FatDbgUni(&(u))
#else
#define DBGP(s)     ((void)0)
#define DBGX(v)     ((void)0)
#define DBGU(u)     ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Limits and tags                                                     */
/* ------------------------------------------------------------------ */

#define FAT_FS_DEVICE_NAME      L"\\FatNt"
#define FAT_FS_NAME             L"FAT"

#define FAT_TAG                 'taFA'
#define FAT_TAG_FCB             'FtaF'
#define FAT_TAG_CCB             'CtaF'
#define FAT_TAG_RUNS            'RtaF'
#define FAT_TAG_BUFFER          'BtaF'
#define FAT_TAG_NAME            'NtaF'
#define FAT_TAG_WORK            'WtaF'
#define FAT_TAG_REGISTRY        'GtaF'

#define FAT_MAP_UNIT            0x1000      /* CcMapData / CcPinRead granule */
#define FAT_PAGE_SIZE           0x1000      /* x86 and amd64 */
#define FAT_ZERO_CHUNK          0x10000     /* zero buffer for disk writes */

/* ------------------------------------------------------------------ */
/* Node types                                                          */
/* ------------------------------------------------------------------ */

#define FAT_NTC_VCB             ((CSHORT)0x0F01)
#define FAT_NTC_FCB             ((CSHORT)0x0F02)    /* file */
#define FAT_NTC_DCB             ((CSHORT)0x0F03)    /* directory */
#define FAT_NTC_VFCB            ((CSHORT)0x0F04)    /* whole volume (DASD) */
#define FAT_NTC_CCB             ((CSHORT)0x0F05)
#define FAT_NTC_META            ((CSHORT)0x0F06)    /* the FAT */

#define FatNodeType(Ptr)        (*((CSHORT *)(Ptr)))

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct _FAT_VCB FAT_VCB, *PFAT_VCB;

/* Cluster run: Count clusters of the stream starting at Vcn map to Lcn.. */
typedef struct _FAT_RUN {
    ULONG       Vcn;
    ULONG       Lcn;
    ULONG       Count;
} FAT_RUN, *PFAT_RUN;

typedef struct _FAT_RUN_LIST {
    PFAT_RUN    Runs;
    ULONG       RunCount;
    ULONG       RunMax;
    ULONG       Clusters;
} FAT_RUN_LIST, *PFAT_RUN_LIST;

/*
 * NT 3.1's ERESOURCE is larger than the structure later DDK/WDK headers (and
 * the mingw cross headers) describe - sizeof(ERESOURCE) there is 0x38, but
 * ExInitializeResource on NT 3.1 writes roughly 0x68 bytes. Every embedded
 * ERESOURCE is therefore followed by this padding so the initialise call
 * cannot run past its reservation into the next field. Harmless on NT 3.5+,
 * where the resource is no larger than the headers say.
 */
#define FAT_ERESOURCE_PAD   0x40

/* Allocated from nonpaged pool: holds ERESOURCEs and the FsRtl header */
typedef struct _FAT_FCB {
    FSRTL_COMMON_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[FAT_ERESOURCE_PAD];
    ERESOURCE   PagingIoResource;
    UCHAR       PagingIoResourcePad[FAT_ERESOURCE_PAD];
    PFAT_VCB    Vcb;
    struct _FAT_FCB *ParentDcb;
    LIST_ENTRY  FcbLinks;
    ULONG       RefCount;           /* file objects + child FCBs */
    ULONG       UncleanCount;       /* handles not yet cleaned up */
    ULONG       NonCachedUncleanCount;
    ULONG       FcbState;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK   FileLock;
    ULONG       FileLockSpare[16];  /* NT 3.1's FILE_LOCK is larger than the headers' */
    PFILE_OBJECT StreamFile;        /* internal cached stream (directory, FAT) */
    PKTHREAD    LazyWriteThread;
    LONGLONG    MetaLbo;            /* the FAT, or the fixed FAT12/16 root */
    ULONG       DirOffset;          /* short-entry byte offset in parent */
    ULONG       SetOffset;          /* first entry (LFN or short) byte offset */
    ULONG       EntryCount;         /* LFN entries + 1 short entry */
    UCHAR       Attributes;         /* FAT attributes */
    UCHAR       NtReserved;         /* 8.3 lower-case bits */
    UCHAR       ShortName[FAT_SFN_LEN]; /* raw 8.3, to rewrite the short entry */
    ULONG       FirstCluster;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER LastAccessTime;
    LONGLONG    IndexNumber;
    UNICODE_STRING Name;            /* last component, long form */
    UNICODE_STRING FullName;        /* built on demand */
    FAT_RUN_LIST RunList;
} FAT_FCB, *PFAT_FCB;

#define FCB_STATE_ROOT              0x0001
#define FCB_STATE_VISITED           0x0002  /* list walk marker */
#define FCB_STATE_DELETE_PENDING    0x0004
#define FCB_STATE_DELETED           0x0008  /* off the disk, kept for Cc and Mm */
#define FCB_STATE_DIRENT_DIRTY      0x0010  /* entry set lags behind the FCB */
#define FCB_STATE_TRUNCATE_ON_CLOSE 0x0020  /* allocation beyond the file size */
#define FCB_STATE_STREAM_OPEN       0x0040  /* internal stream not closed yet */
#define FCB_STATE_TEARDOWN          0x0080  /* freed when its stream closes */
#define FCB_STATE_FIXED_ROOT        0x0100  /* FAT12/16 root: linear, no growth */
#define FCB_STATE_PAGING_FILE       0x0200  /* opened as a paging file; pinned */

typedef struct _FAT_CCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ULONG       Flags;
    ULONG       QueryState;         /* 0: ".", 1: "..", 2: entries */
    ULONG       QueryOffset;        /* next directory offset to scan */
    UNICODE_STRING Pattern;         /* upcased */
} FAT_CCB, *PFAT_CCB;

#define CCB_FLAG_PATTERN_SET        0x0001
#define CCB_FLAG_WILDCARD           0x0002
#define CCB_FLAG_MATCH_ALL          0x0004
#define CCB_FLAG_VOLUME_OPEN        0x0008
#define CCB_FLAG_DISMOUNTED_VOLUME  0x0010      /* this handle dismounted it */
#define CCB_FLAG_DELETE_ON_CLOSE    0x0020
#define CCB_FLAG_USER_SET_WRITE     0x0040      /* keep the caller's write time */
#define CCB_FLAG_USER_SET_ACCESS    0x0080
#define CCB_FLAG_WRITE_HANDLE       0x0100      /* counted in Vcb->WriteCount */

/* Lives in the volume device object's extension */
struct _FAT_VCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[FAT_ERESOURCE_PAD];
    ERESOURCE   AllocResource;
    UCHAR       AllocResourcePad[FAT_ERESOURCE_PAD];
    LIST_ENTRY  VcbLinks;
    PVPB        Vpb;
    PDEVICE_OBJECT TargetDeviceObject;
    PDEVICE_OBJECT VolumeDeviceObject;
    ULONG       VcbState;
    ULONG       OpenCount;          /* all our file objects, internal too */
    ULONG       UncleanCount;       /* user handles */
    ULONG       WriteCount;         /* user handles that may change the volume */
    PFILE_OBJECT LockFileObject;
    PKTHREAD    VerifyThread;       /* mount/verify in progress */
    PFAT_FCB    VolumeFcb;
    PFAT_FCB    RootDcb;
    PFAT_FCB    FatFcb;             /* the active FAT, cached as a linear stream */
    LIST_ENTRY  FcbList;

    /* Geometry and layout */
    UCHAR       FatType;            /* 12 / 16 / 32 */
    ULONG       SectorSize;
    ULONG       SectorShift;
    ULONG       ClusterSize;
    ULONG       ClusterShift;
    ULONG       SectorsPerClusterShift;
    LONGLONG    VolumeBytes;
    LONGLONG    PartitionBytes;
    ULONG       ReservedSectors;
    ULONG       FatSector;          /* first sector of the active FAT */
    ULONG       FatSectors;         /* sectors per FAT */
    ULONG       NumberOfFats;
    ULONG       FirstRootSector;    /* FAT12/16 */
    ULONG       RootDirSectors;     /* FAT12/16 */
    ULONG       RootEntries;        /* FAT12/16 */
    LONGLONG    RootLbo;            /* FAT12/16 fixed root byte offset */
    ULONG       RootBytes;          /* FAT12/16 fixed root byte size */
    ULONG       FirstDataSector;    /* cluster heap start */
    ULONG       ClusterCount;       /* data clusters */
    ULONG       RootCluster;        /* FAT32 */
    ULONG       FsInfoSector;       /* FAT32; 0 if none */
    ULONG       SerialNumber;
    UCHAR       Media;

    ULONG       FatMask;            /* FAT12_MASK / FAT16_MASK / FAT32_MASK */
    ULONG       EndOfChain;         /* value written to end a chain */
    ULONG       BadCluster;

    /* Allocation, under AllocResource */
    ULONG       FreeClusters;       /* 0xFFFFFFFF: unknown */
    ULONG       NextFree;           /* search hint */

    BOOLEAN     DirtyMarked;        /* we cleared the clean-shutdown bit */
    ULONG       LabelLength;        /* characters */
    WCHAR       Label[FAT_MAX_LABEL];

    /* Directory change notification */
    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY  DirNotifyList;
};

#define VCB_STATE_MOUNTED           0x0001
#define VCB_STATE_LOCKED            0x0002
#define VCB_STATE_DISMOUNTED        0x0004
#define VCB_STATE_IN_DISMOUNT       0x0008
#define VCB_STATE_DELETE_PENDING    0x0010
#define VCB_STATE_DASD_WRITTEN      0x0020
#define VCB_STATE_REMOVABLE         0x0040
#define VCB_STATE_PNP_LOCKED        0x0080
#define VCB_STATE_FREE_VPB          0x0100
#define VCB_STATE_REMOVED           0x0200
#define VCB_STATE_READ_ONLY         0x0400  /* write protected */
#define VCB_STATE_KEEP_DIRTY        0x0800  /* dirty at mount or by request */
#define VCB_STATE_FLUSH_ON_CLOSE    0x1000  /* removable or hot-plug media */
#define VCB_STATE_SHUTDOWN          0x2000

typedef struct _FAT_DATA {
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    ERESOURCE       Resource;       /* protects VcbList */
    UCHAR           ResourcePad[FAT_ERESOURCE_PAD];
    LIST_ENTRY      VcbList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    CACHE_MANAGER_CALLBACKS MetaCacheCallbacks;
    UNICODE_STRING  RegistryPath;   /* the service key, NUL-terminated */
} FAT_DATA, *PFAT_DATA;

extern FAT_DATA FatData;

/* Per-request context, lives on the dispatch routine's stack */
typedef struct _FAT_IRP_CONTEXT {
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PFAT_VCB        Vcb;            /* NULL for the file system device */
    ULONG           Flags;
    NTSTATUS        ExceptionStatus;
} FAT_IRP_CONTEXT, *PFAT_IRP_CONTEXT;

#define FAT_CTX_TOP_LEVEL           0x0001
#define FAT_CTX_NO_COMPLETE         0x0002  /* IRP completed, pended or passed down */

/*
 * Mapped piece of a metadata stream (the FAT or a directory), by offset
 * within the stream. While mounting, before the streams exist, a buffer
 * read straight from the disk instead.
 */
typedef struct _FAT_MAP {
    PFAT_FCB    Fcb;
    PVOID       Bcb;
    PUCHAR      Data;
    PUCHAR      Direct;
    LONGLONG    Vbo;
    ULONG       Length;
    BOOLEAN     Pinned;
    BOOLEAN     Dirty;
} FAT_MAP, *PFAT_MAP;

/* Directory scan state */
typedef struct _FAT_SCAN {
    FAT_MAP     Map;
    FAT_DIR_INFO Info;
    UCHAR       Set[FAT_LFN_MAX_ENTRIES * FAT_DIRENT_SIZE + FAT_DIRENT_SIZE];
} FAT_SCAN, *PFAT_SCAN;

/* A close that could not take the VCB, finished by a worker thread */
typedef struct _FAT_CLOSE_ITEM {
    WORK_QUEUE_ITEM Item;
    PFAT_VCB    Vcb;
    PFAT_FCB    Fcb;
    PFAT_CCB    Ccb;
} FAT_CLOSE_ITEM, *PFAT_CLOSE_ITEM;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define FatIsFcb(Fcb)       ((Fcb)->Header.NodeTypeCode == FAT_NTC_FCB)
#define FatIsDcb(Fcb)       ((Fcb)->Header.NodeTypeCode == FAT_NTC_DCB)
#define FatIsVfcb(Fcb)      ((Fcb)->Header.NodeTypeCode == FAT_NTC_VFCB)
#define FatIsMeta(Fcb)      ((Fcb)->Header.NodeTypeCode == FAT_NTC_META)

#define FatClusterToLbo(Vcb, Cluster) \
    (((LONGLONG)(Vcb)->FirstDataSector << (Vcb)->SectorShift) + \
     ((LONGLONG)((Cluster) - FAT_FIRST_CLUSTER) << (Vcb)->ClusterShift))

#define FatRoundUp(Value, Size) \
    (((Value) + ((Size) - 1)) & ~((LONGLONG)(Size) - 1))

#define FatSetDirty(Map)    ((Map)->Dirty = TRUE)

/* Access bits that would change the volume */
#define FAT_WRITE_ACCESS    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | \
                             FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD)

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

/* fatinit.c */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
BOOLEAN FatWriteSupportEnabled(VOID);

/* fatdisp.c */
NTSTATUS NTAPI FatFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS FatVerifyVcb(PFAT_IRP_CONTEXT Ctx, PFAT_VCB Vcb);
NTSTATUS FatVerifyWritable(PFAT_IRP_CONTEXT Ctx, PFAT_VCB Vcb);

/* fatcreat.c */
NTSTATUS FatCommonCreate(PFAT_IRP_CONTEXT Ctx);

/* fatclose.c */
NTSTATUS FatCommonCleanup(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatCommonClose(PFAT_IRP_CONTEXT Ctx);
BOOLEAN  FatTryTeardown(PFAT_VCB Vcb, ULONG InFlightReferences);
VOID     FatDeleteVcb(PFAT_VCB Vcb);
BOOLEAN  FatDismountVcb(PFAT_VCB Vcb);
VOID     FatUnlockVcb(PFAT_VCB Vcb);
VOID     FatPurgeCachedFiles(PFAT_VCB Vcb);

/* fatread.c */
NTSTATUS FatCommonRead(PFAT_IRP_CONTEXT Ctx);

/* fatwrite.c */
NTSTATUS FatCommonWrite(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatZeroFileRange(PFAT_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End);

/* fatdir.c */
NTSTATUS FatCommonDirectoryControl(PFAT_IRP_CONTEXT Ctx);
BOOLEAN  FatNextDirEntry(PFAT_VCB Vcb, PFAT_FCB Dcb, PULONG Offset, PFAT_SCAN Scan);
NTSTATUS FatLookupName(PFAT_VCB Vcb, PFAT_FCB Dcb, PUNICODE_STRING Name, PFAT_SCAN Scan);
PFAT_SCAN FatAllocateScan(VOID);
VOID     FatFreeScan(PFAT_SCAN Scan);

/* fatdirw.c */
NTSTATUS FatCreateDirent(PFAT_VCB Vcb, PFAT_FCB Dcb, PUNICODE_STRING Name,
                         UCHAR Attributes, PFAT_DIR_INFO Info);
NTSTATUS FatUpdateDirent(PFAT_VCB Vcb, PFAT_FCB Fcb);
NTSTATUS FatRemoveDirent(PFAT_VCB Vcb, PFAT_FCB Dcb, ULONG Offset, ULONG Count);
NTSTATUS FatMoveDirent(PFAT_VCB Vcb, PFAT_FCB Fcb, PFAT_FCB TargetDcb, PUNICODE_STRING Name);
NTSTATUS FatIsDirectoryEmpty(PFAT_VCB Vcb, PFAT_FCB Dcb, PBOOLEAN Empty);
NTSTATUS FatInitializeDirectory(PFAT_VCB Vcb, PFAT_FCB Dcb, ULONG FirstCluster);
NTSTATUS FatWriteLabel(PFAT_VCB Vcb, const WCHAR *Label, ULONG Length);
NTSTATUS FatDeleteFromDisk(PFAT_VCB Vcb, PFAT_FCB Fcb);
VOID     FatNotifyChange(PFAT_VCB Vcb, PFAT_FCB Fcb, ULONG Filter, ULONG Action);

/* fatinfo.c */
NTSTATUS FatCommonQueryInformation(PFAT_IRP_CONTEXT Ctx);
ULONG    FatNtAttributes(PFAT_FCB Fcb);
ULONG    FatDirentNtAttributes(UCHAR Attributes);
VOID     FatFillBasicInfo(PFAT_FCB Fcb, PFILE_BASIC_INFORMATION Info);
VOID     FatFillStandardInfo(PFAT_FCB Fcb, PFILE_STANDARD_INFORMATION Info);
VOID     FatFillNetworkOpenInfo(PFAT_FCB Fcb, PFILE_NETWORK_OPEN_INFORMATION Info);

/* fatsetin.c */
NTSTATUS FatCommonSetInformation(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatSetFileSize(PFAT_IRP_CONTEXT Ctx, PFAT_FCB Fcb, PFILE_OBJECT FileObject,
                        LONGLONG NewSize);

/* fatvol.c */
NTSTATUS FatCommonQueryVolumeInformation(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatCommonSetVolumeInformation(PFAT_IRP_CONTEXT Ctx);

/* fatfsctl.c */
NTSTATUS FatCommonFileSystemControl(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatCommonPnp(PFAT_IRP_CONTEXT Ctx);

/* fatmisc.c */
NTSTATUS FatCommonDeviceControl(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatCommonLockControl(PFAT_IRP_CONTEXT Ctx);

/* fatflush.c */
NTSTATUS FatCommonFlushBuffers(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatCommonShutdown(PFAT_IRP_CONTEXT Ctx);
NTSTATUS FatFlushFile(PFAT_VCB Vcb, PFAT_FCB Fcb);
NTSTATUS FatFlushMetadata(PFAT_VCB Vcb);
NTSTATUS FatFlushVolume(PFAT_VCB Vcb, BOOLEAN MarkClean);
VOID     FatMarkVolumeDirty(PFAT_VCB Vcb);
NTSTATUS FatMarkVolumeClean(PFAT_VCB Vcb);

/* fatfast.c */
VOID     FatInitializeFastIo(PFAST_IO_DISPATCH FastIo);
VOID     FatInitializeCacheCallbacks(VOID);
UCHAR    FatIsFastIoPossible(PFAT_FCB Fcb);

/* fatalloc.c */
NTSTATUS FatBuildRunList(PFAT_VCB Vcb, ULONG FirstCluster, ULONG Clusters,
                         PFAT_RUN_LIST RunList);
VOID     FatFreeRunList(PFAT_RUN_LIST RunList);
BOOLEAN  FatLookupVbo(PFAT_VCB Vcb, PFAT_RUN_LIST RunList, LONGLONG Vbo,
                      PLONGLONG Lbo, PULONG Contiguous);
NTSTATUS FatCountFreeClusters(PFAT_VCB Vcb);
NTSTATUS FatAllocateClusters(PFAT_VCB Vcb, PFAT_RUN_LIST RunList, PULONG FirstCluster,
                             ULONG Clusters, BOOLEAN ZeroNew);
VOID     FatFreeClusters(PFAT_VCB Vcb, PFAT_RUN_LIST RunList, PULONG FirstCluster,
                         ULONG Keep);
NTSTATUS FatSetAllocation(PFAT_VCB Vcb, PFAT_FCB Fcb, ULONGLONG Bytes);
ULONG    FatGetFatEntry(PFAT_VCB Vcb, PFAT_MAP Map, ULONG Cluster);
VOID     FatSetFatEntry(PFAT_VCB Vcb, PFAT_MAP Map, ULONG Cluster, ULONG Value);
NTSTATUS FatMirrorFats(PFAT_VCB Vcb);
NTSTATUS FatWriteFsInfo(PFAT_VCB Vcb);

/* fatio.c */
PUCHAR   FatMapStream(PFAT_VCB Vcb, PFAT_FCB Fcb, PFAT_MAP Map, LONGLONG Vbo, ULONG Length);
PUCHAR   FatPinStream(PFAT_VCB Vcb, PFAT_FCB Fcb, PFAT_MAP Map, LONGLONG Vbo, ULONG Length);
VOID     FatUnmap(PFAT_MAP Map);
VOID     FatZeroStream(PFAT_VCB Vcb, PFAT_FCB Fcb, LONGLONG Vbo, ULONG Length);
BOOLEAN  FatStreamToLbo(PFAT_VCB Vcb, PFAT_FCB Fcb, LONGLONG Vbo, PLONGLONG Lbo,
                        PULONG Contiguous);
NTSTATUS FatSyncIo(PDEVICE_OBJECT Device, UCHAR MajorFunction, LONGLONG Offset,
                   ULONG Length, PVOID Buffer, BOOLEAN OverrideVerify);
NTSTATUS FatDeviceIoctl(PDEVICE_OBJECT Device, ULONG IoControlCode, PVOID InputBuffer,
                        ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                        BOOLEAN OverrideVerify, PULONG Information);
NTSTATUS FatFlushDevice(PFAT_VCB Vcb);
VOID     FatLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG Length);
PVOID    FatMapUserBuffer(PIRP Irp);
NTSTATUS FatNonCachedIo(PFAT_IRP_CONTEXT Ctx, PFAT_FCB Fcb, UCHAR MajorFunction,
                        LONGLONG StartingVbo, ULONG ByteCount, LONGLONG ValidData);
NTSTATUS FatZeroDisk(PFAT_VCB Vcb, PFAT_FCB Fcb, LONGLONG Start, LONGLONG End);

#define FatReadSectors(Device, Offset, Length, Buffer, Override) \
    FatSyncIo((Device), IRP_MJ_READ, (Offset), (Length), (Buffer), (Override))
#define FatWriteSectors(Device, Offset, Length, Buffer, Override) \
    FatSyncIo((Device), IRP_MJ_WRITE, (Offset), (Length), (Buffer), (Override))

/* fatstruc.c */
PFAT_FCB FatCreateVolumeFcb(PFAT_VCB Vcb);
PFAT_FCB FatCreateRootDcb(PFAT_VCB Vcb);
PFAT_FCB FatCreateFatFcb(PFAT_VCB Vcb);
NTSTATUS FatCreateFcb(PFAT_VCB Vcb, PFAT_FCB ParentDcb, PFAT_DIR_INFO Info, PFAT_FCB *Fcb);
VOID     FatDeleteFcb(PFAT_FCB Fcb);
VOID     FatDereferenceFcb(PFAT_VCB Vcb, PFAT_FCB Fcb);
PFAT_FCB FatFindFcb(PFAT_VCB Vcb, PFAT_FCB ParentDcb, PUNICODE_STRING Name);
PFAT_FCB FatFindFcbByOffset(PFAT_VCB Vcb, PFAT_FCB ParentDcb, ULONG DirOffset);
NTSTATUS FatOpenStream(PFAT_VCB Vcb, PFAT_FCB Fcb);
VOID     FatCloseStream(PFAT_VCB Vcb, PFAT_FCB Fcb, BOOLEAN Discard);
VOID     FatStreamClosed(PFAT_VCB Vcb, PFAT_FCB Fcb);
PFAT_CCB FatCreateCcb(VOID);
VOID     FatDeleteCcb(PFAT_CCB Ccb);
NTSTATUS FatBuildFullName(PFAT_FCB Fcb);
VOID     FatForgetNames(PFAT_VCB Vcb, PFAT_FCB Fcb);
BOOLEAN  FatIsAncestor(PFAT_FCB Ancestor, PFAT_FCB Fcb);
LARGE_INTEGER FatDosToNtTime(USHORT Date, USHORT Time, UCHAR Tenth);
VOID     FatNtToDosTime(LARGE_INTEGER Time, PUSHORT Date, PUSHORT DosTime, PUCHAR Tenth);
VOID     FatFcbToInfo(PFAT_FCB Fcb, PFAT_DIR_INFO Info);

#endif /* _FAT_H_ */
