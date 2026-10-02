/*
 * FATNT - FCB, CCB, internal streams, names and times
 *
 * All FCBs of a volume sit on Vcb->FcbList. A file has exactly one FCB,
 * found by (parent, name), so every open of it shares one set of section
 * object pointers. An FCB holds a reference on its parent.
 *
 * Directories and the FAT are cached through an internal stream file each.
 * A directory's stream is opened on first use and closed when the
 * directory FCB is no longer referenced; the FCB is freed only once the
 * cache manager has let go of that stream.
 */

#include "fat.h"

/* ------------------------------------------------------------------ */
/* Times (FAT keeps local time with no offset recorded)                */
/* ------------------------------------------------------------------ */

LARGE_INTEGER
FatDosToNtTime (
    USHORT Date,
    USHORT DosTime,
    UCHAR Tenth
    )
{
    FAT_TIME_PARTS Parts;
    TIME_FIELDS Fields;
    LARGE_INTEGER Time;

    Time.QuadPart = 0;

    if (Date == 0 || !FatDecodeTime(Date, DosTime, Tenth, &Parts)) {
        return Time;
    }

    Fields.Year         = (CSHORT)Parts.Year;
    Fields.Month        = (CSHORT)Parts.Month;
    Fields.Day          = (CSHORT)Parts.Day;
    Fields.Hour         = (CSHORT)Parts.Hour;
    Fields.Minute       = (CSHORT)Parts.Minute;
    Fields.Second       = (CSHORT)Parts.Second;
    Fields.Milliseconds = (CSHORT)Parts.Milliseconds;
    Fields.Weekday      = 0;

    if (!RtlTimeFieldsToTime(&Fields, &Time)) {
        Time.QuadPart = 0;
        return Time;
    }

    ExLocalTimeToSystemTime(&Time, &Time);
    return Time;
}

VOID
FatNtToDosTime (
    LARGE_INTEGER Time,
    PUSHORT Date,
    PUSHORT DosTime,
    PUCHAR Tenth
    )
{
    LARGE_INTEGER Local;
    TIME_FIELDS Fields;
    FAT_TIME_PARTS Parts;

    if (Time.QuadPart == 0) {
        *Date = 0;
        *DosTime = 0;
        if (Tenth != NULL) {
            *Tenth = 0;
        }
        return;
    }

    ExSystemTimeToLocalTime(&Time, &Local);
    RtlTimeToTimeFields(&Local, &Fields);

    Parts.Year         = (USHORT)Fields.Year;
    Parts.Month        = (USHORT)Fields.Month;
    Parts.Day          = (USHORT)Fields.Day;
    Parts.Hour         = (USHORT)Fields.Hour;
    Parts.Minute       = (USHORT)Fields.Minute;
    Parts.Second       = (USHORT)Fields.Second;
    Parts.Milliseconds = (USHORT)Fields.Milliseconds;

    FatEncodeTime(&Parts, Date, DosTime, Tenth);
}

/* The FCB's attributes, times, cluster and size in on-disk form */
VOID
FatFcbToInfo (
    PFAT_FCB Fcb,
    PFAT_DIR_INFO Info
    )
{
    RtlZeroMemory(Info, sizeof(FAT_DIR_INFO));

    Info->Attributes   = Fcb->Attributes;
    Info->NtReserved   = Fcb->NtReserved;
    Info->FirstCluster = Fcb->FirstCluster;

    if (FatIsDcb(Fcb)) {
        Info->FileSize = 0;
    } else {
        Info->FileSize = Fcb->Header.FileSize.LowPart;
    }

    FatNtToDosTime(Fcb->CreationTime, &Info->CreateDate, &Info->CreateTime, &Info->CreateTimeTenth);
    FatNtToDosTime(Fcb->LastWriteTime, &Info->WriteDate, &Info->WriteTime, NULL);
    FatNtToDosTime(Fcb->LastAccessTime, &Info->LastAccessDate, &Info->WriteTime, NULL);
    /* LastAccess keeps only the date; recompute write time cleanly */
    FatNtToDosTime(Fcb->LastWriteTime, &Info->WriteDate, &Info->WriteTime, NULL);
}

/* ------------------------------------------------------------------ */
/* FCBs                                                                */
/* ------------------------------------------------------------------ */

static PFAT_FCB
FatAllocateFcb (
    PFAT_VCB Vcb,
    CSHORT NodeType
    )
{
    PFAT_FCB Fcb;

    Fcb = (PFAT_FCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(FAT_FCB), FAT_TAG_FCB);
    if (Fcb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(FAT_FCB));

    Fcb->Header.NodeTypeCode = NodeType;
    Fcb->Header.NodeByteSize = (CSHORT)sizeof(FAT_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;

    ExInitializeResourceLite(&Fcb->Resource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);

    Fcb->Header.Resource = &Fcb->Resource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;

    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);

    Fcb->Vcb = Vcb;
    InitializeListHead(&Fcb->FcbLinks);

    return Fcb;
}

VOID
FatDeleteFcb (
    PFAT_FCB Fcb
    )
{
    RemoveEntryList(&Fcb->FcbLinks);

    FsRtlUninitializeFileLock(&Fcb->FileLock);

    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }

    if (Fcb->FullName.Buffer != NULL) {
        ExFreePool(Fcb->FullName.Buffer);
    }

    FatFreeRunList(&Fcb->RunList);

    ExFreePool(Fcb);
}

/* Represents the whole volume for DASD opens */
PFAT_FCB
FatCreateVolumeFcb (
    PFAT_VCB Vcb
    )
{
    PFAT_FCB Fcb;

    Fcb = FatAllocateFcb(Vcb, FAT_NTC_VFCB);
    if (Fcb == NULL) {
        return NULL;
    }

    Fcb->Header.AllocationSize.QuadPart = Vcb->VolumeBytes;
    Fcb->Header.FileSize.QuadPart = Vcb->VolumeBytes;
    Fcb->Header.ValidDataLength.QuadPart = Vcb->VolumeBytes;

    return Fcb;
}

/* The active FAT, cached as one linear stream. Not on Vcb->FcbList. */
PFAT_FCB
FatCreateFatFcb (
    PFAT_VCB Vcb
    )
{
    PFAT_FCB Fcb;
    LONGLONG Length;

    Fcb = FatAllocateFcb(Vcb, FAT_NTC_META);
    if (Fcb == NULL) {
        return NULL;
    }

    Length = (LONGLONG)Vcb->FatSectors << Vcb->SectorShift;

    Fcb->MetaLbo = (LONGLONG)Vcb->FatSector << Vcb->SectorShift;
    Fcb->Header.AllocationSize.QuadPart = Length;
    Fcb->Header.FileSize.QuadPart = Length;
    Fcb->Header.ValidDataLength.QuadPart = Length;

    return Fcb;
}

/*
 * The root directory. On FAT12/16 it is a fixed region addressed linearly
 * (no cluster chain, no growth); on FAT32 it is an ordinary cluster chain.
 */
PFAT_FCB
FatCreateRootDcb (
    PFAT_VCB Vcb
    )
{
    PFAT_FCB Dcb;
    NTSTATUS Status;

    Dcb = FatAllocateFcb(Vcb, FAT_NTC_DCB);
    if (Dcb == NULL) {
        return NULL;
    }

    Dcb->FcbState = FCB_STATE_ROOT;
    Dcb->Attributes = FAT_ATTR_DIRECTORY;
    Dcb->DirOffset = 0xFFFFFFFF;
    Dcb->SetOffset = 0xFFFFFFFF;

    if (Vcb->FatType == FAT_TYPE_32) {

        Status = FatBuildRunList(Vcb, Vcb->RootCluster, 0, &Dcb->RunList);

        if (!NT_SUCCESS(Status) ||
            ((ULONGLONG)Dcb->RunList.Clusters << Vcb->ClusterShift) > FAT_MAX_DIR_SIZE) {

            FatDeleteFcb(Dcb);
            return NULL;
        }

        Dcb->FirstCluster = Vcb->RootCluster;
        Dcb->IndexNumber = Vcb->RootCluster;
        Dcb->Header.AllocationSize.QuadPart =
            (LONGLONG)Dcb->RunList.Clusters << Vcb->ClusterShift;

    } else {

        Dcb->FcbState |= FCB_STATE_FIXED_ROOT;
        Dcb->MetaLbo = Vcb->RootLbo;
        Dcb->FirstCluster = 0;
        Dcb->IndexNumber = 1;
        Dcb->Header.AllocationSize.QuadPart = Vcb->RootBytes;
    }

    Dcb->Header.FileSize = Dcb->Header.AllocationSize;
    Dcb->Header.ValidDataLength = Dcb->Header.AllocationSize;

    InsertTailList(&Vcb->FcbList, &Dcb->FcbLinks);

    return Dcb;
}

NTSTATUS
FatCreateFcb (
    PFAT_VCB Vcb,
    PFAT_FCB ParentDcb,
    PFAT_DIR_INFO Info,
    PFAT_FCB *NewFcb
    )
{
    PFAT_FCB Fcb;
    BOOLEAN IsDirectory;
    ULONGLONG DataLength;
    ULONGLONG Clusters;
    ULONG ChainClusters;
    NTSTATUS Status;

    *NewFcb = NULL;

    IsDirectory = (BOOLEAN)((Info->Attributes & FAT_ATTR_DIRECTORY) != 0);

    /* A directory's length is the length of its cluster chain */
    if (IsDirectory) {

        if (Info->FirstCluster == 0) {
            return STATUS_FILE_CORRUPT_ERROR;
        }

        DataLength = 0;
        ChainClusters = 0;      /* follow the chain to its end */

    } else {

        DataLength = Info->FileSize;
        Clusters = (DataLength + Vcb->ClusterSize - 1) >> Vcb->ClusterShift;

        if (Clusters > Vcb->ClusterCount) {
            return STATUS_FILE_CORRUPT_ERROR;
        }

        ChainClusters = (ULONG)Clusters;
    }

    Fcb = FatAllocateFcb(Vcb, (CSHORT)(IsDirectory ? FAT_NTC_DCB : FAT_NTC_FCB));
    if (Fcb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Fcb->Name.Length = (USHORT)(Info->NameLength * sizeof(WCHAR));
    Fcb->Name.MaximumLength = Fcb->Name.Length;
    Fcb->Name.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Fcb->Name.Length, FAT_TAG_NAME);

    if (Fcb->Name.Buffer == NULL) {
        FatDeleteFcb(Fcb);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Fcb->Name.Buffer, Info->Name, Fcb->Name.Length);

    Status = FatBuildRunList(Vcb, Info->FirstCluster, ChainClusters, &Fcb->RunList);

    if (!NT_SUCCESS(Status)) {
        FatDeleteFcb(Fcb);
        return Status;
    }

    if (IsDirectory &&
        (Fcb->RunList.Clusters == 0 ||
         ((ULONGLONG)Fcb->RunList.Clusters << Vcb->ClusterShift) > FAT_MAX_DIR_SIZE)) {

        FatDeleteFcb(Fcb);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Fcb->ParentDcb    = ParentDcb;
    Fcb->DirOffset    = Info->Offset;
    Fcb->SetOffset    = Info->SetOffset;
    Fcb->EntryCount   = Info->EntryCount;
    Fcb->Attributes   = Info->Attributes;
    Fcb->NtReserved   = Info->NtReserved;
    Fcb->FirstCluster = Info->FirstCluster;
    Fcb->IndexNumber  = ((LONGLONG)ParentDcb->FirstCluster << 32) | Info->Offset;
    RtlCopyMemory(Fcb->ShortName, Info->ShortName, FAT_SFN_LEN);

    Fcb->Header.AllocationSize.QuadPart =
        (LONGLONG)Fcb->RunList.Clusters << Vcb->ClusterShift;

    if (IsDirectory) {
        Fcb->Header.FileSize = Fcb->Header.AllocationSize;
        Fcb->Header.ValidDataLength = Fcb->Header.AllocationSize;
    } else {
        Fcb->Header.FileSize.QuadPart = (LONGLONG)DataLength;
        /* FAT keeps no valid-data length on disk: all of the file is valid */
        Fcb->Header.ValidDataLength.QuadPart = (LONGLONG)DataLength;
    }

    Fcb->CreationTime = FatDosToNtTime(Info->CreateDate, Info->CreateTime, Info->CreateTimeTenth);
    Fcb->LastWriteTime = FatDosToNtTime(Info->WriteDate, Info->WriteTime, 0);
    Fcb->LastAccessTime = FatDosToNtTime(Info->LastAccessDate, 0, 0);

    InsertTailList(&Vcb->FcbList, &Fcb->FcbLinks);
    ParentDcb->RefCount++;

    *NewFcb = Fcb;
    return STATUS_SUCCESS;
}

/*
 * Drops one reference; unreferenced FCBs are freed along with any parent
 * that loses its last reference. The root and volume FCBs stay until the
 * VCB goes away. A directory whose stream is still open is freed when
 * that stream's close arrives. Called with Vcb->Resource exclusive.
 */
VOID
FatDereferenceFcb (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    PFAT_FCB Parent;

    while (Fcb != NULL) {

        Fcb->RefCount--;

        if (Fcb->RefCount != 0 || Fcb == Vcb->RootDcb || Fcb == Vcb->VolumeFcb) {
            break;
        }

        if (Fcb->FcbState & FCB_STATE_STREAM_OPEN) {

            /* FatStreamClosed finishes the job, perhaps right away */
            Fcb->FcbState |= FCB_STATE_TEARDOWN;
            FatCloseStream(Vcb, Fcb, (BOOLEAN)((Fcb->FcbState & FCB_STATE_DELETED) != 0));
            break;
        }

        Parent = Fcb->ParentDcb;
        FatDeleteFcb(Fcb);
        Fcb = Parent;
    }
}

PFAT_FCB
FatFindFcb (
    PFAT_VCB Vcb,
    PFAT_FCB ParentDcb,
    PUNICODE_STRING Name
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

        /* Deleted files and directories on their way out do not count */
        if (Fcb->ParentDcb == ParentDcb &&
            !(Fcb->FcbState & (FCB_STATE_DELETED | FCB_STATE_TEARDOWN)) &&
            FatNamesEqualNoCase(Fcb->Name.Buffer, Fcb->Name.Length / sizeof(WCHAR),
                                Name->Buffer, Name->Length / sizeof(WCHAR))) {

            return Fcb;
        }
    }

    return NULL;
}

/* Finds an open FCB by the byte offset of its short entry in the parent */
PFAT_FCB
FatFindFcbByOffset (
    PFAT_VCB Vcb,
    PFAT_FCB ParentDcb,
    ULONG DirOffset
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Fcb;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Fcb = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

        if (Fcb->ParentDcb == ParentDcb && Fcb->DirOffset == DirOffset &&
            !(Fcb->FcbState & (FCB_STATE_DELETED | FCB_STATE_TEARDOWN))) {

            return Fcb;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Internal streams                                                    */
/* ------------------------------------------------------------------ */

/*
 * Opens the cached stream of a directory, the FAT or the bitmap. The
 * stream file object counts in Vcb->OpenCount but not in Fcb->RefCount.
 */
NTSTATUS
FatOpenStream (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    PFILE_OBJECT StreamFile = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb->StreamFile != NULL) {
        return STATUS_SUCCESS;
    }

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FAT_STATUS_DISMOUNTED;
    }

    __try {

        /*
         * The stream must be associated with our own volume device object,
         * not the underlying real (disk) device: FATNT keeps one metadata
         * stream per FCB addressed by FCB-relative offsets (the FAT, the
         * fixed root, each directory), and maps those offsets to disk LBOs in
         * FatStreamToLbo. Cached and paging I/O on the stream therefore has
         * to come back to FATNT for that mapping. Pointing the stream at
         * RealDevice sends the I/O straight to the disk at the raw file
         * offset, so e.g. the fixed root (FCB offset 0) would read the boot
         * sector (disk offset 0) instead.
         */
        StreamFile = IoCreateStreamFileObject(NULL, Vcb->VolumeDeviceObject);

        StreamFile->Vpb = Vcb->Vpb;
        StreamFile->FsContext = Fcb;
        StreamFile->FsContext2 = NULL;
        StreamFile->SectionObjectPointer = &Fcb->SectionObjectPointers;
        StreamFile->ReadAccess = TRUE;
        StreamFile->WriteAccess = TRUE;
        StreamFile->DeleteAccess = TRUE;

        Vcb->OpenCount++;
        Fcb->FcbState |= FCB_STATE_STREAM_OPEN;

        CcInitializeCacheMap(StreamFile,
                             (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                             TRUE,
                             &FatData.MetaCacheCallbacks,
                             Fcb);

        Fcb->StreamFile = StreamFile;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();

        if (StreamFile != NULL) {
            /* Our close handler takes back the count */
            ObDereferenceObject(StreamFile);
        }
    }

    return Status;
}

/*
 * Closes the internal stream. Dirty data is written first, or thrown
 * away with Discard (the object is being deleted or the volume is gone).
 * Called with Vcb->Resource exclusive; the close can arrive before this
 * returns.
 */
VOID
FatCloseStream (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    BOOLEAN Discard
    )
{
    PFILE_OBJECT StreamFile = Fcb->StreamFile;
    IO_STATUS_BLOCK Iosb;

    UNREFERENCED_PARAMETER(Vcb);

    if (StreamFile == NULL) {
        return;
    }

    if (!Discard) {
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
    }

    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);

    Fcb->StreamFile = NULL;

    /* Let the section go now rather than when memory runs short, so the
       close (which may free Fcb) comes with our own dereference */
    CcUninitializeCacheMap(StreamFile, NULL, NULL);
    (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
    ObDereferenceObject(StreamFile);
}

/* The close of an internal stream arrived */
VOID
FatStreamClosed (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    PFAT_FCB Parent;

    Fcb->FcbState &= ~FCB_STATE_STREAM_OPEN;
    Vcb->OpenCount--;

    if ((Fcb->FcbState & FCB_STATE_TEARDOWN) && Fcb->RefCount == 0) {

        Parent = Fcb->ParentDcb;
        FatDeleteFcb(Fcb);
        FatDereferenceFcb(Vcb, Parent);
    }
}

/* ------------------------------------------------------------------ */
/* CCBs                                                                */
/* ------------------------------------------------------------------ */

PFAT_CCB
FatCreateCcb (
    VOID
    )
{
    PFAT_CCB Ccb;

    Ccb = (PFAT_CCB)ExAllocatePoolWithTag(PagedPool, sizeof(FAT_CCB), FAT_TAG_CCB);
    if (Ccb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Ccb, sizeof(FAT_CCB));
    Ccb->NodeTypeCode = FAT_NTC_CCB;
    Ccb->NodeByteSize = (CSHORT)sizeof(FAT_CCB);

    return Ccb;
}

VOID
FatDeleteCcb (
    PFAT_CCB Ccb
    )
{
    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
    }

    ExFreePool(Ccb);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* "\dir\file", kept in the FCB for change notification */
NTSTATUS
FatBuildFullName (
    PFAT_FCB Fcb
    )
{
    PFAT_FCB Walk;
    ULONG Length = 0;
    PWSTR Buffer;
    PWSTR Write;

    if (Fcb->FullName.Buffer != NULL) {
        return STATUS_SUCCESS;
    }

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    if (Length > 0xFFFE) {
        return STATUS_NAME_TOO_LONG;
    }

    Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, FAT_TAG_NAME);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {

        Buffer[0] = L'\\';

    } else {

        Write = (PWSTR)((PUCHAR)Buffer + Length);

        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    Fcb->FullName.Buffer = Buffer;
    Fcb->FullName.Length = (USHORT)Length;
    Fcb->FullName.MaximumLength = (USHORT)Length;

    return STATUS_SUCCESS;
}

BOOLEAN
FatIsAncestor (
    PFAT_FCB Ancestor,
    PFAT_FCB Fcb
    )
{
    for (; Fcb != NULL; Fcb = Fcb->ParentDcb) {
        if (Fcb == Ancestor) {
            return TRUE;
        }
    }

    return FALSE;
}

/* After a rename: drop the cached full names of Fcb and all below it */
VOID
FatForgetNames (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    PLIST_ENTRY Entry;
    PFAT_FCB Walk;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Walk = CONTAINING_RECORD(Entry, FAT_FCB, FcbLinks);

        if (Walk->FullName.Buffer != NULL && FatIsAncestor(Fcb, Walk)) {
            ExFreePool(Walk->FullName.Buffer);
            RtlZeroMemory(&Walk->FullName, sizeof(UNICODE_STRING));
        }
    }
}
