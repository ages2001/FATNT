/*
 * FATNT - IRP_MJ_READ
 */

#include "fat.h"

/* DASD reads of the whole volume */
static NTSTATUS
FatReadVolume (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Vfcb,
    PFAT_CCB Ccb,
    LONGLONG StartingByte,
    ULONG ByteCount
    )
{
    PFAT_VCB Vcb = Vfcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    LONGLONG Limit;
    NTSTATUS Status;

    if (Ccb == NULL || (Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* The handle that dismounted or locked the volume keeps working */
    if (!(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME) && Vcb->LockFileObject != FileObject) {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (((ULONG)StartingByte & (Vcb->SectorSize - 1)) != 0 ||
        (ByteCount & (Vcb->SectorSize - 1)) != 0) {

        return STATUS_INVALID_PARAMETER;
    }

    Limit = Vcb->PartitionBytes;

    if (StartingByte >= Limit) {
        return STATUS_END_OF_FILE;
    }

    if (StartingByte + ByteCount > Limit) {
        ByteCount = (ULONG)(Limit - StartingByte);
    }

    FatLockUserBuffer(Irp, IoWriteAccess, ByteCount);

    Status = FatNonCachedIo(Ctx, Vfcb, IRP_MJ_READ, StartingByte, ByteCount, Limit);

    if (NT_SUCCESS(Status)) {

        Irp->IoStatus.Information = ByteCount;

        if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
            FileObject->CurrentByteOffset.QuadPart = StartingByte + ByteCount;
        }
    }

    return Status;
}

/* Paging reads of an internal stream: a directory, the FAT or the bitmap */
static NTSTATUS
FatReadStream (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Fcb,
    LONGLONG StartingVbo,
    ULONG ByteCount
    )
{
    PIRP Irp = Ctx->Irp;
    LONGLONG FileSize;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!(Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);

    __try {

        FileSize = Fcb->Header.FileSize.QuadPart;

        if (StartingVbo >= FileSize) {
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingVbo + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingVbo);
        }

        Status = FatNonCachedIo(Ctx, Fcb, IRP_MJ_READ, StartingVbo,
                                (ULONG)FatRoundUp(ByteCount, Fcb->Vcb->SectorSize), FileSize);

        if (NT_SUCCESS(Status)) {
            Irp->IoStatus.Information = ByteCount;
        }

    } __finally {

        FatRelease(&Fcb->PagingIoResource);
    }

    return Status;
}

static NTSTATUS
FatReadFile (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Fcb,
    LARGE_INTEGER StartingByte,
    ULONG ByteCount
    )
{
    PFAT_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    BOOLEAN PagingIo = (BOOLEAN)((Irp->Flags & IRP_PAGING_IO) != 0);
    BOOLEAN NonCached = (BOOLEAN)((Irp->Flags & IRP_NOCACHE) != 0);
    BOOLEAN Acquired = FALSE;
    BOOLEAN PagingAcquired = FALSE;
    IO_STATUS_BLOCK Iosb;
    LONGLONG FileSize;
    ULONG Transfer;
    PVOID Buffer;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!PagingIo) {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (NonCached &&
            (((ULONG)StartingByte.QuadPart & (Vcb->SectorSize - 1)) != 0 ||
             (ByteCount & (Vcb->SectorSize - 1)) != 0)) {

            return STATUS_INVALID_PARAMETER;
        }
    }

    __try {

        if (PagingIo) {

            (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);
            PagingAcquired = TRUE;

        } else {

            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            Acquired = TRUE;

            if (!FsRtlCheckLockForReadAccess(&Fcb->FileLock, Irp)) {
                Status = STATUS_FILE_LOCK_CONFLICT;
                __leave;
            }

            /* Data written through the cache must reach the disk first */
            if (NonCached && Fcb->SectionObjectPointers.DataSectionObject != NULL) {

                CcFlushCache(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, &Iosb);

                if (!NT_SUCCESS(Iosb.Status)) {
                    Status = Iosb.Status;
                    __leave;
                }
            }
        }

        FileSize = Fcb->Header.FileSize.QuadPart;

        if (StartingByte.QuadPart >= FileSize) {
            Irp->IoStatus.Information = 0;
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingByte.QuadPart + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingByte.QuadPart);
        }

        if (!NonCached) {

            if (FileObject->PrivateCacheMap == NULL) {

                CcInitializeCacheMap(FileObject,
                                     (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                                     FALSE,
                                     &FatData.CacheManagerCallbacks,
                                     Fcb);
            }

            if (IrpSp->MinorFunction & IRP_MN_MDL) {

                CcMdlRead(FileObject, &StartingByte, ByteCount, &Irp->MdlAddress, &Irp->IoStatus);

            } else {

                Buffer = FatMapUserBuffer(Irp);

                if (!CcCopyRead(FileObject, &StartingByte, ByteCount, TRUE, Buffer, &Irp->IoStatus)) {
                    Status = STATUS_UNSUCCESSFUL;
                    __leave;
                }
            }

            Status = Irp->IoStatus.Status;

        } else {

            /* Whole sectors from the disk; the tail past EOF is zeroed */
            Transfer = (ULONG)FatRoundUp(ByteCount, Vcb->SectorSize);

            if (!PagingIo) {
                FatLockUserBuffer(Irp, IoWriteAccess, Transfer);
            }

            Status = FatNonCachedIo(Ctx, Fcb, IRP_MJ_READ, StartingByte.QuadPart, Transfer,
                                    Fcb->Header.ValidDataLength.QuadPart);
        }

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (!PagingIo && (FileObject->Flags & FO_SYNCHRONOUS_IO)) {
                FileObject->CurrentByteOffset.QuadPart = StartingByte.QuadPart + ByteCount;
            }
        }

    } __finally {

        if (Acquired) {
            FatRelease(&Fcb->Resource);
        }

        if (PagingAcquired) {
            FatRelease(&Fcb->PagingIoResource);
        }
    }

    return Status;
}

NTSTATUS
FatCommonRead (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;
    ULONG ByteCount = IrpSp->Parameters.Read.Length;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlReadComplete(FileObject, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        return STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = 0;

    if (ByteCount == 0) {
        return STATUS_SUCCESS;
    }

    if (FatIsVfcb(Fcb)) {
        return FatReadVolume(Ctx, Fcb, Ccb, IrpSp->Parameters.Read.ByteOffset.QuadPart, ByteCount);
    }

    if (Ccb == NULL && (FatIsDcb(Fcb) || FatIsMeta(Fcb))) {
        return FatReadStream(Ctx, Fcb, IrpSp->Parameters.Read.ByteOffset.QuadPart, ByteCount);
    }

    if (!FatIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    return FatReadFile(Ctx, Fcb, IrpSp->Parameters.Read.ByteOffset, ByteCount);
}
