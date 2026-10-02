/*
 * FATNT - user-mode test: runs the driver against real FAT12/16/32 images
 * through a fake I/O manager (kern.c). Images are made with mkfs.fat and
 * seeded with mtools; the driver's view is checked against the host's, and
 * the image is checked with fsck.fat afterwards.
 */
#include "ntifs.h"
#include <stdio.h>
#include <stdlib.h>
#include "kern.h"
#ifndef FILE_GENERIC_WRITE
#define FILE_GENERIC_WRITE (READ_CONTROL|FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES|FILE_WRITE_EA|FILE_APPEND_DATA|SYNCHRONIZE)
#endif

NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);

static DRIVER_OBJECT g_fsdrv, g_diskdrv;
static PDEVICE_OBJECT g_disk;
static FILE *g_img;
static ULONG g_sector = 512;
static LONGLONG g_partlen;
static int g_disk_reads, g_disk_writes;

#define T(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_errors++; } } while (0)
#define TS(st, exp) do { NTSTATUS _s=(st); if (_s!=(NTSTATUS)(exp)) { fprintf(stderr, "FAIL %s:%d: %s = %08x want %08x\n", __FILE__, __LINE__, #st, (unsigned)_s,(unsigned)(exp)); g_errors++; } } while (0)

static void Balanced(const char *w)
{
    FinishOtherThreads(); RunWorkers();
    if (g_crit || g_res || g_bcb) { fprintf(stderr, "UNBALANCED after %s: crit=%d res=%d bcb=%d\n", w, g_crit, g_res, g_bcb); g_errors++; g_crit=g_res=0; }
}

/* ---------------- fake disk ---------------- */
static NTSTATUS DiskDispatch(PDEVICE_OBJECT d, PIRP i)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(i);
    NTSTATUS st = STATUS_SUCCESS;
    i->IoStatus.Information = 0;
    switch (sp->MajorFunction) {
    case IRP_MJ_READ: case IRP_MJ_WRITE: {
        LONGLONG off = sp->Parameters.Read.ByteOffset.QuadPart; ULONG len = sp->Parameters.Read.Length;
        PUCHAR buf = MmGetMdlVirtualAddress(i->MdlAddress);
        if (off % g_sector || len % g_sector) { fprintf(stderr, "unaligned %lld %u\n",(long long)off,len); g_errors++; }
        if (off + len > g_partlen) { st = STATUS_INVALID_PARAMETER; break; }
        fseeko(g_img, off, SEEK_SET);
        if (sp->MajorFunction == IRP_MJ_READ) { if (fread(buf,1,len,g_img)!=len) memset(buf,0,len); g_disk_reads++; }
        else { fwrite(buf,1,len,g_img); g_disk_writes++; }
        i->IoStatus.Information = len;
        break; }
    case IRP_MJ_DEVICE_CONTROL: {
        ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
        if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
            DISK_GEOMETRY *g = i->AssociatedIrp.SystemBuffer; memset(g,0,sizeof(*g));
            g->BytesPerSector = g_sector; g->SectorsPerTrack = 63; g->TracksPerCylinder = 255;
            g->Cylinders.QuadPart = g_partlen/(g_sector*63*255); i->IoStatus.Information = sizeof(*g);
        } else if (code == IOCTL_DISK_GET_PARTITION_INFO) {
            PARTITION_INFORMATION *p = i->AssociatedIrp.SystemBuffer; memset(p,0,sizeof(*p));
            p->PartitionLength.QuadPart = g_partlen; p->PartitionType = 6; p->RecognizedPartition = 1;
            i->IoStatus.Information = sizeof(*p);
        } else if (code == IOCTL_DISK_IS_WRITABLE) {
            st = STATUS_SUCCESS;
        } else st = STATUS_INVALID_DEVICE_REQUEST;
        break; }
    case IRP_MJ_FLUSH_BUFFERS: break;
    default: st = STATUS_SUCCESS;
    }
    i->IoStatus.Status = st; IoCompleteRequest(i, 0); return st;
}

static void SetupDisk(const char *img)
{
    int k; VPB *vpb;
    g_img = fopen(img, "r+b"); if (!g_img) { perror(img); exit(2); }
    fseeko(g_img, 0, SEEK_END); g_partlen = ftello(g_img);
    for (k = 0; k < 28; k++) g_diskdrv.MajorFunction[k] = DiskDispatch;
    IoCreateDevice(&g_diskdrv, 0, NULL, FILE_DEVICE_DISK, 0, FALSE, &g_disk);
    g_disk->Flags &= ~DO_DEVICE_INITIALIZING;
    vpb = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), 'bpV'); memset(vpb,0,sizeof(VPB));
    vpb->RealDevice = g_disk; g_disk->Vpb = vpb;
}

/* ---------------- fake I/O manager ---------------- */
static NTSTATUS Mount(void)
{
    PIRP i = IoAllocateIrp(1, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL; sp->MinorFunction = IRP_MN_MOUNT_VOLUME;
    sp->Parameters.MountVolume.Vpb = g_disk->Vpb; sp->Parameters.MountVolume.DeviceObject = g_disk;
    IoCallDriver(g_fsdev, i); st = i->IoStatus.Status; IoFreeIrp(i);
    if (NT_SUCCESS(st)) g_disk->Vpb->Flags |= VPB_MOUNTED;
    Balanced("mount");
    return st;
}

static PWCHAR Utf16(const char *s, size_t *out)
{
    size_t n = strlen(s), o = 0, k = 0; PWCHAR w = malloc((n+1)*2);
    while (k < n) w[o++] = (unsigned char)s[k++];
    w[o] = 0; if (out) *out = o; return w;
}

static NTSTATUS Open(const char *upath, PFILE_OBJECT related, ACCESS_MASK access, ULONG share,
                     ULONG disp, ULONG options, ULONG attrs, PFILE_OBJECT *out, ULONG_PTR *infop)
{
    char path[1400]; size_t pk, n; PVPB vpb = g_disk->Vpb; PFILE_OBJECT f; PIRP i; PIO_STACK_LOCATION sp;
    IO_SECURITY_CONTEXT sc; ACCESS_STATE as; NTSTATUS st; int k;
    *out = NULL;
    strcpy(path, upath); for (pk=0; path[pk]; pk++) if (path[pk]=='/') path[pk]='\\';
    for (k = 0; k < 3; k++) {
        if (!(vpb->Flags & VPB_MOUNTED)) { st = Mount(); if (!NT_SUCCESS(st)) return st; }
        vpb->ReferenceCount++;
        f = calloc(1, sizeof(FILE_OBJECT)); f->Type=5; f->DeviceObject=g_disk; f->Vpb=vpb; f->RefCount=1; f->RelatedFileObject=related; g_fo++;
        f->FileName.Buffer = Utf16(path,&n); f->FileName.Length = f->FileName.MaximumLength = (USHORT)(n*2);
        f->Flags = FO_SYNCHRONOUS_IO | ((options & 0x8) ? FO_NO_INTERMEDIATE_BUFFERING : 0);
        memset(&as,0,sizeof(as)); as.RemainingDesiredAccess = access; sc.AccessState=&as; sc.DesiredAccess=access;
        i = IoAllocateIrp(vpb->DeviceObject->StackSize, FALSE); sp = IoGetNextIrpStackLocation(i);
        sp->MajorFunction = IRP_MJ_CREATE; sp->FileObject = f;
        sp->Parameters.Create.SecurityContext = &sc; sp->Parameters.Create.Options = (disp<<24)|options;
        sp->Parameters.Create.ShareAccess = (USHORT)share; sp->Parameters.Create.FileAttributes = (USHORT)attrs;
        i->RequestorMode = UserMode;
        IoCallDriver(vpb->DeviceObject, i);
        st = i->IoStatus.Status; if (infop) *infop = i->IoStatus.Information; IoFreeIrp(i);
        if (!NT_SUCCESS(st) || st == STATUS_REPARSE) { vpb->ReferenceCount--; free(f->FileName.Buffer); free(f); g_fo--; }
        else { *out = f; Balanced("create"); return st; }
        if (st != STATUS_REPARSE) break;
    }
    Balanced("create");
    return st;
}

static void Close(PFILE_OBJECT f)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_CLEANUP; sp->FileObject = f;
    IoCallDriver(d, i); IoFreeIrp(i);
    ObDereferenceObject(f);
    Balanced("close");
}

static NTSTATUS Rw(PFILE_OBJECT f, UCHAR mj, LONGLONG off, ULONG len, PVOID buf, ULONG flags, ULONG_PTR *info)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = mj; sp->FileObject = f; i->RequestorMode = UserMode;
    sp->Parameters.Read.ByteOffset.QuadPart = off; sp->Parameters.Read.Length = len;
    i->UserBuffer = buf; i->Flags = flags;
    IoCallDriver(d, i); st = i->IoStatus.Status; *info = i->IoStatus.Information; IoFreeIrp(i);
    Balanced("rw");
    return st;
}
#define Read(f,o,l,b,nc,ip)  Rw((f), IRP_MJ_READ,  (o),(l),(b), (nc)?IRP_NOCACHE:0, (ip))
#define Write(f,o,l,b,ip)    Rw((f), IRP_MJ_WRITE, (o),(l),(b), 0, (ip))

static NTSTATUS SetInfo(PFILE_OBJECT f, ULONG cls, PVOID buf, ULONG len)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = IRP_MJ_SET_INFORMATION; sp->FileObject = f; i->RequestorMode = UserMode;
    sp->Parameters.SetFile.FileInformationClass = cls; sp->Parameters.SetFile.Length = len;
    i->AssociatedIrp.SystemBuffer = buf;
    IoCallDriver(d, i); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("setinfo");
    return st;
}

static NTSTATUS QueryInfo(PFILE_OBJECT f, ULONG cls, PVOID buf, ULONG len, ULONG_PTR *info)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = IRP_MJ_QUERY_INFORMATION; sp->FileObject = f;
    sp->Parameters.QueryFile.FileInformationClass = cls; sp->Parameters.QueryFile.Length = len;
    i->AssociatedIrp.SystemBuffer = buf;
    IoCallDriver(d, i); st = i->IoStatus.Status; *info = i->IoStatus.Information; IoFreeIrp(i);
    Balanced("qinfo");
    return st;
}

static NTSTATUS Fsctl(PFILE_OBJECT f, ULONG code)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    ULONG out = 0;
    sp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL; sp->MinorFunction = IRP_MN_USER_FS_REQUEST;
    sp->FileObject = f; sp->Parameters.FileSystemControl.FsControlCode = code;
    sp->Parameters.FileSystemControl.OutputBufferLength = 4; i->AssociatedIrp.SystemBuffer = &out;
    IoCallDriver(d, i); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("fsctl");
    return st;
}

/* Flush, lock and dismount, then close the volume handle, to force a remount */
static void Remount(void)
{
    PFILE_OBJECT v; ULONG_PTR info;
    TS(Open("", NULL, FILE_GENERIC_READ|FILE_GENERIC_WRITE, 7, FILE_OPEN, 0, 0, &v, &info), STATUS_SUCCESS);
    if (!v) return;
    TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);
    TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
    Close(v);
    g_disk->Vpb->Flags &= ~VPB_MOUNTED;
    MmTrimAll();
}

/* ---------------- helpers ---------------- */
static unsigned long long Fnv(const unsigned char *p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL; size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static int DirHasName(PFILE_OBJECT dir, const char *want)
{
    char buf[8192]; ULONG_PTR info; NTSTATUS st; int found = 0; int first = 1;
    for (;;) {
        PDEVICE_OBJECT d = IoGetRelatedDeviceObject(dir);
        PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
        sp->MajorFunction = IRP_MJ_DIRECTORY_CONTROL; sp->MinorFunction = IRP_MN_QUERY_DIRECTORY;
        sp->FileObject = dir;
        sp->Parameters.QueryDirectory.Length = sizeof(buf);
        sp->Parameters.QueryDirectory.FileInformationClass = FileBothDirectoryInformation;
        sp->Parameters.QueryDirectory.FileName = NULL;
        sp->Flags = first ? SL_RESTART_SCAN : 0; first = 0;
        i->UserBuffer = buf;
        IoCallDriver(d, i); st = i->IoStatus.Status; info = i->IoStatus.Information; IoFreeIrp(i);
        Balanced("qdir");
        if (!NT_SUCCESS(st) || info == 0) break;
        {
            ULONG off = 0;
            for (;;) {
                PFILE_BOTH_DIR_INFORMATION e = (PFILE_BOTH_DIR_INFORMATION)(buf + off);
                char name[280]; ULONG k, nl = e->FileNameLength / 2;
                for (k = 0; k < nl && k < 279; k++) name[k] = (char)e->FileName[k];
                name[k] = 0;
                if (!strcmp(name, want)) found = 1;
                if (e->NextEntryOffset == 0) break;
                off += e->NextEntryOffset;
            }
        }
    }
    return found;
}

/* ---------------- the test battery ---------------- */
static void RunOne(const char *label)
{
    PFILE_OBJECT f, dir; ULONG_PTR info; NTSTATUS st;
    unsigned char *wbuf, *rbuf; ULONG n; int errs0 = g_errors;
    FILE_END_OF_FILE_INFORMATION eof;
    FILE_DISPOSITION_INFORMATION disp;
    FILE_STANDARD_INFORMATION si;

    printf("[%s] partition %lld bytes\n", label, (long long)g_partlen);

    /* 1. mount and read a file seeded by mtools (short name) */
    st = Open("/HELLO.TXT", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) {
        rbuf = calloc(1, 4096);
        st = Read(f, 0, 4096, rbuf, FALSE, &info);
        TS(st, STATUS_SUCCESS);
        T(info == 13);
        T(memcmp(rbuf, "hello, fatnt\n", 13) == 0);
        free(rbuf);
        Close(f);
    }

    /* 2. a long-named file seeded by mtools must be found and open by LFN */
    st = Open("/", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 1 /*dir*/, 0, &dir, &info);
    TS(st, STATUS_SUCCESS);
    if (dir) {
        T(DirHasName(dir, "A Long File Name.txt"));
        Close(dir);
    }
    st = Open("/A Long File Name.txt", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) Close(f);

    /* 3. create a subdirectory and a file, write a large pattern, read back */
    st = Open("/sub", NULL, FILE_GENERIC_WRITE, 7, FILE_CREATE, 1 /*dir*/, 0, &dir, &info);
    TS(st, STATUS_SUCCESS);
    T(info == FILE_CREATED);
    if (dir) Close(dir);

    st = Open("/sub/data.bin", NULL, FILE_GENERIC_READ|FILE_GENERIC_WRITE, 7, FILE_CREATE, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) {
        n = 200000;
        wbuf = malloc(n); rbuf = malloc(n);
        { ULONG k; for (k = 0; k < n; k++) wbuf[k] = (unsigned char)(k * 37 + 11); }
        st = Write(f, 0, n, wbuf, &info);
        TS(st, STATUS_SUCCESS); T(info == n);
        st = Read(f, 0, n, rbuf, FALSE, &info);
        TS(st, STATUS_SUCCESS); T(info == n);
        T(Fnv(rbuf, n) == Fnv(wbuf, n));
        Close(f);
        free(wbuf); free(rbuf);
    }

    /* 4. sparse extend: set EOF large, write a few bytes at 0, the gap must
          read back as zeros after a remount (FAT keeps no valid-data length) */
    st = Open("/sparse.bin", NULL, FILE_GENERIC_READ|FILE_GENERIC_WRITE, 7, FILE_CREATE, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) {
        eof.EndOfFile.QuadPart = 100000;
        TS(SetInfo(f, FileEndOfFileInformation, &eof, sizeof(eof)), STATUS_SUCCESS);
        { unsigned char hd[4] = {1,2,3,4}; st = Write(f, 0, 4, hd, &info); TS(st, STATUS_SUCCESS); }
        Close(f);
    }

    /* 5. remount and verify persistence + zero-fill */
    Remount();

    st = Open("/sub/data.bin", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) {
        TS(QueryInfo(f, FileStandardInformation, &si, sizeof(si), &info), STATUS_SUCCESS);
        T(si.EndOfFile.QuadPart == 200000);
        n = 200000; rbuf = malloc(n);
        st = Read(f, 0, n, rbuf, FALSE, &info); TS(st, STATUS_SUCCESS);
        { ULONG k; unsigned long long h = 1469598103934665603ULL;
          for (k=0;k<n;k++){ unsigned char b=(unsigned char)(k*37+11); h^=b; h*=1099511628211ULL; }
          T(Fnv(rbuf, n) == h); }
        free(rbuf);
        Close(f);
    }

    st = Open("/sparse.bin", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) {
        rbuf = malloc(100000);
        st = Read(f, 0, 100000, rbuf, FALSE, &info); TS(st, STATUS_SUCCESS);
        T(rbuf[0]==1 && rbuf[3]==4);
        { ULONG k, bad=0; for (k=16; k<100000; k++) if (rbuf[k]) bad++; T(bad==0); }
        free(rbuf);
        Close(f);
    }

    /* 6. delete the file, remount, confirm it is gone */
    st = Open("/sub/data.bin", NULL, FILE_GENERIC_READ|DELETE, 7, FILE_OPEN, 0, 0, &f, &info);
    TS(st, STATUS_SUCCESS);
    if (f) {
        disp.DeleteFile = TRUE;
        TS(SetInfo(f, FileDispositionInformation, &disp, sizeof(disp)), STATUS_SUCCESS);
        Close(f);
    }
    Remount();
    st = Open("/sub/data.bin", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, 0, &f, &info);
    TS(st, STATUS_OBJECT_NAME_NOT_FOUND);
    if (f) Close(f);

    Remount();   /* final flush of metadata before fsck */

    printf("[%s] %d checks failed\n", label, g_errors - errs0);
}

int main(int argc, char **argv)
{
    UNICODE_STRING reg; WCHAR regbuf[] = {0x5c,0x52,0x65,0x67,0};
    if (argc < 3) { fprintf(stderr, "usage: %s <image> <label>\n", argv[0]); return 2; }
    reg.Buffer = regbuf; reg.Length = sizeof(regbuf) - sizeof(WCHAR); reg.MaximumLength = sizeof(regbuf);

    TS(DriverEntry(&g_fsdrv, &reg), STATUS_SUCCESS);
    SetupDisk(argv[1]);
    RunOne(argv[2]);

    printf("disk: %d reads, %d writes\n", g_disk_reads, g_disk_writes);
    printf("TOTAL ERRORS: %d\n", g_errors);
    return g_errors ? 1 : 0;
}
