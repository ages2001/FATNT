/*
 * FATNT - changing directories: creating, rewriting, moving and removing
 * entry sets, growing directories, initializing "." and "..", the volume
 * label, change notification.
 *
 * Everything here runs with Vcb->Resource exclusive.
 */

#include "fat.h"

/* ------------------------------------------------------------------ */
/* Entry access                                                        */
/* ------------------------------------------------------------------ */

static VOID
FatReadEntries (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    ULONG Offset,
    ULONG Count,
    PUCHAR Set
    )
{
    FAT_MAP Map;
    ULONG i;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (i = 0; i < Count; i++) {
            RtlCopyMemory(Set + i * FAT_DIRENT_SIZE,
                          FatMapStream(Vcb, Dcb, &Map, Offset + i * FAT_DIRENT_SIZE,
                                       FAT_DIRENT_SIZE),
                          FAT_DIRENT_SIZE);
        }

    } __finally {

        FatUnmap(&Map);
    }
}

static VOID
FatWriteEntries (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    ULONG Offset,
    ULONG First,
    ULONG Count,
    PUCHAR Set
    )
{
    FAT_MAP Map;
    ULONG i;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (i = First; i < First + Count; i++) {
            RtlCopyMemory(FatPinStream(Vcb, Dcb, &Map, Offset + i * FAT_DIRENT_SIZE,
                                       FAT_DIRENT_SIZE),
                          Set + i * FAT_DIRENT_SIZE,
                          FAT_DIRENT_SIZE);
            FatSetDirty(&Map);
        }

    } __finally {

        FatUnmap(&Map);
    }
}

/* ------------------------------------------------------------------ */
/* Growth and free slots                                               */
/* ------------------------------------------------------------------ */

static NTSTATUS
FatGrowDirectory (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb
    )
{
    LONGLONG OldSize = Dcb->Header.AllocationSize.QuadPart;
    NTSTATUS Status;

    /* The FAT12/16 root directory is a fixed region and cannot grow */
    if (Dcb->FcbState & FCB_STATE_FIXED_ROOT) {
        return STATUS_DISK_FULL;
    }

    if (OldSize + Vcb->ClusterSize > FAT_MAX_DIR_SIZE) {
        return STATUS_CANNOT_MAKE;
    }

    Status = FatSetAllocation(Vcb, Dcb, (ULONGLONG)OldSize + Vcb->ClusterSize);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Dcb->Header.FileSize = Dcb->Header.AllocationSize;
    Dcb->Header.ValidDataLength = Dcb->Header.AllocationSize;

    if (Dcb->StreamFile != NULL) {
        CcSetFileSizes(Dcb->StreamFile, (PCC_FILE_SIZES)&Dcb->Header.AllocationSize);
    }

    /* The new cluster was zeroed on the disk by FatAllocateClusters; make
       the cache match so the end-of-directory mark is seen */
    FatZeroStream(Vcb, Dcb, OldSize, Vcb->ClusterSize);

    return STATUS_SUCCESS;
}

static NTSTATUS
FatFindFreeSlots (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    ULONG Count,
    PULONG Offset
    )
{
    FAT_MAP Map;
    ULONG Size = Dcb->Header.AllocationSize.LowPart;
    ULONG Pos;
    ULONG Run = 0;
    ULONG RunStart = 0;
    UCHAR Type;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (Pos = 0; Pos + FAT_DIRENT_SIZE <= Size; Pos += FAT_DIRENT_SIZE) {

            Type = *FatMapStream(Vcb, Dcb, &Map, Pos, 1);

            if (Type == FAT_DIRENT_END) {

                if (Run == 0) {
                    RunStart = Pos;
                }

                Run += (Size - Pos) / FAT_DIRENT_SIZE;
                break;
            }

            if (Type == FAT_DIRENT_FREE) {
                if (Run == 0) {
                    RunStart = Pos;
                }
                if (++Run == Count) {
                    break;
                }
            } else {
                Run = 0;
            }
        }

    } __finally {

        FatUnmap(&Map);
    }

    if (Run >= Count) {
        *Offset = RunStart;
        return STATUS_SUCCESS;
    }

    if (Run == 0) {
        RunStart = Size;
    }

    while ((Dcb->Header.AllocationSize.LowPart - RunStart) / FAT_DIRENT_SIZE < Count) {

        Status = FatGrowDirectory(Vcb, Dcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    *Offset = RunStart;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Short-name collisions                                               */
/* ------------------------------------------------------------------ */

static BOOLEAN
FatShortNameExists (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    const UCHAR *ShortName
    )
{
    FAT_MAP Map;
    ULONG Size = Dcb->Header.AllocationSize.LowPart;
    ULONG Pos;
    PUCHAR Entry;
    UCHAR Attr;
    BOOLEAN Found = FALSE;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (Pos = 0; Pos + FAT_DIRENT_SIZE <= Size; Pos += FAT_DIRENT_SIZE) {

            Entry = FatMapStream(Vcb, Dcb, &Map, Pos, FAT_DIRENT_SIZE);

            if (Entry[0] == FAT_DIRENT_END) {
                break;
            }

            if (Entry[0] == FAT_DIRENT_FREE) {
                continue;
            }

            Attr = Entry[11];

            if ((Attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                continue;       /* long-name entry */
            }

            if (RtlCompareMemory(Entry, ShortName, FAT_SFN_LEN) == FAT_SFN_LEN) {
                Found = TRUE;
                break;
            }
        }

    } __finally {

        FatUnmap(&Map);
    }

    return Found;
}

/* ------------------------------------------------------------------ */
/* Directory initialization                                            */
/* ------------------------------------------------------------------ */

/* Writes "." and ".." into a freshly allocated, zeroed directory cluster */
NTSTATUS
FatInitializeDirectory (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    ULONG FirstCluster
    )
{
    UCHAR Set[2 * FAT_DIRENT_SIZE];
    PFAT_DIRENT Dot = (PFAT_DIRENT)Set;
    PFAT_DIRENT DotDot = (PFAT_DIRENT)(Set + FAT_DIRENT_SIZE);
    FAT_DIR_INFO Info;
    LARGE_INTEGER Now;
    FAT_TIME_PARTS Parts;
    TIME_FIELDS Fields;
    LARGE_INTEGER Local;
    ULONG ParentCluster;
    ULONG i;

    RtlZeroMemory(Set, sizeof(Set));
    RtlZeroMemory(&Info, sizeof(Info));

    KeQuerySystemTime(&Now);
    ExSystemTimeToLocalTime(&Now, &Local);
    RtlTimeToTimeFields(&Local, &Fields);
    Parts.Year = (USHORT)Fields.Year;   Parts.Month = (USHORT)Fields.Month;
    Parts.Day = (USHORT)Fields.Day;     Parts.Hour = (USHORT)Fields.Hour;
    Parts.Minute = (USHORT)Fields.Minute; Parts.Second = (USHORT)Fields.Second;
    Parts.Milliseconds = (USHORT)Fields.Milliseconds;
    FatEncodeTime(&Parts, &Info.CreateDate, &Info.CreateTime, &Info.CreateTimeTenth);

    for (i = 0; i < FAT_SFN_LEN; i++) {
        Dot->Name[i] = ' ';
        DotDot->Name[i] = ' ';
    }

    Dot->Name[0] = '.';
    Dot->Attributes = FAT_ATTR_DIRECTORY;
    Dot->CreateDate = Info.CreateDate; Dot->CreateTime = Info.CreateTime;
    Dot->CreateTimeTenth = Info.CreateTimeTenth;
    Dot->WriteDate = Info.CreateDate; Dot->WriteTime = Info.CreateTime;
    Dot->LastAccessDate = Info.CreateDate;
    Dot->FirstClusterHigh = (USHORT)(FirstCluster >> 16);
    Dot->FirstClusterLow = (USHORT)(FirstCluster & 0xFFFF);

    DotDot->Name[0] = '.';
    DotDot->Name[1] = '.';
    DotDot->Attributes = FAT_ATTR_DIRECTORY;
    DotDot->CreateDate = Info.CreateDate; DotDot->CreateTime = Info.CreateTime;
    DotDot->CreateTimeTenth = Info.CreateTimeTenth;
    DotDot->WriteDate = Info.CreateDate; DotDot->WriteTime = Info.CreateTime;
    DotDot->LastAccessDate = Info.CreateDate;

    /* ".." points at the parent, or 0 when the parent is the root */
    ParentCluster = (Dcb->FcbState & FCB_STATE_ROOT) ? 0 : Dcb->FirstCluster;
    DotDot->FirstClusterHigh = (USHORT)(ParentCluster >> 16);
    DotDot->FirstClusterLow = (USHORT)(ParentCluster & 0xFFFF);

    /* The cluster is not cached as a stream yet: write it straight out */
    {
        PUCHAR Cluster;
        LONGLONG Lbo = FatClusterToLbo(Vcb, FirstCluster);
        NTSTATUS Status;

        Cluster = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->ClusterSize, FAT_TAG_BUFFER);
        if (Cluster == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(Cluster, Vcb->ClusterSize);
        RtlCopyMemory(Cluster, Set, sizeof(Set));

        Status = FatWriteSectors(Vcb->TargetDeviceObject, Lbo, Vcb->ClusterSize, Cluster, FALSE);

        ExFreePool(Cluster);
        return Status;
    }
}

/* ------------------------------------------------------------------ */
/* Entry sets                                                          */
/* ------------------------------------------------------------------ */

/*
 * Adds a new file or directory named Name to Dcb and returns its decoded
 * entry in Info. A directory gets one zeroed cluster with "." and "..".
 */
NTSTATUS
FatCreateDirent (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    PUNICODE_STRING Name,
    UCHAR Attributes,
    PFAT_DIR_INFO Info
    )
{
    UCHAR Set[(FAT_LFN_MAX_ENTRIES + 1) * FAT_DIRENT_SIZE];
    UCHAR ShortName[FAT_SFN_LEN];
    ULONG NameLength = Name->Length / sizeof(WCHAR);
    FAT_DIR_INFO Build;
    LARGE_INTEGER Now;
    FAT_TIME_PARTS Parts;
    TIME_FIELDS Fields;
    LARGE_INTEGER Local;
    UCHAR NtReserved = 0;
    BOOLEAN Lossy;
    ULONG Count;
    ULONG Offset;
    ULONG DirCluster = 0;
    ULONG Tail;
    NTSTATUS Status;

    RtlZeroMemory(&Build, sizeof(Build));

    KeQuerySystemTime(&Now);
    ExSystemTimeToLocalTime(&Now, &Local);
    RtlTimeToTimeFields(&Local, &Fields);
    Parts.Year = (USHORT)Fields.Year;   Parts.Month = (USHORT)Fields.Month;
    Parts.Day = (USHORT)Fields.Day;     Parts.Hour = (USHORT)Fields.Hour;
    Parts.Minute = (USHORT)Fields.Minute; Parts.Second = (USHORT)Fields.Second;
    Parts.Milliseconds = (USHORT)Fields.Milliseconds;

    FatEncodeTime(&Parts, &Build.CreateDate, &Build.CreateTime, &Build.CreateTimeTenth);
    Build.WriteDate = Build.CreateDate;
    Build.WriteTime = Build.CreateTime;
    Build.LastAccessDate = Build.CreateDate;
    Build.Attributes = Attributes;

    /* Pick the short name, generating a numeric tail when needed */
    if (FatBuildShortName(Name->Buffer, NameLength, ShortName, &NtReserved, &Lossy)) {

        /* A pure 8.3 name; a same-name collision is caught by the caller */

    } else {

        UCHAR Basis[FAT_SFN_LEN];
        BOOLEAN Dummy;

        FatBuildShortName(Name->Buffer, NameLength, Basis, &NtReserved, &Dummy);
        NtReserved = 0;

        for (Tail = 1; Tail <= 999999; Tail++) {
            RtlCopyMemory(ShortName, Basis, FAT_SFN_LEN);
            FatApplyNumericTail(ShortName, Tail);
            if (!FatShortNameExists(Vcb, Dcb, ShortName)) {
                break;
            }
        }

        if (Tail > 999999) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    /* A directory needs a first cluster with "." and ".." */
    if (Attributes & FAT_ATTR_DIRECTORY) {

        FAT_RUN_LIST Runs;
        RtlZeroMemory(&Runs, sizeof(Runs));

        Status = FatAllocateClusters(Vcb, &Runs, &DirCluster, 1, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        FatFreeRunList(&Runs);

        Build.FirstCluster = DirCluster;
        Build.FileSize = 0;
    }

    Count = FatBuildEntrySet(Name->Buffer, NameLength, ShortName, NtReserved, &Build, Set);

    Status = FatFindFreeSlots(Vcb, Dcb, Count, &Offset);
    if (!NT_SUCCESS(Status)) {
        goto Fail;
    }

    FatWriteEntries(Vcb, Dcb, Offset, 0, Count, Set);

    if (Attributes & FAT_ATTR_DIRECTORY) {

        /* The DCB for "." / ".." wants the real directory cluster */
        Build.FirstCluster = DirCluster;

        {
            FAT_FCB TempDcb;
            RtlZeroMemory(&TempDcb, sizeof(TempDcb));
            TempDcb.FirstCluster = Dcb->FirstCluster;
            TempDcb.FcbState = (Dcb->FcbState & FCB_STATE_ROOT);
            Status = FatInitializeDirectory(Vcb, &TempDcb, DirCluster);
        }

        if (!NT_SUCCESS(Status)) {
            FatRemoveDirent(Vcb, Dcb, Offset, Count);
            goto Fail;
        }
    }

    /* Decode what we wrote so the caller gets a clean Info */
    if (FatParseDirEntry(Set, Count, Offset, Info) != FAT_NAME_OK) {
        Status = STATUS_FILE_CORRUPT_ERROR;
        FatRemoveDirent(Vcb, Dcb, Offset, Count);
        goto Fail;
    }

    return STATUS_SUCCESS;

Fail:
    if (DirCluster != 0) {
        FAT_RUN_LIST Runs;
        RtlZeroMemory(&Runs, sizeof(Runs));
        if (NT_SUCCESS(FatBuildRunList(Vcb, DirCluster, 1, &Runs))) {
            ULONG First = DirCluster;
            FatFreeClusters(Vcb, &Runs, &First, 0);
        }
        FatFreeRunList(&Runs);
    }

    return Status;
}

/* Rewrites the short entry of Fcb from its current state (name unchanged) */
NTSTATUS
FatUpdateDirent (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    UCHAR Entry[FAT_DIRENT_SIZE];
    PFAT_DIRENT Dir = (PFAT_DIRENT)Entry;
    FAT_DIR_INFO Info;
    ULONG i;

    if ((Fcb->FcbState & (FCB_STATE_ROOT | FCB_STATE_DELETED)) || Fcb->ParentDcb == NULL ||
        (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED))) {

        Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;
        return STATUS_SUCCESS;
    }

    FatReadEntries(Vcb, Fcb->ParentDcb, Fcb->DirOffset, 1, Entry);

    if ((Dir->Attributes & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME ||
        Dir->Name[0] == FAT_DIRENT_FREE || Dir->Name[0] == FAT_DIRENT_END) {

        FAT_DBG((FAT_PFX "Short entry of %wZ moved away\n", &Fcb->Name));
        return STATUS_FILE_CORRUPT_ERROR;
    }

    FatFcbToInfo(Fcb, &Info);

    /* Keep the name bytes; rewrite attributes, times, cluster and size */
    Dir->Attributes       = Info.Attributes;
    Dir->NtReserved       = Fcb->NtReserved;
    Dir->CreateTimeTenth  = Info.CreateTimeTenth;
    Dir->CreateTime       = Info.CreateTime;
    Dir->CreateDate       = Info.CreateDate;
    Dir->LastAccessDate   = Info.LastAccessDate;
    Dir->FirstClusterHigh = (USHORT)(Info.FirstCluster >> 16);
    Dir->WriteTime        = Info.WriteTime;
    Dir->WriteDate        = Info.WriteDate;
    Dir->FirstClusterLow  = (USHORT)(Info.FirstCluster & 0xFFFF);
    Dir->FileSize         = Info.FileSize;

    for (i = 0; i < FAT_SFN_LEN; i++) {
        Dir->Name[i] = Fcb->ShortName[i];
    }

    FatWriteEntries(Vcb, Fcb->ParentDcb, Fcb->DirOffset, 0, 1, Entry);

    Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;

    return STATUS_SUCCESS;
}

NTSTATUS
FatRemoveDirent (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    ULONG Offset,
    ULONG Count
    )
{
    FAT_MAP Map;
    PUCHAR Entry;
    ULONG i;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (i = 0; i < Count; i++) {
            Entry = FatPinStream(Vcb, Dcb, &Map, Offset + i * FAT_DIRENT_SIZE, FAT_DIRENT_SIZE);
            Entry[0] = FAT_DIRENT_FREE;
            FatSetDirty(&Map);
        }

    } __finally {

        FatUnmap(&Map);
    }

    return STATUS_SUCCESS;
}

/*
 * Gives Fcb a new name in TargetDcb: the new entry set is written first,
 * then the old one removed. The caller has made sure the name is free.
 */
NTSTATUS
FatMoveDirent (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFAT_FCB TargetDcb,
    PUNICODE_STRING Name
    )
{
    UCHAR Set[(FAT_LFN_MAX_ENTRIES + 1) * FAT_DIRENT_SIZE];
    UCHAR ShortName[FAT_SFN_LEN];
    ULONG NameLength = Name->Length / sizeof(WCHAR);
    PFAT_FCB OldParent = Fcb->ParentDcb;
    FAT_DIR_INFO Info;
    PWSTR NewName;
    UCHAR NtReserved = 0;
    BOOLEAN Lossy;
    ULONG Count;
    ULONG Offset;
    ULONG Tail;
    NTSTATUS Status;

    NewName = (PWSTR)ExAllocatePoolWithTag(PagedPool, Name->Length, FAT_TAG_NAME);
    if (NewName == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(NewName, Name->Buffer, Name->Length);

    if (!FatBuildShortName(Name->Buffer, NameLength, ShortName, &NtReserved, &Lossy)) {

        UCHAR Basis[FAT_SFN_LEN];
        BOOLEAN Dummy;

        FatBuildShortName(Name->Buffer, NameLength, Basis, &NtReserved, &Dummy);
        NtReserved = 0;

        for (Tail = 1; Tail <= 999999; Tail++) {
            RtlCopyMemory(ShortName, Basis, FAT_SFN_LEN);
            FatApplyNumericTail(ShortName, Tail);
            if (!FatShortNameExists(Vcb, TargetDcb, ShortName)) {
                break;
            }
        }

        if (Tail > 999999) {
            ExFreePool(NewName);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    FatFcbToInfo(Fcb, &Info);
    Count = FatBuildEntrySet(Name->Buffer, NameLength, ShortName, NtReserved, &Info, Set);

    Status = FatFindFreeSlots(Vcb, TargetDcb, Count, &Offset);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(NewName);
        return Status;
    }

    FatWriteEntries(Vcb, TargetDcb, Offset, 0, Count, Set);
    FatRemoveDirent(Vcb, OldParent, Fcb->SetOffset, Fcb->EntryCount);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }

    Fcb->Name.Buffer = NewName;
    Fcb->Name.Length = Name->Length;
    Fcb->Name.MaximumLength = Name->Length;

    RtlCopyMemory(Fcb->ShortName, ShortName, FAT_SFN_LEN);
    Fcb->NtReserved = NtReserved;
    Fcb->SetOffset = Offset;
    Fcb->DirOffset = Offset + (Count - 1) * FAT_DIRENT_SIZE;
    Fcb->EntryCount = Count;
    Fcb->IndexNumber = ((LONGLONG)TargetDcb->FirstCluster << 32) | Fcb->DirOffset;
    Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;

    /* A directory moved to a new parent must point ".." at it */
    if (TargetDcb != OldParent && FatIsDcb(Fcb) && Fcb->FirstCluster != 0) {

        UCHAR DotDot[FAT_DIRENT_SIZE];
        PFAT_DIRENT Dd = (PFAT_DIRENT)DotDot;
        ULONG ParentCluster = (TargetDcb->FcbState & FCB_STATE_ROOT) ? 0 : TargetDcb->FirstCluster;

        FatReadEntries(Vcb, Fcb, FAT_DIRENT_SIZE, 1, DotDot);

        if (Dd->Name[0] == '.' && Dd->Name[1] == '.') {
            Dd->FirstClusterHigh = (USHORT)(ParentCluster >> 16);
            Dd->FirstClusterLow = (USHORT)(ParentCluster & 0xFFFF);
            FatWriteEntries(Vcb, Fcb, FAT_DIRENT_SIZE, 0, 1, DotDot);
        }
    }

    if (TargetDcb != OldParent) {
        TargetDcb->RefCount++;
        Fcb->ParentDcb = TargetDcb;
        FatDereferenceFcb(Vcb, OldParent);
    }

    FatForgetNames(Vcb, Fcb);

    return STATUS_SUCCESS;
}

/*
 * Removes a file or an empty directory from the disk: its entry set
 * first, then its clusters. The FCB stays, marked deleted, until its last
 * reference goes.
 */
NTSTATUS
FatDeleteFromDisk (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb
    )
{
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_DELETED) {
        return STATUS_SUCCESS;
    }

    if (FatIsDcb(Fcb)) {
        FatCloseStream(Vcb, Fcb, TRUE);
    }

    Status = FatRemoveDirent(Vcb, Fcb->ParentDcb, Fcb->SetOffset, Fcb->EntryCount);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Fcb->FcbState |= FCB_STATE_DELETED;
    Fcb->FcbState &= ~(FCB_STATE_DIRENT_DIRTY | FCB_STATE_TRUNCATE_ON_CLOSE);

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    Fcb->Header.FileSize.QuadPart = 0;
    Fcb->Header.ValidDataLength.QuadPart = 0;

    FatRelease(&Fcb->PagingIoResource);

    return FatSetAllocation(Vcb, Fcb, 0);
}

NTSTATUS
FatIsDirectoryEmpty (
    PFAT_VCB Vcb,
    PFAT_FCB Dcb,
    PBOOLEAN Empty
    )
{
    ULONG Offset = 0;
    PFAT_SCAN Scan;

    *Empty = TRUE;

    Scan = FatAllocateScan();
    if (Scan == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (FatNextDirEntry(Vcb, Dcb, &Offset, Scan)) {
        *Empty = FALSE;
    }

    FatFreeScan(Scan);

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Volume label                                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
FatWriteLabel (
    PFAT_VCB Vcb,
    const WCHAR *Label,
    ULONG Length
    )
{
    PFAT_FCB Root = Vcb->RootDcb;
    UCHAR Entry[FAT_DIRENT_SIZE];
    PFAT_DIRENT Dir = (PFAT_DIRENT)Entry;
    FAT_MAP Map;
    ULONG Size = Root->Header.AllocationSize.LowPart;
    ULONG Pos;
    ULONG Found = 0xFFFFFFFF;
    ULONG i;
    UCHAR Attr;
    UCHAR Type;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (Pos = 0; Pos + FAT_DIRENT_SIZE <= Size; Pos += FAT_DIRENT_SIZE) {

            PUCHAR e = FatMapStream(Vcb, Root, &Map, Pos, FAT_DIRENT_SIZE);
            Type = e[0];

            if (Type == FAT_DIRENT_END) {
                break;
            }

            if (Type == FAT_DIRENT_FREE) {
                continue;
            }

            Attr = e[11];

            if ((Attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_VOLUME_ID) {
                Found = Pos;
                break;
            }
        }

    } __finally {

        FatUnmap(&Map);
    }

    if (Found == 0xFFFFFFFF) {

        if (Length == 0) {
            Vcb->LabelLength = 0;
            return STATUS_SUCCESS;
        }

        Status = FatFindFreeSlots(Vcb, Root, 1, &Found);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    RtlZeroMemory(Entry, sizeof(Entry));

    for (i = 0; i < FAT_SFN_LEN; i++) {
        Dir->Name[i] = ' ';
    }

    Dir->Attributes = FAT_ATTR_VOLUME_ID;

    /* The label is stored OEM-upcased in the 11 name bytes */
    for (i = 0; i < Length && i < FAT_MAX_LABEL; i++) {
        WCHAR Ch = FatUpcaseChar(Label[i]);
        Dir->Name[i] = (UCHAR)((Ch < 0x100) ? Ch : '_');
        Vcb->Label[i] = Ch;
    }

    Vcb->LabelLength = (Length > FAT_MAX_LABEL) ? FAT_MAX_LABEL : Length;

    FatWriteEntries(Vcb, Root, Found, 0, 1, Entry);

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Change notification                                                 */
/* ------------------------------------------------------------------ */

VOID
FatNotifyChange (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    ULONG Filter,
    ULONG Action
    )
{
    if (Fcb->ParentDcb == NULL || !NT_SUCCESS(FatBuildFullName(Fcb))) {
        return;
    }

    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)&Fcb->FullName,
                                (USHORT)(Fcb->FullName.Length - Fcb->Name.Length),
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
}
