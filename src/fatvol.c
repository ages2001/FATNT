/*
 * FATNT - IRP_MJ_QUERY_VOLUME_INFORMATION and IRP_MJ_SET_VOLUME_INFORMATION
 */

#include "fat.h"

static NTSTATUS
FatCopyString (
    PWSTR Target,
    ULONG Room,
    const WCHAR *Source,
    ULONG Bytes,
    PULONG Copied
    )
{
    *Copied = (Bytes < Room) ? Bytes : Room;
    RtlCopyMemory(Target, Source, *Copied);

    return (*Copied < Bytes) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS
FatCommonQueryVolumeInformation (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFAT_VCB Vcb = Ctx->Vcb;
    ULONG Class = IrpSp->Parameters.QueryVolume.FsInformationClass;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_FS_VOLUME_INFORMATION Volume;
    PFILE_FS_SIZE_INFORMATION Size;
    PFILE_FS_DEVICE_INFORMATION Device;
    PFILE_FS_ATTRIBUTE_INFORMATION Attribute;
    PFAT_FILE_FS_FULL_SIZE_INFORMATION FullSize;
    ULONG Base;
    ULONG Copied;
    ULONG Used = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Class) {

        case FileFsVolumeInformation:
            Base = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Volume = (PFILE_FS_VOLUME_INFORMATION)Buffer;
            RtlZeroMemory(Volume, Base);
            Volume->VolumeSerialNumber = Vcb->SerialNumber;
            Volume->VolumeLabelLength = Vcb->LabelLength * sizeof(WCHAR);
            Volume->SupportsObjects = FALSE;

            Status = FatCopyString(Volume->VolumeLabel, Length - Base, Vcb->Label,
                                   Vcb->LabelLength * sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        case FileFsSizeInformation:
            Size = (PFILE_FS_SIZE_INFORMATION)Buffer;
            Size->TotalAllocationUnits.QuadPart = Vcb->ClusterCount;
            Size->AvailableAllocationUnits.QuadPart = Vcb->FreeClusters;
            Size->SectorsPerAllocationUnit = (ULONG)1 << Vcb->SectorsPerClusterShift;
            Size->BytesPerSector = Vcb->SectorSize;
            Used = sizeof(FILE_FS_SIZE_INFORMATION);
            break;

        case FAT_CLASS_FS_FULL_SIZE:
            if (Length < sizeof(FAT_FILE_FS_FULL_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            FullSize = (PFAT_FILE_FS_FULL_SIZE_INFORMATION)Buffer;
            FullSize->TotalAllocationUnits.QuadPart = Vcb->ClusterCount;
            FullSize->CallerAvailableAllocationUnits.QuadPart = Vcb->FreeClusters;
            FullSize->ActualAvailableAllocationUnits.QuadPart = Vcb->FreeClusters;
            FullSize->SectorsPerAllocationUnit = (ULONG)1 << Vcb->SectorsPerClusterShift;
            FullSize->BytesPerSector = Vcb->SectorSize;
            Used = sizeof(FAT_FILE_FS_FULL_SIZE_INFORMATION);
            break;

        case FileFsDeviceInformation:
            Device = (PFILE_FS_DEVICE_INFORMATION)Buffer;
            Device->DeviceType = FILE_DEVICE_DISK;
            Device->Characteristics = Vcb->Vpb->RealDevice->Characteristics;
            Used = sizeof(FILE_FS_DEVICE_INFORMATION);
            break;

        case FileFsAttributeInformation:
            Base = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Attribute = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
            Attribute->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES |
                                              FILE_UNICODE_ON_DISK;

            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Attribute->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
            }
            Attribute->MaximumComponentNameLength = FAT_MAX_NAME;
            Attribute->FileSystemNameLength = sizeof(FAT_FS_NAME) - sizeof(WCHAR);

            Status = FatCopyString(Attribute->FileSystemName, Length - Base, FAT_FS_NAME,
                                   sizeof(FAT_FS_NAME) - sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Used;
        }

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    return Status;
}

/* Some NT4 headers lack FILE_FS_LABEL_INFORMATION */
typedef struct _FAT_FS_LABEL {
    ULONG       VolumeLabelLength;
    WCHAR       VolumeLabel[1];
} FAT_FS_LABEL, *PFAT_FS_LABEL;

NTSTATUS
FatCommonSetVolumeInformation (
    PFAT_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFAT_VCB Vcb = Ctx->Vcb;
    PFAT_FS_LABEL Label = (PFAT_FS_LABEL)Ctx->Irp->AssociatedIrp.SystemBuffer;
    /* NT4 ntddk.h has no SetVolume member; QueryVolume has the same layout */
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    ULONG Count;
    ULONG i;
    KIRQL Irql;
    NTSTATUS Status;

    if (IrpSp->Parameters.QueryVolume.FsInformationClass != FileFsLabelInformation) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length < FIELD_OFFSET(FAT_FS_LABEL, VolumeLabel) ||
        Label->VolumeLabelLength > Length - FIELD_OFFSET(FAT_FS_LABEL, VolumeLabel) ||
        (Label->VolumeLabelLength & 1) != 0 ||
        Label->VolumeLabelLength > FAT_MAX_LABEL * sizeof(WCHAR)) {

        return STATUS_INVALID_VOLUME_LABEL;
    }

    Count = Label->VolumeLabelLength / sizeof(WCHAR);

    for (i = 0; i < Count; i++) {
        if (!FatIsLegalLongChar(Label->VolumeLabel[i])) {
            return STATUS_INVALID_VOLUME_LABEL;
        }
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = FatVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = FatWriteLabel(Vcb, Label->VolumeLabel, Count);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        IoAcquireVpbSpinLock(&Irql);

        Vcb->Vpb->VolumeLabelLength = (USHORT)(Count * sizeof(WCHAR));
        for (i = 0; i < Count; i++) {
            Vcb->Vpb->VolumeLabel[i] = Label->VolumeLabel[i];
        }

        IoReleaseVpbSpinLock(Irql);

    } __finally {

        FatRelease(&Vcb->Resource);
    }

    return Status;
}
