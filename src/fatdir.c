/*
 * FATNT - directory scanning and IRP_MJ_DIRECTORY_CONTROL
 *
 * A directory holds 32-byte entries: optional long-name (VFAT) entries
 * followed by the 8.3 short entry. "." and ".." and the volume label are
 * skipped by the scan; the listing synthesizes "." and "..".
 */

#include "fat.h"

PFAT_SCAN
FatAllocateScan (
    VOID
    )
{
    PFAT_SCAN Scan;

    Scan = (PFAT_SCAN)ExAllocatePoolWithTag(PagedPool, sizeof(FAT_SCAN), FAT_TAG_BUFFER);
    if (Scan != NULL) {
        RtlZeroMemory(&Scan->Map, sizeof(FAT_MAP));
    }

    return Scan;
}

VOID
FatFreeScan (
    PFAT_SCAN Scan
    )
{
    if (Scan != NULL) {
        FatUnmap(&Scan->Map);
        ExFreePool(Scan);
    }
}

static BOOLEAN
FatIsDotEntry (
    const FAT_DIR_INFO *Info
    )
{
    if (!(Info->Attributes & FAT_ATTR_DIRECTORY)) {
        return FALSE;
    }

    if (Info->ShortName[0] == '.' ) {

        if (Info->ShortName[1] == ' ') {
            return TRUE;        /* "." */
        }

        if (Info->ShortName[1] == '.' && Info->ShortName[2] == ' ') {
            return TRUE;        /* ".." */
        }
    }

    return FALSE;
}

/*
 * Finds the next real file or directory entry at or after *Offset and
 * decodes it into Scan->Info. Long-name sets, free entries, "." and ".."
 * and the volume label are stepped over. *Offset moves past the entry.
 */
BOOLEAN
FatNextDirEntry (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    PULONG Offset,
    PFAT_SCAN Scan
    )
{
    ULONG Size = Dcb->Header.AllocationSize.LowPart;
    ULONG Current = *Offset;
    ULONG Window;
    ULONG Avail;
    ULONG Result;
    ULONG i;
    PUCHAR Entry;

    while (Current + FAT_DIRENT_SIZE <= Size) {

        /* Copy a window of entries so the set is contiguous for parsing */
        Window = (FAT_LFN_MAX_ENTRIES + 1);
        if (Current + Window * FAT_DIRENT_SIZE > Size) {
            Window = (Size - Current) / FAT_DIRENT_SIZE;
        }

        Avail = 0;
        for (i = 0; i < Window; i++) {
            Entry = FatMapStream(Vcb, Dcb, &Scan->Map,
                                 (LONGLONG)Current + i * FAT_DIRENT_SIZE, FAT_DIRENT_SIZE);
            RtlCopyMemory(Scan->Set + i * FAT_DIRENT_SIZE, Entry, FAT_DIRENT_SIZE);
            Avail++;
        }

        Result = FatParseDirEntry(Scan->Set, Avail, Current, &Scan->Info);

        if (Result == FAT_NAME_END) {
            break;
        }

        if (Result == FAT_NAME_FREE || Result == FAT_NAME_LABEL) {
            Current += Scan->Info.EntryCount * FAT_DIRENT_SIZE;
            continue;
        }

        /* FAT_NAME_OK */
        if (FatIsDotEntry(&Scan->Info)) {
            Current += Scan->Info.EntryCount * FAT_DIRENT_SIZE;
            continue;
        }

        *Offset = Scan->Info.SetOffset + Scan->Info.EntryCount * FAT_DIRENT_SIZE;
        return TRUE;
    }

    *Offset = Current;
    return FALSE;
}

NTSTATUS
FatLookupName (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    PUNICODE_STRING Name,
    PFAT_SCAN Scan
    )
{
    ULONG Length = Name->Length / sizeof(WCHAR);
    ULONG Offset = 0;
    WCHAR Expanded[13];
    ULONG ExpLen;

    while (FatNextDirEntry(Vcb, Dcb, &Offset, Scan)) {


        if (FatNamesEqualNoCase(Scan->Info.Name, Scan->Info.NameLength, Name->Buffer, Length)) {
            return STATUS_SUCCESS;
        }

        /* A long-named entry may also be opened by its 8.3 name */
        if (Scan->Info.HasLongName) {
            ExpLen = FatExpandShortName(Scan->Info.ShortName, Scan->Info.NtReserved, Expanded);
            if (FatNamesEqualNoCase(Expanded, ExpLen, Name->Buffer, Length)) {
                return STATUS_SUCCESS;
            }
        }
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_QUERY_DIRECTORY                                              */
/* ------------------------------------------------------------------ */

typedef struct _FAT_LIST_ENTRY {
    ULONG           FileIndex;
    LARGE_INTEGER   CreationTime;
    LARGE_INTEGER   LastAccessTime;
    LARGE_INTEGER   LastWriteTime;
    LARGE_INTEGER   EndOfFile;
    LARGE_INTEGER   AllocationSize;
    ULONG           Attributes;
    UNICODE_STRING  Name;
} FAT_LIST_ENTRY, *PFAT_LIST_ENTRY;

static VOID
FatListFromFcb (
    PFAT_FCB Fcb,
    PFAT_LIST_ENTRY List,
    PWSTR Name,
    USHORT NameLength,
    ULONG FileIndex
    )
{
    List->FileIndex = FileIndex;

    if (Fcb != NULL) {
        List->CreationTime = Fcb->CreationTime;
        List->LastAccessTime = Fcb->LastAccessTime;
        List->LastWriteTime = Fcb->LastWriteTime;
    } else {
        List->CreationTime.QuadPart = 0;
        List->LastAccessTime.QuadPart = 0;
        List->LastWriteTime.QuadPart = 0;
    }

    List->EndOfFile.QuadPart = 0;
    List->AllocationSize.QuadPart = 0;
    List->Attributes = FILE_ATTRIBUTE_DIRECTORY;
    List->Name.Buffer = Name;
    List->Name.Length = NameLength;
    List->Name.MaximumLength = NameLength;
}

static VOID
FatListFromInfo (
    PFAT_VCB Vcb,
    PFAT_DIR_INFO Info,
    PFAT_LIST_ENTRY List
    )
{
    List->FileIndex = FAT_FIRST_CLUSTER + (Info->Offset >> FAT_DIRENT_SHIFT);
    List->CreationTime = FatDosToNtTime(Info->CreateDate, Info->CreateTime, Info->CreateTimeTenth);
    List->LastWriteTime = FatDosToNtTime(Info->WriteDate, Info->WriteTime, 0);
    List->LastAccessTime = FatDosToNtTime(Info->LastAccessDate, 0, 0);
    List->Attributes = FatDirentNtAttributes(Info->Attributes);

    if (Info->Attributes & FAT_ATTR_DIRECTORY) {
        List->EndOfFile.QuadPart = 0;
        List->AllocationSize.QuadPart = 0;
    } else {
        List->EndOfFile.QuadPart = Info->FileSize;
        List->AllocationSize.QuadPart = FatRoundUp((LONGLONG)Info->FileSize, Vcb->ClusterSize);
    }

    List->Name.Buffer = Info->Name;
    List->Name.Length = (USHORT)(Info->NameLength * sizeof(WCHAR));
    List->Name.MaximumLength = List->Name.Length;
}

static ULONG
FatListBaseLength (
    FILE_INFORMATION_CLASS Class
    )
{
    switch (Class) {
    case FileDirectoryInformation:
        return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
    case FileFullDirectoryInformation:
        return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
    case FileBothDirectoryInformation:
        return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
    case FileNamesInformation:
        return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
    default:
        return 0;
    }
}

static ULONG
FatFillListEntry (
    FILE_INFORMATION_CLASS Class,
    PUCHAR Buffer,
    ULONG Room,
    PFAT_LIST_ENTRY List
    )
{
    PFILE_DIRECTORY_INFORMATION Dir;
    PFILE_FULL_DIR_INFORMATION Full;
    PFILE_BOTH_DIR_INFORMATION Both;
    PFILE_NAMES_INFORMATION Names;
    ULONG Base = FatListBaseLength(Class);
    ULONG Copy;

    RtlZeroMemory(Buffer, Base);

    Copy = List->Name.Length;
    if (Copy > Room - Base) {
        Copy = Room - Base;
    }

    switch (Class) {

    case FileDirectoryInformation:
        Dir = (PFILE_DIRECTORY_INFORMATION)Buffer;
        Dir->FileIndex = List->FileIndex;
        Dir->CreationTime = List->CreationTime;
        Dir->LastAccessTime = List->LastAccessTime;
        Dir->LastWriteTime = List->LastWriteTime;
        Dir->ChangeTime = List->LastWriteTime;
        Dir->EndOfFile = List->EndOfFile;
        Dir->AllocationSize = List->AllocationSize;
        Dir->FileAttributes = List->Attributes;
        Dir->FileNameLength = List->Name.Length;
        RtlCopyMemory(Dir->FileName, List->Name.Buffer, Copy);
        break;

    case FileFullDirectoryInformation:
        Full = (PFILE_FULL_DIR_INFORMATION)Buffer;
        Full->FileIndex = List->FileIndex;
        Full->CreationTime = List->CreationTime;
        Full->LastAccessTime = List->LastAccessTime;
        Full->LastWriteTime = List->LastWriteTime;
        Full->ChangeTime = List->LastWriteTime;
        Full->EndOfFile = List->EndOfFile;
        Full->AllocationSize = List->AllocationSize;
        Full->FileAttributes = List->Attributes;
        Full->FileNameLength = List->Name.Length;
        RtlCopyMemory(Full->FileName, List->Name.Buffer, Copy);
        break;

    case FileBothDirectoryInformation:
        Both = (PFILE_BOTH_DIR_INFORMATION)Buffer;
        Both->FileIndex = List->FileIndex;
        Both->CreationTime = List->CreationTime;
        Both->LastAccessTime = List->LastAccessTime;
        Both->LastWriteTime = List->LastWriteTime;
        Both->ChangeTime = List->LastWriteTime;
        Both->EndOfFile = List->EndOfFile;
        Both->AllocationSize = List->AllocationSize;
        Both->FileAttributes = List->Attributes;
        Both->FileNameLength = List->Name.Length;
        Both->ShortNameLength = 0;
        RtlCopyMemory(Both->FileName, List->Name.Buffer, Copy);
        break;

    case FileNamesInformation:
        Names = (PFILE_NAMES_INFORMATION)Buffer;
        Names->FileIndex = List->FileIndex;
        Names->FileNameLength = List->Name.Length;
        RtlCopyMemory(Names->FileName, List->Name.Buffer, Copy);
        break;

    default:
        break;
    }

    return Copy;
}

static BOOLEAN
FatListMatches (
    PFAT_CCB Ccb,
    PUNICODE_STRING Name
    )
{
    if (Ccb->Flags & CCB_FLAG_MATCH_ALL) {
        return TRUE;
    }

    if (Ccb->Flags & CCB_FLAG_WILDCARD) {
        return FsRtlIsNameInExpression(&Ccb->Pattern, Name, TRUE, NULL);
    }

    return FatNamesEqualNoCase(Ccb->Pattern.Buffer, Ccb->Pattern.Length / sizeof(WCHAR),
                               Name->Buffer, Name->Length / sizeof(WCHAR));
}

static NTSTATUS
FatSetPattern (
    PFAT_CCB Ccb,
    PUNICODE_STRING FileName
    )
{
    UNICODE_STRING Pattern;
    NTSTATUS Status;

    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
        RtlZeroMemory(&Ccb->Pattern, sizeof(UNICODE_STRING));
    }

    Ccb->Flags &= ~(CCB_FLAG_WILDCARD | CCB_FLAG_MATCH_ALL);

    if (FileName == NULL || FileName->Length == 0 ||
        (FileName->Length == sizeof(WCHAR) && FileName->Buffer[0] == L'*')) {

        Ccb->Flags |= CCB_FLAG_MATCH_ALL | CCB_FLAG_PATTERN_SET;
        return STATUS_SUCCESS;
    }

    Pattern.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, FileName->Length, FAT_TAG_NAME);
    if (Pattern.Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Pattern.Length = 0;
    Pattern.MaximumLength = FileName->Length;

    Status = RtlUpcaseUnicodeString(&Pattern, FileName, FALSE);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Pattern.Buffer);
        return Status;
    }

    Ccb->Pattern = Pattern;

    if (FsRtlDoesNameContainWildCards(&Ccb->Pattern)) {
        Ccb->Flags |= CCB_FLAG_WILDCARD;
    }

    Ccb->Flags |= CCB_FLAG_PATTERN_SET;
    return STATUS_SUCCESS;
}

static NTSTATUS
FatQueryDirectory (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Dcb,
    PFAT_CCB Ccb
    )
{
    PFAT_VCB Vcb = Dcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    FILE_INFORMATION_CLASS Class;
    PUNICODE_STRING FileName;
    ULONG Length;
    ULONG FileIndex;
    PUCHAR Buffer;
    ULONG Base;
    ULONG EntryLength;
    ULONG Room;
    ULONG Copied;
    ULONG NextOffset = 0;
    ULONG LastOffset = 0;
    ULONG Returned = 0;
    ULONG Information = 0;
    BOOLEAN InitialQuery = FALSE;
    BOOLEAN IsRoot = (BOOLEAN)(Dcb->ParentDcb == NULL);
    ULONG SavedState;
    ULONG SavedOffset;
    FAT_LIST_ENTRY List;
    PFAT_SCAN Scan = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    Class     = FatXSp(IrpSp)->Parameters.QueryDirectory.FileInformationClass;
    FileName  = FatXSp(IrpSp)->Parameters.QueryDirectory.FileName;
    Length    = FatXSp(IrpSp)->Parameters.QueryDirectory.Length;
    FileIndex = FatXSp(IrpSp)->Parameters.QueryDirectory.FileIndex;

    Base = FatListBaseLength(Class);
    if (Base == 0) {
        return STATUS_INVALID_INFO_CLASS;
    }

    if (Length < Base) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    Buffer = (PUCHAR)FatMapUserBuffer(Irp);

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Dcb->Resource, TRUE);

    __try {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (!(Ccb->Flags & CCB_FLAG_PATTERN_SET) ||
            ((IrpSp->Flags & SL_RESTART_SCAN) && FileName != NULL && FileName->Length != 0)) {

            Status = FatSetPattern(Ccb, FileName);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            InitialQuery = TRUE;
            Ccb->QueryState = 0;
            Ccb->QueryOffset = 0;
        }

        if (IrpSp->Flags & SL_RESTART_SCAN) {
            Ccb->QueryState = 0;
            Ccb->QueryOffset = 0;
        }

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {
            if (FileIndex < FAT_FIRST_CLUSTER) {
                Ccb->QueryState = 2;
                Ccb->QueryOffset = 0;
            } else {
                Ccb->QueryState = 2;
                Ccb->QueryOffset = (FileIndex - FAT_FIRST_CLUSTER) << FAT_DIRENT_SHIFT;
                Ccb->QueryOffset += FAT_DIRENT_SIZE;
            }
        }

        if (IsRoot && Ccb->QueryState < 2) {
            Ccb->QueryState = 2;
        }

        Scan = FatAllocateScan();
        if (Scan == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        for (;;) {

            SavedState = Ccb->QueryState;
            SavedOffset = Ccb->QueryOffset;

            if (Ccb->QueryState == 0) {

                FatListFromFcb(Dcb, &List, L".", sizeof(WCHAR), 0);
                Ccb->QueryState = 1;

            } else if (Ccb->QueryState == 1) {

                FatListFromFcb(Dcb->ParentDcb, &List, L"..", 2 * sizeof(WCHAR), 1);
                Ccb->QueryState = 2;

            } else {

                if (!FatNextDirEntry(Vcb, Dcb, &Ccb->QueryOffset, Scan)) {
                    break;
                }

                FatListFromInfo(Vcb, &Scan->Info, &List);
            }

            if (!FatListMatches(Ccb, &List.Name)) {
                continue;
            }

            EntryLength = Base + List.Name.Length;
            Room = Length - NextOffset;

            if (NextOffset + Base > Length || (Returned != 0 && EntryLength > Room)) {

                Ccb->QueryState = SavedState;
                Ccb->QueryOffset = SavedOffset;

                if (Returned == 0) {
                    Status = STATUS_BUFFER_OVERFLOW;
                }

                break;
            }

            if (Returned != 0) {
                *(PULONG)(Buffer + LastOffset) = NextOffset - LastOffset;
            }

            Copied = FatFillListEntry(Class, Buffer + NextOffset, Room, &List);

            Returned++;
            LastOffset = NextOffset;
            Information = NextOffset + Base + Copied;

            if (Copied < List.Name.Length) {
                Status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            if (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
                break;
            }

            NextOffset = (Information + 7) & ~7UL;

            if (NextOffset >= Length) {
                break;
            }
        }

        if (Returned == 0 && NT_SUCCESS(Status)) {
            Status = InitialQuery ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        }

        Irp->IoStatus.Information = Information;

    } __finally {

        FatFreeScan(Scan);
        FatRelease(&Dcb->Resource);
        FatRelease(&Vcb->Resource);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_NOTIFY_CHANGE_DIRECTORY                                      */
/* ------------------------------------------------------------------ */

static NTSTATUS
FatNotifyChangeDirectory (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Dcb,
    PFAT_CCB Ccb
    )
{
    PFAT_VCB Vcb = Dcb->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = FatBuildFullName(Dcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

#ifdef FAT_NT31
        /* Change notification is not provided on NT 3.1 */
        Status = STATUS_SUCCESS;
#else
        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync,
                                       &Vcb->DirNotifyList,
                                       Ccb,
                                       (PSTRING)&Dcb->FullName,
                                       (BOOLEAN)((IrpSp->Flags & SL_WATCH_TREE) != 0),
                                       FALSE,
                                       FatXSp(IrpSp)->Parameters.NotifyDirectory.CompletionFilter,
                                       Ctx->Irp,
                                       NULL,
                                       NULL);

        Ctx->Flags |= FAT_CTX_NO_COMPLETE;
        Status = STATUS_PENDING;
#endif

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    return Status;
}

NTSTATUS
FatCommonDirectoryControl (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_FCB Dcb = (PFAT_FCB)FileObject->FsContext;
    PFAT_CCB Ccb = (PFAT_CCB)FileObject->FsContext2;

    if (Dcb == NULL || Ccb == NULL || !FatIsDcb(Dcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_DIRECTORY:
        return FatQueryDirectory(Ctx, Dcb, Ccb);

    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        return FatNotifyChangeDirectory(Ctx, Dcb, Ccb);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
