/*
 * FATNT - cluster chains, run lists and free space
 *
 * Every stream (file, subdirectory) is described by a run list built once
 * from its FAT chain. There is no allocation bitmap: free clusters are
 * the FAT entries that read as zero, found by scanning from a hint. FAT12
 * entries are 12 bits and can straddle a byte pair, so the FAT is read
 * and written a byte at a time through its cached stream.
 */

#include "fat.h"

/* ------------------------------------------------------------------ */
/* Raw FAT entry access                                                */
/* ------------------------------------------------------------------ */

static UCHAR
FatReadFatByte (
    PFAT_VCB Vcb,
    PFAT_MAP Map,
    ULONG ByteOffset
    )
{
    return *FatMapStream(Vcb, Vcb->FatFcb, Map, (LONGLONG)ByteOffset, 1);
}

static VOID
FatWriteFatByte (
    PFAT_VCB Vcb,
    PFAT_MAP Map,
    ULONG ByteOffset,
    UCHAR Value
    )
{
    PUCHAR p = FatPinStream(Vcb, Vcb->FatFcb, Map, (LONGLONG)ByteOffset, 1);
    *p = Value;
    FatSetDirty(Map);
}

ULONG
FatGetFatEntry (
    PFAT_VCB Vcb,
    PFAT_MAP Map,
    ULONG Cluster
    )
{
    ULONG Value;
    ULONG Off;

    switch (Vcb->FatType) {

    case FAT_TYPE_12:
        Off = Cluster + (Cluster / 2);
        {
            UCHAR b0 = FatReadFatByte(Vcb, Map, Off);
            UCHAR b1 = FatReadFatByte(Vcb, Map, Off + 1);
            if (Cluster & 1) {
                Value = ((ULONG)b0 >> 4) | ((ULONG)b1 << 4);
            } else {
                Value = (ULONG)b0 | (((ULONG)b1 & 0x0F) << 8);
            }
            Value &= FAT12_MASK;
        }
        break;

    case FAT_TYPE_16:
        Off = Cluster * 2;
        {
            UCHAR b0 = FatReadFatByte(Vcb, Map, Off);
            UCHAR b1 = FatReadFatByte(Vcb, Map, Off + 1);
            Value = ((ULONG)b0 | ((ULONG)b1 << 8)) & FAT16_MASK;
        }
        break;

    default:
        Off = Cluster * 4;
        {
            UCHAR b0 = FatReadFatByte(Vcb, Map, Off);
            UCHAR b1 = FatReadFatByte(Vcb, Map, Off + 1);
            UCHAR b2 = FatReadFatByte(Vcb, Map, Off + 2);
            UCHAR b3 = FatReadFatByte(Vcb, Map, Off + 3);
            Value = ((ULONG)b0 | ((ULONG)b1 << 8) | ((ULONG)b2 << 16) | ((ULONG)b3 << 24))
                    & FAT32_MASK;
        }
        break;
    }

    return Value;
}

VOID
FatSetFatEntry (
    PFAT_VCB Vcb,
    PFAT_MAP Map,
    ULONG Cluster,
    ULONG Value
    )
{
    ULONG Off;

    switch (Vcb->FatType) {

    case FAT_TYPE_12:
        Off = Cluster + (Cluster / 2);
        if (Cluster & 1) {
            UCHAR b0 = FatReadFatByte(Vcb, Map, Off);
            FatWriteFatByte(Vcb, Map, Off, (UCHAR)((b0 & 0x0F) | ((Value << 4) & 0xF0)));
            FatWriteFatByte(Vcb, Map, Off + 1, (UCHAR)((Value >> 4) & 0xFF));
        } else {
            UCHAR b1 = FatReadFatByte(Vcb, Map, Off + 1);
            FatWriteFatByte(Vcb, Map, Off, (UCHAR)(Value & 0xFF));
            FatWriteFatByte(Vcb, Map, Off + 1, (UCHAR)((b1 & 0xF0) | ((Value >> 8) & 0x0F)));
        }
        break;

    case FAT_TYPE_16:
        Off = Cluster * 2;
        FatWriteFatByte(Vcb, Map, Off, (UCHAR)(Value & 0xFF));
        FatWriteFatByte(Vcb, Map, Off + 1, (UCHAR)((Value >> 8) & 0xFF));
        break;

    default:
        Off = Cluster * 4;
        {
            /* The top four bits of a FAT32 entry are reserved */
            UCHAR b3 = FatReadFatByte(Vcb, Map, Off + 3);
            FatWriteFatByte(Vcb, Map, Off, (UCHAR)(Value & 0xFF));
            FatWriteFatByte(Vcb, Map, Off + 1, (UCHAR)((Value >> 8) & 0xFF));
            FatWriteFatByte(Vcb, Map, Off + 2, (UCHAR)((Value >> 16) & 0xFF));
            FatWriteFatByte(Vcb, Map, Off + 3, (UCHAR)((b3 & 0xF0) | ((Value >> 24) & 0x0F)));
        }
        break;
    }
}

static BOOLEAN
FatIsEndOfChain (
    PFAT_VCB Vcb,
    ULONG Value
    )
{
    switch (Vcb->FatType) {
    case FAT_TYPE_12: return (BOOLEAN)(Value >= FAT12_EOC);
    case FAT_TYPE_16: return (BOOLEAN)(Value >= FAT16_EOC);
    default:          return (BOOLEAN)(Value >= FAT32_EOC);
    }
}

static BOOLEAN
FatIsValidCluster (
    PFAT_VCB Vcb,
    ULONG Cluster
    )
{
    return (BOOLEAN)(Cluster >= FAT_FIRST_CLUSTER &&
                     Cluster < Vcb->ClusterCount + FAT_FIRST_CLUSTER);
}

/* ------------------------------------------------------------------ */
/* Run lists                                                           */
/* ------------------------------------------------------------------ */

static NTSTATUS
FatAppendRun (
    PFAT_RUN_LIST RunList,
    ULONG Lcn,
    ULONG Count
    )
{
    PFAT_RUN Last;
    PFAT_RUN NewRuns;
    ULONG NewMax;

    if (RunList->RunCount != 0) {

        Last = &RunList->Runs[RunList->RunCount - 1];

        if (Last->Lcn + Last->Count == Lcn) {
            Last->Count += Count;
            RunList->Clusters += Count;
            return STATUS_SUCCESS;
        }
    }

    if (RunList->RunCount == RunList->RunMax) {

        NewMax = RunList->RunMax ? RunList->RunMax * 2 : 4;

        NewRuns = (PFAT_RUN)ExAllocatePoolWithTag(PagedPool, NewMax * sizeof(FAT_RUN), FAT_TAG_RUNS);
        if (NewRuns == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (RunList->Runs != NULL) {
            RtlCopyMemory(NewRuns, RunList->Runs, RunList->RunCount * sizeof(FAT_RUN));
            ExFreePool(RunList->Runs);
        }

        RunList->Runs = NewRuns;
        RunList->RunMax = NewMax;
    }

    RunList->Runs[RunList->RunCount].Vcn = RunList->Clusters;
    RunList->Runs[RunList->RunCount].Lcn = Lcn;
    RunList->Runs[RunList->RunCount].Count = Count;
    RunList->RunCount++;
    RunList->Clusters += Count;

    return STATUS_SUCCESS;
}

static NTSTATUS
FatReserveRuns (
    PFAT_RUN_LIST RunList,
    ULONG Extra
    )
{
    PFAT_RUN NewRuns;
    ULONG NewMax;

    if (RunList->RunCount + Extra <= RunList->RunMax) {
        return STATUS_SUCCESS;
    }

    NewMax = RunList->RunCount + Extra + 4;

    NewRuns = (PFAT_RUN)ExAllocatePoolWithTag(PagedPool, NewMax * sizeof(FAT_RUN), FAT_TAG_RUNS);
    if (NewRuns == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (RunList->Runs != NULL) {
        RtlCopyMemory(NewRuns, RunList->Runs, RunList->RunCount * sizeof(FAT_RUN));
        ExFreePool(RunList->Runs);
    }

    RunList->Runs = NewRuns;
    RunList->RunMax = NewMax;

    return STATUS_SUCCESS;
}

VOID
FatFreeRunList (
    PFAT_RUN_LIST RunList
    )
{
    if (RunList->Runs != NULL) {
        ExFreePool(RunList->Runs);
    }

    RtlZeroMemory(RunList, sizeof(FAT_RUN_LIST));
}

static ULONG
FatLastCluster (
    PFAT_RUN_LIST RunList
    )
{
    PFAT_RUN Last = &RunList->Runs[RunList->RunCount - 1];

    return Last->Lcn + Last->Count - 1;
}

/*
 * Clusters is the expected length; 0 follows the chain to its end.
 * FirstCluster == 0 means an empty stream.
 */
NTSTATUS
FatBuildRunList (
    PFAT_VCB Vcb,
    ULONG FirstCluster,
    ULONG Clusters,
    PFAT_RUN_LIST RunList
    )
{
    FAT_MAP Map;
    ULONG Cluster;
    ULONG Next;
    ULONG Count = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(RunList, sizeof(FAT_RUN_LIST));
    RtlZeroMemory(&Map, sizeof(Map));

    if (FirstCluster == 0) {
        return (Clusters == 0) ? STATUS_SUCCESS : STATUS_FILE_CORRUPT_ERROR;
    }

    if (!FatIsValidCluster(Vcb, FirstCluster)) {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    __try {

        Cluster = FirstCluster;

        for (;;) {

            if (!FatIsValidCluster(Vcb, Cluster)) {
                Status = STATUS_FILE_CORRUPT_ERROR;
                __leave;
            }

            Status = FatAppendRun(RunList, Cluster, 1);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Count++;

            if (Clusters != 0 && Count == Clusters) {
                break;
            }

            if (Count > Vcb->ClusterCount) {
                Status = STATUS_FILE_CORRUPT_ERROR;
                __leave;
            }

            Next = FatGetFatEntry(Vcb, &Map, Cluster);

            if (FatIsEndOfChain(Vcb, Next)) {

                if (Clusters != 0) {
                    Status = STATUS_FILE_CORRUPT_ERROR;
                    __leave;
                }

                break;
            }

            Cluster = Next;
        }

    } __finally {

        FatUnmap(&Map);

        if (AbnormalTermination() || !NT_SUCCESS(Status)) {
            FatFreeRunList(RunList);
        }
    }

    if (!NT_SUCCESS(Status)) {
        FAT_DBG((FAT_PFX "Bad cluster chain at %lu\n", FirstCluster));
    }

    return Status;
}

BOOLEAN
FatLookupVbo (
    PFAT_VCB Vcb,
    PFAT_RUN_LIST RunList,
    LONGLONG Vbo,
    PLONGLONG Lbo,
    PULONG Contiguous
    )
{
    PFAT_RUN Run;
    ULONG Vcn;
    ULONG Low;
    ULONG High;
    ULONG Middle;
    ULONG Within;
    ULONG ClusterOffset;
    ULONGLONG Remaining;

    if (Vbo < 0 || (ULONGLONG)Vbo >= ((ULONGLONG)RunList->Clusters << Vcb->ClusterShift)) {
        return FALSE;
    }

    Vcn = (ULONG)((ULONGLONG)Vbo >> Vcb->ClusterShift);

    Low = 0;
    High = RunList->RunCount;

    while (Low < High) {

        Middle = (Low + High) / 2;
        Run = &RunList->Runs[Middle];

        if (Vcn < Run->Vcn) {
            High = Middle;
        } else if (Vcn >= Run->Vcn + Run->Count) {
            Low = Middle + 1;
        } else {

            Within = Vcn - Run->Vcn;
            ClusterOffset = (ULONG)Vbo & (Vcb->ClusterSize - 1);

            *Lbo = FatClusterToLbo(Vcb, Run->Lcn + Within) + ClusterOffset;

            Remaining = ((ULONGLONG)(Run->Count - Within) << Vcb->ClusterShift) - ClusterOffset;
            *Contiguous = (Remaining > 0x40000000) ? 0x40000000 : (ULONG)Remaining;

            return TRUE;
        }
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Free count                                                          */
/* ------------------------------------------------------------------ */

NTSTATUS
FatCountFreeClusters (
    PFAT_VCB Vcb
    )
{
    FAT_MAP Map;
    ULONG Cluster;
    ULONG Free = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (Cluster = FAT_FIRST_CLUSTER; Cluster < Vcb->ClusterCount + FAT_FIRST_CLUSTER;
             Cluster++) {

            if (FatGetFatEntry(Vcb, &Map, Cluster) == 0) {
                Free++;
            }
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();
    }

    FatUnmap(&Map);

    if (NT_SUCCESS(Status)) {
        Vcb->FreeClusters = Free;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Zeroing new clusters straight to the disk                           */
/* ------------------------------------------------------------------ */

static NTSTATUS
FatZeroClusters (
    PFAT_VCB Vcb,
    ULONG Lcn,
    ULONG Count
    )
{
    PUCHAR Zero;
    LONGLONG Lbo = FatClusterToLbo(Vcb, Lcn);
    ULONGLONG Left = (ULONGLONG)Count << Vcb->ClusterShift;
    ULONG Chunk;
    NTSTATUS Status = STATUS_SUCCESS;

    Zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, FAT_ZERO_CHUNK, FAT_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Zero, FAT_ZERO_CHUNK);

    while (Left != 0 && NT_SUCCESS(Status)) {

        Chunk = (Left > FAT_ZERO_CHUNK) ? FAT_ZERO_CHUNK : (ULONG)Left;

        Status = FatWriteSectors(Vcb->TargetDeviceObject, Lbo, Chunk, Zero, FALSE);

        Lbo += Chunk;
        Left -= Chunk;
    }

    ExFreePool(Zero);
    return Status;
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Finds the next free cluster at or after *From (wrapping once) and
 * claims it by writing the end-of-chain mark, so a later scan in the same
 * allocation will not find it again. Returns FALSE when the volume is
 * full.
 */
static BOOLEAN
FatClaimFreeCluster (
    PFAT_VCB Vcb,
    PFAT_MAP Map,
    ULONG From,
    PULONG Cluster
    )
{
    ULONG Total = Vcb->ClusterCount;
    ULONG Index;
    ULONG Scanned;

    if (!FatIsValidCluster(Vcb, From)) {
        From = FAT_FIRST_CLUSTER;
    }

    Index = From;

    for (Scanned = 0; Scanned < Total; Scanned++) {

        if (Index >= Vcb->ClusterCount + FAT_FIRST_CLUSTER) {
            Index = FAT_FIRST_CLUSTER;
        }

        if (FatGetFatEntry(Vcb, Map, Index) == 0) {
            FatSetFatEntry(Vcb, Map, Index, Vcb->EndOfChain);
            *Cluster = Index;
            return TRUE;
        }

        Index++;
    }

    return FALSE;
}

/*
 * Adds Clusters clusters to the end of a stream. *FirstCluster is 0 for
 * an empty stream and is set to the new head. With ZeroNew the new
 * clusters are zeroed on the disk (directories). Called with the stream's
 * paging resource exclusive when others can read its run list.
 */
NTSTATUS
FatAllocateClusters (
    PFAT_VCB Vcb,
    PFAT_RUN_LIST RunList,
    PULONG FirstCluster,
    ULONG Clusters,
    BOOLEAN ZeroNew
    )
{
    FAT_MAP Map;
    FAT_RUN_LIST New;
    ULONG Prev;
    ULONG Cluster = 0;
    ULONG Done = 0;
    ULONG i;
    BOOLEAN WasEmpty = (BOOLEAN)(RunList->RunCount == 0);
    NTSTATUS Status = STATUS_SUCCESS;

    if (Clusters == 0) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&Map, sizeof(Map));
    RtlZeroMemory(&New, sizeof(New));

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    __try {

        if ((Vcb->FreeClusters != FAT_FSINFO_UNKNOWN && Clusters > Vcb->FreeClusters) ||
            (ULONGLONG)RunList->Clusters + Clusters > Vcb->ClusterCount) {

            Status = STATUS_DISK_FULL;
            __leave;
        }

        FatMarkVolumeDirty(Vcb);

        Prev = WasEmpty ? 0 : FatLastCluster(RunList);

        for (Done = 0; Done < Clusters; Done++) {

            if (!FatClaimFreeCluster(Vcb, &Map, Vcb->NextFree, &Cluster)) {
                Status = STATUS_DISK_FULL;
                __leave;
            }

            Status = FatAppendRun(&New, Cluster, 1);
            if (!NT_SUCCESS(Status)) {
                /* Give this one back; the rest are freed in the handler */
                FatSetFatEntry(Vcb, &Map, Cluster, 0);
                __leave;
            }

            /* Chain the previous cluster to this one */
            if (Prev != 0) {
                FatSetFatEntry(Vcb, &Map, Prev, Cluster);
            }

            Prev = Cluster;
            Vcb->NextFree = Cluster + 1;
        }

        Status = FatReserveRuns(RunList, New.RunCount);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (ZeroNew) {
            for (i = 0; i < New.RunCount; i++) {
                Status = FatZeroClusters(Vcb, New.Runs[i].Lcn, New.Runs[i].Count);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }
        }

        /* Commit: nothing below can fail */
        if (Vcb->FreeClusters != FAT_FSINFO_UNKNOWN) {
            Vcb->FreeClusters -= Clusters;
        }

        if (WasEmpty) {
            *FirstCluster = New.Runs[0].Lcn;
        }

        for (i = 0; i < New.RunCount; i++) {
            (VOID)FatAppendRun(RunList, New.Runs[i].Lcn, New.Runs[i].Count);
        }

    } __finally {

        if (AbnormalTermination() || !NT_SUCCESS(Status)) {

            /* Free every cluster claimed in this call and unlink the chain */
            for (i = 0; i < New.RunCount; i++) {
                ULONG c;
                for (c = 0; c < New.Runs[i].Count; c++) {
                    FatSetFatEntry(Vcb, &Map, New.Runs[i].Lcn + c, 0);
                }
            }

            if (!WasEmpty && RunList->RunCount != 0) {
                FatSetFatEntry(Vcb, &Map, FatLastCluster(RunList), Vcb->EndOfChain);
            }
        }

        FatUnmap(&Map);
        FatFreeRunList(&New);
        FatRelease(&Vcb->AllocResource);
    }

    return Status;
}

/*
 * Keeps the first Keep clusters of a stream and frees the rest, writing
 * zero into their FAT entries. With Keep == 0 the whole chain goes and
 * *FirstCluster becomes 0. The caller has dropped every cached view of
 * the freed part.
 */
VOID
FatFreeClusters (
    PFAT_VCB Vcb,
    PFAT_RUN_LIST RunList,
    PULONG FirstCluster,
    ULONG Keep
    )
{
    FAT_MAP Map;
    PFAT_RUN Run;
    ULONG From;
    ULONG Freed;
    ULONG c;

    if (Keep >= RunList->Clusters) {
        return;
    }

    RtlZeroMemory(&Map, sizeof(Map));

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    __try {

        FatMarkVolumeDirty(Vcb);

        while (RunList->RunCount != 0) {

            Run = &RunList->Runs[RunList->RunCount - 1];

            if (Run->Vcn + Run->Count <= Keep) {
                break;
            }

            From = (Run->Vcn > Keep) ? Run->Vcn : Keep;
            Freed = Run->Vcn + Run->Count - From;

            for (c = 0; c < Freed; c++) {
                FatSetFatEntry(Vcb, &Map, Run->Lcn + (From - Run->Vcn) + c, 0);
            }

            if (Vcb->FreeClusters != FAT_FSINFO_UNKNOWN) {
                Vcb->FreeClusters += Freed;
            }

            RunList->Clusters -= Freed;
            Run->Count -= Freed;

            if (Run->Count == 0) {
                RunList->RunCount--;
            }
        }

        if (RunList->RunCount != 0) {
            FatSetFatEntry(Vcb, &Map, FatLastCluster(RunList), Vcb->EndOfChain);
        } else {
            *FirstCluster = 0;
        }

    } __finally {

        FatUnmap(&Map);
        FatRelease(&Vcb->AllocResource);
    }
}

/*
 * Sets the allocation of a file or directory to Bytes rounded up to
 * clusters. Called with the FCB exclusive; shrinking callers have already
 * cut FileSize and the cache.
 */
NTSTATUS
FatSetAllocation (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    ULONGLONG Bytes
    )
{
    ULONGLONG Clusters;
    ULONG Have = Fcb->RunList.Clusters;
    BOOLEAN IsDir = (BOOLEAN)FatIsDcb(Fcb);
    NTSTATUS Status = STATUS_SUCCESS;

    Clusters = (Bytes + Vcb->ClusterSize - 1) >> Vcb->ClusterShift;

    if (Clusters > Vcb->ClusterCount) {
        return STATUS_DISK_FULL;
    }

    if ((ULONG)Clusters == Have) {
        return STATUS_SUCCESS;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    __try {

        if ((ULONG)Clusters > Have) {

            Status = FatAllocateClusters(Vcb, &Fcb->RunList, &Fcb->FirstCluster,
                                         (ULONG)Clusters - Have, IsDir);
        } else {

            FatFreeClusters(Vcb, &Fcb->RunList, &Fcb->FirstCluster, (ULONG)Clusters);
        }

        Fcb->Header.AllocationSize.QuadPart = (LONGLONG)Fcb->RunList.Clusters << Vcb->ClusterShift;
        Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

    } __finally {

        FatRelease(&Fcb->PagingIoResource);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Second FAT and FSInfo                                               */
/* ------------------------------------------------------------------ */

/* Copies the active FAT over the other FAT copies on the disk */
NTSTATUS
FatMirrorFats (
    PFAT_VCB Vcb
    )
{
    PUCHAR Buffer;
    LONGLONG Src;
    LONGLONG Dst;
    ULONGLONG Left;
    ULONG FatBytes = Vcb->FatSectors << Vcb->SectorShift;
    ULONG Chunk;
    ULONG Fat;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Vcb->NumberOfFats < 2) {
        return STATUS_SUCCESS;
    }

    Buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, FAT_ZERO_CHUNK, FAT_TAG_BUFFER);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Fat = 1; Fat < Vcb->NumberOfFats && NT_SUCCESS(Status); Fat++) {

        Src = (LONGLONG)Vcb->FatSector << Vcb->SectorShift;
        Dst = (LONGLONG)(Vcb->FatSector + Fat * Vcb->FatSectors) << Vcb->SectorShift;
        Left = FatBytes;

        while (Left != 0) {

            Chunk = (Left > FAT_ZERO_CHUNK) ? FAT_ZERO_CHUNK : (ULONG)Left;

            Status = FatReadSectors(Vcb->TargetDeviceObject, Src, Chunk, Buffer, FALSE);
            if (!NT_SUCCESS(Status)) {
                break;
            }

            Status = FatWriteSectors(Vcb->TargetDeviceObject, Dst, Chunk, Buffer, FALSE);
            if (!NT_SUCCESS(Status)) {
                break;
            }

            Src += Chunk;
            Dst += Chunk;
            Left -= Chunk;
        }
    }

    ExFreePool(Buffer);
    return Status;
}

/* Writes the free count and next-free hint into the FAT32 FSInfo sector */
NTSTATUS
FatWriteFsInfo (
    PFAT_VCB Vcb
    )
{
    PFAT_FSINFO FsInfo;
    LONGLONG Offset;
    NTSTATUS Status;

    if (Vcb->FatType != FAT_TYPE_32 || Vcb->FsInfoSector == 0) {
        return STATUS_SUCCESS;
    }

    FsInfo = (PFAT_FSINFO)ExAllocatePoolWithTag(NonPagedPool, Vcb->SectorSize, FAT_TAG_BUFFER);
    if (FsInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Offset = (LONGLONG)Vcb->FsInfoSector << Vcb->SectorShift;

    Status = FatReadSectors(Vcb->TargetDeviceObject, Offset, Vcb->SectorSize, FsInfo, FALSE);

    if (NT_SUCCESS(Status) && FsInfo->LeadSignature == FAT_FSINFO_LEAD &&
        FsInfo->StructSignature == FAT_FSINFO_STRUCT) {

        FsInfo->FreeCount = Vcb->FreeClusters;
        FsInfo->NextFree = Vcb->NextFree;

        Status = FatWriteSectors(Vcb->TargetDeviceObject, Offset, Vcb->SectorSize, FsInfo, FALSE);
    }

    ExFreePool(FsInfo);
    return Status;
}
