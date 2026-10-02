/*
 * FATNT - FAT12/16/32 on-disk format
 *
 * Needs only the basic NT types (UCHAR, USHORT, ULONG, ULONGLONG, WCHAR,
 * BOOLEAN), so the helpers in fatsup.c can also be built outside the
 * kernel (the format and check tools and the test harness use them).
 */

#ifndef _FATDISK_H_
#define _FATDISK_H_

/* ------------------------------------------------------------------ */
/* BIOS parameter block and boot sector                                */
/* ------------------------------------------------------------------ */

#pragma pack(1)

/*
 * The common BPB (DOS 3.31). The fields past TotalSectors32 differ
 * between FAT12/16 and FAT32, so they are two overlays below.
 */
typedef struct _FAT_BPB {
    USHORT      BytesPerSector;
    UCHAR       SectorsPerCluster;
    USHORT      ReservedSectors;
    UCHAR       NumberOfFats;
    USHORT      RootEntries;            /* 0 on FAT32 */
    USHORT      TotalSectors16;         /* 0 when TotalSectors32 is used */
    UCHAR       Media;
    USHORT      FatSize16;              /* 0 on FAT32 */
    USHORT      SectorsPerTrack;
    USHORT      Heads;
    ULONG       HiddenSectors;
    ULONG       TotalSectors32;
} FAT_BPB, *PFAT_BPB;

/* FAT12/16 extended BPB, right after FAT_BPB */
typedef struct _FAT_EBPB16 {
    UCHAR       DriveNumber;
    UCHAR       Reserved1;
    UCHAR       BootSignature;          /* 0x29 when the three below are valid */
    ULONG       VolumeSerialNumber;
    UCHAR       VolumeLabel[11];
    UCHAR       FileSystemType[8];      /* "FAT12   ", "FAT16   ", "FAT     " */
} FAT_EBPB16, *PFAT_EBPB16;

/* FAT32 BPB extension, right after FAT_BPB */
typedef struct _FAT_EBPB32 {
    ULONG       FatSize32;              /* sectors per FAT */
    USHORT      ExtFlags;
    USHORT      FsVersion;
    ULONG       RootCluster;
    USHORT      FsInfoSector;
    USHORT      BackupBootSector;
    UCHAR       Reserved[12];
    UCHAR       DriveNumber;
    UCHAR       Reserved1;
    UCHAR       BootSignature;          /* 0x29 */
    ULONG       VolumeSerialNumber;
    UCHAR       VolumeLabel[11];
    UCHAR       FileSystemType[8];      /* "FAT32   " */
} FAT_EBPB32, *PFAT_EBPB32;

/* A complete boot sector. The extended parts land at fixed offsets. */
typedef struct _FAT_BOOT_SECTOR {
    UCHAR       JumpBoot[3];
    UCHAR       OemName[8];
    FAT_BPB     Bpb;                    /* offset 11 */
    union {
        struct {
            FAT_EBPB16 Ebpb;           /* offset 36 */
            UCHAR   BootCode[448];
        } Fat16;
        struct {
            FAT_EBPB32 Ebpb;           /* offset 36 */
            UCHAR   BootCode[420];
        } Fat32;
    } u;
    USHORT      BootSignature;          /* 0xAA55 at offset 510 */
} FAT_BOOT_SECTOR, *PFAT_BOOT_SECTOR;

#define FAT_EBPB16_OFFSET           36
#define FAT_EBPB32_OFFSET           36
#define FAT_EXT_BOOT_SIGNATURE      0x29
#define FAT_BOOT_SIGNATURE          0xAA55

/* FAT32 FSInfo sector */
typedef struct _FAT_FSINFO {
    ULONG       LeadSignature;          /* 0x41615252 */
    UCHAR       Reserved1[480];
    ULONG       StructSignature;        /* 0x61417272 */
    ULONG       FreeCount;              /* 0xFFFFFFFF when unknown */
    ULONG       NextFree;               /* hint; 0xFFFFFFFF when unknown */
    UCHAR       Reserved2[12];
    ULONG       TrailSignature;         /* 0xAA550000 */
} FAT_FSINFO, *PFAT_FSINFO;

#define FAT_FSINFO_LEAD             0x41615252UL
#define FAT_FSINFO_STRUCT           0x61417272UL
#define FAT_FSINFO_TRAIL            0xAA550000UL
#define FAT_FSINFO_UNKNOWN          0xFFFFFFFFUL

#pragma pack()

/* ------------------------------------------------------------------ */
/* FAT entries                                                         */
/* ------------------------------------------------------------------ */

/* The first two entries are reserved; cluster numbering starts at 2 */
#define FAT_FIRST_CLUSTER           2

/* End-of-chain and bad-cluster marks, per FAT width (compared masked) */
#define FAT12_BAD                   0x0FF7UL
#define FAT12_EOC                   0x0FF8UL        /* >= is end of chain */
#define FAT12_MASK                  0x0FFFUL

#define FAT16_BAD                   0xFFF7UL
#define FAT16_EOC                   0xFFF8UL
#define FAT16_MASK                  0xFFFFUL

#define FAT32_BAD                   0x0FFFFFF7UL
#define FAT32_EOC                   0x0FFFFFF8UL
#define FAT32_MASK                  0x0FFFFFFFUL

/* Count thresholds that pick the FAT type (Microsoft's definition) */
#define FAT12_MAX_CLUSTERS          4084
#define FAT16_MAX_CLUSTERS          65524

/* FAT type, stored in the VCB */
#define FAT_TYPE_12                 12
#define FAT_TYPE_16                 16
#define FAT_TYPE_32                 32

/* Clean/error bits kept in FAT[1] (dirty flag) */
#define FAT16_CLEAN_SHUTDOWN        0x8000UL
#define FAT16_HARD_ERROR            0x4000UL
#define FAT32_CLEAN_SHUTDOWN        0x08000000UL
#define FAT32_HARD_ERROR            0x04000000UL

/* ------------------------------------------------------------------ */
/* Directory entries                                                   */
/* ------------------------------------------------------------------ */

#define FAT_DIRENT_SIZE             32
#define FAT_DIRENT_SHIFT            5

/* Short (8.3) directory entry */
#pragma pack(1)
typedef struct _FAT_DIRENT {
    UCHAR       Name[11];               /* 8 + 3, space padded, no dot */
    UCHAR       Attributes;
    UCHAR       NtReserved;             /* bit 3: base lower, bit 4: ext lower */
    UCHAR       CreateTimeTenth;        /* 0..199 (10 ms) */
    USHORT      CreateTime;
    USHORT      CreateDate;
    USHORT      LastAccessDate;
    USHORT      FirstClusterHigh;       /* FAT32 only; 0 otherwise */
    USHORT      WriteTime;
    USHORT      WriteDate;
    USHORT      FirstClusterLow;
    ULONG       FileSize;
} FAT_DIRENT, *PFAT_DIRENT;

/* Long file name (VFAT) entry; shares the space of a FAT_DIRENT */
typedef struct _FAT_LFN_ENTRY {
    UCHAR       Order;                  /* sequence; 0x40 flags the last */
    WCHAR       Name1[5];
    UCHAR       Attributes;             /* always FAT_ATTR_LONG_NAME */
    UCHAR       Type;                   /* 0 */
    UCHAR       Checksum;               /* of the short name */
    WCHAR       Name2[6];
    USHORT      FirstClusterLow;        /* always 0 */
    WCHAR       Name3[2];
} FAT_LFN_ENTRY, *PFAT_LFN_ENTRY;
#pragma pack()

#define FAT_ATTR_READONLY           0x01
#define FAT_ATTR_HIDDEN             0x02
#define FAT_ATTR_SYSTEM             0x04
#define FAT_ATTR_VOLUME_ID          0x08
#define FAT_ATTR_DIRECTORY          0x10
#define FAT_ATTR_ARCHIVE            0x20
#define FAT_ATTR_LONG_NAME          (FAT_ATTR_READONLY | FAT_ATTR_HIDDEN | \
                                     FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)
#define FAT_ATTR_LONG_NAME_MASK     (FAT_ATTR_LONG_NAME | FAT_ATTR_DIRECTORY | \
                                     FAT_ATTR_ARCHIVE)
/* Attributes an application may set */
#define FAT_ATTR_SETTABLE           (FAT_ATTR_READONLY | FAT_ATTR_HIDDEN | \
                                     FAT_ATTR_SYSTEM | FAT_ATTR_ARCHIVE)

#define FAT_NT_RES_NAME_LOWER       0x08
#define FAT_NT_RES_EXT_LOWER        0x10

#define FAT_DIRENT_FREE             0xE5    /* entry is unused */
#define FAT_DIRENT_END              0x00    /* this and all after are free */
#define FAT_DIRENT_E5_SUBST         0x05    /* real first byte when it is 0xE5 */

#define FAT_LFN_LAST                0x40
#define FAT_LFN_ORDER_MASK          0x3F
#define FAT_LFN_CHARS_PER_ENTRY     13
#define FAT_LFN_MAX_ENTRIES         20          /* 20 * 13 = 260 */

#define FAT_MAX_LFN                 255
#define FAT_MAX_NAME                255
#define FAT_SFN_BASE                8
#define FAT_SFN_EXT                 3
#define FAT_SFN_LEN                 11
#define FAT_MAX_LABEL               11

/* A directory can grow to 2 MB on FAT (65536 * 32 entries) */
#define FAT_MAX_DIR_SIZE            0x200000UL

/* ------------------------------------------------------------------ */
/* Decoded directory entry (fatsup.c)                                  */
/* ------------------------------------------------------------------ */

typedef struct _FAT_DIR_INFO {
    ULONG       Offset;             /* short-entry byte offset in its directory */
    ULONG       SetOffset;          /* first LFN entry (or short entry) offset */
    ULONG       EntryCount;         /* LFN entries + 1 (the short entry) */
    UCHAR       Attributes;
    UCHAR       NtReserved;
    ULONG       FirstCluster;
    ULONG       FileSize;
    USHORT      CreateDate;
    USHORT      CreateTime;
    UCHAR       CreateTimeTenth;
    USHORT      WriteDate;
    USHORT      WriteTime;
    USHORT      LastAccessDate;
    UCHAR       ShortName[FAT_SFN_LEN]; /* raw 8.3, space padded */
    UCHAR       NameLength;             /* long name length, characters */
    BOOLEAN     HasLongName;
    WCHAR       Name[FAT_MAX_LFN + 1];  /* long name, or the 8.3 expansion */
} FAT_DIR_INFO, *PFAT_DIR_INFO;

typedef struct _FAT_TIME_PARTS {
    USHORT      Year;
    USHORT      Month;
    USHORT      Day;
    USHORT      Hour;
    USHORT      Minute;
    USHORT      Second;
    USHORT      Milliseconds;
} FAT_TIME_PARTS, *PFAT_TIME_PARTS;

/* FatCheckBootSector results */
#define FAT_BOOT_OK                 0
#define FAT_BOOT_NOT_FAT            1
#define FAT_BOOT_BAD_GEOMETRY       2
#define FAT_BOOT_BAD_LAYOUT         3

/* FatParseDirName results */
#define FAT_NAME_OK                 0       /* a real entry, decoded */
#define FAT_NAME_FREE               1       /* 0xE5: skip */
#define FAT_NAME_END                2       /* 0x00: end of directory */
#define FAT_NAME_LABEL              3       /* volume label entry */

/* ------------------------------------------------------------------ */
/* Geometry computed from the BPB (fatsup.c fills it)                  */
/* ------------------------------------------------------------------ */

typedef struct _FAT_GEOMETRY {
    UCHAR       FatType;                /* 12 / 16 / 32 */
    ULONG       BytesPerSector;
    ULONG       SectorsPerCluster;
    ULONG       ClusterSize;
    ULONG       ReservedSectors;
    ULONG       NumberOfFats;
    ULONG       FatSectors;             /* sectors per FAT */
    ULONG       RootEntries;            /* 12/16 only */
    ULONG       RootDirSectors;         /* 12/16: fixed region; 32: 0 */
    ULONG       FirstRootSector;        /* 12/16 only */
    ULONG       FirstDataSector;        /* cluster heap start */
    ULONG       TotalSectors;
    ULONG       ClusterCount;           /* data clusters */
    ULONG       RootCluster;            /* FAT32 only */
    ULONG       FsInfoSector;           /* FAT32 only; 0 if none */
    ULONG       SerialNumber;
    UCHAR       Media;
} FAT_GEOMETRY, *PFAT_GEOMETRY;

/* ------------------------------------------------------------------ */
/* Helpers in fatsup.c                                                 */
/* ------------------------------------------------------------------ */

ULONG
FatCheckBootSector (
    const UCHAR *Sector,
    ULONG DeviceSectorSize,
    ULONGLONG DeviceSectors,
    PFAT_GEOMETRY Geometry
    );

UCHAR
FatLongNameChecksum (
    const UCHAR *ShortName
    );

/*
 * Set points at a short entry preceded by its LFN entries (if any).
 * SetStart is the byte offset of the first entry examined; the caller
 * passes the entries from there. Returns FAT_NAME_*.
 */
ULONG
FatParseDirEntry (
    const UCHAR *Entries,       /* from the first LFN or the short entry */
    ULONG Available,            /* entries available from Entries on */
    ULONG BaseOffset,           /* byte offset of Entries[0] in the directory */
    PFAT_DIR_INFO Info
    );

/* Expands a raw 8.3 name into Unicode (handles the lower-case NT bits) */
ULONG
FatExpandShortName (
    const UCHAR *ShortName,
    UCHAR NtReserved,
    WCHAR *Name
    );

BOOLEAN
FatIsLegalShortChar (
    UCHAR Char
    );

BOOLEAN
FatIsLegalLongChar (
    WCHAR Char
    );

/*
 * Builds an 8.3 name from a long name. Returns TRUE and fills ShortName
 * (space padded) and *NtReserved when the long name already is a legal
 * 8.3 name (so no LFN is needed); returns FALSE when a numeric-tail name
 * must be generated (FatGenerateShortName), leaving a basis in ShortName.
 */
BOOLEAN
FatBuildShortName (
    const WCHAR *Name,
    ULONG Length,
    UCHAR *ShortName,
    UCHAR *NtReserved,
    BOOLEAN *LossyOrLong
    );

/* Writes the ~N numeric tail into a short-name basis */
VOID
FatApplyNumericTail (
    UCHAR *ShortName,
    ULONG Number
    );

/* Number of LFN entries needed for a name of Length characters (0 if none) */
ULONG
FatLongEntryCount (
    const WCHAR *Name,
    ULONG Length,
    const UCHAR *ShortName,
    UCHAR NtReserved
    );

/*
 * Lays an entry set (LFN entries + short entry) into Set and returns the
 * number of 32-byte entries written.
 */
ULONG
FatBuildEntrySet (
    const WCHAR *Name,
    ULONG Length,
    const UCHAR *ShortName,
    UCHAR NtReserved,
    const FAT_DIR_INFO *Info,
    UCHAR *Set
    );

BOOLEAN
FatNamesEqualNoCase (
    const WCHAR *Name1,
    ULONG Length1,
    const WCHAR *Name2,
    ULONG Length2
    );

/* FAT date/time (DOS) <-> parts */
BOOLEAN
FatDecodeTime (
    USHORT Date,
    USHORT Time,
    UCHAR Tenth,
    PFAT_TIME_PARTS Parts
    );

VOID
FatEncodeTime (
    const FAT_TIME_PARTS *Parts,
    PUSHORT Date,
    PUSHORT Time,
    PUCHAR Tenth
    );

WCHAR
FatUpcaseChar (
    WCHAR Char
    );

#endif /* _FATDISK_H_ */
