/*
 * FATNT - IRP_MJ_DEVICE_CONTROL and IRP_MJ_LOCK_CONTROL
 */

#include "fat.h"

/* Device requests on any of our handles go to the disk */
NTSTATUS
FatCommonDeviceControl (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;
    PFAT_VCB Vcb = Ctx->Vcb;

    if (FileObject->FsContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* After a removal the target device may be gone */
    if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) &&
        (Ccb == NULL || !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME))) {

        return FAT_STATUS_DISMOUNTED;
    }

    FatSkipStack(Ctx->Irp);
    Ctx->Flags |= FAT_CTX_NO_COMPLETE;

    return IoCallDriver(Vcb->TargetDeviceObject, Ctx->Irp);
}

NTSTATUS
FatCommonLockControl (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_FCB Fcb = (PFAT_FCB)Ctx->IrpSp->FileObject->FsContext;
    NTSTATUS Status;

    if (Fcb == NULL || !FatIsFcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = FatVerifyVcb(Ctx, Fcb->Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);

    __try {

        /* Completes the IRP itself */
        Status = FsRtlProcessFileLock(&Fcb->FileLock, Ctx->Irp, NULL);
        Ctx->Flags |= FAT_CTX_NO_COMPLETE;

        Fcb->Header.IsFastIoPossible = FatIsFastIoPossible(Fcb);

    } __finally {

        FatRelease(&Fcb->Resource);
    }

    return Status;
}
