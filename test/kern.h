#ifndef KERN_H
#define KERN_H
#include <setjmp.h>
extern __thread int g_crit;
extern int g_switches, g_deadlocks;
int OtherThreadsLive(void); void FinishOtherThreads(void); void RunOnOtherThread(void (*Routine)(PVOID), PVOID Context);
extern int g_res, g_pool, g_mdl, g_bcb, g_irp, g_devices, g_verbose, g_fo, g_errors, g_paging_reads, g_fastio_reads;
extern jmp_buf *g_raise_jb;
extern NTSTATUS g_raised;
extern PDEVICE_OBJECT g_fsdev;
extern PIRP g_pending_notify;
void ObReferenceFile(PFILE_OBJECT f);
void MmTrim(PSECTION_OBJECT_POINTERS s);
void MmTrimAll(void);
void LazyWriteAll(void);
void RunWorkers(void);
int PendingWorkers(void);
int CacheDirtyPages(void);
int CacheSections(void);
extern int g_chaos, g_chaos_trims, g_lost_dirty, g_paging_writes, g_lock_conflicts, g_mapped_mode, g_fastio_writes, g_reports, g_workers_run;
extern PDEVICE_OBJECT g_shutdown_dev;
extern LONGLONG g_now;
PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT f);
#endif
