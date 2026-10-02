/*
 * FATNT - disk I/O
 *
 * Metadata (FAT, allocation bitmap, directories) is cached through one
 * internal stream per object and reached with CcMapData / CcPinRead by
 * offset within the object. While mounting, before those streams exist,
 * it is read straight from the disk. File data goes to the disk
 * non-cached, one synchronous request per contiguous run.
 */

#include "fat.h"

/* ------------------------------------------------------------------ */
/* Stream offsets to volume offsets                                    */
/* ------------------------------------------------------------------ */

/*
 * Maps Vbo of a stream to its volume offset and the number of bytes that
 * follow contiguously on the disk.
 */
BOOLEAN
FatStreamToLbo (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    LONGLONG Vbo,
    PLONGLONG Lbo,
    PULONG Contiguous
    )
{
    LONGLONG Left;

    /* The whole volume, the FAT and the FAT12/16 fixed root map linearly */
    if (FatIsVfcb(Fcb) || Fcb->MetaLbo != 0) {

        Left = Fcb->Header.AllocationSize.QuadPart - Vbo;

        if (Vbo < 0 || Left <= 0) {
            return FALSE;
        }

        *Lbo = Fcb->MetaLbo + Vbo;
        *Contiguous = (Left > 0x40000000) ? 0x40000000 : (ULONG)Left;
        if (Fcb->FcbState & FCB_STATE_ROOT) {
        }
        return TRUE;
    }

    return FatLookupVbo(Vcb, &Fcb->RunList, Vbo, Lbo, Contiguous);
}

/* ------------------------------------------------------------------ */
/* Metadata stream mapping                                             */
/* ------------------------------------------------------------------ */

static VOID
FatReleaseView (
    PFAT_MAP Map
    )
{
    if (Map->Bcb != NULL) {

        if (Map->Dirty) {
            CcSetDirtyPinnedData(Map->Bcb, NULL);
        }

        CcUnpinData(Map->Bcb);
        Map->Bcb = NULL;
    }

    Map->Dirty = FALSE;
    Map->Pinned = FALSE;
    Map->Data = NULL;
}

VOID
FatUnmap (
    PFAT_MAP Map
    )
{
    FatReleaseView(Map);

    if (Map->Direct != NULL) {
        ExFreePool(Map->Direct);
        Map->Direct = NULL;
    }

    Map->Fcb = NULL;
}

/*
 * Returns a pointer to Length bytes at offset Vbo of the metadata stream
 * of Fcb. The range must not cross an FAT_MAP_UNIT boundary. Pin: the
 * caller will change the data and then call FatSetDirty. Raises on I/O
 * errors.
 */
static PUCHAR
FatMapOrPin (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFAT_MAP Map,
    LONGLONG Vbo,
    ULONG Length,
    BOOLEAN Pin
    )
{
    LARGE_INTEGER Start;
    LONGLONG Lbo;
    ULONG Contiguous;
    ULONG MapLength;
    ULONG Done;
    ULONG Chunk;
    NTSTATUS Status;

    if (Map->Data != NULL &&
        Map->Fcb == Fcb &&
        (Map->Pinned || !Pin) &&
        Vbo >= Map->Vbo &&
        Vbo + Length <= Map->Vbo + Map->Length) {

        return Map->Data + (ULONG)(Vbo - Map->Vbo);
    }

    Start.QuadPart = Vbo & ~(LONGLONG)(FAT_MAP_UNIT - 1);

    if (Vbo < 0 || Vbo + Length > Fcb->Header.FileSize.QuadPart ||
        Vbo + Length > Start.QuadPart + FAT_MAP_UNIT) {

        ExRaiseStatus(STATUS_FILE_CORRUPT_ERROR);
    }

    MapLength = FAT_MAP_UNIT;
    if (Start.QuadPart + MapLength > Fcb->Header.FileSize.QuadPart) {
        MapLength = (ULONG)(Fcb->Header.FileSize.QuadPart - Start.QuadPart);
    }

    FatReleaseView(Map);
    Map->Fcb = Fcb;

    if (Vcb->VcbState & VCB_STATE_MOUNTED) {

        if (Map->Direct != NULL) {
            ExFreePool(Map->Direct);
            Map->Direct = NULL;
        }

        if (Fcb->StreamFile == NULL) {
            Status = FatOpenStream(Vcb, Fcb);
            if (!NT_SUCCESS(Status)) {
                ExRaiseStatus(Status);
            }
        }

        if (Pin) {

            FatMarkVolumeDirty(Vcb);

            if (!CcPinRead(Fcb->StreamFile, &Start, MapLength, TRUE, &Map->Bcb, (PVOID *)&Map->Data)) {
                Map->Bcb = NULL;
                Map->Data = NULL;
                ExRaiseStatus(STATUS_UNSUCCESSFUL);
            }

            Map->Pinned = TRUE;

        } else if (!CcMapData(Fcb->StreamFile, &Start, MapLength, TRUE, &Map->Bcb, (PVOID *)&Map->Data)) {

            Map->Bcb = NULL;
            Map->Data = NULL;
            ExRaiseStatus(STATUS_UNSUCCESSFUL);
        }

    } else {

        /* Mounting: straight from the disk, never written */
        if (Pin || (Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            ExRaiseStatus(FAT_STATUS_DISMOUNTED);
        }

        if (Map->Direct == NULL) {
            Map->Direct = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, FAT_MAP_UNIT, FAT_TAG_BUFFER);
            if (Map->Direct == NULL) {
                ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
            }
        }

        /* A unit can span several small clusters in different places */
        for (Done = 0; Done < MapLength; Done += Chunk) {

            if (!FatStreamToLbo(Vcb, Fcb, Start.QuadPart + Done, &Lbo, &Contiguous)) {
                ExRaiseStatus(STATUS_FILE_CORRUPT_ERROR);
            }

            Chunk = MapLength - Done;
            if (Chunk > Contiguous) {
                Chunk = Contiguous;
            }

            Status = FatReadSectors(Vcb->TargetDeviceObject, Lbo,
                                    (ULONG)FatRoundUp(Chunk, Vcb->SectorSize),
                                    Map->Direct + Done, TRUE);
            if (!NT_SUCCESS(Status)) {
                ExRaiseStatus(Status);
            }
        }

        Map->Data = Map->Direct;
    }

    Map->Vbo = Start.QuadPart;
    Map->Length = MapLength;

    return Map->Data + (ULONG)(Vbo - Map->Vbo);
}

PUCHAR
FatMapStream (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFAT_MAP Map,
    LONGLONG Vbo,
    ULONG Length
    )
{
    return FatMapOrPin(Vcb, Fcb, Map, Vbo, Length, FALSE);
}

PUCHAR
FatPinStream (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    PFAT_MAP Map,
    LONGLONG Vbo,
    ULONG Length
    )
{
    return FatMapOrPin(Vcb, Fcb, Map, Vbo, Length, TRUE);
}

/*
 * Zeroes a range of a metadata stream in the cache (new directory
 * clusters). Whole units are prepared without reading the disk.
 */
VOID
FatZeroStream (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    LONGLONG Vbo,
    ULONG Length
    )
{
    FAT_MAP Map;
    LARGE_INTEGER Start;
    PVOID Bcb;
    PVOID Buffer;
    ULONG Chunk;
    PUCHAR Data;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        while (Length != 0) {

            Chunk = FAT_MAP_UNIT - ((ULONG)Vbo & (FAT_MAP_UNIT - 1));
            if (Chunk > Length) {
                Chunk = Length;
            }

            if (Chunk == FAT_MAP_UNIT) {

                if (Fcb->StreamFile == NULL) {
                    NTSTATUS Status = FatOpenStream(Vcb, Fcb);
                    if (!NT_SUCCESS(Status)) {
                        ExRaiseStatus(Status);
                    }
                }

                FatMarkVolumeDirty(Vcb);

                Start.QuadPart = Vbo;

                if (!CcPreparePinWrite(Fcb->StreamFile, &Start, FAT_MAP_UNIT, TRUE, TRUE,
                                       &Bcb, &Buffer)) {
                    ExRaiseStatus(STATUS_UNSUCCESSFUL);
                }

                CcSetDirtyPinnedData(Bcb, NULL);
                CcUnpinData(Bcb);

            } else {

                Data = FatPinStream(Vcb, Fcb, &Map, Vbo, Chunk);
                RtlZeroMemory(Data, Chunk);
                FatSetDirty(&Map);
            }

            Vbo += Chunk;
            Length -= Chunk;
        }

    } __finally {

        FatUnmap(&Map);
    }
}

/* ------------------------------------------------------------------ */
/* Synchronous requests to the target device                           */
/* ------------------------------------------------------------------ */

NTSTATUS
FatSyncIo (
    PDEVICE_OBJECT Device,
    UCHAR MajorFunction,
    LONGLONG Offset,
    ULONG Length,
    PVOID Buffer,
    BOOLEAN OverrideVerify
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER ByteOffset;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ByteOffset.QuadPart = Offset;

    Irp = IoBuildSynchronousFsdRequest(MajorFunction, Device, Buffer, Length,
                                       &ByteOffset, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    Status = IoCallDriver(Device, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

NTSTATUS
FatDeviceIoctl (
    PDEVICE_OBJECT Device,
    ULONG IoControlCode,
    PVOID InputBuffer,
    ULONG InputLength,
    PVOID OutputBuffer,
    ULONG OutputLength,
    BOOLEAN OverrideVerify,
    PULONG Information
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode, Device, InputBuffer, InputLength,
                                        OutputBuffer, OutputLength, FALSE, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    Status = IoCallDriver(Device, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (Information != NULL) {
        *Information = (ULONG)Iosb.Information;
    }

    return Status;
}

/* Asks the disk to commit its write cache; not every driver supports it */
NTSTATUS
FatFlushDevice (
    PFAT_VCB Vcb
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS, Vcb->TargetDeviceObject,
                                       NULL, 0, NULL, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Caller buffers                                                      */
/* ------------------------------------------------------------------ */

VOID
FatLockUserBuffer (
    PIRP Irp,
    LOCK_OPERATION Operation,
    ULONG Length
    )
{
    PMDL Mdl;

    if (Irp->MdlAddress != NULL) {
        return;
    }

    Mdl = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);
    if (Mdl == NULL) {
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    }

    __try {

        MmProbeAndLockPages(Mdl, Irp->RequestorMode, Operation);

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        IoFreeMdl(Mdl);
        Irp->MdlAddress = NULL;
        Mdl = NULL;
    }

    if (Mdl == NULL) {
        ExRaiseStatus(STATUS_INVALID_USER_BUFFER);
    }
}

PVOID
FatMapUserBuffer (
    PIRP Irp
    )
{
    PVOID Address;

    if (Irp->MdlAddress == NULL) {
        return Irp->UserBuffer;
    }

    Address = FatMdlAddress(Irp->MdlAddress);
    if (Address == NULL) {
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    }

    return Address;
}

/* ------------------------------------------------------------------ */
/* Non-cached transfers                                                */
/* ------------------------------------------------------------------ */

static NTSTATUS
NTAPI
FatSyncCompletion (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * Transfers Length bytes between the disk at Lbo and the part of the
 * master MDL that starts BufferOffset bytes in.
 */
static NTSTATUS
FatTransferRun (
    PFAT_VCB Vcb,
    UCHAR MajorFunction,
    PIRP MasterIrp,
    ULONG BufferOffset,
    LONGLONG Lbo,
    ULONG Length
    )
{
    PMDL MasterMdl = MasterIrp->MdlAddress;
    PMDL Mdl = MasterMdl;
    PUCHAR Va;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;
    KEVENT Event;
    NTSTATUS Status;

    Irp = IoAllocateIrp(Vcb->TargetDeviceObject->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (BufferOffset != 0) {

        Va = (PUCHAR)MmGetMdlVirtualAddress(MasterMdl) + BufferOffset;

        Mdl = IoAllocateMdl(Va, Length, FALSE, FALSE, NULL);
        if (Mdl == NULL) {
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        IoBuildPartialMdl(MasterMdl, Mdl, Va, Length);
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp->MdlAddress = Mdl;
    Irp->Flags = IRP_NOCACHE | (MasterIrp->Flags & IRP_PAGING_IO);
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->UserIosb = NULL;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = MajorFunction;
    IrpSp->Parameters.Read.Length = Length;
    IrpSp->Parameters.Read.ByteOffset.QuadPart = Lbo;

    if (Vcb->VerifyThread == KeGetCurrentThread()) {
        IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp, FatSyncCompletion, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    Status = Irp->IoStatus.Status;

    if (NT_SUCCESS(Status) && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Mdl != MasterMdl) {
        IoFreeMdl(Mdl);
    }

    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    return Status;
}

/*
 * Moves ByteCount bytes (a whole number of sectors) between the disk and
 * Irp->MdlAddress, starting at StartingVbo of Fcb. Reads return zeros from
 * ValidData on.
 */
NTSTATUS
FatNonCachedIo (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_FCB Fcb,
    UCHAR MajorFunction,
    LONGLONG StartingVbo,
    ULONG ByteCount,
    LONGLONG ValidData
    )
{
    PFAT_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    LONGLONG TransferEnd;
    LONGLONG Vbo;
    LONGLONG Lbo;
    ULONG Contiguous;
    ULONG Offset;
    ULONG Length;
    ULONG ZeroOffset;
    PUCHAR Buffer;
    NTSTATUS Status = STATUS_SUCCESS;

    TransferEnd = StartingVbo + ByteCount;

    if (MajorFunction == IRP_MJ_READ && TransferEnd > ValidData) {

        /* Only whole sectors up to valid data come from the disk */
        TransferEnd = FatRoundUp(ValidData, Vcb->SectorSize);

        if (TransferEnd < StartingVbo) {
            TransferEnd = StartingVbo;
        }

        if (TransferEnd > StartingVbo + ByteCount) {
            TransferEnd = StartingVbo + ByteCount;
        }
    }

    Vbo = StartingVbo;
    Offset = 0;

    while (Vbo < TransferEnd) {

        if (FatIsVfcb(Fcb)) {

            Lbo = Vbo;
            Contiguous = (ULONG)(TransferEnd - Vbo);

        } else if (!FatStreamToLbo(Vcb, Fcb, Vbo, &Lbo, &Contiguous)) {

            FAT_DBG((FAT_PFX "No cluster for offset %lX of FCB %p\n", (ULONG)Vbo, Fcb));
            return STATUS_FILE_CORRUPT_ERROR;
        }

        Length = (ULONG)(TransferEnd - Vbo);
        if (Length > Contiguous) {
            Length = Contiguous;
        }

        Status = FatTransferRun(Vcb, MajorFunction, Irp, Offset, Lbo, Length);

        if (!NT_SUCCESS(Status)) {
            FAT_DBG((FAT_PFX "Disk transfer at %lX:%lX failed %lX\n",
                     (ULONG)(Lbo >> 32), (ULONG)Lbo, Status));
            return Status;
        }

        Vbo += Length;
        Offset += Length;
    }

    /* Zero what lies past valid data */
    if (MajorFunction == IRP_MJ_READ && StartingVbo + ByteCount > ValidData) {

        if (ValidData > StartingVbo) {
            ZeroOffset = (ULONG)(ValidData - StartingVbo);
        } else {
            ZeroOffset = 0;
        }

        Buffer = (PUCHAR)FatMdlAddress(Irp->MdlAddress);
        if (Buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(Buffer + ZeroOffset, ByteCount - ZeroOffset);
    }

    return Status;
}

/*
 * Writes zeros over [Start, End) of Fcb on the disk. A partial first
 * sector keeps its bytes before Start. End is rounded up to a sector and
 * clipped to the allocation.
 */
NTSTATUS
FatZeroDisk (
    PFAT_VCB Vcb,
    PFAT_FCB Fcb,
    LONGLONG Start,
    LONGLONG End
    )
{
    PUCHAR Zero;
    LONGLONG Vbo;
    LONGLONG Lbo;
    ULONG Contiguous;
    ULONG Length;
    ULONG Within;
    NTSTATUS Status = STATUS_SUCCESS;

    End = FatRoundUp(End, Vcb->SectorSize);
    if (End > Fcb->Header.AllocationSize.QuadPart) {
        End = Fcb->Header.AllocationSize.QuadPart;
    }

    if (Start >= End) {
        return STATUS_SUCCESS;
    }

    Zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, FAT_ZERO_CHUNK, FAT_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Vbo = Start & ~(LONGLONG)(Vcb->SectorSize - 1);

    /* Keep what precedes Start in its sector */
    Within = (ULONG)(Start - Vbo);

    if (Within != 0) {

        if (!FatStreamToLbo(Vcb, Fcb, Vbo, &Lbo, &Contiguous)) {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }

        Status = FatReadSectors(Vcb->TargetDeviceObject, Lbo, Vcb->SectorSize, Zero, FALSE);
        if (!NT_SUCCESS(Status)) {
            goto Done;
        }

        RtlZeroMemory(Zero + Within, Vcb->SectorSize - Within);

        Status = FatWriteSectors(Vcb->TargetDeviceObject, Lbo, Vcb->SectorSize, Zero, FALSE);
        if (!NT_SUCCESS(Status)) {
            goto Done;
        }

        Vbo += Vcb->SectorSize;
    }

    RtlZeroMemory(Zero, FAT_ZERO_CHUNK);

    while (Vbo < End) {

        if (!FatStreamToLbo(Vcb, Fcb, Vbo, &Lbo, &Contiguous)) {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }

        Length = FAT_ZERO_CHUNK;
        if (Length > Contiguous) {
            Length = Contiguous;
        }
        if ((LONGLONG)Length > End - Vbo) {
            Length = (ULONG)(End - Vbo);
        }

        Status = FatWriteSectors(Vcb->TargetDeviceObject, Lbo, Length, Zero, FALSE);
        if (!NT_SUCCESS(Status)) {
            goto Done;
        }

        Vbo += Length;
    }

Done:
    ExFreePool(Zero);
    return Status;
}
