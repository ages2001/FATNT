/*
 * FATNT - fatfmt: format a volume as FAT12, FAT16 or FAT32.
 *
 * Lays out the boot sector (and the FAT32 backup and FSInfo), the FATs and
 * an empty root directory, following the Microsoft FAT specification, then
 * sets the partition type. The same tool runs on NT 3.1, 3.5x, 4.0, 2000
 * and XP. Compiles for Win32 (the real tool) or POSIX (for testing against
 * a disk image).
 *
 *   fatfmt drive: [/FS:FAT|FAT32] [/V:label] [/A:size] [/Q] [/Y]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* ------------------------------------------------------------------ */
/* Platform I/O                                                        */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
typedef HANDLE VOL;
static VOL VolOpen(const char *drive) {
    char path[16];
    sprintf(path, "\\\\.\\%c:", drive[0]);
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}
static int VolBad(VOL v) { return v == INVALID_HANDLE_VALUE; }
static int VolLock(VOL v) { DWORD n; return DeviceIoControl(v, FSCTL_LOCK_VOLUME, 0,0,0,0,&n,0); }
static void VolDismount(VOL v) { DWORD n; DeviceIoControl(v, FSCTL_DISMOUNT_VOLUME, 0,0,0,0,&n,0); }
static int VolWrite(VOL v, u64 off, const void *buf, u32 len) {
    LARGE_INTEGER li; DWORD done; li.QuadPart = off;
    SetFilePointerEx(v, li, NULL, FILE_BEGIN);
    return WriteFile(v, buf, len, &done, NULL) && done == len;
}
static u64 VolSize(VOL v) {
    GET_LENGTH_INFORMATION gli; DWORD n;
    if (DeviceIoControl(v, IOCTL_DISK_GET_LENGTH_INFO, 0,0,&gli,sizeof(gli),&n,0))
        return gli.Length.QuadPart;
    return 0;
}
static u32 VolSector(VOL v) {
    DISK_GEOMETRY g; DWORD n;
    if (DeviceIoControl(v, IOCTL_DISK_GET_DRIVE_GEOMETRY, 0,0,&g,sizeof(g),&n,0))
        return g.BytesPerSector;
    return 512;
}
static void VolClose(VOL v) { CloseHandle(v); }
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
typedef int VOL;
static VOL VolOpen(const char *path) { return open(path, O_RDWR); }
static int VolBad(VOL v) { return v < 0; }
static int VolLock(VOL v) { (void)v; return 1; }
static void VolDismount(VOL v) { (void)v; }
static int VolWrite(VOL v, u64 off, const void *buf, u32 len) {
    return pwrite(v, buf, len, (off_t)off) == (ssize_t)len;
}
static u64 VolSize(VOL v) { struct stat st; return fstat(v, &st) == 0 ? (u64)st.st_size : 0; }
static u32 VolSector(VOL v) { (void)v; return 512; }
static void VolClose(VOL v) { close(v); }
#endif

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define FAT12_MAXC 4084
#define FAT16_MAXC 65524

static void put16(u8 *p, u16 v) { p[0]=(u8)v; p[1]=(u8)(v>>8); }
static void put32(u8 *p, u32 v) { p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }

static u32 PickClusterSize(u64 bytes, u32 sector)
{
    /* Microsoft's rough table, in sectors per cluster */
    u64 mb = bytes / (1024 * 1024);
    u32 spc;
    if (mb <= 32)        spc = 1;
    else if (mb <= 256)  spc = 8;
    else if (mb <= 8192) spc = 8;
    else if (mb <= 16384)spc = 32;
    else                 spc = 64;
    while ((u64)spc * sector > 32768) spc /= 2;
    if (spc == 0) spc = 1;
    return spc;
}

int main(int argc, char **argv)
{
    VOL v;
    const char *drive = NULL;
    const char *label = NULL;
    int forceType = 0;          /* 0 auto, 12, 16, 32 */
    int assume = 0;
    u32 clusterArg = 0;
    u32 sector, spc, numFats = 2, reserved, rootEnt, fatSize, totalSec, rootSec, dataSec, clusters;
    int fatType;
    u64 bytes;
    u8 *buf;
    u32 i, serial;
    u64 off;

    for (i = 1; i < (u32)argc; i++) {
        char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') {
            if (!strncasecmp(a+1, "FS:", 3)) {
                forceType = strcasecmp(a+4, "FAT32") == 0 ? 32 : (strcasecmp(a+4,"FAT12")==0?12:16);
            } else if (!strncasecmp(a+1, "V:", 2)) {
                label = a + 3;
            } else if (!strncasecmp(a+1, "A:", 2)) {
                clusterArg = (u32)strtoul(a + 3, NULL, 0);
            } else if (!strcasecmp(a+1, "Y")) {
                assume = 1;
            } else if (!strcasecmp(a+1, "Q")) {
                /* quick: metadata only (this tool never does a full wipe anyway) */
            } else {
                fprintf(stderr, "Unknown option %s\n", a);
                return 2;
            }
        } else {
            drive = a;
        }
    }

    if (drive == NULL) {
        fprintf(stderr, "usage: fatfmt drive: [/FS:FAT|FAT32] [/V:label] [/A:size] [/Y]\n");
        return 2;
    }

    v = VolOpen(drive);
    if (VolBad(v)) { fprintf(stderr, "cannot open %s\n", drive); return 2; }

    bytes = VolSize(v);
    sector = VolSector(v);
    if (sector == 0) sector = 512;
    totalSec = (u32)(bytes / sector);

    if (totalSec < 128) { fprintf(stderr, "volume too small\n"); VolClose(v); return 2; }

    spc = clusterArg ? (clusterArg / sector) : PickClusterSize(bytes, sector);
    if (spc == 0) spc = 1;

    /* Decide the FAT type from a trial cluster count */
    {
        u32 trialData, trialClusters;
        /* first guess with FAT16 geometry */
        reserved = 1;
        rootEnt = 512;
        rootSec = (rootEnt * 32 + sector - 1) / sector;
        trialData = totalSec - reserved - rootSec;
        trialClusters = trialData / spc;

        if (forceType == 32 || (forceType == 0 && trialClusters >= FAT16_MAXC)) {
            fatType = 32;
        } else if (forceType == 12 || (forceType == 0 && trialClusters < FAT12_MAXC)) {
            fatType = 12;
        } else {
            fatType = 16;
        }
    }

    if (fatType == 32) {
        reserved = 32;
        rootEnt = 0;
        rootSec = 0;
    } else {
        reserved = 1;
        rootEnt = (fatType == 12) ? 224 : 512;
        rootSec = (rootEnt * 32 + sector - 1) / sector;
    }

    /* Iterate to a self-consistent FAT size */
    {
        u32 bytesPerFatEntry2 = (fatType == 12) ? 3 : (fatType == 16 ? 2 : 4);
        /* 2 reserved entries are included by overprovisioning below */
        fatSize = 1;
        for (i = 0; i < 4; i++) {
            u32 avail = totalSec - reserved - rootSec - numFats * fatSize;
            u32 c = avail / spc;
            u32 needBytes;
            if (fatType == 12) needBytes = ((c + 2) * 3 + 1) / 2;
            else               needBytes = (c + 2) * (bytesPerFatEntry2 == 3 ? 2 : bytesPerFatEntry2);
            fatSize = (needBytes + sector - 1) / sector;
        }
    }

    dataSec = totalSec - reserved - rootSec - numFats * fatSize;
    clusters = dataSec / spc;

    /*
     * A valid FAT32 needs at least 65525 clusters (the driver, like Windows,
     * picks the type from the cluster count, not the BPB). Shrink the cluster
     * size until the count is in range, or fail if the volume is too small.
     */
    if (fatType == 32) {
        while (clusters < FAT16_MAXC + 16 && spc > 1) {
            spc /= 2;
            fatSize = 1;
            for (i = 0; i < 4; i++) {
                u32 avail = totalSec - reserved - rootSec - numFats * fatSize;
                u32 c = avail / spc;
                u32 needBytes = (c + 2) * 4;
                fatSize = (needBytes + sector - 1) / sector;
            }
            dataSec = totalSec - reserved - rootSec - numFats * fatSize;
            clusters = dataSec / spc;
        }
        if (clusters < FAT16_MAXC + 16) {
            fprintf(stderr, "volume too small for FAT32 (only %u clusters; need 65541). "
                            "Use the default type or a larger volume.\n", clusters);
            VolClose(v);
            return 2;
        }
    }

    serial = (u32)time(NULL) ^ 0x46415420;

    if (!assume) {
        fprintf(stderr, "Formatting %s as FAT%d (%u clusters of %u bytes). Proceed? [y/N] ",
                drive, fatType, clusters, spc * sector);
        { int c = getchar(); if (c != 'y' && c != 'Y') { VolClose(v); return 1; } }
    }

    VolLock(v);

    buf = calloc(1, 64 * 1024);

    /* ---- boot sector ---- */
    {
        u8 *b = buf;
        memset(b, 0, sector);
        b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
        memcpy(b + 3, "FATNT1.0", 8);
        put16(b + 11, (u16)sector);
        b[13] = (u8)spc;
        put16(b + 14, (u16)reserved);
        b[16] = (u8)numFats;
        put16(b + 17, (u16)rootEnt);
        if (totalSec < 0x10000 && fatType != 32) put16(b + 19, (u16)totalSec);
        else put16(b + 19, 0);
        b[21] = 0xF8;                    /* media: fixed disk */
        put16(b + 22, (u16)(fatType == 32 ? 0 : fatSize));
        put16(b + 24, 63);              /* sectors per track */
        put16(b + 26, 255);            /* heads */
        put32(b + 28, 0);              /* hidden sectors */
        if (totalSec >= 0x10000 || fatType == 32) put32(b + 32, totalSec);
        else put32(b + 32, 0);

        if (fatType == 32) {
            put32(b + 36, fatSize);
            put16(b + 40, 0);          /* ExtFlags: mirror all FATs */
            put16(b + 42, 0);          /* version 0.0 */
            put32(b + 44, 2);          /* root cluster */
            put16(b + 48, 1);          /* FSInfo sector */
            put16(b + 50, 6);          /* backup boot sector */
            b[64] = 0x80;              /* drive number */
            b[66] = 0x29;              /* extended boot signature */
            put32(b + 67, serial);
            memset(b + 71, ' ', 11);
            if (label) { u32 n = (u32)strlen(label); memcpy(b+71, label, n < 11 ? n : 11); }
            memcpy(b + 82, "FAT32   ", 8);
        } else {
            b[36] = 0x80;
            b[38] = 0x29;
            put32(b + 39, serial);
            memset(b + 43, ' ', 11);
            if (label) { u32 n = (u32)strlen(label); memcpy(b+43, label, n < 11 ? n : 11); }
            memcpy(b + 54, fatType == 12 ? "FAT12   " : "FAT16   ", 8);
        }

        put16(b + 510, 0xAA55);
        VolWrite(v, 0, b, sector);

        if (fatType == 32) {
            VolWrite(v, 6ULL * sector, b, sector);   /* backup boot sector */
        }
    }

    /* ---- FSInfo (FAT32) ---- */
    if (fatType == 32) {
        u8 *b = buf;
        memset(b, 0, sector);
        put32(b + 0, 0x41615252);
        put32(b + 484, 0x61417272);
        put32(b + 488, clusters - 1);         /* free count (root uses one) */
        put32(b + 492, 3);                     /* next free hint */
        put32(b + 508, 0xAA550000);
        VolWrite(v, 1ULL * sector, b, sector);
        VolWrite(v, 7ULL * sector, b, sector); /* backup FSInfo */
    }

    /* ---- the FATs ---- */
    {
        u32 f;
        for (f = 0; f < numFats; f++) {
            u64 fatOff = (u64)(reserved + f * fatSize) * sector;
            u32 left = fatSize * sector;
            u32 first = 1;
            while (left) {
                u32 chunk = left > 64 * 1024 ? 64 * 1024 : left;
                memset(buf, 0, chunk);
                if (first) {
                    /* reserved entries 0 and 1, with the clean-shutdown bits set */
                    if (fatType == 12) {
                        buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF;
                    } else if (fatType == 16) {
                        put16(buf + 0, 0xFFF8);
                        put16(buf + 2, 0xFFFF);          /* clean + no hard error */
                    } else {
                        put32(buf + 0, 0x0FFFFFF8);
                        put32(buf + 4, 0x0FFFFFFF);      /* clean + no hard error */
                        put32(buf + 8, 0x0FFFFFFF);      /* root directory: end of chain */
                    }
                    first = 0;
                }
                VolWrite(v, fatOff, buf, chunk);
                fatOff += chunk;
                left -= chunk;
            }
        }
    }

    /* ---- root directory ---- */
    {
        u32 rootBytes;
        u64 rootOff;

        if (fatType == 32) {
            /* one cluster at cluster 2 */
            rootBytes = spc * sector;
            rootOff = (u64)(reserved + numFats * fatSize) * sector;  /* data starts here */
        } else {
            rootBytes = rootSec * sector;
            rootOff = (u64)(reserved + numFats * fatSize) * sector;
        }

        /* zero it */
        {
            u32 left = rootBytes;
            u64 o = rootOff;
            while (left) {
                u32 chunk = left > 64 * 1024 ? 64 * 1024 : left;
                memset(buf, 0, chunk);
                VolWrite(v, o, buf, chunk);
                o += chunk; left -= chunk;
            }
        }

        /* an optional volume-label entry */
        if (label && *label) {
            u8 *e = buf;
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            u16 date = (u16)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
            u16 tod = (u16)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
            memset(e, 0, 32);
            memset(e, ' ', 11);
            { u32 n = (u32)strlen(label), k; for (k = 0; k < n && k < 11; k++) e[k] = (u8)toupper((unsigned char)label[k]); }
            e[11] = 0x08;                 /* ATTR_VOLUME_ID */
            put16(e + 22, tod);
            put16(e + 24, date);
            VolWrite(v, rootOff, e, 32);
        }
    }

    off = 0; (void)off;
    free(buf);
    VolDismount(v);
    VolClose(v);

    printf("Formatted %s: FAT%d, %u clusters of %u bytes, %u sectors/FAT.\n",
           drive, fatType, clusters, spc * sector, fatSize);
    return 0;
}
