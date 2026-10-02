/*
 * FATNT - fast I/O and cache manager callbacks
 */

#include "fat.h"

UCHAR
FatIsFastIoPossible (
    PFAT_FCB Fcb
    )
{
    if (Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FastIoIsNotPossible;
    }

    if (FsRtlAreThereCurrentFileLocks(&Fcb->FileLock)) {
        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
}

static BOOLEAN
FatFastIoUsable (
    PFAT_FCB Fcb
    )
{
    if (Fcb == NULL || (!FatIsFcb(Fcb) && !FatIsDcb(Fcb))) {
        return FALSE;
    }

    if ((Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Fcb->Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {

        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NTAPI
FatFastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    LARGE_INTEGER LargeLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!FatFastIoUsable(Fcb) || !FatIsFcb(Fcb)) {
        return FALSE;
    }

    LargeLength.QuadPart = Length;

    if (CheckForReadOperation) {
        return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                         FileObject, IoGetCurrentProcess());
    }

    if (Fcb->Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return FALSE;
    }

    return FsRtlFastCheckLockForWrite(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                      FileObject, IoGetCurrentProcess());
}

static BOOLEAN
NTAPI
FatFastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    FILE_BASIC_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!FatFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    FatFillBasicInfo(Fcb, &Info);

    FatRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
FatFastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    FILE_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!FatFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    FatFillStandardInfo(Fcb, &Info);

    FatRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
FatFastQueryNetworkOpenInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_NETWORK_OPEN_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    FILE_NETWORK_OPEN_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!FatFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    FatFillNetworkOpenInfo(Fcb, &Info);

    FatRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    return TRUE;
}

VOID
FatInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    FastIo->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible = FatFastIoCheckIfPossible;
    FastIo->FastIoRead = FsRtlCopyRead;
    FastIo->FastIoWrite = FsRtlCopyWrite;
    FastIo->FastIoQueryBasicInfo = FatFastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = FatFastQueryStandardInfo;
    FastIo->FastIoQueryNetworkOpenInfo = FatFastQueryNetworkOpenInfo;
}

/* ------------------------------------------------------------------ */
/* Cache manager callbacks                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
NTAPI
FatAcquireForLazyWrite (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    /* Paging writes from this thread must not move VDL */
    Fcb->LazyWriteThread = KeGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
FatReleaseFromLazyWrite (
    PVOID Context
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread = NULL;

    FatRelease(&Fcb->PagingIoResource);
    FsRtlExitFileSystem();
}

static BOOLEAN
NTAPI
FatAcquireForReadAhead (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
FatReleaseFromReadAhead (
    PVOID Context
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    FatRelease(&Fcb->Resource);
    FsRtlExitFileSystem();
}

/* Metadata streams need no synchronization with the cache manager */
static BOOLEAN
NTAPI
FatNoOpAcquire (
    PVOID Context,
    BOOLEAN Wait
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Wait);

    return TRUE;
}

static VOID
NTAPI
FatNoOpRelease (
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

VOID
FatInitializeCacheCallbacks (
    VOID
    )
{
    FatData.CacheManagerCallbacks.AcquireForLazyWrite = FatAcquireForLazyWrite;
    FatData.CacheManagerCallbacks.ReleaseFromLazyWrite = FatReleaseFromLazyWrite;
    FatData.CacheManagerCallbacks.AcquireForReadAhead = FatAcquireForReadAhead;
    FatData.CacheManagerCallbacks.ReleaseFromReadAhead = FatReleaseFromReadAhead;

    FatData.MetaCacheCallbacks.AcquireForLazyWrite = FatNoOpAcquire;
    FatData.MetaCacheCallbacks.ReleaseFromLazyWrite = FatNoOpRelease;
    FatData.MetaCacheCallbacks.AcquireForReadAhead = FatNoOpAcquire;
    FatData.MetaCacheCallbacks.ReleaseFromReadAhead = FatNoOpRelease;
}
