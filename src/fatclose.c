/*
 * FATNT - IRP_MJ_CLEANUP, IRP_MJ_CLOSE, dismount and volume teardown
 *
 * Cleanup of the last handle is where a file is deleted, extra
 * allocation is given back and the entry set gets the final sizes and
 * times. A dismounted volume stays in memory until its last file object
 * is closed. Then the VPB is detached (so the next access mounts afresh)
 * and the volume device object is deleted.
 */

#include "fat.h"

VOID
FatUnlockVcb (
    PFAT_VCB Vcb
    )
{
    KIRQL Irql;

    Vcb->VcbState &= ~VCB_STATE_LOCKED;
    Vcb->LockFileObject = NULL;

    IoAcquireVpbSpinLock(&Irql);
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    IoReleaseVpbSpinLock(Irql);
}

/*
 * Drops cached data of files nobody has open, so that file objects held
 * only by the cache manager or memory manager get closed. Callers that
 * want the data kept flush first. The list is rescanned after each purge
 * because closes can arrive while it runs.
 */
VOID
FatPurgeCachedFiles (
    PFAT_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;
    BOOLEAN Restart;

    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

            if (Fcb->FcbState & FCB_STATE_VISITED) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;

            if (!FatIsFcb(Fcb) ||
                Fcb->UncleanCount != 0 ||
                (Fcb->SectionObjectPointers.DataSectionObject == NULL &&
                 Fcb->SectionObjectPointers.ImageSectionObject == NULL)) {

                continue;
            }

            Fcb->RefCount++;

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
            (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);

            FatDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }
}

/* Closes every internal stream, throwing away what is cached */
static VOID
FatCloseAllStreams (
    PFAT_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;
    BOOLEAN Restart;

    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

            if (Fcb->StreamFile != NULL) {
                FatCloseStream(Vcb, Fcb, TRUE);
                Restart = TRUE;
                break;
            }
        }

    } while (Restart);

    if (Vcb->FatFcb != NULL) {
        FatCloseStream(Vcb, Vcb->FatFcb, TRUE);
    }
}

/*
 * Called with Vcb->Resource held exclusive. Callers that can still reach
 * the disk flush the volume first. Returns TRUE when the VCB can be
 * deleted right away; the caller then releases the resource and calls
 * FatDeleteVcb.
 */
BOOLEAN
FatDismountVcb (
    PFAT_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FALSE;
    }

    FAT_DBG((FAT_PFX "Dismounting volume %08lX\n", Vcb->SerialNumber));

    Vcb->VcbState |= VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT;
    Vcb->VcbState &= ~VCB_STATE_MOUNTED;

    /* Send cached I/O on open files back through the IRP path */
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    FatPurgeCachedFiles(Vcb);
    FatCloseAllStreams(Vcb);

    Vcb->VcbState &= ~VCB_STATE_IN_DISMOUNT;

    if (FatTryTeardown(Vcb, 0)) {
        return TRUE;
    }

    /*
     * The volume still has open references (its own metadata streams, or user
     * files - e.g. the running system volume while autochk runs), so it cannot
     * be torn down and handed back to the I/O manager for a fresh mount: on NT
     * 3.1 that remount path cannot complete while the old VPB is still mounted,
     * and the verify that a later open triggers would hang. The caches and
     * metadata streams have already been purged and closed above, so instead
     * of leaving the volume dismounted-but-alive we keep it mounted; the next
     * access reopens the streams and re-reads the on-disk metadata, which is
     * exactly the fresh state a remount would have produced.
     */
    Vcb->VcbState &= ~VCB_STATE_DISMOUNTED;
    Vcb->VcbState |= VCB_STATE_MOUNTED;
    Vcb->VcbState &= ~VCB_STATE_DASD_WRITTEN;

    return FALSE;
}

/*
 * Called with Vcb->Resource held exclusive. InFlightReferences is the
 * VPB reference the current request itself holds (1 for a create, 0 for
 * a close).
 */
BOOLEAN
FatTryTeardown (
    PFAT_VCB Vcb,
    ULONG InFlightReferences
    )
{
    PVPB Vpb = Vcb->Vpb;
    KIRQL Irql;
    BOOLEAN FreeVpb = FALSE;

    if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Vcb->VcbState & (VCB_STATE_IN_DISMOUNT | VCB_STATE_DELETE_PENDING)) ||
        Vcb->OpenCount != 0) {

        return FALSE;
    }

    IoAcquireVpbSpinLock(&Irql);

    if (Vpb->ReferenceCount != InFlightReferences) {
        IoReleaseVpbSpinLock(Irql);
        return FALSE;
    }

    if (Vpb->RealDevice->Vpb == Vpb) {

        Vpb->DeviceObject = NULL;
        Vpb->Flags &= ~(VPB_MOUNTED | VPB_LOCKED);

    } else if (InFlightReferences == 0) {

        /* The I/O manager replaced this VPB during a verify */
        FreeVpb = TRUE;
    }

    IoReleaseVpbSpinLock(Irql);

    Vcb->VcbState |= VCB_STATE_DELETE_PENDING;

    if (FreeVpb) {
        Vcb->VcbState |= VCB_STATE_FREE_VPB;
    }

    return TRUE;
}

/* Called without any locks held */
VOID
FatDeleteVcb (
    PFAT_VCB Vcb
    )
{
    PFAT_FCB Fcb;
    PVPB FreeVpb = NULL;

    FAT_DBG((FAT_PFX "Deleting volume %08lX\n", Vcb->SerialNumber));

    (VOID)ExAcquireResourceExclusiveLite(&FatData.Resource, TRUE);
    RemoveEntryList(&Vcb->VcbLinks);
    FatRelease(&FatData.Resource);

    while (!IsListEmpty(&Vcb->FcbList)) {
        Fcb = CONTAINING_RECORD(Vcb->FcbList.Flink, FAT_FCB, FcbLinks);
        FatDeleteFcb(Fcb);
    }

    if (Vcb->VolumeFcb != NULL) {
        FatDeleteFcb(Vcb->VolumeFcb);
    }

    if (Vcb->FatFcb != NULL) {
        FatDeleteFcb(Vcb->FatFcb);
    }

    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);

    if (Vcb->VcbState & VCB_STATE_FREE_VPB) {
        FreeVpb = Vcb->Vpb;
    }

    ExDeleteResourceLite(&Vcb->AllocResource);
    ExDeleteResourceLite(&Vcb->Resource);

    ObDereferenceObject(Vcb->TargetDeviceObject);
    IoDeleteDevice(Vcb->VolumeDeviceObject);

    if (FreeVpb != NULL) {
        ExFreePool(FreeVpb);
    }
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

/* Last handle of a delete-pending file or directory: remove it */
static VOID
FatDeleteOnCleanup (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    BOOLEAN Empty = TRUE;
    NTSTATUS Status;

    if (FatIsDcb(Fcb)) {

        /* Entries may have come back since the delete was requested */
        (VOID)FatIsDirectoryEmpty(Vcb, Fcb, &Empty);

        if (!Empty) {
            Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
            return;
        }

    } else {

        /* Nothing cached may reach the clusters about to be freed */
        (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        Fcb->Header.FileSize.QuadPart = 0;
        Fcb->Header.ValidDataLength.QuadPart = 0;
        FatRelease(&Fcb->PagingIoResource);

        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
    }

    Status = FatDeleteFromDisk(Vcb, Fcb);

    if (!NT_SUCCESS(Status)) {
        FAT_DBG((FAT_PFX "Delete of %wZ failed %lX\n", &Fcb->Name, Status));
    }

    FatNotifyChange(Vcb, Fcb,
                    FatIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                    FILE_ACTION_REMOVED);
}

NTSTATUS
FatCommonCleanup (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;
    PFAT_VCB Vcb = Ctx->Vcb;
    LARGE_INTEGER Now;
    LARGE_INTEGER Zero;
    PLARGE_INTEGER TruncateSize = NULL;
    ULONG Filter = 0;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN Writable;
    BOOLEAN Modified;

    /* IoCreateStreamFileObject cleans up before FsContext is set */
    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_SUCCESS;
    }

    Zero.QuadPart = 0;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Writable = (BOOLEAN)!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY |
                                               VCB_STATE_SHUTDOWN));

        if (FatIsVfcb(Fcb)) {

            if (Vcb->LockFileObject == FileObject) {

                FatUnlockVcb(Vcb);

                /* Metadata may have been rewritten under us */
                if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                    (VOID)FatDismountVcb(Vcb);
                }
            }

        } else {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;

            if (FatIsDcb(Fcb)) {
                FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
            } else {
                FsRtlFastUnlockAll(&Fcb->FileLock, FileObject,
                                   IoGetRequestorProcess(Ctx->Irp), NULL);
            }

            if (Ccb->Flags & CCB_FLAG_DELETE_ON_CLOSE) {
                Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
            }

            Modified = (BOOLEAN)((FileObject->Flags & FO_FILE_MODIFIED) != 0);

            if (Writable && !(Fcb->FcbState & FCB_STATE_DELETED)) {

                if (Modified) {

                    KeQuerySystemTime(&Now);

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_WRITE)) {
                        Fcb->LastWriteTime = Now;
                        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
                    }

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_ACCESS)) {
                        Fcb->LastAccessTime = Now;
                    }

                    if (!(Fcb->Attributes & FAT_ATTR_ARCHIVE) && FatIsFcb(Fcb)) {
                        Fcb->Attributes |= FAT_ATTR_ARCHIVE;
                        Filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
                    }

                    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
                }

                if (FileObject->Flags & FO_FILE_SIZE_CHANGED) {
                    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
                    Filter |= FILE_NOTIFY_CHANGE_SIZE;
                }

                if (Fcb->UncleanCount == 1 && (Fcb->FcbState & FCB_STATE_DELETE_PENDING)) {

                    FatDeleteOnCleanup(Vcb, Fcb);

                    if (Fcb->FcbState & FCB_STATE_DELETED) {
                        TruncateSize = &Zero;
                        Filter = 0;
                    }

                } else if (Fcb->UncleanCount == 1 &&
                           (Fcb->FcbState & FCB_STATE_TRUNCATE_ON_CLOSE)) {

                    (VOID)FatSetAllocation(Vcb, Fcb, (ULONGLONG)Fcb->Header.FileSize.QuadPart);
                    Fcb->FcbState &= ~FCB_STATE_TRUNCATE_ON_CLOSE;
                }

                if (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) {
                    (VOID)FatUpdateDirent(Vcb, Fcb);
                }

                if (Filter != 0) {
                    FatNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);
                }

                /* Removable media: this file is on the disk once closed */
                if ((Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE) && FatIsFcb(Fcb) &&
                    (Modified || (FileObject->Flags & FO_FILE_SIZE_CHANGED)) &&
                    !(Fcb->FcbState & FCB_STATE_DELETED)) {

                    (VOID)FatFlushFile(Vcb, Fcb);
                }
            }

            if (FatIsFcb(Fcb)) {

                CcUninitializeCacheMap(FileObject, TruncateSize, NULL);

                /* On a dismounted volume let the last handle take the cache
                   with it, so the file objects close and the VCB can go */
                if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) && Fcb->UncleanCount == 1) {
                    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
                    (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
                }
            }

            if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
                Fcb->NonCachedUncleanCount--;
            }
        }

        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        Fcb->UncleanCount--;
        Vcb->UncleanCount--;

        if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {

            Ccb->Flags &= ~CCB_FLAG_WRITE_HANDLE;
            Vcb->WriteCount--;

            /* Removable media is left clean once nothing writes to it */
            if (Vcb->WriteCount == 0 && Writable && (Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE)) {
                (VOID)FatFlushVolume(Vcb, TRUE);
            }
        }

        if (FatIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = FatIsFastIoPossible(Fcb);
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

    } __finally {

        if (FcbAcquired) {
            FatRelease(&Fcb->Resource);
        }

        FatRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Close                                                               */
/* ------------------------------------------------------------------ */

/* Called with Vcb->Resource exclusive; TRUE: the VCB can be deleted */
static BOOLEAN
FatCloseFcb (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFAT_CCB Ccb
    )
{
    if (Ccb == NULL) {

        /* An internal stream */
        if (FatIsDcb(Fcb) || FatIsMeta(Fcb)) {
            FatStreamClosed(Vcb, Fcb);
        }

    } else {

        FatDeleteCcb(Ccb);

        /* Mapped writes after the last cleanup can still move VDL */
        if (Fcb->RefCount == 1 && (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) &&
            !(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY))) {

            (VOID)FatUpdateDirent(Vcb, Fcb);
        }

        FatDereferenceFcb(Vcb, Fcb);
        Vcb->OpenCount--;
    }

    return FatTryTeardown(Vcb, 0);
}

static VOID
NTAPI
FatCloseWorker (
    PVOID Context
    )
{
    PFAT_CLOSE_ITEM Item = (PFAT_CLOSE_ITEM)Context;
    PFAT_VCB Vcb = Item->Vcb;
    BOOLEAN Delete;

    FsRtlEnterFileSystem();

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Delete = FatCloseFcb(Vcb, Item->Fcb, Item->Ccb);

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    if (Delete) {
        FatDeleteVcb(Vcb);
    }

    FsRtlExitFileSystem();

    ExFreePool(Item);
}

/*
 * A close can arrive from inside the cache or memory manager while this
 * thread already holds the volume shared; waiting for it exclusive would
 * then deadlock. Such closes are finished by a worker thread.
 */
NTSTATUS
FatCommonClose (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;
    PFAT_VCB Vcb = Ctx->Vcb;
    PFAT_CLOSE_ITEM Item;
    BOOLEAN Delete;

    if (Fcb == NULL) {
        return STATUS_SUCCESS;
    }

    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;

    if (!ExAcquireResourceExclusiveLite(&Vcb->Resource, FALSE)) {

        Item = NULL;

        if (IoGetTopLevelIrp() != Ctx->Irp) {
            Item = (PFAT_CLOSE_ITEM)ExAllocatePoolWithTag(NonPagedPool, sizeof(FAT_CLOSE_ITEM),
                                                          FAT_TAG_WORK);
        }

        if (Item != NULL) {

            Item->Vcb = Vcb;
            Item->Fcb = Fcb;
            Item->Ccb = Ccb;

            /* Deprecated in the WDK, but the only work queue NT 3.51/4.0 have */
#if defined(_MSC_VER) && _MSC_VER >= 1300
#pragma warning(push)
#pragma warning(disable: 4995 4996)
#endif
            ExInitializeWorkItem(&Item->Item, FatCloseWorker, Item);
            ExQueueWorkItem(&Item->Item, DelayedWorkQueue);
#if defined(_MSC_VER) && _MSC_VER >= 1300
#pragma warning(pop)
#endif

            return STATUS_SUCCESS;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    }

    __try {

        Delete = FatCloseFcb(Vcb, Fcb, Ccb);

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    if (Delete) {
        FatDeleteVcb(Vcb);
    }

    return STATUS_SUCCESS;
}
