/*
 * FATNT - drive letters for FAT32 partitions on Windows NT 3.1 - 4.0
 *
 * NT 3.1 - 4.0 give drive letters only to the partition types they know:
 * FAT12/16 (01h, 04h, 06h) and IFS (07h). A FAT32 partition made by another
 * system (0Bh, 0Ch) or an LBA FAT16 one (0Eh) gets none, so nothing would
 * ever open it and offer it to FATNT - and Disk Administrator shows it as
 * "Unknown". A system thread looks at every disk a few times after start,
 * reads each such partition's boot sector through the whole-disk device (so
 * nothing gets mounted on the way) and links a free letter to every FAT
 * partition that has none: \DosDevices\X: -> \Device\HarddiskN\PartitionM.
 * The letter is kept in the service key ("Letter.HarddiskN.PartitionM" =
 * "X") and the same one is taken next time when free; it can be set there
 * by hand too. Windows 2000 and later letter these types themselves.
 *
 *   AssignDriveLetters (REG_DWORD)  1 or missing: on, 0: off
 */

#include "fat.h"

#define FAT_MAX_DISKS           32
#define FAT_LAYOUT_ENTRIES      128

static const char FatHex[] = "0123456789";

/* "\Device\HarddiskN\PartitionM" */
static VOID
FatPartitionName (
    ULONG Disk,
    ULONG Partition,
    PWCHAR Out
    )
{
    static const WCHAR Head[] = L"\\Device\\Harddisk";
    static const WCHAR Mid[] = L"\\Partition";
    ULONG n = 0, i;
    WCHAR Digits[12];
    ULONG d;

    for (i = 0; Head[i]; i++) Out[n++] = Head[i];
    d = 0;
    do { Digits[d++] = (WCHAR)FatHex[Disk % 10]; Disk /= 10; } while (Disk);
    while (d) Out[n++] = Digits[--d];
    for (i = 0; Mid[i]; i++) Out[n++] = Mid[i];
    d = 0;
    do { Digits[d++] = (WCHAR)FatHex[Partition % 10]; Partition /= 10; } while (Partition);
    while (d) Out[n++] = Digits[--d];
    Out[n] = 0;
}

/* Target of a symbolic link object, or FALSE */
static BOOLEAN
FatLinkTarget (
    PCWSTR Name,
    PWCHAR Target,
    ULONG TargetChars
    )
{
    UNICODE_STRING N, T;
    OBJECT_ATTRIBUTES Oa;
    HANDLE H;
    NTSTATUS Status;

    RtlInitUnicodeString(&N, Name);
    InitializeObjectAttributes(&Oa, &N, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenSymbolicLinkObject(&H, GENERIC_READ, &Oa);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }
    T.Buffer = Target;
    T.Length = 0;
    T.MaximumLength = (USHORT)((TargetChars - 1) * sizeof(WCHAR));
    Status = ZwQuerySymbolicLinkObject(H, &T, NULL);
    ZwClose(H);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }
    Target[T.Length / sizeof(WCHAR)] = 0;
    return TRUE;
}

static BOOLEAN
FatSameName (
    PCWSTR A,
    PCWSTR B
    )
{
    UNICODE_STRING Ua, Ub;
    RtlInitUnicodeString(&Ua, A);
    RtlInitUnicodeString(&Ub, B);
    return RtlEqualUnicodeString(&Ua, &Ub, TRUE);
}

/* Bit n set: letter 'A'+n is taken; Owner gets the letter linked to Name/Alias, or 0 */
static ULONG
FatUsedLetters (
    PCWSTR Name,
    PCWSTR Alias,
    PWCHAR Owner
    )
{
    WCHAR Link[] = L"\\DosDevices\\A:";
    WCHAR Target[260];
    ULONG Used = 0;
    ULONG i;

    *Owner = 0;
    for (i = 0; i < 26; i++) {
        Link[12] = (WCHAR)(L'A' + i);
        if (FatLinkTarget(Link, Target, 260)) {
            Used |= 1UL << i;
            if (FatSameName(Target, Name) || (Alias[0] != 0 && FatSameName(Target, Alias))) {
                *Owner = (WCHAR)(L'A' + i);
            }
        }
    }
    return Used;
}

/* The value name the letter of a partition is kept under */
static VOID
FatLetterValueName (
    ULONG Disk,
    ULONG Partition,
    PWCHAR Out
    )
{
    static const WCHAR A[] = L"Letter.Harddisk";
    static const WCHAR B[] = L".Partition";
    ULONG n = 0, i, d;
    WCHAR Digits[12];

    for (i = 0; A[i]; i++) Out[n++] = A[i];
    d = 0;
    do { Digits[d++] = (WCHAR)FatHex[Disk % 10]; Disk /= 10; } while (Disk);
    while (d) Out[n++] = Digits[--d];
    for (i = 0; B[i]; i++) Out[n++] = B[i];
    d = 0;
    do { Digits[d++] = (WCHAR)FatHex[Partition % 10]; Partition /= 10; } while (Partition);
    while (d) Out[n++] = Digits[--d];
    Out[n] = 0;
}

static WCHAR
FatSavedLetter (
    PCWSTR ValueName
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    WCHAR Buffer[8];
    UNICODE_STRING Value;

    if (FatData.RegistryPath.Buffer == NULL) {
        return 0;
    }
    Buffer[0] = 0;
    Value.Buffer = Buffer;
    Value.Length = 0;
    Value.MaximumLength = sizeof(Buffer);
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = (PWSTR)ValueName;
    Table[0].EntryContext = &Value;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, FatData.RegistryPath.Buffer,
                                           Table, NULL, NULL)) || Value.Length == 0) {
        return 0;
    }
    if (Buffer[0] >= L'a' && Buffer[0] <= L'z') {
        return (WCHAR)(Buffer[0] - 32);
    }
    return (Buffer[0] >= L'C' && Buffer[0] <= L'Z') ? Buffer[0] : 0;
}

static BOOLEAN
FatAutoLetters (
    VOID
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 1, Default = 1;

    if (FatData.RegistryPath.Buffer == NULL) {
        return TRUE;
    }
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = L"AssignDriveLetters";
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);
    (VOID)RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, FatData.RegistryPath.Buffer, Table, NULL, NULL);
    return (BOOLEAN)(Value != 0);
}

/* One partition: a FAT boot sector at its start? */
static BOOLEAN
FatPartitionIsFat (
    PDEVICE_OBJECT Disk,
    LONGLONG Start,
    PUCHAR Buffer
    )
{
    FAT_GEOMETRY Geo;

    if (!NT_SUCCESS(FatSyncIo(Disk, IRP_MJ_READ, Start, 512, Buffer, TRUE))) {
        return FALSE;
    }
    return (BOOLEAN)(FatCheckBootSector(Buffer, 512, 0, &Geo) == FAT_BOOT_OK);
}

static VOID
FatScanDisk (
    ULONG DiskNumber,
    PDRIVE_LAYOUT_INFORMATION Layout,
    ULONG LayoutSize,
    PUCHAR Buffer
    )
{
    WCHAR Name[64];
    WCHAR Alias[260];
    WCHAR ValueName[64];
    WCHAR Link[] = L"\\DosDevices\\A:";
    UNICODE_STRING DiskName, LinkName, TargetName;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Device;
    WCHAR Owner, Letter;
    ULONG Used, i, Counted;
    NTSTATUS Status;

    FatPartitionName(DiskNumber, 0, Name);
    RtlInitUnicodeString(&DiskName, Name);
    Status = IoGetDeviceObjectPointer(&DiskName, FILE_READ_ATTRIBUTES, &FileObject, &Device);
    if (!NT_SUCCESS(Status)) {
        return;
    }

    RtlZeroMemory(Layout, LayoutSize);
    Status = FatDeviceIoctl(Device, IOCTL_DISK_GET_DRIVE_LAYOUT, NULL, 0, Layout, LayoutSize, TRUE, NULL);

    Counted = 0;
    for (i = 0; NT_SUCCESS(Status) && i < Layout->PartitionCount && i < FAT_LAYOUT_ENTRIES; i++) {

        PPARTITION_INFORMATION P = &Layout->PartitionEntry[i];
        ULONG Number;

        if (P->PartitionType == PARTITION_ENTRY_UNUSED || IsContainerPartition(P->PartitionType)) {
            continue;
        }
        /* NT 3.1 - 4.0 leave PartitionNumber 0 here: the disk driver numbers
           the used, non-container entries in this order */
        Number = P->PartitionNumber ? P->PartitionNumber : ++Counted;
        if (P->PartitionNumber) Counted = P->PartitionNumber;
        if (P->PartitionLength.QuadPart < 8192) {
            continue;
        }
        /* only the FAT types this NT does not give letters to itself:
           FAT32 (0Bh, 0Ch) and LBA FAT16 (0Eh) on NT 3.1 - 4.0 */
        if (P->PartitionType != 0x0B && P->PartitionType != 0x0C && P->PartitionType != 0x0E) {
            continue;
        }
        if (!FatPartitionIsFat(Device, P->StartingOffset.QuadPart, Buffer)) {
            continue;
        }

        FatPartitionName(DiskNumber, Number, Name);
        Alias[0] = 0;
        (VOID)FatLinkTarget(Name, Alias, 260);      /* 2000/XP: \Device\HarddiskVolumeN */

        Used = FatUsedLetters(Name, Alias, &Owner);
        if (Owner != 0) {
            continue;                               /* it has one */
        }

        FatLetterValueName(DiskNumber, Number, ValueName);
        Letter = FatSavedLetter(ValueName);
        if (Letter == 0 || (Used & (1UL << (Letter - L'A')))) {
            for (Letter = L'D'; Letter <= L'Z'; Letter++) {
                if (!(Used & (1UL << (Letter - L'A')))) {
                    break;
                }
            }
            if (Letter > L'Z') {
                break;
            }
        }

        Link[12] = Letter;
        RtlInitUnicodeString(&LinkName, Link);
        RtlInitUnicodeString(&TargetName, Name);
        Status = IoCreateSymbolicLink(&LinkName, &TargetName);
        if (NT_SUCCESS(Status)) {
            WCHAR Value[2];
            Value[0] = Letter;
            Value[1] = 0;
            FAT_DBG((FAT_PFX "%c: is FAT (disk %lu partition %lu)\n", (char)Letter, DiskNumber, Number));
            if (FatData.RegistryPath.Buffer != NULL) {
                (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, FatData.RegistryPath.Buffer,
                                            ValueName, REG_SZ, Value, sizeof(Value));
            }
        }
        Status = STATUS_SUCCESS;
    }

    ObDereferenceObject(FileObject);
}

static VOID
FatAssignLetters (
    VOID
    )
{
    ULONG LayoutSize = sizeof(DRIVE_LAYOUT_INFORMATION) + FAT_LAYOUT_ENTRIES * sizeof(PARTITION_INFORMATION);
    PDRIVE_LAYOUT_INFORMATION Layout;
    PUCHAR Buffer;
    ULONG Disk;

    Layout = (PDRIVE_LAYOUT_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, LayoutSize, FAT_TAG_BUFFER);
    Buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 1024, FAT_TAG_BUFFER);
    if (Layout != NULL && Buffer != NULL) {
        for (Disk = 0; Disk < FAT_MAX_DISKS; Disk++) {
            FatScanDisk(Disk, Layout, LayoutSize, Buffer);
        }
    }
    if (Layout != NULL) ExFreePool(Layout);
    if (Buffer != NULL) ExFreePool(Buffer);
}

static VOID
NTAPI
FatLetterThread (
    PVOID Context
    )
{
    static const LONG Delays[] = { 0, 3, 5, 10, 20, 30 };
    LARGE_INTEGER Due;
    ULONG i;

    UNREFERENCED_PARAMETER(Context);

    /* Loaded at boot (FATNT is the boot file system), the system has not
       given out its own letters yet: wait for them, or ours would take the
       ones it means for its volumes */
    for (i = 0; i < 90; i++) {
        WCHAR Owner;
        if (FatUsedLetters(L"", L"", &Owner) & ~3UL) {
            break;
        }
        Due.QuadPart = -10000000;
        KeDelayExecutionThread(KernelMode, FALSE, &Due);
    }

    for (i = 0; i < sizeof(Delays) / sizeof(Delays[0]); i++) {
        if (Delays[i] != 0) {
            Due.QuadPart = -(LONGLONG)Delays[i] * 10000000;
            KeDelayExecutionThread(KernelMode, FALSE, &Due);
        }
        FatAssignLetters();
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID
FatStartDriveLetters (
    VOID
    )
{
    HANDLE Thread;
    OBJECT_ATTRIBUTES Oa;

    if (!FatAutoLetters()) {
        return;
    }
    InitializeObjectAttributes(&Oa, NULL, 0, NULL, NULL);    /* NT 3.1 - 4.0 know no OBJ_KERNEL_HANDLE */
    if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, &Oa, NULL, NULL,
                                        FatLetterThread, NULL))) {
        ZwClose(Thread);
    }
}
