/*
 * FATNT - IRP_MJ_CREATE
 *
 * Opens, creates, overwrites and supersedes files and directories, opens
 * the volume itself, and opens the parent of a rename target
 * (SL_OPEN_TARGET_DIRECTORY). exFAT has no security: MAXIMUM_ALLOWED is
 * granted as everything the read-only attribute and the medium allow.
 */

#include "fat.h"

#ifndef FILE_ALL_ACCESS
#define FILE_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1FF)
#endif

#ifndef FILE_EXISTS
#define FILE_EXISTS                 0x00000004
#endif
#ifndef FILE_DOES_NOT_EXIST
#define FILE_DOES_NOT_EXIST         0x00000005
#endif

/* Access that writes data, as opposed to attributes */
#define FAT_DATA_WRITE_ACCESS   (FILE_WRITE_DATA | FILE_APPEND_DATA)

/* Splits off the next "\"-separated component */
static BOOLEAN
FatNextComponent (
    PUNICODE_STRING Remaining,
    PUNICODE_STRING Component
    )
{
    USHORT Count = Remaining->Length / sizeof(WCHAR);
    USHORT i;

    if (Count == 0) {
        return FALSE;
    }

    for (i = 0; i < Count && Remaining->Buffer[i] != L'\\'; i++) {
        ;
    }

    Component->Buffer = Remaining->Buffer;
    Component->Length = (USHORT)(i * sizeof(WCHAR));
    Component->MaximumLength = Component->Length;

    if (i < Count) {
        i++;
    }

    Remaining->Buffer += i;
    Remaining->Length = (USHORT)(Remaining->Length - i * sizeof(WCHAR));
    Remaining->MaximumLength = Remaining->Length;

    return TRUE;
}

static BOOLEAN
FatIsValidComponent (
    PUNICODE_STRING Component
    )
{
    USHORT Count = Component->Length / sizeof(WCHAR);
    USHORT i;

    if (Count == 0 || Count > FAT_MAX_NAME) {
        return FALSE;
    }

    if ((Count == 1 && Component->Buffer[0] == L'.') ||
        (Count == 2 && Component->Buffer[0] == L'.' && Component->Buffer[1] == L'.')) {

        return FALSE;
    }

    for (i = 0; i < Count; i++) {
        if (!FatIsLegalLongChar(Component->Buffer[i])) {
            return FALSE;
        }
    }

    return TRUE;
}

/* Frees FCBs the path walk created but nobody opened */
static VOID
FatDropUnopened (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    if (Fcb != NULL && Fcb->RefCount == 0 &&
        Fcb != Vcb->RootDcb && Fcb != Vcb->VolumeFcb) {

        /* Goes through the normal teardown, parents included */
        Fcb->RefCount = 1;
        FatDereferenceFcb(Vcb, Fcb);
    }
}

/*
 * MAXIMUM_ALLOWED becomes everything the medium and the read-only
 * attribute allow; the caller gets exactly what it asked for otherwise.
 */
static ACCESS_MASK
FatResolveAccess (
    PIO_STACK_LOCATION IrpSp,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    PIO_SECURITY_CONTEXT SecurityContext = IrpSp->Parameters.Create.SecurityContext;
    PACCESS_STATE AccessState = SecurityContext->AccessState;
    ACCESS_MASK Access = SecurityContext->DesiredAccess;
    ACCESS_MASK Granted;

    if (Access & MAXIMUM_ALLOWED) {

        Granted = FILE_ALL_ACCESS;

        if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
            Granted &= ~FAT_WRITE_ACCESS;
        } else if (Fcb != NULL && FatIsFcb(Fcb) && (Fcb->Attributes & FAT_ATTR_READONLY)) {
            Granted &= ~FAT_DATA_WRITE_ACCESS;
        }

        Granted |= Access & ~MAXIMUM_ALLOWED;

        if (AccessState != NULL) {
            AccessState->PreviouslyGrantedAccess |= Granted;
            AccessState->RemainingDesiredAccess &= ~(Granted | MAXIMUM_ALLOWED);
        }

        Access = Granted;
    }

    return Access;
}

static NTSTATUS
FatOpenVolume (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_FCB Vfcb = Vcb->VolumeFcb;
    PFAT_CCB Ccb;
    NTSTATUS Status;

    if (CreateOptions & FILE_DIRECTORY_FILE) {
        return STATUS_NOT_A_DIRECTORY;
    }

    if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) {
        return STATUS_ACCESS_DENIED;
    }

    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        return STATUS_CANNOT_DELETE;
    }

    if (Vfcb->UncleanCount != 0) {
        Status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject, &Vfcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject, &Vfcb->ShareAccess);
    }

    Ccb = FatCreateCcb();
    if (Ccb == NULL) {
        IoRemoveShareAccess(FileObject, &Vfcb->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ccb->Flags |= CCB_FLAG_VOLUME_OPEN;

    FileObject->FsContext = Vfcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Vfcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;

    Vfcb->RefCount++;
    Vfcb->UncleanCount++;
    Vcb->OpenCount++;
    Vcb->UncleanCount++;

    Ctx->Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/* Attaches the file object to the FCB after the share check */
static NTSTATUS
FatOpenFcb (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    ULONG CreateOptions
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_CCB Ccb;
    NTSTATUS Status;


    if (Fcb->UncleanCount != 0) {
        Status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess);
    }

    Ccb = FatCreateCcb();
    if (Ccb == NULL) {
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        Ccb->Flags |= CCB_FLAG_DELETE_ON_CLOSE;
    }

    if (DesiredAccess & FAT_WRITE_ACCESS) {
        Ccb->Flags |= CCB_FLAG_WRITE_HANDLE;
        Vcb->WriteCount++;
    }

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;

    /*
     * A paging file: the Memory Manager reads and writes it directly with
     * non-cached paging I/O, mapped to disk through the FCB run list. Mark it
     * so it is never cached, truncated, moved or deleted while open.
     */
    if (Ctx->IrpSp->Flags & SL_OPEN_PAGING_FILE) {
        Fcb->FcbState |= FCB_STATE_PAGING_FILE;
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    if (!(FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
        !(Fcb->FcbState & FCB_STATE_PAGING_FILE)) {
        FileObject->Flags |= FO_CACHE_SUPPORTED;
    }

    Fcb->RefCount++;
    Fcb->UncleanCount++;
    Vcb->OpenCount++;
    Vcb->UncleanCount++;

    if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
        Fcb->NonCachedUncleanCount++;
    }

    if (FatIsFcb(Fcb)) {
        Fcb->Header.IsFastIoPossible = FatIsFastIoPossible(Fcb);
    }

    Ctx->Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/* Takes back FatOpenFcb when a later step fails */
static VOID
FatUndoOpen (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;

    IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

    if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {
        Vcb->WriteCount--;
    }

    if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
        Fcb->NonCachedUncleanCount--;
    }

    Fcb->UncleanCount--;
    Fcb->RefCount--;
    Vcb->OpenCount--;
    Vcb->UncleanCount--;

    FatDeleteCcb(Ccb);

    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;
}

/* FILE_SUPERSEDE, FILE_OVERWRITE and FILE_OVERWRITE_IF of an existing file */
static NTSTATUS
FatOverwriteFile (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    ULONG CreateDisposition
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    USHORT Attributes = (USHORT)(Ctx->IrpSp->Parameters.Create.FileAttributes & FAT_ATTR_SETTABLE);
    LARGE_INTEGER Zero;
    NTSTATUS Status;

    Zero.QuadPart = 0;

    if (!MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Zero)) {
        return STATUS_USER_MAPPED_FILE;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);

        Status = FatSetFileSize(Ctx, Fcb, FileObject, 0);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Attributes |= FAT_ATTR_ARCHIVE;

        if (CreateDisposition == FILE_SUPERSEDE) {
            Fcb->Attributes = Attributes;
        } else {
            Fcb->Attributes |= Attributes;
        }

        KeQuerySystemTime(&Fcb->LastWriteTime);
        Fcb->LastAccessTime = Fcb->LastWriteTime;

        Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
        Status = FatUpdateDirent(Vcb, Fcb);

        FatNotifyChange(Vcb, Fcb,
                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                        FILE_NOTIFY_CHANGE_SIZE,
                        FILE_ACTION_MODIFIED);

    } __finally {

        FatRelease(&Fcb->Resource);
    }

    return Status;
}

/* Name was not found in Parent: make it */
static NTSTATUS
FatCreateNew (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Parent,
    PUNICODE_STRING Name,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    USHORT ShareAccess,
    BOOLEAN TrailingBackslash,
    PFAT_SCAN Scan,
    PFAT_FCB *Created
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    ULONG FileAttributes = IrpSp->Parameters.Create.FileAttributes;
    BOOLEAN IsDirectory = (BOOLEAN)((CreateOptions & FILE_DIRECTORY_FILE) != 0);
    ACCESS_MASK DesiredAccess;
    PFAT_FCB Fcb = NULL;
    USHORT Attributes;
    NTSTATUS Status;

    *Created = NULL;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    if (Parent->FcbState & (FCB_STATE_DELETE_PENDING | FCB_STATE_DELETED)) {
        return STATUS_DELETE_PENDING;
    }

    if (IsDirectory && CreateDisposition != FILE_CREATE && CreateDisposition != FILE_OPEN_IF) {
        return STATUS_INVALID_PARAMETER;
    }

    if (TrailingBackslash && !IsDirectory) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    if (IsDirectory) {
        Attributes = (USHORT)((FileAttributes & FAT_ATTR_SETTABLE & ~FAT_ATTR_ARCHIVE) |
                              FAT_ATTR_DIRECTORY);
    } else {
        Attributes = (USHORT)((FileAttributes & FAT_ATTR_SETTABLE) | FAT_ATTR_ARCHIVE);
    }

    Status = FatCreateDirent(Vcb, Parent, Name, Attributes, &Scan->Info);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = FatCreateFcb(Vcb, Parent, &Scan->Info, &Fcb);

    if (NT_SUCCESS(Status)) {

        /* A new file may be written even if it is born read-only */
        DesiredAccess = FatResolveAccess(IrpSp, Vcb, NULL);

        Status = FatOpenFcb(Ctx, Vcb, Fcb, DesiredAccess, ShareAccess, CreateOptions);
    }

    if (!NT_SUCCESS(Status)) {

        /* Take the new entry back off the disk */
        if (Fcb != NULL) {
            (VOID)FatDeleteFromDisk(Vcb, Fcb);
            *Created = Fcb;
        } else {
            (VOID)FatRemoveDirent(Vcb, Parent, Scan->Info.SetOffset, Scan->Info.EntryCount);
        }

        return Status;
    }

    *Created = Fcb;

    FatNotifyChange(Vcb, Fcb,
                    IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                    FILE_ACTION_ADDED);

    Ctx->Irp->IoStatus.Information = FILE_CREATED;
    return STATUS_SUCCESS;
}

/*
 * SL_OPEN_TARGET_DIRECTORY: open Parent for a rename into it and leave
 * only the final component in the file object's name.
 */
static NTSTATUS
FatOpenTargetDirectory (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb,
    PFAT_FCB Parent,
    PUNICODE_STRING Name,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    PFAT_SCAN Scan
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    BOOLEAN Exists;
    NTSTATUS Status;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    if (FatFindFcb(Vcb, Parent, Name) != NULL) {

        Exists = TRUE;

    } else {

        Status = FatLookupName(Vcb, Parent, Name, Scan);
        FatUnmap(&Scan->Map);

        if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
            return Status;
        }

        Exists = (BOOLEAN)NT_SUCCESS(Status);
    }

    Status = FatOpenFcb(Ctx, Vcb, Parent, DesiredAccess, ShareAccess, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlMoveMemory(FileObject->FileName.Buffer, Name->Buffer, Name->Length);
    FileObject->FileName.Length = Name->Length;

    Ctx->Irp->IoStatus.Information = Exists ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
    return STATUS_SUCCESS;
}

NTSTATUS
FatCommonCreate (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PFILE_OBJECT RelatedFileObject = FileObject->RelatedFileObject;
    PFAT_FCB RelatedFcb = NULL;
    PFAT_FCB Current = NULL;
    PFAT_FCB Child;
    PFAT_FCB Created;
    UNICODE_STRING Remaining;
    UNICODE_STRING Component;
    ULONG CreateDisposition;
    ULONG CreateOptions;
    ACCESS_MASK DesiredAccess;
    ACCESS_MASK ShareCheckAccess;
    USHORT ShareAccess;
    BOOLEAN TrailingBackslash = FALSE;
    BOOLEAN OpenTarget;
    BOOLEAN Overwrite;
    BOOLEAN LastComponent;
    BOOLEAN Opened = FALSE;
    PFAT_SCAN Scan = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    CreateDisposition = (IrpSp->Parameters.Create.Options >> 24) & 0xFF;
    CreateOptions = IrpSp->Parameters.Create.Options & 0x00FFFFFF;
    ShareAccess = IrpSp->Parameters.Create.ShareAccess;
    OpenTarget = (BOOLEAN)((IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY) != 0);

    Remaining = FileObject->FileName;


    if (RelatedFileObject != NULL) {
        RelatedFcb = (PFAT_FCB)RelatedFileObject->FsContext;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (IrpSp->Flags & SL_OPEN_PAGING_FILE) {

            if (CreateOptions & FILE_DIRECTORY_FILE) {
                /* A paging file must be a normal file, not a directory */
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            /*
             * Open the existing, already-sized paging file as-is: never
             * supersede, overwrite or truncate it (the Memory Manager opens
             * it with a don't-care disposition), and never create it here.
             */
            CreateDisposition = FILE_OPEN;
        }

        if (CreateOptions & FILE_OPEN_BY_FILE_ID) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (CreateDisposition > FILE_OVERWRITE_IF ||
            ((CreateOptions & FILE_DIRECTORY_FILE) && (CreateOptions & FILE_NON_DIRECTORY_FILE))) {

            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* The volume itself */
        if (Remaining.Length == 0 &&
            (RelatedFcb == NULL || FatIsVfcb(RelatedFcb))) {

            if (OpenTarget) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            DesiredAccess = FatResolveAccess(IrpSp, Vcb, NULL);

            Status = FatOpenVolume(Ctx, Vcb, DesiredAccess, ShareAccess,
                                   CreateDisposition, CreateOptions);
            __leave;
        }

        if (RelatedFcb != NULL) {

            if (Remaining.Length != 0 && Remaining.Buffer[0] == L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (FatIsVfcb(RelatedFcb)) {
                Current = Vcb->RootDcb;
            } else if (FatIsFcb(RelatedFcb) && Remaining.Length != 0) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                __leave;
            } else {
                Current = RelatedFcb;
            }

        } else {

            if (Remaining.Buffer[0] != L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            Remaining.Buffer++;
            Remaining.Length -= sizeof(WCHAR);
            Current = Vcb->RootDcb;
        }

        if (Remaining.Length != 0 &&
            Remaining.Buffer[Remaining.Length / sizeof(WCHAR) - 1] == L'\\') {

            TrailingBackslash = TRUE;
            Remaining.Length -= sizeof(WCHAR);

            if (Remaining.Length == 0 ||
                Remaining.Buffer[Remaining.Length / sizeof(WCHAR) - 1] == L'\\') {

                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        Remaining.MaximumLength = Remaining.Length;

        if (Remaining.Length == 0 && OpenTarget) {
            /* The root has no parent to rename into */
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        Scan = FatAllocateScan();
        if (Scan == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* Walk the path */
        while (FatNextComponent(&Remaining, &Component)) {

            LastComponent = (BOOLEAN)(Remaining.Length == 0);

            if (!FatIsValidComponent(&Component)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }


            if (!FatIsDcb(Current)) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                __leave;
            }

            if (LastComponent && OpenTarget) {

                DesiredAccess = FatResolveAccess(IrpSp, Vcb, Current);

                Status = FatOpenTargetDirectory(Ctx, Vcb, Current, &Component,
                                                DesiredAccess, ShareAccess, Scan);
                if (NT_SUCCESS(Status)) {
                    Opened = TRUE;
                }

                __leave;
            }

            Child = FatFindFcb(Vcb, Current, &Component);

            if (Child == NULL) {

                Status = FatLookupName(Vcb, Current, &Component, Scan);
                FatUnmap(&Scan->Map);

                if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {

                    if (!LastComponent) {
                        Status = STATUS_OBJECT_PATH_NOT_FOUND;
                        __leave;
                    }

                    if (CreateDisposition == FILE_OPEN || CreateDisposition == FILE_OVERWRITE) {
                        __leave;
                    }

                    Status = FatCreateNew(Ctx, Vcb, Current, &Component, CreateDisposition,
                                          CreateOptions, ShareAccess, TrailingBackslash,
                                          Scan, &Created);

                    if (NT_SUCCESS(Status)) {
                        Opened = TRUE;
                    } else if (Created != NULL) {
                        /* Dropped below, with any parents it kept */
                        Current = Created;
                    }

                    __leave;
                }

                if (!NT_SUCCESS(Status)) {
                    __leave;
                }

                Status = FatCreateFcb(Vcb, Current, &Scan->Info, &Child);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Current = Child;
        }

        /* Current exists */
        if (CreateDisposition == FILE_CREATE) {
            Status = STATUS_OBJECT_NAME_COLLISION;
            __leave;
        }

        if (FatIsDcb(Current)) {

            if (CreateOptions & FILE_NON_DIRECTORY_FILE) {
                Status = STATUS_FILE_IS_A_DIRECTORY;
                __leave;
            }

            if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }

        } else {

            if (CreateOptions & FILE_DIRECTORY_FILE) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }

            if (TrailingBackslash) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        if (Current->FcbState & FCB_STATE_DELETE_PENDING) {
            Status = STATUS_DELETE_PENDING;
            __leave;
        }

        DesiredAccess = FatResolveAccess(IrpSp, Vcb, Current);
        ShareCheckAccess = DesiredAccess;

        Overwrite = (BOOLEAN)(CreateDisposition == FILE_SUPERSEDE ||
                              CreateDisposition == FILE_OVERWRITE ||
                              CreateDisposition == FILE_OVERWRITE_IF);

        if (Overwrite) {

            ShareCheckAccess |= FILE_WRITE_DATA;

            /* Win32 CREATE_ALWAYS keeps hidden and system files hidden and system */
            if (((Current->Attributes & FAT_ATTR_HIDDEN) &&
                 !(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_HIDDEN)) ||
                ((Current->Attributes & FAT_ATTR_SYSTEM) &&
                 !(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_SYSTEM))) {

                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
        }

        if ((ShareCheckAccess & FAT_WRITE_ACCESS) || (CreateOptions & FILE_DELETE_ON_CLOSE)) {

            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Status = STATUS_MEDIA_WRITE_PROTECTED;
                __leave;
            }
        }

        if (FatIsFcb(Current) && (Current->Attributes & FAT_ATTR_READONLY)) {

            if (ShareCheckAccess & FAT_DATA_WRITE_ACCESS) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            if (CreateOptions & FILE_DELETE_ON_CLOSE) {
                Status = STATUS_CANNOT_DELETE;
                __leave;
            }
        }

        if ((CreateOptions & FILE_DELETE_ON_CLOSE) && (Current->FcbState & FCB_STATE_ROOT)) {
            Status = STATUS_CANNOT_DELETE;
            __leave;
        }

        Status = FatOpenFcb(Ctx, Vcb, Current, ShareCheckAccess, ShareAccess, CreateOptions);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Overwrite) {

            Status = FatOverwriteFile(Ctx, Vcb, Current, CreateDisposition);

            if (!NT_SUCCESS(Status)) {
                FatUndoOpen(Ctx, Vcb, Current);
                __leave;
            }

            Ctx->Irp->IoStatus.Information =
                (CreateDisposition == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }

        Opened = TRUE;

    } __finally {

        if (!Opened || AbnormalTermination()) {

            if (Current != NULL && Current != RelatedFcb) {
                FatDropUnopened(Vcb, Current);
            }
        }

        FatFreeScan(Scan);
        FatRelease(&Vcb->Resource);
    }


    return Status;
}
