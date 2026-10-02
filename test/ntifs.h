/* Minimal fake NT kernel environment for running EXFATNT in user mode on Linux. */
#ifndef FAKE_NTIFS_H
#define FAKE_NTIFS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* SEH emulation: no exceptions, __leave jumps to the finally block */
#define __try
#define __finally       __exf_leave: __attribute__((unused));
#define __except(x)     if (0)
#define __leave         goto __exf_leave
#define AbnormalTermination() 0
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)0)
#define GetExceptionCode() 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0

#define NTAPI
#define UNALIGNED
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define DBG 1

typedef void VOID, *PVOID;
typedef uint8_t UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef char CHAR, *PCHAR, CCHAR;
typedef uint16_t USHORT, *PUSHORT, WCHAR, *PWCHAR, *PWSTR;
typedef int16_t SHORT, CSHORT;
typedef uint32_t ULONG, *PULONG, ACCESS_MASK, DEVICE_TYPE;
typedef int32_t LONG, *PLONG, NTSTATUS;
typedef uint64_t ULONGLONG, *PULONGLONG, ULONG_PTR, *PULONG_PTR;
typedef int64_t LONGLONG, *PLONGLONG;
typedef UCHAR KIRQL, *PKIRQL;
typedef CCHAR KPROCESSOR_MODE;
typedef ULONG_PTR ERESOURCE_THREAD;

#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define FIELD_OFFSET(t, f) ((LONG)offsetof(t, f))
#define CONTAINING_RECORD(a, t, f) ((t *)((PUCHAR)(a) - offsetof(t, f)))
#define RtlZeroMemory(d, l) memset((d), 0, (l))
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))
#define RtlMoveMemory(d, s, l) memmove((d), (s), (l))
#define KernelMode 0
#define UserMode 1

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
#define InitializeListHead(h) ((h)->Flink = (h)->Blink = (h))
#define IsListEmpty(h) ((h)->Flink == (h))
static inline void RemoveEntryList(PLIST_ENTRY e) { PLIST_ENTRY b = e->Blink, f = e->Flink; b->Flink = f; f->Blink = b; }
static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e) { PLIST_ENTRY b = h->Blink; e->Flink = h; e->Blink = b; b->Flink = e; h->Blink = e; }

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _STRING { USHORT Length, MaximumLength; PCHAR Buffer; } STRING, *PSTRING;

typedef struct _TIME_FIELDS { CSHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday; } TIME_FIELDS, *PTIME_FIELDS;

#include "fakestat.h"

#define EXF_MAXT 4
typedef struct _ERESOURCE { int Shared; int Exclusive; int Initialized; int S[EXF_MAXT], X[EXF_MAXT]; } ERESOURCE, *PERESOURCE;
typedef struct _KEVENT { int Signaled; } KEVENT, *PKEVENT;
typedef struct _KTHREAD { int Dummy; } KTHREAD, *PKTHREAD, *PETHREAD;
typedef struct _EPROCESS { int Dummy; } EPROCESS, *PEPROCESS;
typedef struct _FILE_LOCK { int Locks; BOOLEAN FastIoIsQuestionable; } FILE_LOCK, *PFILE_LOCK;
typedef struct _SHARE_ACCESS { ULONG OpenCount, Readers, Writers, Deleters, SharedRead, SharedWrite, SharedDelete; } SHARE_ACCESS, *PSHARE_ACCESS;
typedef struct _SECTION_OBJECT_POINTERS { PVOID DataSectionObject, SharedCacheMap, ImageSectionObject; } SECTION_OBJECT_POINTERS, *PSECTION_OBJECT_POINTERS;
typedef PVOID PNOTIFY_SYNC;

typedef enum { FastIoIsNotPossible = 0, FastIoIsPossible, FastIoIsQuestionable } FAST_IO_POSSIBLE;

#define FSRTL_FLAG_USER_MAPPED_FILE 0x20
typedef struct _FSRTL_COMMON_FCB_HEADER {
    CSHORT NodeTypeCode; CSHORT NodeByteSize; UCHAR Flags; UCHAR IsFastIoPossible; UCHAR Flags2; UCHAR Reserved;
    PERESOURCE Resource; PERESOURCE PagingIoResource;
    LARGE_INTEGER AllocationSize, FileSize, ValidDataLength;
} FSRTL_COMMON_FCB_HEADER;

typedef struct _CC_FILE_SIZES { LARGE_INTEGER AllocationSize, FileSize, ValidDataLength; } CC_FILE_SIZES, *PCC_FILE_SIZES;

typedef BOOLEAN (*PACQUIRE_FOR_LAZY_WRITE)(PVOID, BOOLEAN);
typedef VOID (*PRELEASE_FROM_LAZY_WRITE)(PVOID);
typedef BOOLEAN (*PACQUIRE_FOR_READ_AHEAD)(PVOID, BOOLEAN);
typedef VOID (*PRELEASE_FROM_READ_AHEAD)(PVOID);
typedef struct _CACHE_MANAGER_CALLBACKS {
    PACQUIRE_FOR_LAZY_WRITE AcquireForLazyWrite; PRELEASE_FROM_LAZY_WRITE ReleaseFromLazyWrite;
    PACQUIRE_FOR_READ_AHEAD AcquireForReadAhead; PRELEASE_FROM_READ_AHEAD ReleaseFromReadAhead;
} CACHE_MANAGER_CALLBACKS, *PCACHE_MANAGER_CALLBACKS;
#define FSRTL_CACHE_TOP_LEVEL_IRP 2

struct _DEVICE_OBJECT; struct _IRP; struct _FILE_OBJECT; struct _DRIVER_OBJECT;

typedef struct _VPB {
    CSHORT Type, Size; USHORT Flags; USHORT VolumeLabelLength;
    struct _DEVICE_OBJECT *DeviceObject, *RealDevice;
    ULONG SerialNumber; ULONG ReferenceCount; WCHAR VolumeLabel[32];
} VPB, *PVPB;
#define VPB_MOUNTED 1
#define VPB_LOCKED 2

typedef NTSTATUS (*PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *, struct _IRP *);

typedef struct _DEVICE_OBJECT {
    CSHORT Type; USHORT Size; LONG ReferenceCount;
    struct _DRIVER_OBJECT *DriverObject;
    ULONG Flags; ULONG Characteristics; PVPB Vpb; PVOID DeviceExtension;
    DEVICE_TYPE DeviceType; CCHAR StackSize; ULONG AlignmentRequirement; USHORT SectorSize;
    struct _DEVICE_OBJECT *AttachedDevice;
    int Deleted;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
#define DO_VERIFY_VOLUME 0x2
#define DO_DEVICE_INITIALIZING 0x80
#define FILE_REMOVABLE_MEDIA 0x1

typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _FILE_OBJECT {
    CSHORT Type, Size; PDEVICE_OBJECT DeviceObject; PVPB Vpb;
    PVOID FsContext, FsContext2; PSECTION_OBJECT_POINTERS SectionObjectPointer; PVOID PrivateCacheMap;
    struct _FILE_OBJECT *RelatedFileObject;
    BOOLEAN ReadAccess, WriteAccess, DeleteAccess, SharedRead, SharedWrite, SharedDelete;
    BOOLEAN DeletePending;
    ULONG Flags; UNICODE_STRING FileName; LARGE_INTEGER CurrentByteOffset;
    int RefCount; int Closed;
} FILE_OBJECT, *PFILE_OBJECT;
#define FO_CACHE_SUPPORTED 0x4
#define FO_SYNCHRONOUS_IO 0x2
#define FO_NO_INTERMEDIATE_BUFFERING 0x8
#define FO_WRITE_THROUGH 0x10
#define FO_STREAM_FILE 0x100
#define FO_FILE_MODIFIED 0x1000
#define FO_FILE_SIZE_CHANGED 0x2000
#define FO_CLEANUP_COMPLETE 0x4000

typedef struct _MDL { struct _MDL *Next; CSHORT Size; CSHORT MdlFlags; PVOID MappedSystemVa; PVOID StartVa; ULONG ByteCount; ULONG ByteOffset; int Locked; } MDL, *PMDL;
#define MmGetMdlVirtualAddress(m) ((PVOID)((PUCHAR)(m)->StartVa + (m)->ByteOffset))
#define MmGetSystemAddressForMdlSafe(m, p) MmGetMdlVirtualAddress(m)
#define MmGetSystemAddressForMdl(m) MmGetMdlVirtualAddress(m)
typedef enum { IoReadAccess, IoWriteAccess, IoModifyAccess } LOCK_OPERATION;
#define NormalPagePriority 16

typedef enum _FILE_INFORMATION_CLASS {
    FileDirectoryInformation = 1, FileFullDirectoryInformation, FileBothDirectoryInformation, FileBasicInformation,
    FileStandardInformation, FileInternalInformation, FileEaInformation, FileAccessInformation, FileNameInformation,
    FileRenameInformation, FileLinkInformation, FileNamesInformation, FileDispositionInformation, FilePositionInformation,
    FileFullEaInformation, FileModeInformation, FileAlignmentInformation, FileAllInformation, FileAllocationInformation,
    FileEndOfFileInformation, FileAlternateNameInformation, FileStreamInformation, FilePipeInformation,
    FilePipeLocalInformation, FilePipeRemoteInformation, FileMailslotQueryInformation, FileMailslotSetInformation,
    FileCompressionInformation, FileObjectIdInformation, FileCompletionInformation, FileMoveClusterInformation,
    FileQuotaInformation, FileReparsePointInformation, FileNetworkOpenInformation, FileAttributeTagInformation
} FILE_INFORMATION_CLASS;
typedef enum { FileFsVolumeInformation = 1, FileFsLabelInformation, FileFsSizeInformation, FileFsDeviceInformation,
    FileFsAttributeInformation, FileFsControlInformation, FileFsFullSizeInformation } FS_INFORMATION_CLASS;

typedef struct _ACCESS_STATE { ACCESS_MASK RemainingDesiredAccess, PreviouslyGrantedAccess; } ACCESS_STATE, *PACCESS_STATE;
typedef struct _IO_SECURITY_CONTEXT { PVOID SecurityQos; PACCESS_STATE AccessState; ACCESS_MASK DesiredAccess; ULONG FullCreateOptions; } IO_SECURITY_CONTEXT, *PIO_SECURITY_CONTEXT;

typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT, struct _IRP *, PVOID);

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction, MinorFunction, Flags, Control;
    union {
        struct { PIO_SECURITY_CONTEXT SecurityContext; ULONG Options; USHORT FileAttributes; USHORT ShareAccess; ULONG EaLength; } Create;
        struct { ULONG Length; ULONG Key; LARGE_INTEGER ByteOffset; } Read;
        struct { ULONG Length; ULONG Key; LARGE_INTEGER ByteOffset; } Write;
        struct { ULONG Length; PUNICODE_STRING FileName; FILE_INFORMATION_CLASS FileInformationClass; ULONG FileIndex; } QueryDirectory;
        struct { ULONG Length; ULONG CompletionFilter; } NotifyDirectory;
        struct { ULONG Length; FILE_INFORMATION_CLASS FileInformationClass; } QueryFile;
        struct { ULONG Length; FILE_INFORMATION_CLASS FileInformationClass; PFILE_OBJECT FileObject; union { struct { BOOLEAN ReplaceIfExists; BOOLEAN AdvanceOnly; }; ULONG ClusterCount; }; } SetFile;
        struct { ULONG Length; FS_INFORMATION_CLASS FsInformationClass; } QueryVolume;
        struct { ULONG OutputBufferLength; ULONG InputBufferLength; ULONG FsControlCode; PVOID Type3InputBuffer; } FileSystemControl;
        struct { ULONG OutputBufferLength; ULONG InputBufferLength; ULONG IoControlCode; PVOID Type3InputBuffer; } DeviceIoControl;
        struct { PVPB Vpb; PDEVICE_OBJECT DeviceObject; } MountVolume;
        struct { PVPB Vpb; PDEVICE_OBJECT DeviceObject; } VerifyVolume;
    } Parameters;
    PDEVICE_OBJECT DeviceObject; PFILE_OBJECT FileObject;
    PIO_COMPLETION_ROUTINE CompletionRoutine; PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

#define IRP_MAX_STACK 10
typedef struct _IRP {
    CSHORT Type; USHORT Size; PMDL MdlAddress; ULONG Flags;
    union { PVOID SystemBuffer; } AssociatedIrp;
    IO_STATUS_BLOCK IoStatus; KPROCESSOR_MODE RequestorMode; BOOLEAN PendingReturned;
    CHAR StackCount; CHAR CurrentLocation;
    PIO_STATUS_BLOCK UserIosb; PKEVENT UserEvent; PVOID UserBuffer;
    struct { struct { PETHREAD Thread; PIO_STACK_LOCATION CurrentStackLocation; PFILE_OBJECT OriginalFileObject; } Overlay; } Tail;
    int AutoFree; int Completed; PMDL OwnMdl;
    IO_STACK_LOCATION Stack[IRP_MAX_STACK];
} IRP, *PIRP;
#define IRP_NOCACHE 0x1
#define IRP_PAGING_IO 0x2
#define IRP_SYNCHRONOUS_PAGING_IO 0x40
#define IoGetCurrentIrpStackLocation(i) ((i)->Tail.Overlay.CurrentStackLocation)
#define IoGetNextIrpStackLocation(i) ((i)->Tail.Overlay.CurrentStackLocation - 1)

typedef struct _EXCEPTION_RECORD { NTSTATUS ExceptionCode; ULONG ExceptionFlags; PVOID ExceptionRecord; PVOID ExceptionAddress; ULONG NumberParameters; ULONG_PTR ExceptionInformation[15]; } EXCEPTION_RECORD;
typedef struct _EXCEPTION_POINTERS { EXCEPTION_RECORD *ExceptionRecord; PVOID ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

typedef BOOLEAN (*PFAST_IO_CHECK_IF_POSSIBLE)(PFILE_OBJECT, PLARGE_INTEGER, ULONG, BOOLEAN, ULONG, BOOLEAN, PIO_STATUS_BLOCK, PDEVICE_OBJECT);
typedef BOOLEAN (*PFAST_IO_READ)(PFILE_OBJECT, PLARGE_INTEGER, ULONG, BOOLEAN, ULONG, PVOID, PIO_STATUS_BLOCK, PDEVICE_OBJECT);
typedef BOOLEAN (*PFAST_IO_WRITE)(PFILE_OBJECT, PLARGE_INTEGER, ULONG, BOOLEAN, ULONG, PVOID, PIO_STATUS_BLOCK, PDEVICE_OBJECT);
struct _FILE_BASIC_INFORMATION; struct _FILE_STANDARD_INFORMATION; struct _FILE_NETWORK_OPEN_INFORMATION;
typedef BOOLEAN (*PFAST_IO_QUERY_BASIC_INFO)(PFILE_OBJECT, BOOLEAN, struct _FILE_BASIC_INFORMATION *, PIO_STATUS_BLOCK, PDEVICE_OBJECT);
typedef BOOLEAN (*PFAST_IO_QUERY_STANDARD_INFO)(PFILE_OBJECT, BOOLEAN, struct _FILE_STANDARD_INFORMATION *, PIO_STATUS_BLOCK, PDEVICE_OBJECT);
typedef BOOLEAN (*PFAST_IO_QUERY_NETWORK_OPEN_INFO)(PFILE_OBJECT, BOOLEAN, struct _FILE_NETWORK_OPEN_INFORMATION *, PIO_STATUS_BLOCK, PDEVICE_OBJECT);
typedef struct _FAST_IO_DISPATCH {
    ULONG SizeOfFastIoDispatch;
    PFAST_IO_CHECK_IF_POSSIBLE FastIoCheckIfPossible;
    PFAST_IO_READ FastIoRead;
    PFAST_IO_WRITE FastIoWrite;
    PFAST_IO_QUERY_BASIC_INFO FastIoQueryBasicInfo;
    PFAST_IO_QUERY_STANDARD_INFO FastIoQueryStandardInfo;
    PFAST_IO_QUERY_NETWORK_OPEN_INFO FastIoQueryNetworkOpenInfo;
} FAST_IO_DISPATCH, *PFAST_IO_DISPATCH;

typedef struct _DRIVER_OBJECT {
    PFAST_IO_DISPATCH FastIoDispatch;
    PDRIVER_DISPATCH MajorFunction[28];
} DRIVER_OBJECT, *PDRIVER_OBJECT;

/* Information structures */
typedef struct _FILE_BASIC_INFORMATION { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime; ULONG FileAttributes; } FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;
typedef struct _FILE_STANDARD_INFORMATION { LARGE_INTEGER AllocationSize, EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending, Directory; } FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;
typedef struct _FILE_INTERNAL_INFORMATION { LARGE_INTEGER IndexNumber; } FILE_INTERNAL_INFORMATION, *PFILE_INTERNAL_INFORMATION;
typedef struct _FILE_EA_INFORMATION { ULONG EaSize; } FILE_EA_INFORMATION, *PFILE_EA_INFORMATION;
typedef struct _FILE_ACCESS_INFORMATION { ACCESS_MASK AccessFlags; } FILE_ACCESS_INFORMATION;
typedef struct _FILE_POSITION_INFORMATION { LARGE_INTEGER CurrentByteOffset; } FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;
typedef struct _FILE_MODE_INFORMATION { ULONG Mode; } FILE_MODE_INFORMATION;
typedef struct _FILE_ALIGNMENT_INFORMATION { ULONG AlignmentRequirement; } FILE_ALIGNMENT_INFORMATION;
typedef struct _FILE_NAME_INFORMATION { ULONG FileNameLength; WCHAR FileName[1]; } FILE_NAME_INFORMATION, *PFILE_NAME_INFORMATION;
typedef struct _FILE_ALL_INFORMATION {
    FILE_BASIC_INFORMATION BasicInformation; FILE_STANDARD_INFORMATION StandardInformation; FILE_INTERNAL_INFORMATION InternalInformation;
    FILE_EA_INFORMATION EaInformation; FILE_ACCESS_INFORMATION AccessInformation; FILE_POSITION_INFORMATION PositionInformation;
    FILE_MODE_INFORMATION ModeInformation; FILE_ALIGNMENT_INFORMATION AlignmentInformation; FILE_NAME_INFORMATION NameInformation;
} FILE_ALL_INFORMATION, *PFILE_ALL_INFORMATION;
typedef struct _FILE_NETWORK_OPEN_INFORMATION { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, AllocationSize, EndOfFile; ULONG FileAttributes; } FILE_NETWORK_OPEN_INFORMATION, *PFILE_NETWORK_OPEN_INFORMATION;
typedef struct _FILE_DIRECTORY_INFORMATION { ULONG NextEntryOffset, FileIndex; LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize; ULONG FileAttributes, FileNameLength; WCHAR FileName[1]; } FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;
typedef struct _FILE_FULL_DIR_INFORMATION { ULONG NextEntryOffset, FileIndex; LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize; ULONG FileAttributes, FileNameLength, EaSize; WCHAR FileName[1]; } FILE_FULL_DIR_INFORMATION, *PFILE_FULL_DIR_INFORMATION;
typedef struct _FILE_BOTH_DIR_INFORMATION { ULONG NextEntryOffset, FileIndex; LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize; ULONG FileAttributes, FileNameLength, EaSize; CCHAR ShortNameLength; WCHAR ShortName[12]; WCHAR FileName[1]; } FILE_BOTH_DIR_INFORMATION, *PFILE_BOTH_DIR_INFORMATION;
typedef struct _FILE_NAMES_INFORMATION { ULONG NextEntryOffset, FileIndex, FileNameLength; WCHAR FileName[1]; } FILE_NAMES_INFORMATION, *PFILE_NAMES_INFORMATION;
typedef struct _FILE_FS_VOLUME_INFORMATION { LARGE_INTEGER VolumeCreationTime; ULONG VolumeSerialNumber, VolumeLabelLength; BOOLEAN SupportsObjects; WCHAR VolumeLabel[1]; } FILE_FS_VOLUME_INFORMATION, *PFILE_FS_VOLUME_INFORMATION;
typedef struct _FILE_FS_SIZE_INFORMATION { LARGE_INTEGER TotalAllocationUnits, AvailableAllocationUnits; ULONG SectorsPerAllocationUnit, BytesPerSector; } FILE_FS_SIZE_INFORMATION, *PFILE_FS_SIZE_INFORMATION;
typedef struct _FILE_FS_DEVICE_INFORMATION { DEVICE_TYPE DeviceType; ULONG Characteristics; } FILE_FS_DEVICE_INFORMATION, *PFILE_FS_DEVICE_INFORMATION;
typedef struct _FILE_FS_LABEL_INFORMATION { ULONG VolumeLabelLength; WCHAR VolumeLabel[1]; } FILE_FS_LABEL_INFORMATION;
typedef struct _FILE_RENAME_INFORMATION { BOOLEAN ReplaceIfExists; PVOID RootDirectory; ULONG FileNameLength; WCHAR FileName[1]; } FILE_RENAME_INFORMATION, *PFILE_RENAME_INFORMATION;
typedef struct _FILE_DISPOSITION_INFORMATION { BOOLEAN DeleteFile; } FILE_DISPOSITION_INFORMATION, *PFILE_DISPOSITION_INFORMATION;
typedef struct _FILE_END_OF_FILE_INFORMATION { LARGE_INTEGER EndOfFile; } FILE_END_OF_FILE_INFORMATION, *PFILE_END_OF_FILE_INFORMATION;
typedef struct _FILE_ALLOCATION_INFORMATION { LARGE_INTEGER AllocationSize; } FILE_ALLOCATION_INFORMATION, *PFILE_ALLOCATION_INFORMATION;
typedef struct _FILE_FS_ATTRIBUTE_INFORMATION { ULONG FileSystemAttributes; LONG MaximumComponentNameLength; ULONG FileSystemNameLength; WCHAR FileSystemName[1]; } FILE_FS_ATTRIBUTE_INFORMATION, *PFILE_FS_ATTRIBUTE_INFORMATION;

typedef struct _DISK_GEOMETRY { LARGE_INTEGER Cylinders; ULONG MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector; } DISK_GEOMETRY;
typedef struct _PARTITION_INFORMATION { LARGE_INTEGER StartingOffset, PartitionLength; ULONG HiddenSectors, PartitionNumber; UCHAR PartitionType; BOOLEAN BootIndicator, RecognizedPartition, RewritePartition; } PARTITION_INFORMATION;

/* Constants */
#define IRP_MJ_CREATE 0x00
#define IRP_MJ_CLOSE 0x02
#define IRP_MJ_READ 0x03
#define IRP_MJ_WRITE 0x04
#define IRP_MJ_QUERY_INFORMATION 0x05
#define IRP_MJ_SET_INFORMATION 0x06
#define IRP_MJ_QUERY_EA 0x07
#define IRP_MJ_SET_EA 0x08
#define IRP_MJ_FLUSH_BUFFERS 0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0a
#define IRP_MJ_SET_VOLUME_INFORMATION 0x0b
#define IRP_MJ_DIRECTORY_CONTROL 0x0c
#define IRP_MJ_FILE_SYSTEM_CONTROL 0x0d
#define IRP_MJ_DEVICE_CONTROL 0x0e
#define IRP_MJ_LOCK_CONTROL 0x11
#define IRP_MJ_CLEANUP 0x12
#define IRP_MJ_PNP 0x1b
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b
#define IRP_MN_NORMAL 0
#define IRP_MN_MDL 2
#define IRP_MN_COMPLETE 4
#define IRP_MN_QUERY_DIRECTORY 1
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY 2
#define IRP_MN_USER_FS_REQUEST 0
#define IRP_MN_MOUNT_VOLUME 1
#define IRP_MN_VERIFY_VOLUME 2
#define IRP_MN_KERNEL_CALL 4
#define IRP_MN_QUERY_REMOVE_DEVICE 1
#define IRP_MN_REMOVE_DEVICE 2
#define IRP_MN_CANCEL_REMOVE_DEVICE 3
#define IRP_MN_SURPRISE_REMOVAL 0x17
#define SL_OPEN_PAGING_FILE 0x2
#define SL_OPEN_TARGET_DIRECTORY 0x4
#define SL_OVERRIDE_VERIFY_VOLUME 0x2
#define SL_RESTART_SCAN 0x1
#define SL_RETURN_SINGLE_ENTRY 0x2
#define SL_INDEX_SPECIFIED 0x4
#define SL_WATCH_TREE 0x1
#define IO_NO_INCREMENT 0
#define IO_DISK_INCREMENT 1
#define FILE_SUPERSEDE 0
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define FILE_OVERWRITE_IF 5
#define FILE_SUPERSEDED 0
#define FILE_OPENED 1
#define FILE_CREATED 2
#define FILE_OVERWRITTEN 3
#define FILE_EXISTS 4
#define FILE_DOES_NOT_EXIST 5
#define FILE_WRITE_TO_END_OF_FILE 0xffffffff
#define FILE_DIRECTORY_FILE 0x1
#define FILE_NON_DIRECTORY_FILE 0x40
#define FILE_DELETE_ON_CLOSE 0x1000
#define FILE_OPEN_BY_FILE_ID 0x2000
#define FILE_READ_DATA 0x1
#define FILE_WRITE_DATA 0x2
#define FILE_APPEND_DATA 0x4
#define FILE_READ_EA 0x8
#define FILE_WRITE_EA 0x10
#define FILE_EXECUTE 0x20
#define FILE_DELETE_CHILD 0x40
#define FILE_READ_ATTRIBUTES 0x80
#define FILE_WRITE_ATTRIBUTES 0x100
#define DELETE 0x10000
#define READ_CONTROL 0x20000
#define STANDARD_RIGHTS_REQUIRED 0xF0000
#define SYNCHRONIZE 0x100000
#define MAXIMUM_ALLOWED 0x2000000
#define FILE_GENERIC_READ (READ_CONTROL | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA | SYNCHRONIZE)
#define FILE_GENERIC_EXECUTE (READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_EXECUTE | SYNCHRONIZE)
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_SYSTEM 0x4
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_NOTIFY_CHANGE_FILE_NAME 0x1
#define FILE_NOTIFY_CHANGE_DIR_NAME 0x2
#define FILE_NOTIFY_CHANGE_ATTRIBUTES 0x4
#define FILE_NOTIFY_CHANGE_SIZE 0x8
#define FILE_NOTIFY_CHANGE_LAST_WRITE 0x10
#define FILE_NOTIFY_CHANGE_LAST_ACCESS 0x20
#define FILE_NOTIFY_CHANGE_CREATION 0x40
#define FILE_ACTION_ADDED 1
#define FILE_ACTION_REMOVED 2
#define FILE_ACTION_MODIFIED 3
#define FILE_ACTION_RENAMED_OLD_NAME 4
#define FILE_ACTION_RENAMED_NEW_NAME 5
#define FILE_CASE_PRESERVED_NAMES 2
#define FILE_UNICODE_ON_DISK 4
#define FILE_READ_ONLY_VOLUME 0x80000
#define FILE_DEVICE_DISK 7
#define FILE_DEVICE_DISK_FILE_SYSTEM 8
#define FILE_DEVICE_FILE_SYSTEM 9
#define FILE_ANY_ACCESS 0
#define METHOD_BUFFERED 0
#define CTL_CODE(d, f, m, a) (((d) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define FSCTL_REQUEST_OPLOCK_LEVEL_1 CTL_CODE(9, 0, 0, 0)
#define FSCTL_REQUEST_OPLOCK_LEVEL_2 CTL_CODE(9, 1, 0, 0)
#define FSCTL_REQUEST_BATCH_OPLOCK CTL_CODE(9, 2, 0, 0)
#define FSCTL_OPLOCK_BREAK_ACKNOWLEDGE CTL_CODE(9, 3, 0, 0)
#define FSCTL_OPBATCH_ACK_CLOSE_PENDING CTL_CODE(9, 4, 0, 0)
#define FSCTL_OPLOCK_BREAK_NOTIFY CTL_CODE(9, 5, 0, 0)
#define FSCTL_LOCK_VOLUME CTL_CODE(9, 6, 0, 0)
#define FSCTL_UNLOCK_VOLUME CTL_CODE(9, 7, 0, 0)
#define FSCTL_DISMOUNT_VOLUME CTL_CODE(9, 8, 0, 0)
#define FSCTL_IS_VOLUME_MOUNTED CTL_CODE(9, 10, 0, 0)
#define FSCTL_IS_PATHNAME_VALID CTL_CODE(9, 11, 0, 0)
#define FSCTL_MARK_VOLUME_DIRTY CTL_CODE(9, 12, 0, 0)
#define FSCTL_OPLOCK_BREAK_ACK_NO_2 CTL_CODE(9, 20, 0, 0)
#define FSCTL_REQUEST_FILTER_OPLOCK CTL_CODE(9, 23, 0, 0)
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x70000
#define IOCTL_DISK_GET_PARTITION_INFO 0x74004
#define IOCTL_DISK_BASE 7
#define IOCTL_DISK_IS_WRITABLE 0x70024
#define FSCTL_IS_VOLUME_DIRTY CTL_CODE(9, 30, 0, 0)
#define IRP_MJ_SHUTDOWN 0x10
#define FSRTL_VOLUME_MOUNT 6
#define MDL_MAPPED_TO_SYSTEM_VA 1
#define MDL_SOURCE_IS_NONPAGED_POOL 4
typedef enum { NonPagedPool = 0, PagedPool = 1 } POOL_TYPE;
typedef VOID (*PWORKER_THREAD_ROUTINE)(PVOID);
typedef struct _WORK_QUEUE_ITEM { LIST_ENTRY List; PWORKER_THREAD_ROUTINE WorkerRoutine; PVOID Parameter; } WORK_QUEUE_ITEM, *PWORK_QUEUE_ITEM;
typedef enum { CriticalWorkQueue, DelayedWorkQueue } WORK_QUEUE_TYPE;
#define ExInitializeWorkItem(i, r, p) ((i)->WorkerRoutine = (r), (i)->Parameter = (p), (i)->List.Flink = NULL)
typedef enum { MmFlushForDelete, MmFlushForWrite } MMFLUSH_TYPE;
typedef enum { NotificationEvent, SynchronizationEvent } EVENT_TYPE;
#define Executive 0

/* Functions */
#define FsRtlEnterFileSystem() KeEnterCriticalRegion()
#define FsRtlExitFileSystem() KeLeaveCriticalRegion()
#define ExGetCurrentResourceThread() ((ERESOURCE_THREAD)1)
#define PsGetCurrentThread() KeGetCurrentThread()
#define FsRtlAreThereCurrentFileLocks(l) ((l)->FastIoIsQuestionable)
#define IoCallDriver IofCallDriver
#define IoCompleteRequest IofCompleteRequest
#define ObDereferenceObject ObfDereferenceObject
#define ObReferenceObject ObfReferenceObject
#define IoSetCompletionRoutine(irp, r, c, s, e, a) do { PIO_STACK_LOCATION _n = IoGetNextIrpStackLocation(irp); _n->CompletionRoutine = (r); _n->Context = (c); } while (0)

void KeEnterCriticalRegion(void); void KeLeaveCriticalRegion(void);
PKTHREAD KeGetCurrentThread(void);
PVOID ExAllocatePoolWithTag(POOL_TYPE t, size_t n, ULONG tag);
void ExFreePool(PVOID p);
void ExInitializeResourceLite(PERESOURCE r); void ExDeleteResourceLite(PERESOURCE r);
BOOLEAN ExAcquireResourceSharedLite(PERESOURCE r, BOOLEAN w); BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE r, BOOLEAN w);
void ExReleaseResourceForThreadLite(PERESOURCE r, ERESOURCE_THREAD t);
void ExRaiseStatus(NTSTATUS s) __attribute__((noreturn));
void ExLocalTimeToSystemTime(PLARGE_INTEGER a, PLARGE_INTEGER b);
BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS f, PLARGE_INTEGER t);
void RtlInitUnicodeString(PUNICODE_STRING s, const WCHAR *w);
NTSTATUS RtlUpcaseUnicodeString(PUNICODE_STRING d, PUNICODE_STRING s, BOOLEAN a);
NTSTATUS IofCallDriver(PDEVICE_OBJECT d, PIRP i);
void IofCompleteRequest(PIRP i, CCHAR b);
void ObfDereferenceObject(PVOID o); void ObfReferenceObject(PVOID o);
NTSTATUS ObReferenceObjectByPointer(PVOID o, ACCESS_MASK a, PVOID t, KPROCESSOR_MODE m);
NTSTATUS IoCreateDevice(PDRIVER_OBJECT d, ULONG ext, PUNICODE_STRING n, DEVICE_TYPE t, ULONG c, BOOLEAN e, PDEVICE_OBJECT *o);
void IoDeleteDevice(PDEVICE_OBJECT d);
void IoRegisterFileSystem(PDEVICE_OBJECT d);
PFILE_OBJECT IoCreateStreamFileObject(PFILE_OBJECT f, PDEVICE_OBJECT d);
PIRP IoAllocateIrp(CCHAR s, BOOLEAN q); void IoFreeIrp(PIRP i);
PMDL IoAllocateMdl(PVOID va, ULONG len, BOOLEAN sec, BOOLEAN q, PIRP i); void IoFreeMdl(PMDL m);
void IoBuildPartialMdl(PMDL s, PMDL t, PVOID va, ULONG len);
void MmProbeAndLockPages(PMDL m, KPROCESSOR_MODE mode, LOCK_OPERATION op);
PIRP IoBuildSynchronousFsdRequest(ULONG mj, PDEVICE_OBJECT d, PVOID b, ULONG l, PLARGE_INTEGER o, PKEVENT e, PIO_STATUS_BLOCK s);
PIRP IoBuildDeviceIoControlRequest(ULONG c, PDEVICE_OBJECT d, PVOID in, ULONG il, PVOID out, ULONG ol, BOOLEAN internal, PKEVENT e, PIO_STATUS_BLOCK s);
void KeInitializeEvent(PKEVENT e, EVENT_TYPE t, BOOLEAN s);
LONG KeSetEvent(PKEVENT e, LONG inc, BOOLEAN w);
NTSTATUS KeWaitForSingleObject(PVOID o, int r, int m, BOOLEAN a, PLARGE_INTEGER t);
PIRP IoGetTopLevelIrp(void); void IoSetTopLevelIrp(PIRP i);
PEPROCESS IoGetCurrentProcess(void); PEPROCESS IoGetRequestorProcess(PIRP i);
PDEVICE_OBJECT IoGetDeviceToVerify(PETHREAD t); void IoSetDeviceToVerify(PETHREAD t, PDEVICE_OBJECT d);
void IoSetHardErrorOrVerifyDevice(PIRP i, PDEVICE_OBJECT d);
NTSTATUS IoVerifyVolume(PDEVICE_OBJECT d, BOOLEAN r);
void IoAcquireVpbSpinLock(PKIRQL i); void IoReleaseVpbSpinLock(KIRQL i);
NTSTATUS IoCheckShareAccess(ACCESS_MASK a, ULONG s, PFILE_OBJECT f, PSHARE_ACCESS sa, BOOLEAN u);
void IoSetShareAccess(ACCESS_MASK a, ULONG s, PFILE_OBJECT f, PSHARE_ACCESS sa);
void IoRemoveShareAccess(PFILE_OBJECT f, PSHARE_ACCESS sa);
void CcInitializeCacheMap(PFILE_OBJECT f, PCC_FILE_SIZES s, BOOLEAN p, PCACHE_MANAGER_CALLBACKS c, PVOID ctx);
BOOLEAN CcUninitializeCacheMap(PFILE_OBJECT f, PLARGE_INTEGER t, PVOID e);
BOOLEAN CcCopyRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, PVOID b, PIO_STATUS_BLOCK s);
void CcMdlRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, PMDL *m, PIO_STATUS_BLOCK s);
void CcMdlReadComplete(PFILE_OBJECT f, PMDL m);
BOOLEAN CcMapData(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, ULONG w, PVOID *bcb, PVOID *buf);
void CcUnpinData(PVOID bcb);
BOOLEAN CcPurgeCacheSection(PSECTION_OBJECT_POINTERS s, PLARGE_INTEGER o, ULONG l, BOOLEAN u);
BOOLEAN CcPinRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, ULONG w, PVOID *bcb, PVOID *buf);
BOOLEAN CcPreparePinWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN z, ULONG w, PVOID *bcb, PVOID *buf);
void CcSetDirtyPinnedData(PVOID bcb, PLARGE_INTEGER lsn);
BOOLEAN CcCopyWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, PVOID b);
BOOLEAN CcZeroData(PFILE_OBJECT f, PLARGE_INTEGER s, PLARGE_INTEGER e, BOOLEAN w);
BOOLEAN CcCanIWrite(PFILE_OBJECT f, ULONG n, BOOLEAN w, BOOLEAN r);
void CcPrepareMdlWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, PMDL *m, PIO_STATUS_BLOCK s);
void CcMdlWriteComplete(PFILE_OBJECT f, PLARGE_INTEGER o, PMDL m);
void CcSetFileSizes(PFILE_OBJECT f, PCC_FILE_SIZES s);
void CcFlushCache(PSECTION_OBJECT_POINTERS s, PLARGE_INTEGER o, ULONG l, PIO_STATUS_BLOCK io);
BOOLEAN MmCanFileBeTruncated(PSECTION_OBJECT_POINTERS s, PLARGE_INTEGER n);
BOOLEAN MmFlushImageSection(PSECTION_OBJECT_POINTERS s, MMFLUSH_TYPE t);
BOOLEAN FsRtlCopyWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, ULONG k, PVOID b, PIO_STATUS_BLOCK s, PDEVICE_OBJECT d);
BOOLEAN FsRtlCheckLockForWriteAccess(PFILE_LOCK l, PIRP i);
BOOLEAN FsRtlFastCheckLockForWrite(PFILE_LOCK l, PLARGE_INTEGER o, PLARGE_INTEGER n, ULONG k, PFILE_OBJECT f, PVOID p);
void FsRtlNotifyFullReportChange(PNOTIFY_SYNC s, PLIST_ENTRY l, PSTRING n, USHORT off, PSTRING st, PSTRING np, ULONG f, ULONG a, PVOID c);
NTSTATUS IoRegisterShutdownNotification(PDEVICE_OBJECT d);
void ExQueueWorkItem(PWORK_QUEUE_ITEM i, WORK_QUEUE_TYPE t);
void KeQuerySystemTime(PLARGE_INTEGER t);
void ExSystemTimeToLocalTime(PLARGE_INTEGER a, PLARGE_INTEGER b);
void RtlTimeToTimeFields(PLARGE_INTEGER t, PTIME_FIELDS f);
size_t RtlCompareMemory(const void *a, const void *b, size_t n);
BOOLEAN MmForceSectionClosed(PSECTION_OBJECT_POINTERS s, BOOLEAN d);
BOOLEAN FsRtlIsNtstatusExpected(NTSTATUS s); NTSTATUS FsRtlNormalizeNtstatus(NTSTATUS s, NTSTATUS d);
BOOLEAN FsRtlIsNameInExpression(PUNICODE_STRING e, PUNICODE_STRING n, BOOLEAN i, PWCHAR);
BOOLEAN FsRtlDoesNameContainWildCards(PUNICODE_STRING n);
void FsRtlInitializeFileLock(PFILE_LOCK l, PVOID a, PVOID b); void FsRtlUninitializeFileLock(PFILE_LOCK l);
BOOLEAN FsRtlCheckLockForReadAccess(PFILE_LOCK l, PIRP i);
BOOLEAN FsRtlFastCheckLockForRead(PFILE_LOCK l, PLARGE_INTEGER o, PLARGE_INTEGER n, ULONG k, PFILE_OBJECT f, PVOID p);
NTSTATUS FsRtlFastUnlockAll(PFILE_LOCK l, PFILE_OBJECT f, PEPROCESS p, PVOID c);
NTSTATUS FsRtlProcessFileLock(PFILE_LOCK l, PIRP i, PVOID c);
BOOLEAN FsRtlCopyRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, ULONG k, PVOID b, PIO_STATUS_BLOCK s, PDEVICE_OBJECT d);
void FsRtlNotifyInitializeSync(PNOTIFY_SYNC *s); void FsRtlNotifyUninitializeSync(PNOTIFY_SYNC *s);
void FsRtlNotifyCleanup(PNOTIFY_SYNC s, PLIST_ENTRY l, PVOID c);
void FsRtlNotifyFullChangeDirectory(PNOTIFY_SYNC s, PLIST_ENTRY l, PVOID c, PSTRING n, BOOLEAN w, BOOLEAN i, ULONG f, PIRP irp, PVOID t, PVOID sc);
NTSTATUS FsRtlNotifyVolumeEvent(PFILE_OBJECT f, ULONG e);
ULONG DbgPrint(const char *f, ...);


/* Registry queries: the fake reads values from the environment (REG_<name>) */
#define RTL_REGISTRY_ABSOLUTE 0
#define RTL_QUERY_REGISTRY_DIRECT 0x20
#define REG_DWORD 4
typedef NTSTATUS (*PRTL_QUERY_REGISTRY_ROUTINE)(PWSTR, ULONG, PVOID, ULONG, PVOID, PVOID);
typedef struct _RTL_QUERY_REGISTRY_TABLE {
    PRTL_QUERY_REGISTRY_ROUTINE QueryRoutine; ULONG Flags; PWSTR Name; PVOID EntryContext;
    ULONG DefaultType; PVOID DefaultData; ULONG DefaultLength;
} RTL_QUERY_REGISTRY_TABLE, *PRTL_QUERY_REGISTRY_TABLE;
typedef const WCHAR *PCWSTR;
NTSTATUS RtlQueryRegistryValues(ULONG RelativeTo, PCWSTR Path, PRTL_QUERY_REGISTRY_TABLE Table, PVOID Context, PVOID Environment);
#endif
