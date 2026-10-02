/*
 * FATNT - IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN and the dirty flag
 *
 * FAT16 and FAT32 keep a clean-shutdown bit in the second reserved FAT
 * entry (FAT[1]). It is cleared before the first change to the volume and
 * set again once everything cached has been written: on a volume flush,
 * lock, dismount or shutdown, and on removable media when its last writer
 * closes. FAT12 has no such bit. A volume that was dirty when mounted
 * stays dirty for the check tool.
 */

#include "fat.h"

/* ------------------------------------------------------------------ */
/* The clean-shutdown bit in FAT[1]                                    */
/* ------------------------------------------------------------------ */

/*
 * Reads, changes and writes the first sector of every FAT copy so FAT[1]
 * carries (or loses) the clean-shutdown bit. FAT12 has no such bit.
 */
static NTSTATUS
FatSetCleanBitDisk (
    PFAT_VCB Vcb,
    BOOLEAN Clean
    )
{
    PUCHAR Sector;
    LONGLONG Lbo;
    ULONG Fat;
    ULONG Value;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Vcb->FatType == FAT_TYPE_12) {
        return STATUS_SUCCESS;
    }

    Sector = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->SectorSize, FAT_TAG_BUFFER);
    if (Sector == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Fat = 0; Fat < Vcb->NumberOfFats; Fat++) {

        Lbo = (LONGLONG)(Vcb->ReservedSectors + Fat * Vcb->FatSectors) << Vcb->SectorShift;

        Status = FatReadSectors(Vcb->TargetDeviceObject, Lbo, Vcb->SectorSize, Sector, FALSE);
        if (!NT_SUCCESS(Status)) {
            break;
        }

        if (Vcb->FatType == FAT_TYPE_16) {

            Value = (ULONG)Sector[2] | ((ULONG)Sector[3] << 8);
            if (Clean) {
                Value |= FAT16_CLEAN_SHUTDOWN;
            } else {
                Value &= ~FAT16_CLEAN_SHUTDOWN;
            }
            Sector[2] = (UCHAR)(Value & 0xFF);
            Sector[3] = (UCHAR)((Value >> 8) & 0xFF);

        } else {

            Value = (ULONG)Sector[4] | ((ULONG)Sector[5] << 8) |
                    ((ULONG)Sector[6] << 16) | ((ULONG)Sector[7] << 24);
            if (Clean) {
                Value |= FAT32_CLEAN_SHUTDOWN;
            } else {
                Value &= ~FAT32_CLEAN_SHUTDOWN;
            }
            Sector[4] = (UCHAR)(Value & 0xFF);
            Sector[5] = (UCHAR)((Value >> 8) & 0xFF);
            Sector[6] = (UCHAR)((Value >> 16) & 0xFF);
            Sector[7] = (UCHAR)((Value >> 24) & 0xFF);
        }

        Status = FatWriteSectors(Vcb->TargetDeviceObject, Lbo, Vcb->SectorSize, Sector, FALSE);
        if (!NT_SUCCESS(Status)) {
            break;
        }
    }

    ExFreePool(Sector);

    if (!NT_SUCCESS(Status)) {
        FAT_DBG((FAT_PFX "FAT[1] clean bit update failed %lX\n", Status));
    }

    return Status;
}

/*
 * Keeps the cached FAT's FAT[1] in step with the disk, so a later
 * writeback of the first FAT page does not revert the clean bit.
 */
static VOID
FatSetCleanBitCached (
    PFAT_VCB Vcb,
    BOOLEAN Clean
    )
{
    FAT_MAP Map;
    ULONG Value;

    if (Vcb->FatType == FAT_TYPE_12 || Vcb->FatFcb->StreamFile == NULL) {
        return;
    }

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        Value = FatGetFatEntry(Vcb, &Map, 1);

        if (Vcb->FatType == FAT_TYPE_16) {
            Value = Clean ? (Value | FAT16_CLEAN_SHUTDOWN) : (Value & ~FAT16_CLEAN_SHUTDOWN);
        } else {
            Value = Clean ? (Value | FAT32_CLEAN_SHUTDOWN) : (Value & ~FAT32_CLEAN_SHUTDOWN);
        }

        FatSetFatEntry(Vcb, &Map, 1, Value);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ;
    }

    FatUnmap(&Map);
}

/* Before the first change: callers hold Vcb->Resource */
VOID
FatMarkVolumeDirty (
    PFAT_VCB Vcb
    )
{
    if (Vcb->DirtyMarked) {
        return;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    if (!Vcb->DirtyMarked) {

        /* Set the flag before touching the FAT: writing FAT[1] pins the FAT,
           which calls back here, and the guard must already be in place */
        Vcb->DirtyMarked = TRUE;
        FatSetCleanBitCached(Vcb, FALSE);
        (VOID)FatSetCleanBitDisk(Vcb, FALSE);
    }

    FatRelease(&Vcb->AllocResource);
}

/* Once everything is on the disk: called with Vcb->Resource exclusive */
NTSTATUS
FatMarkVolumeClean (
    PFAT_VCB Vcb
    )
{
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    if (Vcb->DirtyMarked && !(Vcb->VcbState & VCB_STATE_KEEP_DIRTY)) {

        FatSetCleanBitCached(Vcb, TRUE);
        Status = FatSetCleanBitDisk(Vcb, TRUE);

        if (NT_SUCCESS(Status)) {
            Vcb->DirtyMarked = FALSE;
        }
    }

    FatRelease(&Vcb->AllocResource);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Flushing                                                            */
/* ------------------------------------------------------------------ */

/* Data and entry set of one file; Vcb->Resource exclusive */
NTSTATUS
FatFlushFile (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
            Status = Iosb.Status;

            /* Let a lazy write in progress finish */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            FatRelease(&Fcb->PagingIoResource);
        }

        /*
         * FAT keeps no valid-data length on disk, so any clusters allocated
         * past the last byte written must be zeroed before the larger size
         * reaches the directory entry, or stale data would show through on
         * the next mount.
         */
        if (FatIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {

            NTSTATUS Zero = FatZeroDisk(Vcb, Fcb, Fcb->Header.ValidDataLength.QuadPart,
                                        Fcb->Header.FileSize.QuadPart);

            if (NT_SUCCESS(Zero)) {
                Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
            } else if (NT_SUCCESS(Status)) {
                Status = Zero;
            }
        }

        if ((Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) && !(Fcb->FcbState & FCB_STATE_DELETED)) {

            if (NT_SUCCESS(Status)) {
                Status = FatUpdateDirent(Vcb, Fcb);
            } else {
                (VOID)FatUpdateDirent(Vcb, Fcb);
            }
        }

    } __finally {

        FatRelease(&Fcb->Resource);
    }

    return Status;
}

static NTSTATUS
FatFlushStream (
    PFAT_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;

    if (Fcb == NULL || Fcb->StreamFile == NULL) {
        return STATUS_SUCCESS;
    }

    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);

    return Iosb.Status;
}

/* Directories, FAT and bitmap; Vcb->Resource exclusive */
NTSTATUS
FatFlushMetadata (
    PFAT_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

        if (FatIsDcb(Fcb)) {
            Result = FatFlushStream(Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }
        }
    }

    Result = FatFlushStream(Vcb->FatFcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    /* The active FAT is now on the disk; copy it to the other FAT(s) and
       record the free count in the FAT32 FSInfo sector */
    Result = FatMirrorFats(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    (VOID)FatWriteFsInfo(Vcb);

    return Status;
}

/*
 * Writes everything cached for the volume, and with MarkClean clears
 * VolumeDirty afterwards. Vcb->Resource exclusive.
 */
NTSTATUS
FatFlushVolume (
    PFAT_VCB Vcb,
    BOOLEAN MarkClean
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;
    BOOLEAN Restart;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    /* A flush can close file objects, so the list is walked with markers */
    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

            if ((Fcb->FcbState & FCB_STATE_VISITED) || !FatIsFcb(Fcb)) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;
            Fcb->RefCount++;

            Result = FatFlushFile(Vcb, Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }

            FatDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }

    Result = FatFlushMetadata(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    if (MarkClean && NT_SUCCESS(Status)) {
        Status = FatMarkVolumeClean(Vcb);
    }

    (VOID)FatFlushDevice(Vcb);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
FatCommonFlushBuffers (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    PFAT_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status) || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
            __leave;
        }

        if (FatIsVfcb(Fcb)) {

            Status = FatFlushVolume(Vcb, TRUE);

        } else {

            if (FatIsFcb(Fcb)) {
                Status = FatFlushFile(Vcb, Fcb);
            }

            if (NT_SUCCESS(Status)) {
                Status = FatFlushMetadata(Vcb);
            } else {
                (VOID)FatFlushMetadata(Vcb);
            }

            (VOID)FatFlushDevice(Vcb);
        }

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    return Status;
}

/* Sent to the file system device: leave every volume clean */
NTSTATUS
FatCommonShutdown (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PLIST_ENTRY Entry;
    PFAT_VCB Vcb;

    UNREFERENCED_PARAMETER(Ctx);

    (VOID)ExAcquireResourceSharedLite(&FatData.Resource, TRUE);

    __try {

        for (Entry = FatData.VcbList.Flink; Entry != &FatData.VcbList; Entry = Entry->Flink) {

            Vcb = CONTAINING_RECORD(Entry, FAT_VCB, VcbLinks);

            (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

            if ((Vcb->VcbState & VCB_STATE_MOUNTED) && !(Vcb->VcbState & VCB_STATE_SHUTDOWN)) {
                (VOID)FatFlushVolume(Vcb, TRUE);
                Vcb->VcbState |= VCB_STATE_SHUTDOWN;
            }

            FatRelease(&Vcb->Resource);
        }

    } __finally {

        FatRelease(&FatData.Resource);
    }

    return STATUS_SUCCESS;
}
