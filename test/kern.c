/* Fake NT kernel: just enough of Io/Cc/Mm/Ex/FsRtl to run EXFATNT single-threaded. */
#include "ntifs.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include <wctype.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include "kern.h"

__thread int g_crit;
int g_res, g_pool, g_mdl, g_bcb, g_irp, g_devices, g_verbose, g_fo;
jmp_buf *g_raise_jb;
NTSTATUS g_raised;
PDEVICE_OBJECT g_fsdev;
static __thread KTHREAD g_thread;
static EPROCESS g_proc;
static __thread PIRP g_toplevel;
static PDEVICE_OBJECT g_verify_dev;
int g_errors;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "KERNEL CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_errors++; } } while (0)

void KeEnterCriticalRegion(void) { g_crit++; }
void KeLeaveCriticalRegion(void) { g_crit--; CHECK(g_crit >= 0); }
PKTHREAD KeGetCurrentThread(void) { return &g_thread; }
ULONG DbgPrint(const char *f, ...) { va_list a; if (g_verbose) { va_start(a, f); vprintf(f, a); va_end(a); } return 0; }

typedef struct { size_t n; ULONG tag; POOL_TYPE t; long magic; } PH;
PVOID ExAllocatePoolWithTag(POOL_TYPE t, size_t n, ULONG tag)
{
    PH *h = malloc(sizeof(PH) + n + 16);
    h->n = n; h->tag = tag; h->t = t; h->magic = 0x1234;
    memset(h + 1, 0xCC, n + 16);
    g_pool++;
    return h + 1;
}
void ExFreePool(PVOID p)
{
    PH *h = (PH *)p - 1; unsigned char *e = (unsigned char *)p + h->n; int i;
    CHECK(h->magic == 0x1234);
    for (i = 0; i < 16; i++) CHECK(e[i] == 0xCC);   /* overrun check */
    h->magic = 0; g_pool--; free(h);
}

/*
 * Threads. Other threads (closes from the memory manager, see GetPage)
 * are real pthreads, but only one runs at a time: a thread runs until it
 * finishes or blocks on a resource, and a release hands the processor to
 * a thread waiting for that resource at once, so the races that matter
 * happen at the worst moment, and the same way every run.
 */
static pthread_mutex_t g_gil = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_run, g_live = 1;
static __thread int t_self;
static int g_tstate[EXF_MAXT];           /* 0 free, 1 ready, 2 waiting, 3 finished */
static PERESOURCE g_twait[EXF_MAXT]; static int g_twaitx[EXF_MAXT];
int g_switches, g_deadlocks;

__attribute__((constructor)) static void GilInit(void) { pthread_mutex_lock(&g_gil); g_tstate[0] = 1; }

static int CanAcquire(PERESOURCE r, int t, int excl)
{
    int u;
    for (u = 0; u < EXF_MAXT; u++) {
        if (u == t) continue;
        if (r->X[u] || (excl && r->S[u])) return 0;
    }
    return 1;
}
static void RunThread(int t)
{
    int me = t_self;
    if (t == me) return;
    g_run = t; g_switches++;
    pthread_cond_broadcast(&g_cv);
    while (g_run != me) pthread_cond_wait(&g_cv, &g_gil);
}
/* The next thread able to run, other than the current one; the main thread first */
static int PickThread(void)
{
    int t;
    for (t = 0; t < EXF_MAXT; t++) {
        if (t == t_self) continue;
        if (g_tstate[t] == 1) return t;
        if (g_tstate[t] == 2 && CanAcquire(g_twait[t], t, g_twaitx[t])) return t;
    }
    return -1;
}
static void WaitFor(PERESOURCE r, int excl)
{
    int t;
    g_tstate[t_self] = 2; g_twait[t_self] = r; g_twaitx[t_self] = excl;
    while (!CanAcquire(r, t_self, excl)) {
        t = PickThread();
        if (t < 0) { fprintf(stderr, "DEADLOCK: thread %d waits for a resource nobody can release\n", t_self); g_errors++; g_deadlocks++; abort(); }
        RunThread(t);
    }
    g_tstate[t_self] = 1; g_twait[t_self] = NULL;
}
/* After a release: a thread waiting for this resource runs now */
static void WakeWaiters(PERESOURCE r)
{
    int t;
    for (t = 0; t < EXF_MAXT; t++)
        if (t != t_self && g_tstate[t] == 2 && g_twait[t] == r && CanAcquire(r, t, g_twaitx[t])) { RunThread(t); return; }
}
static void *ThreadMain(void *a)
{
    void **arg = a; int me = (int)(intptr_t)arg[2];
    pthread_mutex_lock(&g_gil);
    t_self = me;
    while (g_run != me) pthread_cond_wait(&g_cv, &g_gil);
    ((void (*)(PVOID))arg[0])(arg[1]);
    free(arg);
    CHECK(g_crit == 0);
    g_tstate[me] = 3; g_live--;
    /* hand over to whoever can go on */
    {
        int t = PickThread();
        g_run = t < 0 ? 0 : t;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_gil);
    return NULL;
}
/* Starts Routine(Context) on another thread and lets it run until it finishes or blocks */
void RunOnOtherThread(void (*Routine)(PVOID), PVOID Context)
{
    int t; pthread_t th; void **arg;
    for (t = 1; t < EXF_MAXT && g_tstate[t] != 0 && g_tstate[t] != 3; t++) ;
    if (t == EXF_MAXT) { Routine(Context); return; }
    if (g_tstate[t] == 3) g_tstate[t] = 0;
    arg = malloc(3 * sizeof(void *)); arg[0] = (void *)Routine; arg[1] = Context; arg[2] = (void *)(intptr_t)t;
    g_tstate[t] = 1; g_live++;
    pthread_create(&th, NULL, ThreadMain, arg);
    pthread_detach(th);
    RunThread(t);
}
/* The main thread lets every other thread finish (between driver calls) */
int OtherThreadsLive(void) { return g_live - 1; }
void FinishOtherThreads(void)
{
    int t;
    while ((t = PickThread()) >= 0 && g_live > 1) RunThread(t);
    if (g_live > 1) { fprintf(stderr, "STUCK: %d threads still blocked\n", g_live - 1); g_errors++; }
}

void ExInitializeResourceLite(PERESOURCE r) { memset(r, 0, sizeof(*r)); r->Initialized = 1; }
void ExDeleteResourceLite(PERESOURCE r) { CHECK(r->Initialized && r->Shared == 0 && r->Exclusive == 0); r->Initialized = 0; }
BOOLEAN ExAcquireResourceSharedLite(PERESOURCE r, BOOLEAN w)
{
    CHECK(r->Initialized); CHECK(g_crit > 0);
    if (!CanAcquire(r, t_self, 0)) { if (!w) return FALSE; WaitFor(r, 0); }
    r->Shared++; r->S[t_self]++; g_res++; return TRUE;
}
BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE r, BOOLEAN w)
{
    CHECK(r->Initialized); CHECK(g_crit > 0);
    if (r->S[t_self] > 0 && r->X[t_self] == 0) {
        if (!w) return FALSE;       /* this very thread holds it shared */
        fprintf(stderr, "DEADLOCK: exclusive acquire while holding shared\n"); g_errors++;
    }
    if (!CanAcquire(r, t_self, 1)) { if (!w) return FALSE; WaitFor(r, 1); }
    r->Exclusive++; r->X[t_self]++; g_res++; return TRUE;
}
void ExReleaseResourceForThreadLite(PERESOURCE r, ERESOURCE_THREAD t)
{
    CHECK(r->Initialized);
    if (r->S[t_self] > 0) { r->Shared--; r->S[t_self]--; } else { CHECK(r->X[t_self] > 0); r->Exclusive--; r->X[t_self]--; }
    g_res--;
    if (!r->S[t_self] && !r->X[t_self]) WakeWaiters(r);
}
void ExRaiseStatus(NTSTATUS s)
{
    g_raised = s;
    if (g_raise_jb) longjmp(*g_raise_jb, 1);
    fprintf(stderr, "unhandled ExRaiseStatus %08x\n", s); abort();
}

static long long DaysFromCivil(int y, unsigned m, unsigned d)
{
    int era; unsigned yoe, doy, doe;
    y -= m <= 2; era = (y >= 0 ? y : y - 399) / 400; yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}
BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS f, PLARGE_INTEGER t)
{
    long long days;
    if (f->Month < 1 || f->Month > 12 || f->Day < 1 || f->Day > 31 || f->Hour > 23 || f->Minute > 59 || f->Second > 59 || f->Milliseconds > 999) return FALSE;
    days = DaysFromCivil(f->Year, f->Month, f->Day) + 134774;  /* 1601 -> 1970 */
    t->QuadPart = (((days * 86400 + f->Hour * 3600 + f->Minute * 60 + f->Second) * 1000LL) + f->Milliseconds) * 10000LL;
    return TRUE;
}
/* The test machine lives three hours east of UTC */
#define TZ_BIAS (3LL * 3600 * 10000000)
void ExLocalTimeToSystemTime(PLARGE_INTEGER a, PLARGE_INTEGER b) { b->QuadPart = a->QuadPart - TZ_BIAS; }
void ExSystemTimeToLocalTime(PLARGE_INTEGER a, PLARGE_INTEGER b) { b->QuadPart = a->QuadPart + TZ_BIAS; }
LONGLONG g_now;
void KeQuerySystemTime(PLARGE_INTEGER t)
{
    if (!g_now) g_now = ((LONGLONG)time(NULL) + 11644473600LL) * 10000000LL;
    g_now += 123457;                /* time moves on a little with every look */
    t->QuadPart = g_now;
}
void RtlTimeToTimeFields(PLARGE_INTEGER t, PTIME_FIELDS f)
{
    long long ms = t->QuadPart / 10000, days = ms / 86400000, rem = ms % 86400000;
    long long z = days - 134774 + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    f->Year = (CSHORT)(yoe + era * 400 + (m <= 2)); f->Month = (CSHORT)m; f->Day = (CSHORT)d;
    f->Hour = (CSHORT)(rem / 3600000); f->Minute = (CSHORT)(rem / 60000 % 60); f->Second = (CSHORT)(rem / 1000 % 60);
    f->Milliseconds = (CSHORT)(rem % 1000); f->Weekday = (CSHORT)((days + 1) % 7);
}
size_t RtlCompareMemory(const void *a, const void *b, size_t n) { size_t i = 0; while (i < n && ((const UCHAR *)a)[i] == ((const UCHAR *)b)[i]) i++; return i; }

void RtlInitUnicodeString(PUNICODE_STRING s, const WCHAR *w)
{
    size_t n = 0; while (w[n]) n++;
    s->Buffer = (PWSTR)w; s->Length = (USHORT)(n * 2); s->MaximumLength = s->Length + 2;
}
NTSTATUS RtlUpcaseUnicodeString(PUNICODE_STRING d, PUNICODE_STRING s, BOOLEAN a)
{
    int i;
    CHECK(!a); CHECK(d->MaximumLength >= s->Length);
    for (i = 0; i < s->Length / 2; i++) d->Buffer[i] = (WCHAR)towupper(s->Buffer[i]);
    d->Length = s->Length;
    return STATUS_SUCCESS;
}

/* ---------------- IRPs ---------------- */
PIRP IoAllocateIrp(CCHAR s, BOOLEAN q)
{
    PIRP i = calloc(1, sizeof(IRP));
    CHECK(s >= 1 && s < IRP_MAX_STACK);
    i->StackCount = s; i->CurrentLocation = s + 1; i->Tail.Overlay.CurrentStackLocation = &i->Stack[(int)s];
    g_irp++;
    return i;
}
void IoFreeIrp(PIRP i) { g_irp--; free(i); }

static PDEVICE_OBJECT RelatedDevice(PFILE_OBJECT f)
{
    if (f->Vpb && f->Vpb->DeviceObject) return f->Vpb->DeviceObject;
    if (f->DeviceObject->Vpb && f->DeviceObject->Vpb->DeviceObject) return f->DeviceObject->Vpb->DeviceObject;
    return f->DeviceObject;
}
PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT f) { return RelatedDevice(f); }

NTSTATUS IofCallDriver(PDEVICE_OBJECT d, PIRP i)
{
    PIO_STACK_LOCATION sp;
    CHECK(!d->Deleted);
    i->CurrentLocation--; i->Tail.Overlay.CurrentStackLocation--;
    CHECK(i->CurrentLocation >= 1);
    sp = IoGetCurrentIrpStackLocation(i);
    sp->DeviceObject = d;
    return d->DriverObject->MajorFunction[sp->MajorFunction](d, i);
}

void IofCompleteRequest(PIRP i, CCHAR b)
{
    CHECK(!i->Completed);
    while (i->CurrentLocation <= i->StackCount) {
        PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(i);
        i->CurrentLocation++; i->Tail.Overlay.CurrentStackLocation++;
        if (sp->CompletionRoutine) {
            PDEVICE_OBJECT up = (i->CurrentLocation <= i->StackCount) ? IoGetCurrentIrpStackLocation(i)->DeviceObject : NULL;
            PIO_COMPLETION_ROUTINE r = sp->CompletionRoutine;
            sp->CompletionRoutine = NULL;
            if (r(up, i, sp->Context) == STATUS_MORE_PROCESSING_REQUIRED) return;
        }
    }
    i->Completed = 1;
    if (i->UserIosb) *i->UserIosb = i->IoStatus;
    if (i->UserEvent) i->UserEvent->Signaled = 1;
    /* I/O manager unlocks and frees MDLs of non-paging requests */
    if (!(i->Flags & IRP_PAGING_IO) && i->MdlAddress && i->MdlAddress != i->OwnMdl) {
        IoFreeMdl(i->MdlAddress); i->MdlAddress = NULL;
    }
    if (i->AutoFree) { if (i->OwnMdl) IoFreeMdl(i->OwnMdl); IoFreeIrp(i); }
}

PMDL IoAllocateMdl(PVOID va, ULONG len, BOOLEAN sec, BOOLEAN q, PIRP i)
{
    PMDL m = calloc(1, sizeof(MDL));
    m->StartVa = va; m->ByteCount = len; g_mdl++;
    if (i) i->MdlAddress = m;
    return m;
}
void IoFreeMdl(PMDL m) { g_mdl--; free(m); }
void IoBuildPartialMdl(PMDL s, PMDL t, PVOID va, ULONG len)
{
    PUCHAR base = MmGetMdlVirtualAddress(s);
    CHECK((PUCHAR)va >= base && (PUCHAR)va + len <= base + s->ByteCount);
    t->StartVa = va; t->ByteCount = len; t->ByteOffset = 0;
}
void MmProbeAndLockPages(PMDL m, KPROCESSOR_MODE mode, LOCK_OPERATION op) { m->Locked = 1; }

PIRP IoBuildSynchronousFsdRequest(ULONG mj, PDEVICE_OBJECT d, PVOID b, ULONG l, PLARGE_INTEGER o, PKEVENT e, PIO_STATUS_BLOCK s)
{
    PIRP i = IoAllocateIrp(d->StackSize, FALSE);
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = (UCHAR)mj; sp->Parameters.Read.Length = l;
    if (o) sp->Parameters.Read.ByteOffset = *o;
    if (b) i->OwnMdl = i->MdlAddress = IoAllocateMdl(b, l, FALSE, FALSE, NULL);
    i->UserEvent = e; i->UserIosb = s; i->AutoFree = 1;
    return i;
}
PIRP IoBuildDeviceIoControlRequest(ULONG c, PDEVICE_OBJECT d, PVOID in, ULONG il, PVOID out, ULONG ol, BOOLEAN internal, PKEVENT e, PIO_STATUS_BLOCK s)
{
    PIRP i = IoAllocateIrp(d->StackSize, FALSE);
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_DEVICE_CONTROL; sp->Parameters.DeviceIoControl.IoControlCode = c;
    sp->Parameters.DeviceIoControl.OutputBufferLength = ol; sp->Parameters.DeviceIoControl.InputBufferLength = il;
    i->AssociatedIrp.SystemBuffer = out; i->UserEvent = e; i->UserIosb = s; i->AutoFree = 1;
    return i;
}

void KeInitializeEvent(PKEVENT e, EVENT_TYPE t, BOOLEAN s) { e->Signaled = s; }
LONG KeSetEvent(PKEVENT e, LONG inc, BOOLEAN w) { e->Signaled = 1; return 0; }
NTSTATUS KeWaitForSingleObject(PVOID o, int r, int m, BOOLEAN a, PLARGE_INTEGER t)
{
    CHECK(((PKEVENT)o)->Signaled);   /* single-threaded: must be done already */
    return STATUS_SUCCESS;
}

PIRP IoGetTopLevelIrp(void) { return g_toplevel; }
void IoSetTopLevelIrp(PIRP i) { g_toplevel = i; }
PEPROCESS IoGetCurrentProcess(void) { return &g_proc; }
PEPROCESS IoGetRequestorProcess(PIRP i) { return &g_proc; }
PDEVICE_OBJECT IoGetDeviceToVerify(PETHREAD t) { return g_verify_dev; }
void IoSetDeviceToVerify(PETHREAD t, PDEVICE_OBJECT d) { g_verify_dev = d; }
void IoSetHardErrorOrVerifyDevice(PIRP i, PDEVICE_OBJECT d) { g_verify_dev = d; }
void IoAcquireVpbSpinLock(PKIRQL i) { *i = 2; }
void IoReleaseVpbSpinLock(KIRQL i) { }

NTSTATUS IoVerifyVolume(PDEVICE_OBJECT d, BOOLEAN r)
{
    PVPB vpb = d->Vpb; PIRP i; PIO_STACK_LOCATION sp; NTSTATUS st;
    if (!(vpb->Flags & VPB_MOUNTED)) return STATUS_SUCCESS;
    i = IoAllocateIrp(vpb->DeviceObject->StackSize, FALSE);
    sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL; sp->MinorFunction = IRP_MN_VERIFY_VOLUME;
    sp->Parameters.VerifyVolume.Vpb = vpb; sp->Parameters.VerifyVolume.DeviceObject = vpb->DeviceObject;
    st = IoCallDriver(vpb->DeviceObject, i);
    CHECK(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    if (st == STATUS_WRONG_VOLUME && vpb->ReferenceCount != 0) {
        /* the I/O manager gives the device a fresh VPB, the old one stays with the old volume */
        PVPB n = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), 'bpV');
        memset(n, 0, sizeof(VPB)); n->RealDevice = d; d->Vpb = n;
    }
    return st;
}

/* Share access: the NT rules */
NTSTATUS IoCheckShareAccess(ACCESS_MASK a, ULONG s, PFILE_OBJECT f, PSHARE_ACCESS sa, BOOLEAN u)
{
    BOOLEAN rd = (a & (FILE_EXECUTE | FILE_READ_DATA)) != 0, wr = (a & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0, dl = (a & DELETE) != 0;
    f->ReadAccess = rd; f->WriteAccess = wr; f->DeleteAccess = dl;
    if (rd || wr || dl) {
        BOOLEAN sr = (s & FILE_SHARE_READ) != 0, sw = (s & FILE_SHARE_WRITE) != 0, sd = (s & FILE_SHARE_DELETE) != 0;
        f->SharedRead = sr; f->SharedWrite = sw; f->SharedDelete = sd;
        if ((rd && sa->SharedRead < sa->OpenCount) || (wr && sa->SharedWrite < sa->OpenCount) || (dl && sa->SharedDelete < sa->OpenCount) ||
            (sa->Readers && !sr) || (sa->Writers && !sw) || (sa->Deleters && !sd))
            return (NTSTATUS)0xC0000043L;   /* STATUS_SHARING_VIOLATION */
        if (u) { sa->OpenCount++; sa->Readers += rd; sa->Writers += wr; sa->Deleters += dl; sa->SharedRead += sr; sa->SharedWrite += sw; sa->SharedDelete += sd; }
    }
    return STATUS_SUCCESS;
}
void IoSetShareAccess(ACCESS_MASK a, ULONG s, PFILE_OBJECT f, PSHARE_ACCESS sa)
{
    memset(sa, 0, sizeof(*sa));
    (void)IoCheckShareAccess(a, s, f, sa, TRUE);
}
void IoRemoveShareAccess(PFILE_OBJECT f, PSHARE_ACCESS sa)
{
    if (f->ReadAccess || f->WriteAccess || f->DeleteAccess) {
        CHECK(sa->OpenCount > 0);
        sa->OpenCount--; sa->Readers -= f->ReadAccess; sa->Writers -= f->WriteAccess; sa->Deleters -= f->DeleteAccess;
        sa->SharedRead -= f->SharedRead; sa->SharedWrite -= f->SharedWrite; sa->SharedDelete -= f->SharedDelete;
    }
}

/* ---------------- objects and devices ---------------- */
static void SendSimple(PFILE_OBJECT f, UCHAR mj)
{
    PDEVICE_OBJECT d = RelatedDevice(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE);
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = mj; sp->FileObject = f;
    IoCallDriver(d, i);
    CHECK(i->Completed);
    IoFreeIrp(i);
}

void ObReferenceFile(PFILE_OBJECT f) { f->RefCount++; }
void ObfReferenceObject(PVOID o)
{
    if (((PDEVICE_OBJECT)o)->Type == 3) { ((PDEVICE_OBJECT)o)->ReferenceCount++; return; }
    ((PFILE_OBJECT)o)->RefCount++;
}
NTSTATUS ObReferenceObjectByPointer(PVOID o, ACCESS_MASK a, PVOID t, KPROCESSOR_MODE m) { ObfReferenceObject(o); return STATUS_SUCCESS; }
void ObfDereferenceObject(PVOID o)
{
    PFILE_OBJECT f = o;
    if (((PDEVICE_OBJECT)o)->Type == 3) { CHECK(((PDEVICE_OBJECT)o)->ReferenceCount > 0); ((PDEVICE_OBJECT)o)->ReferenceCount--; return; }
    CHECK(f->RefCount > 0);
    if (--f->RefCount == 0) {
        PDEVICE_OBJECT d = RelatedDevice(f);
        PIRP i;
        PIO_STACK_LOCATION sp;
        if (f->Vpb) f->Vpb->ReferenceCount--;    /* before the close, like IopDeleteFile */
        i = IoAllocateIrp(d->StackSize, FALSE);
        sp = IoGetNextIrpStackLocation(i);
        sp->MajorFunction = IRP_MJ_CLOSE; sp->FileObject = f;
        IoCallDriver(d, i);
        CHECK(i->Completed);
        IoFreeIrp(i);
        f->Closed = 1;
        if (f->FileName.Buffer) free(f->FileName.Buffer);
        g_fo--;
        free(f);
    }
}

NTSTATUS IoCreateDevice(PDRIVER_OBJECT drv, ULONG ext, PUNICODE_STRING n, DEVICE_TYPE t, ULONG c, BOOLEAN e, PDEVICE_OBJECT *o)
{
    PDEVICE_OBJECT d = calloc(1, sizeof(DEVICE_OBJECT) + ext + 64);
    d->Type = 3; d->DriverObject = drv; d->DeviceExtension = ext ? (PVOID)(d + 1) : NULL; d->StackSize = 1;
    d->Flags = DO_DEVICE_INITIALIZING; d->DeviceType = t;
    memset(d->DeviceExtension ? d->DeviceExtension : (PVOID)(d + 1), 0xCC, ext);
    g_devices++;
    *o = d;
    return STATUS_SUCCESS;
}
void IoDeleteDevice(PDEVICE_OBJECT d) { CHECK(!d->Deleted); d->Deleted = 1; g_devices--; }
void IoRegisterFileSystem(PDEVICE_OBJECT d) { g_fsdev = d; }

PFILE_OBJECT IoCreateStreamFileObject(PFILE_OBJECT f, PDEVICE_OBJECT d)
{
    PFILE_OBJECT n = calloc(1, sizeof(FILE_OBJECT));
    n->Type = 5; n->DeviceObject = d; n->Flags = FO_STREAM_FILE; n->RefCount = 1; g_fo++;
    if (d->Vpb) d->Vpb->ReferenceCount++;
    SendSimple(n, IRP_MJ_CLEANUP);    /* the internal handle is closed at once */
    return n;
}

/* ---------------- cache manager / memory manager ---------------- */
/*
 * One page cache per section (SECTION_OBJECT_POINTERS). Pages come in
 * through paging reads, become dirty through CcCopyWrite / pinned writes,
 * and go out through paging writes: CcFlushCache, the lazy writer
 * (LazyWriteAll) and nowhere else. A section keeps the file object it was
 * created with referenced until it is released (MmForceSectionClosed,
 * memory pressure in MmTrimAll, or the lazy writer finding it idle).
 */
#define PG 4096
typedef struct PAGE { LONGLONG Off; PUCHAR Data; int Dirty; int Pins; struct PAGE *Next; } PAGE;
typedef struct SCM {
    CC_FILE_SIZES Sizes; LONGLONG ValidDataGoal;
    PCACHE_MANAGER_CALLBACKS Cb; PVOID Ctx; int PrivateMaps;
    PFILE_OBJECT FileObject; PSECTION_OBJECT_POINTERS Sop;
    PAGE *Hash[512]; int Pages; int Busy; int Seen;
    struct SCM *NextAll;
} SCM;
static SCM *g_scms; static int g_scm_gen;   /* any section created or released */
static int CacheSectionsCount(void) { SCM *m; int n = 0; for (m = g_scms; m; m = m->NextAll) n++; return n; }
int g_lost_dirty, g_paging_writes, g_lock_conflicts, g_mapped_mode, g_chaos, g_chaos_trims;
static unsigned g_krng = 777;
int g_disk_full_sim;

static PAGE **Slot(SCM *m, LONGLONG off) { return &m->Hash[(unsigned)((off / PG) * 2654435761u) % 512]; }
static PAGE *Find(SCM *m, LONGLONG off)
{
    PAGE *p; for (p = *Slot(m, off); p; p = p->Next) if (p->Off == off) return p;
    return NULL;
}
static void DropPage(SCM *m, PAGE *pg)
{
    PAGE **pp = Slot(m, pg->Off);
    while (*pp != pg) pp = &(*pp)->Next;
    *pp = pg->Next; free(pg->Data); free(pg); m->Pages--;
}
static int DirtyPages(SCM *m)
{
    int k, n = 0; PAGE *p;
    for (k = 0; k < 512; k++) for (p = m->Hash[k]; p; p = p->Next) n += p->Dirty;
    return n;
}

static void ReleaseSection(PSECTION_OBJECT_POINTERS sop)
{
    SCM *m = sop->SharedCacheMap, **pp;
    int k;
    for (pp = &g_scms; *pp != m; pp = &(*pp)->NextAll) ;
    *pp = m->NextAll; g_scm_gen++;
    for (k = 0; k < 512; k++) while (m->Hash[k]) { if (m->Hash[k]->Dirty) g_lost_dirty++; CHECK(m->Hash[k]->Pins == 0); DropPage(m, m->Hash[k]); }
    sop->SharedCacheMap = NULL; sop->DataSectionObject = NULL;
    ObDereferenceObject(m->FileObject);
    free(m);
}

void CcInitializeCacheMap(PFILE_OBJECT f, PCC_FILE_SIZES s, BOOLEAN p, PCACHE_MANAGER_CALLBACKS c, PVOID ctx)
{
    PSECTION_OBJECT_POINTERS sop = f->SectionObjectPointer;
    SCM *m = sop->SharedCacheMap;
    CHECK(f->PrivateCacheMap == NULL);
    CHECK(f->FsContext != NULL);
    if (!m) {
        m = calloc(1, sizeof(SCM)); m->FileObject = f; f->RefCount++;   /* the section keeps its file object */
        m->Sop = sop; sop->SharedCacheMap = m; sop->DataSectionObject = m;
        m->NextAll = g_scms; g_scms = m; g_scm_gen++;
        m->ValidDataGoal = s->ValidDataLength.QuadPart;
    }
    m->Sizes = *s; m->Cb = c; m->Ctx = ctx; m->PrivateMaps++;
    f->PrivateCacheMap = (PVOID)1;
}

static void TruncatePages(SCM *m, LONGLONG size)
{
    int k; PAGE *p, *n;
    for (k = 0; k < 512; k++) for (p = m->Hash[k]; p; p = n) {
        n = p->Next;
        if (p->Off >= size) { CHECK(p->Pins == 0); DropPage(m, p); }
    }
}

BOOLEAN CcUninitializeCacheMap(PFILE_OBJECT f, PLARGE_INTEGER t, PVOID e)
{
    SCM *m = f->SectionObjectPointer ? f->SectionObjectPointer->SharedCacheMap : NULL;
    if (m && t) { TruncatePages(m, (t->QuadPart + PG - 1) & ~(LONGLONG)(PG - 1)); if (m->Sizes.FileSize.QuadPart > t->QuadPart) m->Sizes.FileSize = *t; }
    if (f->PrivateCacheMap) { f->PrivateCacheMap = NULL; CHECK(m != NULL); m->PrivateMaps--; }
    return TRUE;
}

BOOLEAN CcPurgeCacheSection(PSECTION_OBJECT_POINTERS s, PLARGE_INTEGER o, ULONG l, BOOLEAN u)
{
    SCM *m = s->SharedCacheMap; int k; PAGE *p, *n;
    LONGLONG from = o ? (o->QuadPart & ~(LONGLONG)(PG - 1)) : 0, to = o ? o->QuadPart + (l ? l : 0x7FFFFFFFFFFFLL) : 0x7FFFFFFFFFFFFFFFLL;
    if (!m) return TRUE;
    for (k = 0; k < 512; k++) for (p = m->Hash[k]; p; p = p->Next) if (p->Off >= from && p->Off < to && p->Pins) return FALSE;
    for (k = 0; k < 512; k++) for (p = m->Hash[k]; p; p = n) { n = p->Next; if (p->Off >= from && p->Off < to) DropPage(m, p); }
    return TRUE;
}

BOOLEAN MmForceSectionClosed(PSECTION_OBJECT_POINTERS s, BOOLEAN d)
{
    SCM *m = s->SharedCacheMap;
    if (!m) return TRUE;
    if (m->PrivateMaps || m->Busy) return FALSE;   /* in use: it goes later */
    ReleaseSection(s);
    return TRUE;
}

/* Memory pressure: drop clean pages, and sections nobody uses */
void MmTrim(PSECTION_OBJECT_POINTERS s)
{
    SCM *m = s->SharedCacheMap; int k; PAGE *p, *n;
    if (!m) return;
    for (k = 0; k < 512; k++) for (p = m->Hash[k]; p; p = n) { n = p->Next; if (!p->Dirty && !p->Pins) DropPage(m, p); }
    if (m->PrivateMaps == 0 && m->Pages == 0 && !m->Busy) ReleaseSection(s);
}
void MmTrimAll(void)
{
    SCM *m; int again;
    /* a release can close file objects and change the list: start over */
    do {
        again = 0;
        for (m = g_scms; m; m = m->NextAll) {
            int before = g_scm_gen;
            MmTrim(m->Sop);
            if (g_scm_gen != before) { again = 1; break; }
        }
    } while (again);
}

int g_paging_reads;
static NTSTATUS PagingIo(UCHAR mj, PFILE_OBJECT f, LONGLONG off, ULONG len, PVOID buf, ULONG_PTR *info)
{
    PDEVICE_OBJECT d = RelatedDevice(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE);
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
    NTSTATUS st;
    i->OwnMdl = i->MdlAddress = IoAllocateMdl(buf, len, FALSE, FALSE, NULL);
    i->Flags = IRP_PAGING_IO | IRP_NOCACHE | (mj == IRP_MJ_WRITE ? IRP_SYNCHRONOUS_PAGING_IO : 0); i->RequestorMode = KernelMode;
    sp->MajorFunction = mj; sp->Parameters.Read.Length = len; sp->Parameters.Read.ByteOffset.QuadPart = off; sp->FileObject = f;
    if (mj == IRP_MJ_READ) g_paging_reads++; else g_paging_writes++;
    IoCallDriver(d, i);
    CHECK(i->Completed);
    st = i->IoStatus.Status; *info = i->IoStatus.Information;
    IoFreeMdl(i->OwnMdl); IoFreeIrp(i);
    return st;
}

/* Sections go from the lazy writer (a cache top-level thread) or from a plain system thread */
static void ChaosTrim(PVOID c)
{
    if ((intptr_t)c & 1) IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    MmTrimAll();
    IoSetTopLevelIrp(NULL);
}

/* A page of the section, read in as a fault would */
static PAGE *GetPage(SCM *m, LONGLONG off, int read)
{
    PAGE *p, **slot;
    ULONG_PTR info = 0; NTSTATUS st;
    /* memory pressure from inside a driver call: sections go, closes come */
    if (g_chaos) {
        g_krng = g_krng * 1103515245 + 12345;
        if ((g_krng >> 16) % g_chaos == 0) { m->Busy++; RunOnOtherThread(ChaosTrim, (PVOID)(intptr_t)(g_krng >> 20)); m->Busy--; g_chaos_trims++; }
    }
    p = Find(m, off);
    if (p) return p;
    p = calloc(1, sizeof(PAGE)); p->Off = off; p->Data = malloc(PG);
    if (read) {
        memset(p->Data, 0xEE, PG);
        st = PagingIo(IRP_MJ_READ, m->FileObject, off, PG, p->Data, &info);
        if (st == STATUS_END_OF_FILE) info = 0;
        else if (!NT_SUCCESS(st)) { free(p->Data); free(p); ExRaiseStatus(st); }
        CHECK(info <= PG);
        memset(p->Data + info, 0, PG - info);   /* Mm zeroes short reads */
    } else memset(p->Data, 0, PG);
    /* the paging read may have raced a page in (not here: single thread) */
    CHECK(Find(m, off) == NULL);
    slot = Slot(m, off); p->Next = *slot; *slot = p; m->Pages++;
    return p;
}

BOOLEAN CcCopyRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, PVOID b, PIO_STATUS_BLOCK s)
{
    SCM *m = f->SectionObjectPointer->SharedCacheMap; LONGLONG off = o->QuadPart; ULONG done = 0;
    CHECK(f->PrivateCacheMap != NULL);
    CHECK(o->QuadPart + l <= m->Sizes.FileSize.QuadPart);
    while (done < l) {
        LONGLONG po = (off + done) & ~(LONGLONG)(PG - 1); ULONG in = (ULONG)(off + done - po), n = PG - in;
        PAGE *p = GetPage(m, po, 1);
        if (n > l - done) n = l - done;
        memcpy((PUCHAR)b + done, p->Data + in, n); done += n;
    }
    s->Status = STATUS_SUCCESS; s->Information = l;
    return TRUE;
}

BOOLEAN CcCopyWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, PVOID b)
{
    SCM *m = f->SectionObjectPointer->SharedCacheMap; LONGLONG off = o->QuadPart; ULONG done = 0;
    CHECK(f->PrivateCacheMap != NULL);
    CHECK(o->QuadPart + l <= m->Sizes.FileSize.QuadPart);
    while (done < l) {
        LONGLONG po = (off + done) & ~(LONGLONG)(PG - 1); ULONG in = (ULONG)(off + done - po), n = PG - in;
        PAGE *p;
        if (n > l - done) n = l - done;
        /* Pages wholly written, or past what Cc believes valid, are not read */
        p = GetPage(m, po, !(in == 0 && n == PG) && po < m->ValidDataGoal);
        memcpy(p->Data + in, (PUCHAR)b + done, n); p->Dirty = 1; done += n;
    }
    if (off + l > m->ValidDataGoal) m->ValidDataGoal = off + l;
    if (f->Flags & FO_WRITE_THROUGH) { IO_STATUS_BLOCK io; CcFlushCache(f->SectionObjectPointer, o, l, &io); if (!NT_SUCCESS(io.Status)) ExRaiseStatus(io.Status); }
    return TRUE;
}

BOOLEAN CcZeroData(PFILE_OBJECT f, PLARGE_INTEGER s, PLARGE_INTEGER e, BOOLEAN w)
{
    SCM *m = f->SectionObjectPointer ? f->SectionObjectPointer->SharedCacheMap : NULL;
    LONGLONG off = s->QuadPart, end = e->QuadPart;
    CHECK((off & 511) == 0 && (end & 511) == 0 && off <= end);
    if (m && f->PrivateCacheMap) {
        CHECK(end <= ((m->Sizes.FileSize.QuadPart + 511) & ~511LL));
        while (off < end) {
            LONGLONG po = off & ~(LONGLONG)(PG - 1); ULONG in = (ULONG)(off - po), n = PG - in; PAGE *p;
            if (n > end - off) n = (ULONG)(end - off);
            p = GetPage(m, po, !(in == 0 && n == PG) && po < m->ValidDataGoal);
            memset(p->Data + in, 0, n); p->Dirty = 1; off += n;
        }
        if (end > m->ValidDataGoal) m->ValidDataGoal = end;
    } else {
        /* straight to the disk through synchronous paging writes */
        PUCHAR z = calloc(1, 65536);
        while (off < end) {
            ULONG n = (ULONG)((end - off) > 65536 ? 65536 : end - off); ULONG_PTR info; NTSTATUS st;
            st = PagingIo(IRP_MJ_WRITE, f, off, n, z, &info);
            if (!NT_SUCCESS(st)) { free(z); ExRaiseStatus(st); }
            off += n;
        }
        free(z);
    }
    return TRUE;
}

BOOLEAN CcCanIWrite(PFILE_OBJECT f, ULONG n, BOOLEAN w, BOOLEAN r) { return TRUE; }
void CcMdlRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, PMDL *m, PIO_STATUS_BLOCK s) { abort(); }
void CcMdlReadComplete(PFILE_OBJECT f, PMDL m) { abort(); }
void CcPrepareMdlWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, PMDL *m, PIO_STATUS_BLOCK s) { abort(); }
void CcMdlWriteComplete(PFILE_OBJECT f, PLARGE_INTEGER o, PMDL m) { abort(); }

void CcSetFileSizes(PFILE_OBJECT f, PCC_FILE_SIZES s)
{
    SCM *m = f->SectionObjectPointer->SharedCacheMap;
    CHECK(s->FileSize.QuadPart <= s->AllocationSize.QuadPart);
    CHECK(s->ValidDataLength.QuadPart <= s->FileSize.QuadPart);
    if (!m) return;
    /* whole pages past the new end go, dirty or not; the partial one stays as it is */
    if (s->FileSize.QuadPart < m->Sizes.FileSize.QuadPart) TruncatePages(m, (s->FileSize.QuadPart + PG - 1) & ~(LONGLONG)(PG - 1));
    m->Sizes = *s;
    m->ValidDataGoal = s->ValidDataLength.QuadPart;
}

typedef struct { PAGE *Page; SCM *Scm; int Pinned; long magic; } BCB;
static BOOLEAN MapOrPin(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, PVOID *bcb, PVOID *buf, int pin, int read, int zero)
{
    SCM *m = f->SectionObjectPointer->SharedCacheMap; BCB *b; LONGLONG po = o->QuadPart & ~(LONGLONG)(PG - 1);
    CHECK(f->PrivateCacheMap != NULL);
    CHECK(o->QuadPart + l <= m->Sizes.FileSize.QuadPart);
    CHECK((o->QuadPart - po) + l <= PG);                /* the driver maps within one unit */
    b = malloc(sizeof(BCB)); b->Scm = m; b->Pinned = pin; b->magic = 0x4243;
    b->Page = GetPage(m, po, read);
    if (zero) memset(b->Page->Data + (o->QuadPart - po), 0, l);
    b->Page->Pins++;
    *bcb = b; *buf = b->Page->Data + (o->QuadPart - po);
    g_bcb++;
    return TRUE;
}
BOOLEAN CcMapData(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, ULONG w, PVOID *bcb, PVOID *buf) { return MapOrPin(f, o, l, bcb, buf, 0, 1, 0); }
BOOLEAN CcPinRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, ULONG w, PVOID *bcb, PVOID *buf) { return MapOrPin(f, o, l, bcb, buf, 1, 1, 0); }
BOOLEAN CcPreparePinWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN z, ULONG w, PVOID *bcb, PVOID *buf)
{
    BOOLEAN r;
    CHECK((o->QuadPart & (PG - 1)) == 0 && l == PG);      /* whole pages only, as the driver uses it */
    r = MapOrPin(f, o, l, bcb, buf, 1, 0, z);
    ((BCB *)*bcb)->Page->Dirty = 1;
    return r;
}
void CcSetDirtyPinnedData(PVOID p, PLARGE_INTEGER lsn) { BCB *b = p; CHECK(b->magic == 0x4243); CHECK(b->Pinned); b->Page->Dirty = 1; }
void CcUnpinData(PVOID p) { BCB *b = p; CHECK(b->magic == 0x4243); CHECK(b->Page->Pins > 0); b->Page->Pins--; b->magic = 0; free(b); g_bcb--; }

/* Writes dirty pages in [o, o+l) back through paging writes */
static NTSTATUS FlushRange(SCM *m, LONGLONG from, LONGLONG to)
{
    int k; PAGE *p; NTSTATUS st = STATUS_SUCCESS;
    LONGLONG *offs = NULL; int n = 0, cap = 0, j;
    for (k = 0; k < 512; k++) for (p = m->Hash[k]; p; p = p->Next)
        if (p->Dirty && p->Off + PG > from && p->Off < to) { if (n == cap) { cap = cap ? cap * 2 : 64; offs = realloc(offs, cap * sizeof(LONGLONG)); } offs[n++] = p->Off; }
    /* in file order, like the modified page writer */
    for (k = 1; k < n; k++) for (j = k; j > 0 && offs[j - 1] > offs[j]; j--) { LONGLONG t = offs[j]; offs[j] = offs[j - 1]; offs[j - 1] = t; }
    m->Busy++;
    for (j = 0; j < n; j++) {
        ULONG_PTR info; NTSTATUS s1; PUCHAR copy;
        p = Find(m, offs[j]);
        if (!p || !p->Dirty) continue;
        copy = malloc(PG); memcpy(copy, p->Data, PG);   /* the page stays in memory while it is written */
        p->Dirty = 0;
        s1 = PagingIo(IRP_MJ_WRITE, m->FileObject, offs[j], PG, copy, &info);
        free(copy);
        if (!NT_SUCCESS(s1)) {
            if ((p = Find(m, offs[j])) != NULL) p->Dirty = 1;
            if (s1 == (NTSTATUS)0xC0000054L) g_lock_conflicts++;
            if (NT_SUCCESS(st)) st = s1;
        }
    }
    m->Busy--;
    free(offs);
    return st;
}
void CcFlushCache(PSECTION_OBJECT_POINTERS s, PLARGE_INTEGER o, ULONG l, PIO_STATUS_BLOCK io)
{
    SCM *m = s->SharedCacheMap; NTSTATUS st = STATUS_SUCCESS;
    if (m) st = FlushRange(m, o ? o->QuadPart : 0, o ? o->QuadPart + l : 0x7FFFFFFFFFFFFFFFLL);
    if (io) { io->Status = st; io->Information = 0; }
}

BOOLEAN MmCanFileBeTruncated(PSECTION_OBJECT_POINTERS s, PLARGE_INTEGER n) { return !g_mapped_mode; }
BOOLEAN MmFlushImageSection(PSECTION_OBJECT_POINTERS s, MMFLUSH_TYPE t) { return TRUE; }

/*
 * The lazy writer: every dirty section is written back with the owner's
 * callbacks held, then idle sections are let go (their file objects close).
 */
void LazyWriteAll(void)
{
    SCM *m, *n; int pass;
    for (pass = 0; pass < 2; pass++) {
        for (m = g_scms; m; m = m->NextAll) m->Seen = 0;
        /* a flush can let other threads create and release sections: start over each time */
        for (;;) {
            for (m = g_scms; m && (m->Seen || !DirtyPages(m)); m = m->NextAll) ;
            if (!m) break;
            m->Seen = 1; m->Busy++;
            m->FileObject->RefCount++;
            if (m->Cb->AcquireForLazyWrite(m->Ctx, TRUE)) {
                CHECK(IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP || IoGetTopLevelIrp() == NULL);
                (void)FlushRange(m, 0, 0x7FFFFFFFFFFFFFFFLL);
                m->Cb->ReleaseFromLazyWrite(m->Ctx);
            }
            m->Busy--;
            ObDereferenceObject(m->FileObject);
        }
    }
    for (m = g_scms; m; m = n) {
        n = m->NextAll;
        if (m->PrivateMaps == 0 && !DirtyPages(m)) {
            int k; for (k = 0; k < 512; k++) { PAGE *p; for (p = m->Hash[k]; p; p = p->Next) if (p->Pins) goto busy; }
            ReleaseSection(m->Sop);
            n = g_scms;           /* the list changed under us */
        busy: ;
        }
    }
}
int CacheDirtyPages(void) { SCM *m; int n = 0; for (m = g_scms; m; m = m->NextAll) n += DirtyPages(m); return n; }
int CacheSections(void) { SCM *m; int n = 0; for (m = g_scms; m; m = m->NextAll) n++; return n; }

/* ---------------- worker threads ---------------- */
static PWORK_QUEUE_ITEM g_work[256]; static int g_nwork;
void ExQueueWorkItem(PWORK_QUEUE_ITEM i, WORK_QUEUE_TYPE t) { CHECK(g_nwork < 256); g_work[g_nwork++] = i; }
int g_workers_run;
void RunWorkers(void)
{
    while (g_nwork) {
        PWORK_QUEUE_ITEM i = g_work[0]; PIRP top = IoGetTopLevelIrp();
        memmove(g_work, g_work + 1, --g_nwork * sizeof(g_work[0]));
        IoSetTopLevelIrp(NULL);      /* a fresh system thread */
        i->WorkerRoutine(i->Parameter);
        IoSetTopLevelIrp(top);
        g_workers_run++;
    }
}
int PendingWorkers(void) { return g_nwork; }

/* ---------------- FsRtl ---------------- */
BOOLEAN FsRtlIsNtstatusExpected(NTSTATUS s) { return s != STATUS_ACCESS_VIOLATION && s != STATUS_DATATYPE_MISALIGNMENT; }
NTSTATUS FsRtlNormalizeNtstatus(NTSTATUS s, NTSTATUS d) { return FsRtlIsNtstatusExpected(s) ? s : d; }
static int Match(const WCHAR *p, int pn, const WCHAR *n, int nn)
{
    if (pn == 0) return nn == 0;
    if (*p == '*' || *p == '<') { int i; for (i = 0; i <= nn; i++) if (Match(p + 1, pn - 1, n + i, nn - i)) return 1; return 0; }
    if (nn == 0) return 0;
    if (*p == '?' || *p == '>' || towupper(*n) == *p) return Match(p + 1, pn - 1, n + 1, nn - 1);
    return 0;
}
BOOLEAN FsRtlIsNameInExpression(PUNICODE_STRING e, PUNICODE_STRING n, BOOLEAN i, PWCHAR t)
{
    int k; for (k = 0; k < e->Length / 2; k++) CHECK(e->Buffer[k] == towupper(e->Buffer[k]));
    return Match(e->Buffer, e->Length / 2, n->Buffer, n->Length / 2);
}
BOOLEAN FsRtlDoesNameContainWildCards(PUNICODE_STRING n)
{
    int k; for (k = 0; k < n->Length / 2; k++) { WCHAR c = n->Buffer[k]; if (c == '*' || c == '?' || c == '<' || c == '>' || c == '"') return TRUE; }
    return FALSE;
}
void FsRtlInitializeFileLock(PFILE_LOCK l, PVOID a, PVOID b) { memset(l, 0, sizeof(*l)); }
void FsRtlUninitializeFileLock(PFILE_LOCK l) { }
BOOLEAN FsRtlCheckLockForReadAccess(PFILE_LOCK l, PIRP i) { return TRUE; }
BOOLEAN FsRtlCheckLockForWriteAccess(PFILE_LOCK l, PIRP i) { return TRUE; }
BOOLEAN FsRtlFastCheckLockForWrite(PFILE_LOCK l, PLARGE_INTEGER o, PLARGE_INTEGER n, ULONG k, PFILE_OBJECT f, PVOID p) { return TRUE; }
BOOLEAN FsRtlFastCheckLockForRead(PFILE_LOCK l, PLARGE_INTEGER o, PLARGE_INTEGER n, ULONG k, PFILE_OBJECT f, PVOID p) { return TRUE; }
NTSTATUS FsRtlFastUnlockAll(PFILE_LOCK l, PFILE_OBJECT f, PEPROCESS p, PVOID c) { return STATUS_SUCCESS; }
NTSTATUS FsRtlProcessFileLock(PFILE_LOCK l, PIRP i, PVOID c) { i->IoStatus.Status = STATUS_SUCCESS; IoCompleteRequest(i, 0); return STATUS_SUCCESS; }
int g_fastio_reads;
BOOLEAN FsRtlCopyRead(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, ULONG k, PVOID b, PIO_STATUS_BLOCK s, PDEVICE_OBJECT d)
{
    FSRTL_COMMON_FCB_HEADER *h = f->FsContext;
    BOOLEAN ok;
    if (f->PrivateCacheMap == NULL || h->IsFastIoPossible == FastIoIsNotPossible) return FALSE;
    if (h->IsFastIoPossible == FastIoIsQuestionable &&
        !d->DriverObject->FastIoDispatch->FastIoCheckIfPossible(f, o, l, w, k, TRUE, s, d)) return FALSE;
    FsRtlEnterFileSystem();
    ExAcquireResourceSharedLite(h->Resource, TRUE);
    if (o->QuadPart >= h->FileSize.QuadPart) { s->Status = STATUS_END_OF_FILE; s->Information = 0; ok = TRUE; }
    else {
        if (o->QuadPart + l > h->FileSize.QuadPart) l = (ULONG)(h->FileSize.QuadPart - o->QuadPart);
        ok = CcCopyRead(f, o, l, w, b, s);
        g_fastio_reads++;
    }
    ExReleaseResourceForThreadLite(h->Resource, 0);
    FsRtlExitFileSystem();
    return ok;
}
/*
 * FsRtlCopyWrite as NT does it: extends the file inside its allocation,
 * zeroes from VDL, copies through the cache and moves the sizes, all under
 * the main resource, without asking the file system.
 */
int g_fastio_writes;
BOOLEAN FsRtlCopyWrite(PFILE_OBJECT f, PLARGE_INTEGER o, ULONG l, BOOLEAN w, ULONG k, PVOID b, PIO_STATUS_BLOCK s, PDEVICE_OBJECT d)
{
    FSRTL_COMMON_FCB_HEADER *h = f->FsContext;
    LARGE_INTEGER off = *o, end; BOOLEAN toEof = (o->LowPart == 0xffffffff && o->HighPart == -1), ext;
    if (f->PrivateCacheMap == NULL || h->IsFastIoPossible == FastIoIsNotPossible) return FALSE;
    if (!CcCanIWrite(f, l, w, FALSE)) return FALSE;
    FsRtlEnterFileSystem();
    ExAcquireResourceExclusiveLite(h->Resource, TRUE);
    if (h->IsFastIoPossible == FastIoIsNotPossible ||
        (h->IsFastIoPossible == FastIoIsQuestionable &&
         !d->DriverObject->FastIoDispatch->FastIoCheckIfPossible(f, o, l, w, k, FALSE, s, d))) {
        ExReleaseResourceForThreadLite(h->Resource, 0); FsRtlExitFileSystem(); return FALSE;
    }
    if (toEof) off = h->FileSize;
    end.QuadPart = off.QuadPart + l;
    if (end.QuadPart > h->AllocationSize.QuadPart) { ExReleaseResourceForThreadLite(h->Resource, 0); FsRtlExitFileSystem(); return FALSE; }
    ext = end.QuadPart > h->FileSize.QuadPart;
    if (ext) {
        SCM *m = f->SectionObjectPointer->SharedCacheMap;
        h->FileSize = end;
        m->Sizes.FileSize = end;                     /* CcGetFileSizePointer */
    }
    if (off.QuadPart > h->ValidDataLength.QuadPart) {
        LARGE_INTEGER zs, ze; zs.QuadPart = h->ValidDataLength.QuadPart & ~511LL; ze.QuadPart = off.QuadPart & ~511LL;
        /* the head sector goes through a copy, as the real one does */
        if ((h->ValidDataLength.QuadPart & 511) != 0) {
            UCHAR z[512]; LARGE_INTEGER hs; ULONG n = (ULONG)(512 - (h->ValidDataLength.QuadPart & 511));
            memset(z, 0, sizeof(z)); hs = h->ValidDataLength;
            if (hs.QuadPart + n > off.QuadPart) n = (ULONG)(off.QuadPart - hs.QuadPart);
            CcCopyWrite(f, &hs, n, TRUE, z);
            zs.QuadPart += 512;
        }
        if (zs.QuadPart < ze.QuadPart) CcZeroData(f, &zs, &ze, TRUE);
    }
    CcCopyWrite(f, &off, l, TRUE, b);
    if (end.QuadPart > h->ValidDataLength.QuadPart) h->ValidDataLength = end;
    f->Flags |= FO_FILE_MODIFIED | (ext ? FO_FILE_SIZE_CHANGED : 0);
    f->CurrentByteOffset = end;
    ExReleaseResourceForThreadLite(h->Resource, 0);
    FsRtlExitFileSystem();
    s->Status = STATUS_SUCCESS; s->Information = l;
    g_fastio_writes++;
    return TRUE;
}

static int g_notify;
int g_reports;
void FsRtlNotifyFullReportChange(PNOTIFY_SYNC s, PLIST_ENTRY l, PSTRING n, USHORT off, PSTRING st, PSTRING np, ULONG f, ULONG a, PVOID c)
{
    CHECK(n->Length >= 4 && ((PWCHAR)n->Buffer)[0] == '\\');
    CHECK(off >= 2 && off < n->Length && ((PWCHAR)n->Buffer)[off / 2 - 1] == '\\');
    CHECK(a >= 1 && a <= 5);
    g_reports++;
}
PDEVICE_OBJECT g_shutdown_dev;
NTSTATUS IoRegisterShutdownNotification(PDEVICE_OBJECT d) { g_shutdown_dev = d; return STATUS_SUCCESS; }
void FsRtlNotifyInitializeSync(PNOTIFY_SYNC *s) { *s = malloc(8); g_notify++; }
void FsRtlNotifyUninitializeSync(PNOTIFY_SYNC *s) { free(*s); *s = NULL; g_notify--; }
PIRP g_pending_notify; PVOID g_pending_notify_ctx;
void FsRtlNotifyCleanup(PNOTIFY_SYNC s, PLIST_ENTRY l, PVOID c)
{
    if (g_pending_notify && g_pending_notify_ctx == c) {
        g_pending_notify->IoStatus.Status = (NTSTATUS)0x0000010CL;   /* STATUS_NOTIFY_CLEANUP */
        IoCompleteRequest(g_pending_notify, 0); g_pending_notify = NULL;
    }
}
void FsRtlNotifyFullChangeDirectory(PNOTIFY_SYNC s, PLIST_ENTRY l, PVOID c, PSTRING n, BOOLEAN w, BOOLEAN i, ULONG f, PIRP irp, PVOID t, PVOID sc)
{
    CHECK(g_pending_notify == NULL);
    CHECK(n->Length >= 2 && ((PWCHAR)n->Buffer)[0] == '\\');
    g_pending_notify = irp; g_pending_notify_ctx = c;
}
NTSTATUS FsRtlNotifyVolumeEvent(PFILE_OBJECT f, ULONG e) { return STATUS_SUCCESS; }

/* Registry: DWORD values from REG_<name> in the environment, else the default */
int g_reg_queries;
NTSTATUS RtlQueryRegistryValues(ULONG RelativeTo, PCWSTR Path, PRTL_QUERY_REGISTRY_TABLE Table, PVOID Context, PVOID Environment)
{
    char name[128], *v; int k;
    CHECK(Path != NULL && Path[0] == '\\');
    for (; Table->Name != NULL; Table++) {
        CHECK(Table->Flags & RTL_QUERY_REGISTRY_DIRECT);
        strcpy(name, "REG_");
        for (k = 0; Table->Name[k] && k < 100; k++) name[4 + k] = (char)Table->Name[k];
        name[4 + k] = 0;
        g_reg_queries++;
        v = getenv(name);
        if (v) *(ULONG *)Table->EntryContext = (ULONG)strtoul(v, NULL, 0);
        else { CHECK(Table->DefaultType == REG_DWORD && Table->DefaultLength == sizeof(ULONG)); *(ULONG *)Table->EntryContext = *(ULONG *)Table->DefaultData; }
    }
    return STATUS_SUCCESS;
}
