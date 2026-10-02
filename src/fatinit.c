/*
 * FATNT - driver entry and settings
 *
 * Settings live in the service key and are read at every mount, so a
 * change applies to volumes mounted afterwards:
 *
 *   EnableWriteSupport (REG_DWORD)  1 or missing: read/write, 0: read-only
 */

#include "fat.h"

FAT_DATA FatData;

#ifdef FAT_CROSS
PIRP FatXTopLevelIrp;
#endif

/* Keeps a NUL-terminated copy of the service key path */
static VOID
FatSaveRegistryPath (
    PUNICODE_STRING RegistryPath
    )
{
    USHORT Size;

    if (RegistryPath == NULL || RegistryPath->Length == 0) {
        return;
    }

    Size = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    FatData.RegistryPath.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, Size, FAT_TAG_REGISTRY);

    if (FatData.RegistryPath.Buffer != NULL) {
        RtlCopyMemory(FatData.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        FatData.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        FatData.RegistryPath.Length = RegistryPath->Length;
        FatData.RegistryPath.MaximumLength = Size;
    }
}

/* EnableWriteSupport: nonzero (the default) lets volumes be written */
BOOLEAN
FatWriteSupportEnabled (
    VOID
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 1;
    ULONG Default = 1;

    if (FatData.RegistryPath.Buffer == NULL) {
        return TRUE;
    }

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = L"EnableWriteSupport";
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, FatData.RegistryPath.Buffer,
                                           Table, NULL, NULL))) {
        return TRUE;
    }

    return (BOOLEAN)(Value != 0);
}

NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

    RtlZeroMemory(&FatData, sizeof(FatData));

    FatSaveRegistryPath(RegistryPath);

    RtlInitUnicodeString(&Name, FAT_FS_DEVICE_NAME);

    Status = IoCreateDevice(DriverObject,
                            0,
                            &Name,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status)) {
        FAT_DBG((FAT_PFX "IoCreateDevice failed %lX\n", Status));
        return Status;
    }

    FatData.DriverObject = DriverObject;
    FatData.FileSystemDeviceObject = DeviceObject;

    ExInitializeResourceLite(&FatData.Resource);
    InitializeListHead(&FatData.VcbList);

    /*
     * Wire the dispatch table. The DRIVER_OBJECT.MajorFunction[] array is
     * sized IRP_MJ_MAXIMUM_FUNCTION+1 by the running kernel, and that count
     * grew over time: NT 3.1 - 3.51 stop well below the value the modern
     * (WDK / mingw) headers define for IRP_MJ_MAXIMUM_FUNCTION (0x1b), so a
     * loop to the header's maximum writes past the end of the kernel's array
     * and corrupts the adjacent pool block. Only the low majors exist on NT
     * 3.1 anyway (no Power/WMI/PnP), so cap at the highest one FATNT actually
     * serves - IRP_MJ_CLEANUP (0x12), which every NT since 3.1 provides.
     */
#ifdef FAT_NT31
    for (i = 0; i <= IRP_MJ_CLEANUP; i++) {
        DriverObject->MajorFunction[i] = FatFsdDispatch;
    }
#else
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = FatFsdDispatch;
    }
#endif

    FatInitializeFastIo(&FatData.FastIoDispatch);
    DriverObject->FastIoDispatch = &FatData.FastIoDispatch;

    FatInitializeCacheCallbacks();

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Volumes are left clean at shutdown */
    (VOID)IoRegisterShutdownNotification(DeviceObject);

    IoRegisterFileSystem(DeviceObject);

    FAT_DBG((FAT_PFX "Loaded\n"));

    return STATUS_SUCCESS;
}
