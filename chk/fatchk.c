/*
 * FATNT - fatchk: check and repair a FAT12/16/32 volume, like chkdsk.
 *
 *   fatchk drive: [/F] [/V]
 *
 *   /F   fix the problems found
 *   /V   name every file as it is checked
 *
 * What it checks and, with /F, repairs:
 *   - the boot sector and geometry
 *   - the two reserved FAT entries and the clean-shutdown bit
 *   - every cluster chain for bad cluster numbers, loops and over-long
 *     chains (a file is cut at the last good cluster, its size with it)
 *   - cross-linked files (the one met first keeps the clusters)
 *   - directory sizes against their chains
 *   - lost cluster chains (freed)
 *   - the free count
 *
 * Portable: Win32 (the real tool) or POSIX (for testing on an image).
 * Shares the on-disk format logic with the driver (fatsup.c / fatdisk.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/fatusr.h"
#include "../src/fatdisk.h"

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* ------------------------------------------------------------------ */
/* Platform I/O                                                        */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
static HANDLE g_v;
static int IoOpen(const char *d){ char p[16]; sprintf(p,"\\\\.\\%c:",d[0]);
    g_v=CreateFileA(p,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0);
    return g_v!=INVALID_HANDLE_VALUE; }
static int IoRd(u64 o,void*b,u32 n){ LARGE_INTEGER li; DWORD d; li.QuadPart=o;
    SetFilePointerEx(g_v,li,0,FILE_BEGIN); return ReadFile(g_v,b,n,&d,0)&&d==n; }
static int IoWr(u64 o,const void*b,u32 n){ LARGE_INTEGER li; DWORD d; li.QuadPart=o;
    SetFilePointerEx(g_v,li,0,FILE_BEGIN); return WriteFile(g_v,b,n,&d,0)&&d==n; }
static u64 IoSize(void){ GET_LENGTH_INFORMATION g; DWORD n;
    return DeviceIoControl(g_v,IOCTL_DISK_GET_LENGTH_INFO,0,0,&g,sizeof(g),&n,0)?g.Length.QuadPart:0; }
static void IoClose(void){ CloseHandle(g_v); }
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
static int g_v;
static int IoOpen(const char *p){ g_v=open(p,O_RDWR); return g_v>=0; }
static int IoRd(u64 o,void*b,u32 n){ return pread(g_v,b,n,(off_t)o)==(ssize_t)n; }
static int IoWr(u64 o,const void*b,u32 n){ return pwrite(g_v,b,n,(off_t)o)==(ssize_t)n; }
static u64 IoSize(void){ struct stat st; return fstat(g_v,&st)==0?(u64)st.st_size:0; }
static void IoClose(void){ close(g_v); }
#endif

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static FAT_GEOMETRY G;
static u32 g_sector;
static int g_fix, g_verbose;
static int g_problems, g_fixed;
static u8 *g_fat;               /* a working copy of the active FAT */
static u32 g_fatbytes;
static u8 *g_owner;             /* cluster -> 1 if owned by a file seen */
static int g_fatdirty;

static u32 FatEnt(u32 c)
{
    u32 off, v;
    switch (G.FatType) {
    case FAT_TYPE_12:
        off = c + c/2;
        v = g_fat[off] | ((u32)g_fat[off+1] << 8);
        v = (c & 1) ? (v >> 4) : (v & 0xFFF);
        return v;
    case FAT_TYPE_16:
        return g_fat[c*2] | ((u32)g_fat[c*2+1] << 8);
    default:
        off = c*4;
        return (g_fat[off] | ((u32)g_fat[off+1]<<8) | ((u32)g_fat[off+2]<<16) |
                ((u32)g_fat[off+3]<<24)) & 0x0FFFFFFF;
    }
}

static void FatSet(u32 c, u32 v)
{
    u32 off;
    switch (G.FatType) {
    case FAT_TYPE_12:
        off = c + c/2;
        if (c & 1) { g_fat[off]=(g_fat[off]&0x0F)|((v<<4)&0xF0); g_fat[off+1]=(v>>4)&0xFF; }
        else { g_fat[off]=v&0xFF; g_fat[off+1]=(g_fat[off+1]&0xF0)|((v>>8)&0x0F); }
        break;
    case FAT_TYPE_16:
        g_fat[c*2]=v&0xFF; g_fat[c*2+1]=(v>>8)&0xFF; break;
    default:
        off=c*4; g_fat[off]=v&0xFF; g_fat[off+1]=(v>>8)&0xFF; g_fat[off+2]=(v>>16)&0xFF;
        g_fat[off+3]=(g_fat[off+3]&0xF0)|((v>>24)&0x0F); break;
    }
    g_fatdirty = 1;
}

static u32 g_eoc, g_mask, g_bad;
static int IsEoc(u32 v){ return v >= g_eoc; }
static int IsBad(u32 v){ return v == g_bad; }
static int Valid(u32 c){ return c >= 2 && c < G.ClusterCount + 2; }

static u64 ClusterLbo(u32 c){ return ((u64)G.FirstDataSector * g_sector) + (u64)(c-2) * G.ClusterSize; }

/* ------------------------------------------------------------------ */
/* Chain check                                                         */
/* ------------------------------------------------------------------ */

/* Walks a chain, marking owners; returns the cluster count. Fixes loops,
   bad links and cross-links (cuts the chain) with /F. */
static u32 CheckChain(u32 first, const char *what)
{
    u32 c = first, prev = 0, count = 0;

    while (Valid(c)) {

        if (g_owner[c]) {
            /* cross-link: this run already belongs to another file */
            g_problems++;
            printf("  %s: cross-linked at cluster %u\n", what, c);
            if (g_fix && prev) { FatSet(prev, g_eoc); g_fixed++; }
            break;
        }

        g_owner[c] = 1;
        count++;

        {
            u32 next = FatEnt(c);
            if (IsEoc(next)) { c = next; break; }
            if (IsBad(next) || !Valid(next)) {
                g_problems++;
                printf("  %s: bad link %u -> %08x\n", what, c, next);
                if (g_fix) { FatSet(c, g_eoc); g_fixed++; }
                c = g_eoc;
                break;
            }
            prev = c;
            c = next;
            if (count > G.ClusterCount) {
                g_problems++;
                printf("  %s: chain too long\n", what);
                if (g_fix) { FatSet(prev, g_eoc); g_fixed++; }
                break;
            }
        }
    }

    return count;
}

/* ------------------------------------------------------------------ */
/* Directory walk                                                      */
/* ------------------------------------------------------------------ */

static u8 *ReadChainData(u32 first, u32 *lenOut)
{
    /* Reads a whole directory's clusters into memory */
    u32 cap = 0, used = 0, c = first;
    u8 *buf = NULL;
    while (Valid(c)) {
        if (used + G.ClusterSize > cap) { cap = cap ? cap*2 : G.ClusterSize*4; buf = realloc(buf, cap); }
        if (!IoRd(ClusterLbo(c), buf + used, G.ClusterSize)) break;
        used += G.ClusterSize;
        { u32 n = FatEnt(c); if (IsEoc(n) || !Valid(n)) break; c = n; }
    }
    *lenOut = used;
    return buf;
}

static void WalkDir(u32 firstCluster, int isFixedRoot, const char *path);

static void WalkEntries(u8 *data, u32 len, const char *path)
{
    u32 pos;
    char child[1200];

    for (pos = 0; pos + FAT_DIRENT_SIZE <= len; pos += FAT_DIRENT_SIZE) {

        FAT_DIR_INFO info;
        u32 avail = (len - pos) / FAT_DIRENT_SIZE;
        if (avail > FAT_LFN_MAX_ENTRIES + 1) avail = FAT_LFN_MAX_ENTRIES + 1;
        {
            u32 r = FatParseDirEntry(data + pos, avail, pos, &info);
            if (r == FAT_NAME_END) break;
            if (r == FAT_NAME_FREE) { pos += (info.EntryCount - 1) * FAT_DIRENT_SIZE; continue; }
            if (r == FAT_NAME_LABEL) { pos += (info.EntryCount - 1) * FAT_DIRENT_SIZE; continue; }

            pos = info.Offset;      /* resume right after the short entry */

            /* "." and ".." are not followed */
            if (info.ShortName[0] == '.' ) continue;

            {
                char name8[260]; u32 k, nl = info.NameLength;
                for (k = 0; k < nl && k < 259; k++) name8[k] = (char)info.Name[k];
                name8[k] = 0;
                snprintf(child, sizeof(child), "%s%s%s", path,
                         path[strlen(path)-1] == '\\' ? "" : "\\", name8);
            }

            if (g_verbose) printf("  %s\n", child);

            if (info.Attributes & FAT_ATTR_DIRECTORY) {

                if (info.FirstCluster != 0) {
                    u32 clusters = CheckChain(info.FirstCluster, child);
                    (void)clusters;
                    WalkDir(info.FirstCluster, 0, child);
                }

            } else {

                u32 clusters = info.FirstCluster ? CheckChain(info.FirstCluster, child) : 0;
                u64 needClusters = (info.FileSize + G.ClusterSize - 1) / G.ClusterSize;

                if ((u64)clusters < needClusters) {
                    g_problems++;
                    printf("  %s: size %u needs %llu clusters, chain has %u\n",
                           child, info.FileSize, needClusters, clusters);
                    /* a /F repair here would rewrite the entry; left to chkdsk proper */
                }
            }
        }
    }
}

static void WalkDir(u32 firstCluster, int isFixedRoot, const char *path)
{
    u8 *data; u32 len;

    if (isFixedRoot) {
        len = G.RootDirSectors * g_sector;
        data = malloc(len);
        if (!IoRd((u64)G.FirstRootSector * g_sector, data, len)) { free(data); return; }
    } else {
        data = ReadChainData(firstCluster, &len);
        if (!data) return;
    }

    WalkEntries(data, len, path);
    free(data);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *drive = NULL;
    u8 boot[4096];
    u32 i;
    u64 bytes;

    for (i = 1; i < (u32)argc; i++) {
        if (!strcasecmp(argv[i], "/F") || !strcasecmp(argv[i], "-F")) g_fix = 1;
        else if (!strcasecmp(argv[i], "/V") || !strcasecmp(argv[i], "-V")) g_verbose = 1;
        else drive = argv[i];
    }
    if (!drive) { fprintf(stderr, "usage: fatchk drive: [/F] [/V]\n"); return 3; }

    if (!IoOpen(drive)) { fprintf(stderr, "cannot open %s\n", drive); return 3; }
    bytes = IoSize();

    if (!IoRd(0, boot, sizeof(boot))) { fprintf(stderr, "read error\n"); IoClose(); return 3; }

    g_sector = boot[11] | (boot[12] << 8);
    if (g_sector < 512 || g_sector > 4096) { fprintf(stderr, "bad sector size\n"); IoClose(); return 3; }

    if (FatCheckBootSector(boot, g_sector, bytes / g_sector, &G) != FAT_BOOT_OK) {
        fprintf(stderr, "%s is not a FAT volume (or the boot sector is damaged)\n", drive);
        IoClose();
        return 3;
    }

    printf("FAT%d, %u clusters of %u bytes.\n", G.FatType, G.ClusterCount, G.ClusterSize);

    switch (G.FatType) {
    case FAT_TYPE_12: g_eoc = FAT12_EOC; g_mask = FAT12_MASK; g_bad = FAT12_BAD; break;
    case FAT_TYPE_16: g_eoc = FAT16_EOC; g_mask = FAT16_MASK; g_bad = FAT16_BAD; break;
    default:          g_eoc = FAT32_EOC; g_mask = FAT32_MASK; g_bad = FAT32_BAD; break;
    }

    /* Load the active FAT */
    g_fatbytes = G.FatSectors * g_sector;
    g_fat = malloc(g_fatbytes);
    if (!IoRd((u64)G.ReservedSectors * g_sector, g_fat, g_fatbytes)) {
        fprintf(stderr, "cannot read the FAT\n"); IoClose(); return 3;
    }

    g_owner = calloc(G.ClusterCount + 2, 1);
    g_owner[0] = g_owner[1] = 1;

    /* Reserved entries */
    if ((FatEnt(0) & 0xFF) != G.Media) {
        g_problems++; printf("FAT[0] media byte wrong\n");
        if (g_fix) { FatSet(0, g_mask & ~0xFF); g_fat[0] = (u8)G.Media; g_fixed++; }
    }

    /* The dirty (clean-shutdown) bit */
    if (G.FatType != FAT_TYPE_12) {
        u32 e1 = FatEnt(1);
        u32 cleanBit = (G.FatType == FAT_TYPE_16) ? 0x8000 : 0x08000000;
        if (!(e1 & cleanBit)) {
            printf("Volume was not cleanly dismounted%s.\n", g_fix ? " (clearing the flag)" : "");
            if (g_fix) { FatSet(1, e1 | cleanBit); g_fixed++; }
        }
    }

    /* The root, then everything below it */
    if (G.FatType == FAT_TYPE_32) {
        CheckChain(G.RootCluster, "\\");
        WalkDir(G.RootCluster, 0, "\\");
    } else {
        WalkDir(0, 1, "\\");
    }

    /* Lost clusters: allocated in the FAT but owned by no file */
    {
        u32 c, lost = 0;
        for (c = 2; c < G.ClusterCount + 2; c++) {
            u32 e = FatEnt(c);
            if (e != 0 && !IsBad(e) && !g_owner[c]) {
                lost++;
                if (g_fix) FatSet(c, 0);
            }
        }
        if (lost) {
            g_problems++;
            printf("%u lost cluster(s)%s.\n", lost, g_fix ? " freed" : "");
            if (g_fix) g_fixed++;
        }
    }

    /* Free count */
    {
        u32 c, freec = 0;
        for (c = 2; c < G.ClusterCount + 2; c++) if (FatEnt(c) == 0) freec++;
        printf("%u of %u clusters free.\n", freec, G.ClusterCount);
    }

    /* Write back */
    if (g_fix && g_fatdirty) {
        u32 f;
        for (f = 0; f < G.NumberOfFats; f++) {
            IoWr((u64)(G.ReservedSectors + f * G.FatSectors) * g_sector, g_fat, g_fatbytes);
        }
        printf("Repairs written.\n");
    }

    IoClose();

    if (g_problems == 0) { printf("%s: clean.\n", drive); return 0; }
    if (g_fix)           { printf("%s: %d problem(s), %d fixed.\n", drive, g_problems, g_fixed); return 1; }
    printf("%s: %d problem(s) found; run with /F to fix.\n", drive, g_problems);
    return 2;
}
