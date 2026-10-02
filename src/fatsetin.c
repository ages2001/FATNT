/*
 * FATNT - IRP_MJ_SET_INFORMATION
 *
 * Every class but FilePositionInformation takes the volume exclusive and
 * the file exclusive; the entry set is rewritten before returning.
 */

#include "fat.h"

/* Directory lookups by name: rename checks its target with these */
static BOOLEAN
FatIsValidName (
    PUNICODE_STRING Name
    )
{
    USHORT Count = Name->Length / sizeof(WCHAR);
    USHORT i;

    if (Count == 0 || Count > FAT_MAX_NAME || (Name->Length & 1) != 0) {
        return FALSE;
    }

    if ((Count == 1 && Name->Buffer[0] == L'.') ||
        (Count == 2 && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.')) {

        return FALSE;
    }

    for (i = 0; i < Count; i++) {
        if (!FatIsLegalLongChar(Name->Buffer[i])) {
            return FALSE;
        }
    }

    return TRUE;
}

/* Cuts FileSize and VDL and drops the cached data past them */
static VOID
FatShrinkFileSize (
    PFAT_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG NewSize
    )
{
    LARGE_INTEGER Page;
    IO_STATUS_BLOCK Iosb;

    /* The cached page holding the new end keeps the old bytes after it.
       Should the file grow again they would show through, so that page
       goes to the disk and out of the cache, to come back zeroed past VDL. */
    if ((NewSize & (FAT_PAGE_SIZE - 1)) != 0 &&
        NewSize < Fcb->Header.ValidDataLength.QuadPart &&
        Fcb->SectionObjectPointers.DataSectionObject != NULL) {

        Page.QuadPart = NewSize & ~(LONGLONG)(FAT_PAGE_SIZE - 1);

        CcFlushCache(&Fcb->SectionObjectPointers, &Page, FAT_PAGE_SIZE, &Iosb);
        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Page, FAT_PAGE_SIZE, FALSE);
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    Fcb->Header.FileSize.QuadPart = NewSize;

    if (Fcb->Header.ValidDataLength.QuadPart > NewSize) {
        Fcb->Header.ValidDataLength.QuadPart = NewSize;
    }

    __try {

        if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        }

    } __finally {

        FatRelease(&Fcb->PagingIoResource);
    }
}

/*
 * Changes FileSize, and with it VDL and the allocation when they no
 * longer fit. Called with the volume and the file exclusive; the caller
 * rewrites the entry set.
 */
NTSTATUS
FatSetFileSize (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG NewSize
    )
{
    PFAT_VCB Vcb = Fcb->Vcb;
    LARGE_INTEGER Size;
    BOOLEAN CacheMapInitialized = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Ctx);

    if (NewSize < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewSize == Fcb->Header.FileSize.QuadPart) {
        return STATUS_SUCCESS;
    }

    Size.QuadPart = NewSize;

    if (NewSize < Fcb->Header.FileSize.QuadPart &&
        !MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Size)) {

        return STATUS_USER_MAPPED_FILE;
    }

    __try {

        /* A mapped file with no cache map still has to hear of the size */
        if (Fcb->SectionObjectPointers.DataSectionObject != NULL &&
            Fcb->SectionObjectPointers.SharedCacheMap == NULL &&
            !(FileObject->Flags & FO_CLEANUP_COMPLETE)) {

            CcInitializeCacheMap(FileObject,
                                 (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                                 FALSE,
                                 &FatData.CacheManagerCallbacks,
                                 Fcb);

            CacheMapInitialized = TRUE;
        }

        if (NewSize > Fcb->Header.FileSize.QuadPart) {

            if (NewSize > Fcb->Header.AllocationSize.QuadPart) {

                Status = FatSetAllocation(Vcb, Fcb, (ULONGLONG)NewSize);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Fcb->Header.FileSize.QuadPart = NewSize;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }

        } else {

            FatShrinkFileSize(Fcb, FileObject, NewSize);

            Status = FatSetAllocation(Vcb, Fcb, (ULONGLONG)NewSize);
        }

        Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

    } __finally {

        if (CacheMapInitialized) {
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }
    }

    return Status;
}

static NTSTATUS
FatSetBasicInfo (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFAT_CCB Ccb,
    PFILE_BASIC_INFORMATION Info
    )
{
    ULONG Filter = 0;
    ULONG Attributes;
    USHORT NewAttributes;

    if (Info->FileAttributes != 0) {

        Attributes = Info->FileAttributes;

        if (FatIsDcb(Fcb)) {
            if (Attributes & FILE_ATTRIBUTE_TEMPORARY) {
                return STATUS_INVALID_PARAMETER;
            }
        } else if (Attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return STATUS_INVALID_PARAMETER;
        }

        NewAttributes = (USHORT)((Attributes & FAT_ATTR_SETTABLE) |
                                 (Fcb->Attributes & FAT_ATTR_DIRECTORY));

        if (NewAttributes != Fcb->Attributes) {
            Fcb->Attributes = NewAttributes;
            Filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
        }
    }

    /* Zero leaves a time alone; so does -1, as later Windows versions use it */
    if (Info->CreationTime.QuadPart != 0 && Info->CreationTime.QuadPart != -1) {
        Fcb->CreationTime = Info->CreationTime;
        Filter |= FILE_NOTIFY_CHANGE_CREATION;
    }

    if (Info->LastAccessTime.QuadPart != 0 && Info->LastAccessTime.QuadPart != -1) {
        Fcb->LastAccessTime = Info->LastAccessTime;
        Ccb->Flags |= CCB_FLAG_USER_SET_ACCESS;
        Filter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
    }

    if (Info->LastWriteTime.QuadPart != 0 && Info->LastWriteTime.QuadPart != -1) {
        Fcb->LastWriteTime = Info->LastWriteTime;
        Ccb->Flags |= CCB_FLAG_USER_SET_WRITE;
        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    }

    if (Filter == 0) {
        return STATUS_SUCCESS;
    }

    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
    FatNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);

    return FatUpdateDirent(Vcb, Fcb);
}

static NTSTATUS
FatSetDispositionInfo (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_DISPOSITION_INFORMATION Info
    )
{
    BOOLEAN Empty;
    NTSTATUS Status;

    if (!Info->DeleteFile) {
        Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
        FileObject->DeletePending = FALSE;
        return STATUS_SUCCESS;
    }

    if (Fcb->Attributes & FAT_ATTR_READONLY) {
        return STATUS_CANNOT_DELETE;
    }

    if (FatIsDcb(Fcb)) {

        Status = FatIsDirectoryEmpty(Vcb, Fcb, &Empty);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (!Empty) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }

    } else if (!MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForDelete)) {

        /* A running program */
        return STATUS_CANNOT_DELETE;
    }

    Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
    FileObject->DeletePending = TRUE;

    return STATUS_SUCCESS;
}

/*
 * The rename target exists and may be replaced: remove it. It must be a
 * file nobody has open.
 */
static NTSTATUS
FatReplaceTarget (
    PFAT_VCB Vcb,
    PFAT_FCB TargetDcb,
    PUNICODE_STRING Name,
    PFAT_DIR_INFO Dirent
    )
{
    PFAT_FCB Existing;
    FAT_RUN_LIST Runs;
    ULONG First;
    NTSTATUS Status;

    if (Dirent->Attributes & (FAT_ATTR_DIRECTORY | FAT_ATTR_READONLY)) {
        return STATUS_ACCESS_DENIED;
    }

    Existing = FatFindFcb(Vcb, TargetDcb, Name);

    if (Existing != NULL) {

        if (Existing->UncleanCount != 0 ||
            !MmFlushImageSection(&Existing->SectionObjectPointers, MmFlushForDelete)) {

            return STATUS_ACCESS_DENIED;
        }

        /* Only the cache holds it: drop that and delete through the FCB */
        Existing->RefCount++;

        (VOID)ExAcquireResourceExclusiveLite(&Existing->Resource, TRUE);

        __try {

            (VOID)ExAcquireResourceExclusiveLite(&Existing->PagingIoResource, TRUE);
            Existing->Header.FileSize.QuadPart = 0;
            Existing->Header.ValidDataLength.QuadPart = 0;
            FatRelease(&Existing->PagingIoResource);

            (VOID)CcPurgeCacheSection(&Existing->SectionObjectPointers, NULL, 0, FALSE);

            Status = FatDeleteFromDisk(Vcb, Existing);

        } __finally {

            FatRelease(&Existing->Resource);
        }

        FatNotifyChange(Vcb, Existing, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_REMOVED);

        (VOID)MmForceSectionClosed(&Existing->SectionObjectPointers, TRUE);
        FatDereferenceFcb(Vcb, Existing);

        return Status;
    }

    First = Dirent->FirstCluster;

    Status = FatBuildRunList(Vcb, First,
                             (ULONG)(((ULONGLONG)Dirent->FileSize + Vcb->ClusterSize - 1) >> Vcb->ClusterShift),
                             &Runs);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = FatRemoveDirent(Vcb, TargetDcb, Dirent->SetOffset, Dirent->EntryCount);

    if (NT_SUCCESS(Status)) {
        FatFreeClusters(Vcb, &Runs, &First, 0);
    }

    FatFreeRunList(&Runs);

    return Status;
}

static VOID
FatReportName (
    PFAT_VCB Vcb,
    PUNICODE_STRING FullName,
    USHORT NameOffset,
    ULONG Filter,
    ULONG Action
    )
{
    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)FullName,
                                NameOffset,
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
}

static NTSTATUS
FatSetRenameInfo (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFILE_RENAME_INFORMATION Info
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT TargetObject = IrpSp->Parameters.SetFile.FileObject;
    BOOLEAN ReplaceIfExists = IrpSp->Parameters.SetFile.ReplaceIfExists;
    PFAT_FCB TargetDcb;
    PFAT_FCB OldParent = Fcb->ParentDcb;
    PFAT_FCB Walk;
    PLIST_ENTRY Entry;
    UNICODE_STRING NewName;
    UNICODE_STRING OldFull;
    USHORT OldNameOffset = 0;
    PFAT_SCAN Scan = NULL;
    ULONG Filter;
    NTSTATUS Status;

    RtlZeroMemory(&OldFull, sizeof(OldFull));

    if (TargetObject != NULL) {

        /* The I/O manager opened the target directory for us */
        TargetDcb = (PFAT_FCB)TargetObject->FsContext;

        if (TargetDcb == NULL || !FatIsDcb(TargetDcb)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (TargetDcb->Vcb != Vcb) {
            return STATUS_NOT_SAME_DEVICE;
        }

        NewName = TargetObject->FileName;

    } else {

        TargetDcb = OldParent;

        NewName.Buffer = Info->FileName;
        NewName.Length = (USHORT)Info->FileNameLength;
        NewName.MaximumLength = NewName.Length;

        if (Info->FileNameLength > 0xFFFE) {
            return STATUS_OBJECT_NAME_INVALID;
        }
    }

    if (!FatIsValidName(&NewName)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    if (Fcb->FcbState & FCB_STATE_DELETE_PENDING) {
        return STATUS_DELETE_PENDING;
    }

    if (TargetDcb->FcbState & (FCB_STATE_DELETE_PENDING | FCB_STATE_DELETED)) {
        return STATUS_DELETE_PENDING;
    }

    if (FatIsDcb(Fcb)) {

        /* Not into itself or below */
        if (FatIsAncestor(Fcb, TargetDcb)) {
            return STATUS_INVALID_PARAMETER;
        }

        /* Nothing below it may be open */
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Walk = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

            if (Walk != Fcb && Walk->UncleanCount != 0 && FatIsAncestor(Fcb, Walk)) {
                return STATUS_ACCESS_DENIED;
            }
        }
    }

    /* Exactly the same name: nothing to do */
    if (TargetDcb == OldParent && NewName.Length == Fcb->Name.Length &&
        RtlCompareMemory(NewName.Buffer, Fcb->Name.Buffer, NewName.Length) == NewName.Length) {

        return STATUS_SUCCESS;
    }

    Scan = FatAllocateScan();
    if (Scan == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        Status = FatLookupName(Vcb, TargetDcb, &NewName, Scan);
        FatUnmap(&Scan->Map);

        if (NT_SUCCESS(Status)) {

            /* Another name for itself (a change of case) is fine */
            if (!(TargetDcb == OldParent && Scan->Info.Offset == Fcb->DirOffset)) {

                if (!ReplaceIfExists) {
                    Status = STATUS_OBJECT_NAME_COLLISION;
                    __leave;
                }

                Status = FatReplaceTarget(Vcb, TargetDcb, &NewName, &Scan->Info);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

        } else if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
            __leave;
        }

        /* Keep the old name for the notification */
        if (NT_SUCCESS(FatBuildFullName(Fcb))) {

            OldFull.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Fcb->FullName.Length, FAT_TAG_NAME);

            if (OldFull.Buffer != NULL) {
                RtlCopyMemory(OldFull.Buffer, Fcb->FullName.Buffer, Fcb->FullName.Length);
                OldFull.Length = Fcb->FullName.Length;
                OldFull.MaximumLength = OldFull.Length;
                OldNameOffset = (USHORT)(Fcb->FullName.Length - Fcb->Name.Length);
            }
        }

        Status = FatMoveDirent(Vcb, Fcb, TargetDcb, &NewName);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Filter = FatIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME;

        if (OldFull.Buffer != NULL) {
            FatReportName(Vcb, &OldFull, OldNameOffset, Filter,
                          (TargetDcb == OldParent) ? FILE_ACTION_RENAMED_OLD_NAME : FILE_ACTION_REMOVED);
        }

        FatNotifyChange(Vcb, Fcb, Filter,
                        (TargetDcb == OldParent) ? FILE_ACTION_RENAMED_NEW_NAME : FILE_ACTION_ADDED);

    } __finally {

        FatFreeScan(Scan);

        if (OldFull.Buffer != NULL) {
            ExFreePool(OldFull.Buffer);
        }
    }

    return Status;
}

static NTSTATUS
FatSetAllocationInfo (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_ALLOCATION_INFORMATION Info
    )
{
    LONGLONG NewAllocation = Info->AllocationSize.QuadPart;
    NTSTATUS Status;

    if (!FatIsFcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewAllocation < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewAllocation < Fcb->Header.FileSize.QuadPart) {

        /* Shrinking below the data cuts the file */
        Status = FatSetFileSize(Ctx, Fcb, FileObject, NewAllocation);

        if (NT_SUCCESS(Status)) {
            FileObject->Flags |= FO_FILE_MODIFIED;
            FatNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);
        }

    } else {

        Status = FatSetAllocation(Vcb, Fcb, (ULONGLONG)NewAllocation);

        if (NT_SUCCESS(Status) &&
            Fcb->Header.AllocationSize.QuadPart >
                FatRoundUp(Fcb->Header.FileSize.QuadPart, Vcb->ClusterSize)) {

            /* exFAT cannot record allocation past the data */
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;
        }

        if (NT_SUCCESS(Status) && Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        }
    }

    if (NT_SUCCESS(Status)) {
        Status = FatUpdateDirent(Vcb, Fcb);
    }

    return Status;
}

static NTSTATUS
FatSetEndOfFileInfo (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_END_OF_FILE_INFORMATION Info
    )
{
    NTSTATUS Status;

    if (!FatIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Info->EndOfFile.QuadPart == Fcb->Header.FileSize.QuadPart) {
        return STATUS_SUCCESS;
    }

    Status = FatSetFileSize(Ctx, Fcb, FileObject, Info->EndOfFile.QuadPart);

    if (NT_SUCCESS(Status)) {

        FileObject->Flags |= FO_FILE_MODIFIED;
        FatNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);

        Status = FatUpdateDirent(Vcb, Fcb);
    }

    return Status;
}

NTSTATUS
FatCommonSetInformation (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PFAT_FCB Fcb = (PFAT_FCB)FileObject->FsContext;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;
    PFAT_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.SetFile.FileInformationClass;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    PFILE_POSITION_INFORMATION Position;
    BOOLEAN FcbAcquired = FALSE;
    NTSTATUS Status;

    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Class == FilePositionInformation) {

        if (!FatIsVfcb(Fcb)) {
            Status = FatVerifyVcb(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }

        Position = (PFILE_POSITION_INFORMATION)Buffer;

        if ((FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
            ((ULONG)Position->CurrentByteOffset.QuadPart & (Vcb->SectorSize - 1)) != 0) {

            return STATUS_INVALID_PARAMETER;
        }

        FileObject->CurrentByteOffset = Position->CurrentByteOffset;
        return STATUS_SUCCESS;
    }

    if (FatIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* The lazy writer reporting what reached the disk: the entry set is
       rewritten at cleanup and close anyway */
    if (Class == FileEndOfFileInformation && IrpSp->Parameters.SetFile.AdvanceOnly) {
        return STATUS_SUCCESS;
    }

    if (Fcb->FcbState & FCB_STATE_ROOT) {
        return (Class == FileDispositionInformation) ? STATUS_CANNOT_DELETE : STATUS_INVALID_PARAMETER;
    }

    if (Class == FileLinkInformation) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        FcbAcquired = TRUE;

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        /* A paging file in use must not be deleted, renamed or resized */
        if ((Fcb->FcbState & FCB_STATE_PAGING_FILE) &&
            (Class == FileDispositionInformation || Class == FileRenameInformation ||
             Class == FileAllocationInformation || Class == FileEndOfFileInformation)) {

            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        switch ((ULONG)Class) {

        case FileBasicInformation:
            Status = FatSetBasicInfo(Vcb, Fcb, Ccb, (PFILE_BASIC_INFORMATION)Buffer);
            break;

        case FileDispositionInformation:
            Status = FatSetDispositionInfo(Vcb, Fcb, FileObject, (PFILE_DISPOSITION_INFORMATION)Buffer);
            break;

        case FileRenameInformation:
            Status = FatSetRenameInfo(Ctx, Vcb, Fcb, (PFILE_RENAME_INFORMATION)Buffer);
            break;

        case FileAllocationInformation:
            Status = FatSetAllocationInfo(Ctx, Vcb, Fcb, FileObject,
                                          (PFILE_ALLOCATION_INFORMATION)Buffer);
            break;

        case FileEndOfFileInformation:
            Status = FatSetEndOfFileInfo(Ctx, Vcb, Fcb, FileObject,
                                         (PFILE_END_OF_FILE_INFORMATION)Buffer);
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (FatIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = FatIsFastIoPossible(Fcb);
        }

    } __finally {

        if (FcbAcquired) {
            FatRelease(&Fcb->Resource);
        }

        FatRelease(&Vcb->Resource);
    }

    return Status;
}
