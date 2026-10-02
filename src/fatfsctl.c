/*
 * FATNT - IRP_MJ_FILE_SYSTEM_CONTROL (mount, verify, lock/unlock,
 * dismount, queries) and IRP_MJ_PNP
 */

#include "fat.h"

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static ULONG
FatLog2 (
    ULONG Value
    )
{
    ULONG Shift = 0;

    while ((Value >>= 1) != 0) {
        Shift++;
    }

    return Shift;
}

/* Hot-plug devices (W2K) get the removable-media flush policy */
#define FAT_IOCTL_GET_HOTPLUG_INFO  CTL_CODE(0x0000002d, 0x0305, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _FAT_HOTPLUG_INFO {
    ULONG       Size;
    BOOLEAN     MediaRemovable;
    BOOLEAN     MediaHotplug;
    BOOLEAN     DeviceHotplug;
    BOOLEAN     WriteCacheEnableOverride;
} FAT_HOTPLUG_INFO;

static VOID
FatCheckDeviceState (
    PFAT_VCB Vcb,
    PDEVICE_OBJECT TargetDevice
    )
{
    FAT_HOTPLUG_INFO Hotplug;
    NTSTATUS Status;

    Status = FatDeviceIoctl(TargetDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, TRUE, NULL);

    if (Status == STATUS_MEDIA_WRITE_PROTECTED || !FatWriteSupportEnabled()) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    if (Vcb->VcbState & VCB_STATE_REMOVABLE) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }

    RtlZeroMemory(&Hotplug, sizeof(Hotplug));

    Status = FatDeviceIoctl(TargetDevice, FAT_IOCTL_GET_HOTPLUG_INFO, NULL, 0,
                            &Hotplug, sizeof(Hotplug), TRUE, NULL);

    if (NT_SUCCESS(Status) && (Hotplug.MediaRemovable || Hotplug.DeviceHotplug)) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }
}

/* Reads FAT[1] and reports whether the clean-shutdown bit is clear */
static BOOLEAN
FatVolumeIsDirty (
    PFAT_VCB Vcb
    )
{
    FAT_MAP Map;
    ULONG Value;
    BOOLEAN Dirty = FALSE;

    if (Vcb->FatType == FAT_TYPE_12) {
        return FALSE;       /* FAT12 has no clean-shutdown bit */
    }

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        Value = FatGetFatEntry(Vcb, &Map, 1);

        if (Vcb->FatType == FAT_TYPE_16) {
            Dirty = (BOOLEAN)(!(Value & FAT16_CLEAN_SHUTDOWN));
        } else {
            Dirty = (BOOLEAN)(!(Value & FAT32_CLEAN_SHUTDOWN));
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Dirty = FALSE;
    }

    FatUnmap(&Map);
    return Dirty;
}

/* Reads the free count from the FAT32 FSInfo sector, if it is trustworthy */
static BOOLEAN
FatReadFsInfo (
    PFAT_VCB Vcb
    )
{
    PFAT_FSINFO FsInfo;
    LONGLONG Offset;
    BOOLEAN Ok = FALSE;
    NTSTATUS Status;

    if (Vcb->FatType != FAT_TYPE_32 || Vcb->FsInfoSector == 0) {
        return FALSE;
    }

    FsInfo = (PFAT_FSINFO)ExAllocatePoolWithTag(NonPagedPool, Vcb->SectorSize, FAT_TAG_BUFFER);
    if (FsInfo == NULL) {
        return FALSE;
    }

    Offset = (LONGLONG)Vcb->FsInfoSector << Vcb->SectorShift;

    Status = FatReadSectors(Vcb->TargetDeviceObject, Offset, Vcb->SectorSize, FsInfo, TRUE);

    if (NT_SUCCESS(Status) && FsInfo->LeadSignature == FAT_FSINFO_LEAD &&
        FsInfo->StructSignature == FAT_FSINFO_STRUCT &&
        FsInfo->FreeCount != FAT_FSINFO_UNKNOWN && FsInfo->FreeCount <= Vcb->ClusterCount) {

        Vcb->FreeClusters = FsInfo->FreeCount;

        if (FsInfo->NextFree >= FAT_FIRST_CLUSTER &&
            FsInfo->NextFree < Vcb->ClusterCount + FAT_FIRST_CLUSTER) {

            Vcb->NextFree = FsInfo->NextFree;
        }

        Ok = TRUE;
    }

    ExFreePool(FsInfo);
    return Ok;
}

/* Reads the volume label entry from the root directory, if present */
static VOID
FatReadLabel (
    PFAT_VCB Vcb
    )
{
    FAT_MAP Map;
    PFAT_FCB Root = Vcb->RootDcb;
    ULONG Size = Root->Header.AllocationSize.LowPart;
    ULONG Pos;
    ULONG i;
    ULONG Len;
    PUCHAR Entry;

    RtlZeroMemory(&Map, sizeof(Map));
    Vcb->LabelLength = 0;

    __try {

        for (Pos = 0; Pos + FAT_DIRENT_SIZE <= Size; Pos += FAT_DIRENT_SIZE) {

            Entry = FatMapStream(Vcb, Root, &Map, Pos, FAT_DIRENT_SIZE);

            if (Entry[0] == FAT_DIRENT_END) {
                break;
            }

            if (Entry[0] == FAT_DIRENT_FREE) {
                continue;
            }

            if ((Entry[11] & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_VOLUME_ID) {

                Len = FAT_SFN_LEN;
                while (Len > 0 && Entry[Len - 1] == ' ') {
                    Len--;
                }

                for (i = 0; i < Len; i++) {
                    Vcb->Label[i] = (WCHAR)Entry[i];
                }

                Vcb->LabelLength = Len;
                break;
            }
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Vcb->LabelLength = 0;
    }

    FatUnmap(&Map);
}

static NTSTATUS
FatMountVolume (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT RealDevice = Vpb->RealDevice;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PFAT_VCB Vcb = NULL;
    FAT_GEOMETRY Geo;
    PUCHAR Sector = NULL;
    ULONG SectorSize;
    ULONG Check;
    ULONG i;
    LONGLONG PartitionBytes;
    BOOLEAN ClearedVerify = FALSE;
    NTSTATUS Status;

    Status = FatDeviceIoctl(TargetDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                            &Geometry, sizeof(Geometry), TRUE, NULL);
    if (!NT_SUCCESS(Status)) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    SectorSize = Geometry.BytesPerSector;

    if (SectorSize < 512 || SectorSize > 4096 || (SectorSize & (SectorSize - 1)) != 0) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Status = FatDeviceIoctl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                            &Partition, sizeof(Partition), TRUE, NULL);

    PartitionBytes = NT_SUCCESS(Status) ? Partition.PartitionLength.QuadPart : 0;

    Sector = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, SectorSize, FAT_TAG_BUFFER);
    if (Sector == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        Status = FatReadSectors(TargetDevice, 0, SectorSize, Sector, TRUE);
        if (!NT_SUCCESS(Status)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        RtlZeroMemory(&Geo, sizeof(Geo));
        Check = FatCheckBootSector(Sector, SectorSize,
                                   (ULONGLONG)PartitionBytes / SectorSize, &Geo);

        if (Check != FAT_BOOT_OK) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Status = IoCreateDevice(FatData.DriverObject,
                                sizeof(FAT_VCB),
                                NULL,
                                FILE_DEVICE_DISK_FILE_SYSTEM,
                                0,
                                FALSE,
                                &VolumeDevice);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);

        if (TargetDevice->AlignmentRequirement > VolumeDevice->AlignmentRequirement) {
            VolumeDevice->AlignmentRequirement = TargetDevice->AlignmentRequirement;
        }

        VolumeDevice->SectorSize = (USHORT)SectorSize;

#ifdef DO_DEVICE_INITIALIZING
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

        Vcb = (PFAT_VCB)VolumeDevice->DeviceExtension;
        RtlZeroMemory(Vcb, sizeof(FAT_VCB));

        Vcb->NodeTypeCode = FAT_NTC_VCB;
        Vcb->NodeByteSize = (CSHORT)sizeof(FAT_VCB);
        ExInitializeResourceLite(&Vcb->Resource);
        ExInitializeResourceLite(&Vcb->AllocResource);
        InitializeListHead(&Vcb->FcbList);
        InitializeListHead(&Vcb->DirNotifyList);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);

        Vcb->Vpb = Vpb;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->VolumeDeviceObject = VolumeDevice;
        Vcb->VerifyThread = KeGetCurrentThread();

        /* Geometry */
        Vcb->FatType          = Geo.FatType;
        Vcb->SectorSize       = Geo.BytesPerSector;
        Vcb->SectorShift      = FatLog2(Geo.BytesPerSector);
        Vcb->SectorsPerClusterShift = FatLog2(Geo.SectorsPerCluster);
        Vcb->ClusterSize      = Geo.ClusterSize;
        Vcb->ClusterShift     = Vcb->SectorShift + Vcb->SectorsPerClusterShift;
        Vcb->ReservedSectors  = Geo.ReservedSectors;
        Vcb->NumberOfFats     = Geo.NumberOfFats;
        Vcb->FatSectors       = Geo.FatSectors;
        Vcb->FatSector        = Geo.ReservedSectors;     /* the first (active) FAT */
        Vcb->FirstRootSector  = Geo.FirstRootSector;
        Vcb->RootDirSectors   = Geo.RootDirSectors;
        Vcb->RootEntries      = Geo.RootEntries;
        Vcb->FirstDataSector  = Geo.FirstDataSector;
        Vcb->ClusterCount     = Geo.ClusterCount;
        Vcb->RootCluster      = Geo.RootCluster;
        Vcb->FsInfoSector     = Geo.FsInfoSector;
        Vcb->SerialNumber     = Geo.SerialNumber;
        Vcb->Media            = Geo.Media;
        Vcb->NextFree         = FAT_FIRST_CLUSTER;
        Vcb->FreeClusters     = FAT_FSINFO_UNKNOWN;

        Vcb->VolumeBytes      = (LONGLONG)Geo.TotalSectors << Vcb->SectorShift;
        Vcb->PartitionBytes   = (PartitionBytes > Vcb->VolumeBytes) ? PartitionBytes : Vcb->VolumeBytes;

        Vcb->RootLbo          = (LONGLONG)Geo.FirstRootSector << Vcb->SectorShift;
        Vcb->RootBytes        = Geo.RootDirSectors << Vcb->SectorShift;

        switch (Vcb->FatType) {
        case FAT_TYPE_12:
            Vcb->FatMask = FAT12_MASK; Vcb->EndOfChain = FAT12_MASK; Vcb->BadCluster = FAT12_BAD;
            break;
        case FAT_TYPE_16:
            Vcb->FatMask = FAT16_MASK; Vcb->EndOfChain = FAT16_MASK; Vcb->BadCluster = FAT16_BAD;
            break;
        default:
            Vcb->FatMask = FAT32_MASK; Vcb->EndOfChain = FAT32_MASK; Vcb->BadCluster = FAT32_BAD;
            break;
        }

        if (RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
            Vcb->VcbState |= VCB_STATE_REMOVABLE;
        }

        FatCheckDeviceState(Vcb, TargetDevice);

        if (RealDevice->Flags & DO_VERIFY_VOLUME) {
            RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            ClearedVerify = TRUE;
        }

        /* The FAT and the root directory, read straight from the disk for now */
        Vcb->FatFcb = FatCreateFatFcb(Vcb);
        if (Vcb->FatFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Vcb->RootDcb = FatCreateRootDcb(Vcb);
        if (Vcb->RootDcb == NULL) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }

        /* Dirty flag, free count and label (direct reads while unmounted) */
        if (FatVolumeIsDirty(Vcb)) {
            Vcb->VcbState |= VCB_STATE_KEEP_DIRTY;
        }

        if (!FatReadFsInfo(Vcb)) {
            Status = FatCountFreeClusters(Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        FatReadLabel(Vcb);

        Vcb->VolumeFcb = FatCreateVolumeFcb(Vcb);
        if (Vcb->VolumeFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* From here on paging I/O on the metadata streams must reach us */
        Vpb->DeviceObject = VolumeDevice;
        Vcb->VcbState |= VCB_STATE_MOUNTED;

        Status = FatOpenStream(Vcb, Vcb->FatFcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vpb->SerialNumber = Vcb->SerialNumber;
        Vpb->VolumeLabelLength = (USHORT)(Vcb->LabelLength * sizeof(WCHAR));
        for (i = 0; i < Vcb->LabelLength; i++) {
            Vpb->VolumeLabel[i] = Vcb->Label[i];
        }

        Vcb->VerifyThread = NULL;

        (VOID)ObReferenceObjectByPointer(TargetDevice, 0, NULL, KernelMode);

        (VOID)ExAcquireResourceExclusiveLite(&FatData.Resource, TRUE);
        InsertTailList(&FatData.VcbList, &Vcb->VcbLinks);
        FatRelease(&FatData.Resource);

        FAT_DBG((FAT_PFX "Mounted FAT%u %08lX: %lu clusters of %lu bytes, %lu free%s%s\n",
                 Vcb->FatType, Vcb->SerialNumber, Vcb->ClusterCount, Vcb->ClusterSize,
                 Vcb->FreeClusters,
                 (Vcb->VcbState & VCB_STATE_KEEP_DIRTY) ? ", dirty" : "",
                 (Vcb->VcbState & VCB_STATE_READ_ONLY) ? ", read-only" : ""));

    } __finally {

        if (Sector != NULL) {
            ExFreePool(Sector);
        }

        if (AbnormalTermination() || !NT_SUCCESS(Status)) {

            if (Vcb != NULL) {

                Vpb->DeviceObject = NULL;

                if (ClearedVerify) {
                    RealDevice->Flags |= DO_VERIFY_VOLUME;
                }

                InitializeListHead(&Vcb->VcbLinks);

                while (!IsListEmpty(&Vcb->FcbList)) {
                    FatDeleteFcb(CONTAINING_RECORD(Vcb->FcbList.Flink, FAT_FCB, FcbLinks));
                }

                if (Vcb->VolumeFcb != NULL) {
                    FatDeleteFcb(Vcb->VolumeFcb);
                }

                if (Vcb->FatFcb != NULL && !(Vcb->FatFcb->FcbState & FCB_STATE_STREAM_OPEN)) {
                    FatDeleteFcb(Vcb->FatFcb);
                }

                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
                ExDeleteResourceLite(&Vcb->AllocResource);
                ExDeleteResourceLite(&Vcb->Resource);
            }

            if (VolumeDevice != NULL) {
                IoDeleteDevice(VolumeDevice);
            }
        }
    }

#ifndef FAT_NT4
    if (NT_SUCCESS(Status)) {
        (VOID)FsRtlNotifyVolumeEvent(Vcb->FatFcb->StreamFile, FSRTL_VOLUME_MOUNT);
    }
#endif

    return Status;
}


/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS
FatVerifyVolume (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PVPB Vpb = IrpSp->Parameters.VerifyVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = IrpSp->Parameters.VerifyVolume.DeviceObject;
    PFAT_VCB Vcb;
    PUCHAR Boot = NULL;
    FAT_GEOMETRY VerifyGeo;
    BOOLEAN Same = FALSE;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (VolumeDevice == NULL || VolumeDevice->DriverObject != FatData.DriverObject ||
        VolumeDevice == FatData.FileSystemDeviceObject) {

        return STATUS_WRONG_VOLUME;
    }

    Vcb = (PFAT_VCB)VolumeDevice->DeviceExtension;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        if (!(Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {
            __leave;
        }

        Vcb->VerifyThread = KeGetCurrentThread();

        Boot = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->SectorSize, FAT_TAG_BUFFER);
        if (Boot == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = FatReadSectors(Vcb->TargetDeviceObject, 0, Vcb->SectorSize, Boot, TRUE);

        if (NT_SUCCESS(Status) &&
            FatCheckBootSector(Boot, Vcb->SectorSize, 0, &VerifyGeo) == FAT_BOOT_OK &&
            VerifyGeo.SerialNumber == Vcb->SerialNumber &&
            VerifyGeo.ClusterCount == Vcb->ClusterCount &&
            VerifyGeo.FatType == Vcb->FatType) {

            Same = TRUE;
        }

        if (Same) {

            Status = STATUS_SUCCESS;

        } else {

            FAT_DBG((FAT_PFX "Verify: volume %08lX is gone\n", Vcb->SerialNumber));

            Delete = FatDismountVcb(Vcb);
            Status = STATUS_WRONG_VOLUME;
        }

        Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    } __finally {

        if (Boot != NULL) {
            ExFreePool(Boot);
        }

        Vcb->VerifyThread = NULL;
        FatRelease(&Vcb->Resource);
    }

    if (Delete) {
        FatDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* User requests                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
FatGetVolumeOpen (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_CCB *Ccb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_FCB Fcb;

    if (FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PFAT_FCB)FileObject->FsContext;
    *Ccb = (PFAT_CCB)FileObject->FsContext2;

    if (Fcb == NULL || *Ccb == NULL || !FatIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
FatNotifyVolume (
    PFILE_OBJECT FileObject,
    ULONG Event
    )
{
#ifndef FAT_NT4
    (VOID)FsRtlNotifyVolumeEvent(FileObject, Event);
#else
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Event);
#endif
}

#ifndef FSRTL_VOLUME_DISMOUNT
#define FSRTL_VOLUME_DISMOUNT       1
#endif
#ifndef FSRTL_VOLUME_LOCK
#define FSRTL_VOLUME_LOCK           3
#endif
#ifndef FSRTL_VOLUME_LOCK_FAILED
#define FSRTL_VOLUME_LOCK_FAILED    4
#endif
#ifndef FSRTL_VOLUME_UNLOCK
#define FSRTL_VOLUME_UNLOCK         5
#endif

static NTSTATUS
FatLockVolume (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_CCB Ccb;
    KIRQL Irql;
    NTSTATUS Status;

    Status = FatGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    FatNotifyVolume(FileObject, FSRTL_VOLUME_LOCK);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* Whoever locks is about to work on the disk itself */
        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)FatFlushVolume(Vcb, TRUE);
            FatPurgeCachedFiles(Vcb);
        }

        /* Only the caller's handle may be open */
        if (Vcb->UncleanCount != 1) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;

        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    if (!NT_SUCCESS(Status)) {
        FatNotifyVolume(FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    return Status;
}

static NTSTATUS
FatUnlockVolume (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_CCB Ccb;
    NTSTATUS Status;

    Status = FatGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject) {

            Status = STATUS_NOT_LOCKED;

        } else {

            FatUnlockVcb(Vcb);

            /* Metadata may have been rewritten under us */
            if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                (VOID)FatDismountVcb(Vcb);
            }
        }

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    if (NT_SUCCESS(Status)) {
        FatNotifyVolume(FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return Status;
}

static NTSTATUS
FatDismountVolume (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFAT_CCB Ccb;
    NTSTATUS Status;

    Status = FatGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    FatNotifyVolume(FileObject, FSRTL_VOLUME_DISMOUNT);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)FatFlushVolume(Vcb, TRUE);
        }

        /* The caller's handle stays open, so the VCB cannot go away here */
        Ccb->Flags |= CCB_FLAG_DISMOUNTED_VOLUME;
        (VOID)FatDismountVcb(Vcb);

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
FatIsVolumeDirty (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PULONG Result = (PULONG)Irp->AssociatedIrp.SystemBuffer;

    if (FatXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Dirty for chkdsk, not merely in use */
    *Result = (Vcb->VcbState & VCB_STATE_KEEP_DIRTY) ? VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS
FatMarkVolumeDirtyRequest (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_KEEP_DIRTY;
        FatMarkVolumeDirty(Vcb);

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    return Status;
}

static NTSTATUS
FatUserFsRequest (
    PFAT_IRP_CONTEXT Ctx
    )
{
    ULONG Code = FatXSp(Ctx->IrpSp)->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {

    case FSCTL_LOCK_VOLUME:
        return FatLockVolume(Ctx);

    case FSCTL_UNLOCK_VOLUME:
        return FatUnlockVolume(Ctx);

    case FSCTL_DISMOUNT_VOLUME:
        return FatDismountVolume(Ctx);

    case FSCTL_IS_VOLUME_MOUNTED:
        return FatVerifyVcb(Ctx, Ctx->Vcb);

    case FSCTL_IS_PATHNAME_VALID:
        return STATUS_SUCCESS;

    case FSCTL_IS_VOLUME_DIRTY:
        return FatIsVolumeDirty(Ctx);

    case FSCTL_MARK_VOLUME_DIRTY:
        return FatMarkVolumeDirtyRequest(Ctx);

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return STATUS_OPLOCK_NOT_GRANTED;

    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        return STATUS_INVALID_OPLOCK_PROTOCOL;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
FatCommonFileSystemControl (
    PFAT_IRP_CONTEXT Ctx
    )
{
    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_MOUNT_VOLUME:
        if (Ctx->Vcb != NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return FatMountVolume(Ctx);

    case IRP_MN_VERIFY_VOLUME:
        return FatVerifyVolume(Ctx);

    case IRP_MN_USER_FS_REQUEST:
#ifdef IRP_MN_KERNEL_CALL
    case IRP_MN_KERNEL_CALL:
#endif
        if (Ctx->Vcb == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return FatUserFsRequest(Ctx);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Plug and Play (Windows 2000)                                        */
/* ------------------------------------------------------------------ */

#ifndef FAT_NT4

static NTSTATUS
NTAPI
FatPnpCompletion (
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

/* Sends the IRP down and waits; the caller completes it */
static NTSTATUS
FatForwardAndWait (
    PFAT_IRP_CONTEXT Ctx,
    PFAT_VCB Vcb
    )
{
    PIRP Irp = Ctx->Irp;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    FatCopyStackToNext(Irp);
    IoSetCompletionRoutine(Irp, FatPnpCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Vcb->TargetDeviceObject, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
FatCommonPnp (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PFAT_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status;

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_REMOVE_DEVICE:

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)FatFlushVolume(Vcb, TRUE);
            FatPurgeCachedFiles(Vcb);
        }

        if (Vcb->UncleanCount != 0 || (Vcb->VcbState & VCB_STATE_LOCKED)) {
            FatRelease(&Vcb->Resource);
            return STATUS_ACCESS_DENIED;
        }

        Vcb->VcbState |= VCB_STATE_PNP_LOCKED;
        FatRelease(&Vcb->Resource);

        Status = FatForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (NT_SUCCESS(Status)) {
            Delete = FatDismountVcb(Vcb);
        } else {
            Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        }

        FatRelease(&Vcb->Resource);
        break;

    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:

        Status = FatForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        Vcb->VcbState |= VCB_STATE_REMOVED;
        Delete = FatDismountVcb(Vcb);
        FatRelease(&Vcb->Resource);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        Status = FatForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        FatRelease(&Vcb->Resource);
        break;

    default:

        if (Vcb->VcbState & VCB_STATE_REMOVED) {
            return STATUS_NO_SUCH_DEVICE;
        }

        FatSkipStack(Irp);
        Ctx->Flags |= FAT_CTX_NO_COMPLETE;
        return IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }

    if (Delete) {
        FatDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

#else

NTSTATUS
FatCommonPnp (
    PFAT_IRP_CONTEXT Ctx
    )
{
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#endif
