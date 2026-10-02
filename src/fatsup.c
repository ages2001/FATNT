/*
 * FATNT - on-disk format helpers
 *
 * BPB validation and geometry, 8.3 and long name handling, directory
 * entry decoding and encoding, DOS timestamp conversion. Nothing here
 * calls the kernel, so the format and check tools and the user-mode test
 * harness build it too.
 */

#ifdef FAT_USERMODE
#include "fatusr.h"
#include "fatdisk.h"
#else
#include "fat.h"
#endif

/* ------------------------------------------------------------------ */
/* Case folding                                                        */
/* ------------------------------------------------------------------ */

WCHAR
FatUpcaseChar (
    WCHAR Char
    )
{
    if (Char >= L'a' && Char <= L'z') {
        return (WCHAR)(Char - (L'a' - L'A'));
    }

    /* Latin-1 supplement (avoids the yuml/sharp-s special cases) */
    if (Char >= 0xE0 && Char <= 0xFE && Char != 0xF7) {
        return (WCHAR)(Char - 0x20);
    }

    return Char;
}

BOOLEAN
FatNamesEqualNoCase (
    const WCHAR *Name1,
    ULONG Length1,
    const WCHAR *Name2,
    ULONG Length2
    )
{
    ULONG i;

    if (Length1 != Length2) {
        return FALSE;
    }

    for (i = 0; i < Length1; i++) {
        if (FatUpcaseChar(Name1[i]) != FatUpcaseChar(Name2[i])) {
            return FALSE;
        }
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Boot sector and geometry                                            */
/* ------------------------------------------------------------------ */

ULONG
FatCheckBootSector (
    const UCHAR *Sector,
    ULONG DeviceSectorSize,
    ULONGLONG DeviceSectors,
    PFAT_GEOMETRY Geometry
    )
{
    const FAT_BOOT_SECTOR *Boot = (const FAT_BOOT_SECTOR *)Sector;
    const FAT_BPB *Bpb = &Boot->Bpb;
    ULONG BytesPerSector;
    ULONG SectorsPerCluster;
    ULONG ReservedSectors;
    ULONG NumFats;
    ULONG RootEntries;
    ULONG FatSectors;
    ULONG TotalSectors;
    ULONG RootDirSectors;
    ULONG DataSectors;
    ULONG ClusterCount;
    ULONG FatType;

    if (Sector[510] != 0x55 || Sector[511] != 0xAA) {
        return FAT_BOOT_NOT_FAT;
    }

    /* A jump instruction leads every real FAT boot sector */
    if (!(Sector[0] == 0xEB && Sector[2] == 0x90) && Sector[0] != 0xE9) {
        return FAT_BOOT_NOT_FAT;
    }

    BytesPerSector    = Bpb->BytesPerSector;
    SectorsPerCluster = Bpb->SectorsPerCluster;
    ReservedSectors   = Bpb->ReservedSectors;
    NumFats           = Bpb->NumberOfFats;
    RootEntries       = Bpb->RootEntries;

    if (BytesPerSector != 512 && BytesPerSector != 1024 &&
        BytesPerSector != 2048 && BytesPerSector != 4096) {

        return FAT_BOOT_BAD_GEOMETRY;
    }

    if (DeviceSectorSize != 0 && BytesPerSector != DeviceSectorSize) {
        return FAT_BOOT_BAD_GEOMETRY;
    }

    if (SectorsPerCluster == 0 || (SectorsPerCluster & (SectorsPerCluster - 1)) != 0 ||
        SectorsPerCluster > 128) {

        return FAT_BOOT_BAD_GEOMETRY;
    }

    if (NumFats == 0 || NumFats > 2 || ReservedSectors == 0) {
        return FAT_BOOT_BAD_LAYOUT;
    }

    FatSectors   = Bpb->FatSize16;
    TotalSectors = Bpb->TotalSectors16;

    if (TotalSectors == 0) {
        TotalSectors = Bpb->TotalSectors32;
    }

    if (FatSectors == 0) {
        /* FAT32: the extended BPB holds the FAT size */
        FatSectors = Boot->u.Fat32.Ebpb.FatSize32;
    }

    if (FatSectors == 0 || TotalSectors == 0) {
        return FAT_BOOT_BAD_LAYOUT;
    }

    /* The root directory occupies whole sectors on FAT12/16; none on 32 */
    RootDirSectors = ((RootEntries * FAT_DIRENT_SIZE) + (BytesPerSector - 1)) / BytesPerSector;

    if ((ULONGLONG)ReservedSectors + (ULONGLONG)NumFats * FatSectors + RootDirSectors > TotalSectors) {
        return FAT_BOOT_BAD_LAYOUT;
    }

    DataSectors  = TotalSectors - (ReservedSectors + NumFats * FatSectors + RootDirSectors);
    ClusterCount = DataSectors / SectorsPerCluster;

    if (ClusterCount < FAT12_MAX_CLUSTERS) {
        FatType = FAT_TYPE_12;
    } else if (ClusterCount < FAT16_MAX_CLUSTERS) {
        FatType = FAT_TYPE_16;
    } else {
        FatType = FAT_TYPE_32;
    }

    /* FAT12/16 must have a root region; FAT32 must not */
    if (FatType == FAT_TYPE_32) {
        if (RootEntries != 0 || Bpb->FatSize16 != 0) {
            return FAT_BOOT_BAD_LAYOUT;
        }
    } else {
        if (RootEntries == 0) {
            return FAT_BOOT_BAD_LAYOUT;
        }
    }

    if (DeviceSectors != 0 && TotalSectors > DeviceSectors) {
        return FAT_BOOT_BAD_LAYOUT;
    }

    if (Geometry != NULL) {

        Geometry->FatType           = (UCHAR)FatType;
        Geometry->BytesPerSector    = BytesPerSector;
        Geometry->SectorsPerCluster = SectorsPerCluster;
        Geometry->ClusterSize       = BytesPerSector * SectorsPerCluster;
        Geometry->ReservedSectors   = ReservedSectors;
        Geometry->NumberOfFats      = NumFats;
        Geometry->FatSectors        = FatSectors;
        Geometry->RootEntries       = RootEntries;
        Geometry->RootDirSectors    = RootDirSectors;
        Geometry->FirstRootSector   = ReservedSectors + NumFats * FatSectors;
        Geometry->FirstDataSector   = ReservedSectors + NumFats * FatSectors + RootDirSectors;
        Geometry->TotalSectors      = TotalSectors;
        Geometry->ClusterCount      = ClusterCount;
        Geometry->Media             = Bpb->Media;

        if (FatType == FAT_TYPE_32) {
            Geometry->RootCluster    = Boot->u.Fat32.Ebpb.RootCluster;
            Geometry->FsInfoSector   = Boot->u.Fat32.Ebpb.FsInfoSector;
            Geometry->SerialNumber   = Boot->u.Fat32.Ebpb.VolumeSerialNumber;

            if (Geometry->RootCluster < FAT_FIRST_CLUSTER ||
                Geometry->RootCluster >= ClusterCount + FAT_FIRST_CLUSTER) {

                return FAT_BOOT_BAD_LAYOUT;
            }

        } else {
            Geometry->RootCluster    = 0;
            Geometry->FsInfoSector   = 0;
            Geometry->SerialNumber   = Boot->u.Fat16.Ebpb.VolumeSerialNumber;
        }
    }

    return FAT_BOOT_OK;
}

/* ------------------------------------------------------------------ */
/* Timestamps                                                          */
/* ------------------------------------------------------------------ */

BOOLEAN
FatDecodeTime (
    USHORT Date,
    USHORT Time,
    UCHAR Tenth,
    PFAT_TIME_PARTS Parts
    )
{
    Parts->Year   = (USHORT)(1980 + ((Date >> 9) & 0x7F));
    Parts->Month  = (USHORT)((Date >> 5) & 0x0F);
    Parts->Day    = (USHORT)(Date & 0x1F);
    Parts->Hour   = (USHORT)((Time >> 11) & 0x1F);
    Parts->Minute = (USHORT)((Time >> 5) & 0x3F);
    Parts->Second = (USHORT)((Time & 0x1F) * 2);
    Parts->Milliseconds = 0;

    if (Tenth <= 199) {
        Parts->Second = (USHORT)(Parts->Second + Tenth / 100);
        Parts->Milliseconds = (USHORT)((Tenth % 100) * 10);
    }

    if (Parts->Month < 1 || Parts->Month > 12 || Parts->Day < 1 || Parts->Day > 31 ||
        Parts->Hour > 23 || Parts->Minute > 59 || Parts->Second > 59) {

        return FALSE;
    }

    return TRUE;
}

VOID
FatEncodeTime (
    const FAT_TIME_PARTS *Parts,
    PUSHORT Date,
    PUSHORT Time,
    PUCHAR Tenth
    )
{
    USHORT Year = Parts->Year;

    if (Year < 1980) {
        Year = 1980;
        *Date = (USHORT)((1 << 5) | 1);         /* 1980-01-01 */
        *Time = 0;
        if (Tenth != NULL) {
            *Tenth = 0;
        }
        return;
    }

    if (Year > 2107) {
        Year = 2107;
    }

    *Date = (USHORT)(((Year - 1980) << 9) | ((ULONG)Parts->Month << 5) | Parts->Day);
    *Time = (USHORT)(((ULONG)Parts->Hour << 11) | ((ULONG)Parts->Minute << 5) |
                     (Parts->Second / 2));

    if (Tenth != NULL) {
        *Tenth = (UCHAR)((Parts->Second & 1) * 100 + Parts->Milliseconds / 10);
    }
}

/* ------------------------------------------------------------------ */
/* Short (8.3) names                                                   */
/* ------------------------------------------------------------------ */

BOOLEAN
FatIsLegalShortChar (
    UCHAR Char
    )
{
    if (Char < 0x20) {
        return FALSE;
    }

    switch (Char) {
    case '"': case '*': case '+': case ',': case '.': case '/':
    case ':': case ';': case '<': case '=': case '>': case '?':
    case '[': case '\\': case ']': case '|': case ' ':
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
FatIsLegalLongChar (
    WCHAR Char
    )
{
    if (Char < 0x20) {
        return FALSE;
    }

    switch (Char) {
    case L'"': case L'*': case L'/': case L':': case L'<':
    case L'>': case L'?': case L'\\': case L'|':
        return FALSE;
    }

    return TRUE;
}

UCHAR
FatLongNameChecksum (
    const UCHAR *ShortName
    )
{
    UCHAR Sum = 0;
    ULONG i;

    for (i = 0; i < FAT_SFN_LEN; i++) {
        Sum = (UCHAR)(((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + ShortName[i]);
    }

    return Sum;
}

ULONG
FatExpandShortName (
    const UCHAR *ShortName,
    UCHAR NtReserved,
    WCHAR *Name
    )
{
    ULONG Length = 0;
    ULONG i;
    ULONG BaseLen = FAT_SFN_BASE;
    ULONG ExtLen = FAT_SFN_EXT;
    UCHAR Ch;
    BOOLEAN BaseLower = (BOOLEAN)((NtReserved & FAT_NT_RES_NAME_LOWER) != 0);
    BOOLEAN ExtLower = (BOOLEAN)((NtReserved & FAT_NT_RES_EXT_LOWER) != 0);

    while (BaseLen > 0 && ShortName[BaseLen - 1] == ' ') {
        BaseLen--;
    }

    while (ExtLen > 0 && ShortName[FAT_SFN_BASE + ExtLen - 1] == ' ') {
        ExtLen--;
    }

    for (i = 0; i < BaseLen; i++) {
        Ch = ShortName[i];
        if (i == 0 && Ch == FAT_DIRENT_E5_SUBST) {
            Ch = FAT_DIRENT_FREE;
        }
        if (BaseLower && Ch >= 'A' && Ch <= 'Z') {
            Ch = (UCHAR)(Ch + ('a' - 'A'));
        }
        Name[Length++] = (WCHAR)Ch;
    }

    if (ExtLen > 0) {
        Name[Length++] = L'.';
        for (i = 0; i < ExtLen; i++) {
            Ch = ShortName[FAT_SFN_BASE + i];
            if (ExtLower && Ch >= 'A' && Ch <= 'Z') {
                Ch = (UCHAR)(Ch + ('a' - 'A'));
            }
            Name[Length++] = (WCHAR)Ch;
        }
    }

    Name[Length] = 0;
    return Length;
}

/*
 * TRUE when Name is already a legal 8.3 name: fills ShortName (space
 * padded, upper case) and the NtReserved case bits, and sets *LossyOrLong
 * FALSE. When Name needs a long entry, returns FALSE, sets *LossyOrLong
 * TRUE and leaves an upper-case basis (illegal characters turned into
 * '_') in ShortName for a numeric tail.
 */
BOOLEAN
FatBuildShortName (
    const WCHAR *Name,
    ULONG Length,
    UCHAR *ShortName,
    UCHAR *NtReserved,
    BOOLEAN *LossyOrLong
    )
{
    ULONG Dot;
    ULONG i;
    ULONG BaseLen;
    ULONG ExtLen;
    ULONG Pos;
    WCHAR Ch;
    BOOLEAN Lossy = FALSE;
    BOOLEAN BaseHasUpper = FALSE, BaseHasLower = FALSE;
    BOOLEAN ExtHasUpper = FALSE, ExtHasLower = FALSE;

    for (i = 0; i < FAT_SFN_LEN; i++) {
        ShortName[i] = ' ';
    }
    *NtReserved = 0;
    *LossyOrLong = FALSE;

    /* Trailing dots and spaces never belong to a short name */
    while (Length > 0 && (Name[Length - 1] == L'.' || Name[Length - 1] == L' ')) {
        Length--;
        Lossy = TRUE;
    }

    if (Length == 0) {
        *LossyOrLong = TRUE;
        return FALSE;
    }

    /* Last dot splits base and extension */
    Dot = Length;
    for (i = 0; i < Length; i++) {
        if (Name[i] == L'.') {
            Dot = i;
        }
    }

    /* A leading-dot name or more than one dot cannot be pure 8.3 */
    for (i = 0; i < Dot; i++) {
        if (Name[i] == L'.') {
            Lossy = TRUE;
        }
    }

    BaseLen = (Dot < Length) ? Dot : Length;
    ExtLen  = (Dot < Length) ? (Length - Dot - 1) : 0;

    if (BaseLen == 0) {
        Lossy = TRUE;
    }

    /* Base */
    Pos = 0;
    for (i = 0; i < BaseLen; i++) {
        Ch = Name[i];

        if (Pos >= FAT_SFN_BASE) {
            Lossy = TRUE;
            break;
        }

        if (Ch == L' ') {
            Lossy = TRUE;
            continue;
        }

        if (Ch >= L'a' && Ch <= L'z') {
            BaseHasLower = TRUE;
            ShortName[Pos++] = (UCHAR)(Ch - (L'a' - L'A'));
        } else if (Ch < 0x80 && FatIsLegalShortChar((UCHAR)Ch)) {
            if (Ch >= L'A' && Ch <= L'Z') {
                BaseHasUpper = TRUE;
            }
            ShortName[Pos++] = (UCHAR)Ch;
        } else {
            Lossy = TRUE;
            ShortName[Pos++] = '_';
        }
    }

    if (BaseLen > FAT_SFN_BASE) {
        Lossy = TRUE;
    }

    /* Extension */
    Pos = FAT_SFN_BASE;
    for (i = 0; i < ExtLen; i++) {
        Ch = Name[Dot + 1 + i];

        if (Pos >= FAT_SFN_LEN) {
            Lossy = TRUE;
            break;
        }

        if (Ch == L' ') {
            Lossy = TRUE;
            continue;
        }

        if (Ch >= L'a' && Ch <= L'z') {
            ExtHasLower = TRUE;
            ShortName[Pos++] = (UCHAR)(Ch - (L'a' - L'A'));
        } else if (Ch < 0x80 && FatIsLegalShortChar((UCHAR)Ch)) {
            if (Ch >= L'A' && Ch <= L'Z') {
                ExtHasUpper = TRUE;
            }
            ShortName[Pos++] = (UCHAR)Ch;
        } else {
            Lossy = TRUE;
            ShortName[Pos++] = '_';
        }
    }

    if (ExtLen > FAT_SFN_EXT) {
        Lossy = TRUE;
    }

    /* 0xE5 as the first byte is stored as 0x05 */
    if (ShortName[0] == FAT_DIRENT_FREE) {
        ShortName[0] = FAT_DIRENT_E5_SUBST;
    }

    if (Lossy) {
        *LossyOrLong = TRUE;
        return FALSE;
    }

    /* A mixed-case base or extension cannot be stored with the case bits */
    if (BaseHasUpper && BaseHasLower) {
        *LossyOrLong = TRUE;
        return FALSE;
    }
    if (ExtHasUpper && ExtHasLower) {
        *LossyOrLong = TRUE;
        return FALSE;
    }

    if (BaseHasLower) {
        *NtReserved |= FAT_NT_RES_NAME_LOWER;
    }
    if (ExtHasLower) {
        *NtReserved |= FAT_NT_RES_EXT_LOWER;
    }

    return TRUE;
}

VOID
FatApplyNumericTail (
    UCHAR *ShortName,
    ULONG Number
    )
{
    UCHAR Tail[8];
    ULONG TailLen = 0;
    ULONG BaseLen;
    ULONG Value = Number;
    ULONG i;

    /* Decimal digits of ~Number, most significant first */
    do {
        Tail[TailLen++] = (UCHAR)('0' + Value % 10);
        Value /= 10;
    } while (Value != 0 && TailLen < sizeof(Tail) - 1);

    /* The basis keeps at most 8 - (1 + digits) characters */
    BaseLen = FAT_SFN_BASE;
    while (BaseLen > 0 && ShortName[BaseLen - 1] == ' ') {
        BaseLen--;
    }

    if (BaseLen > FAT_SFN_BASE - (TailLen + 1)) {
        BaseLen = FAT_SFN_BASE - (TailLen + 1);
    }

    ShortName[BaseLen] = '~';

    for (i = 0; i < TailLen; i++) {
        ShortName[BaseLen + 1 + i] = Tail[TailLen - 1 - i];
    }
}

/* ------------------------------------------------------------------ */
/* Long name entry sets                                                */
/* ------------------------------------------------------------------ */

/* TRUE when ShortName (with NtReserved case) expands to exactly Name */
static BOOLEAN
FatShortNameMatches (
    const WCHAR *Name,
    ULONG Length,
    const UCHAR *ShortName,
    UCHAR NtReserved
    )
{
    WCHAR Expanded[13];
    ULONG ExpLen;
    ULONG i;

    ExpLen = FatExpandShortName(ShortName, NtReserved, Expanded);

    if (ExpLen != Length) {
        return FALSE;
    }

    for (i = 0; i < Length; i++) {
        if (Expanded[i] != Name[i]) {
            return FALSE;
        }
    }

    return TRUE;
}

ULONG
FatLongEntryCount (
    const WCHAR *Name,
    ULONG Length,
    const UCHAR *ShortName,
    UCHAR NtReserved
    )
{
    if (FatShortNameMatches(Name, Length, ShortName, NtReserved)) {
        return 0;
    }

    return (Length + FAT_LFN_CHARS_PER_ENTRY - 1) / FAT_LFN_CHARS_PER_ENTRY;
}

static VOID
FatPutLfnChar (
    PFAT_LFN_ENTRY Lfn,
    ULONG Index,
    WCHAR Value
    )
{
    if (Index < 5) {
        Lfn->Name1[Index] = Value;
    } else if (Index < 11) {
        Lfn->Name2[Index - 5] = Value;
    } else {
        Lfn->Name3[Index - 11] = Value;
    }
}

static WCHAR
FatGetLfnChar (
    const FAT_LFN_ENTRY *Lfn,
    ULONG Index
    )
{
    if (Index < 5) {
        return Lfn->Name1[Index];
    } else if (Index < 11) {
        return Lfn->Name2[Index - 5];
    } else {
        return Lfn->Name3[Index - 11];
    }
}

static VOID
FatFillShortEntry (
    PFAT_DIRENT Dir,
    const UCHAR *ShortName,
    UCHAR NtReserved,
    const FAT_DIR_INFO *Info
    )
{
    ULONG i;

    for (i = 0; i < FAT_SFN_LEN; i++) {
        Dir->Name[i] = ShortName[i];
    }

    Dir->Attributes      = Info->Attributes;
    Dir->NtReserved      = NtReserved;
    Dir->CreateTimeTenth = Info->CreateTimeTenth;
    Dir->CreateTime      = Info->CreateTime;
    Dir->CreateDate      = Info->CreateDate;
    Dir->LastAccessDate  = Info->LastAccessDate;
    Dir->FirstClusterHigh= (USHORT)(Info->FirstCluster >> 16);
    Dir->WriteTime       = Info->WriteTime;
    Dir->WriteDate       = Info->WriteDate;
    Dir->FirstClusterLow = (USHORT)(Info->FirstCluster & 0xFFFF);
    Dir->FileSize        = Info->FileSize;
}

ULONG
FatBuildEntrySet (
    const WCHAR *Name,
    ULONG Length,
    const UCHAR *ShortName,
    UCHAR NtReserved,
    const FAT_DIR_INFO *Info,
    UCHAR *Set
    )
{
    ULONG LfnCount = FatLongEntryCount(Name, Length, ShortName, NtReserved);
    UCHAR Checksum = FatLongNameChecksum(ShortName);
    PFAT_LFN_ENTRY Lfn;
    ULONG Seq;
    ULONG Base;
    ULONG i;
    WCHAR Ch;

    /* LFN entries go down from the highest sequence to 1 */
    for (Seq = LfnCount; Seq >= 1; Seq--) {

        Lfn = (PFAT_LFN_ENTRY)(Set + (LfnCount - Seq) * FAT_DIRENT_SIZE);

        for (i = 0; i < FAT_DIRENT_SIZE; i++) {
            ((UCHAR *)Lfn)[i] = 0;
        }

        Lfn->Order = (UCHAR)(Seq | ((Seq == LfnCount) ? FAT_LFN_LAST : 0));
        Lfn->Attributes = FAT_ATTR_LONG_NAME;
        Lfn->Type = 0;
        Lfn->Checksum = Checksum;
        Lfn->FirstClusterLow = 0;

        Base = (Seq - 1) * FAT_LFN_CHARS_PER_ENTRY;

        for (i = 0; i < FAT_LFN_CHARS_PER_ENTRY; i++) {

            if (Base + i < Length) {
                Ch = Name[Base + i];
            } else if (Base + i == Length) {
                Ch = 0;                 /* NUL terminator */
            } else {
                Ch = 0xFFFF;            /* padding */
            }

            FatPutLfnChar(Lfn, i, Ch);
        }
    }

    FatFillShortEntry((PFAT_DIRENT)(Set + LfnCount * FAT_DIRENT_SIZE),
                      ShortName, NtReserved, Info);

    return LfnCount + 1;
}

/* ------------------------------------------------------------------ */
/* Decoding a directory entry set                                      */
/* ------------------------------------------------------------------ */

ULONG
FatParseDirEntry (
    const UCHAR *Entries,
    ULONG Available,
    ULONG BaseOffset,
    PFAT_DIR_INFO Info
    )
{
    const FAT_DIRENT *Dir;
    const FAT_LFN_ENTRY *Lfn;
    ULONG Consumed = 0;
    ULONG LfnCount = 0;
    ULONG MaxOrder = 0;
    UCHAR Checksum = 0;
    WCHAR Assembled[FAT_LFN_MAX_ENTRIES * FAT_LFN_CHARS_PER_ENTRY + 1];
    BOOLEAN LfnValid = FALSE;
    BOOLEAN ExpectLast = TRUE;
    ULONG LastOrder = 0;
    ULONG i;
    ULONG Base;
    ULONG NameLen;

    if (Available == 0) {
        return FAT_NAME_END;
    }

    if (Entries[0] == FAT_DIRENT_END) {
        return FAT_NAME_END;
    }

    if (Entries[0] == FAT_DIRENT_FREE) {
        Info->EntryCount = 1;
        return FAT_NAME_FREE;
    }

    /* Collect any preceding long-name entries */
    while (Consumed < Available) {

        Lfn = (const FAT_LFN_ENTRY *)(Entries + Consumed * FAT_DIRENT_SIZE);

        if (Lfn->Attributes != FAT_ATTR_LONG_NAME ||
            (Lfn->Order & FAT_LFN_ORDER_MASK) == 0 ||
            Lfn->Order == FAT_DIRENT_FREE) {

            break;      /* not a long-name entry: the short entry follows */
        }

        {
            ULONG Order = Lfn->Order & FAT_LFN_ORDER_MASK;

            if (Order > FAT_LFN_MAX_ENTRIES) {
                LfnValid = FALSE;
            } else if (ExpectLast) {

                if (!(Lfn->Order & FAT_LFN_LAST)) {
                    LfnValid = FALSE;
                } else {
                    LfnValid = TRUE;
                    MaxOrder = Order;
                    LastOrder = Order;
                    Checksum = Lfn->Checksum;
                    ExpectLast = FALSE;
                }

            } else if (Order != LastOrder - 1 || Lfn->Checksum != Checksum) {
                LfnValid = FALSE;
            } else {
                LastOrder = Order;
            }

            if (LfnValid && Order >= 1 && Order <= FAT_LFN_MAX_ENTRIES) {
                Base = (Order - 1) * FAT_LFN_CHARS_PER_ENTRY;
                for (i = 0; i < FAT_LFN_CHARS_PER_ENTRY; i++) {
                    Assembled[Base + i] = FatGetLfnChar(Lfn, i);
                }
                LfnCount++;
            }
        }

        Consumed++;
    }

    if (Consumed >= Available) {
        /* The set ran off the end without a short entry */
        Info->EntryCount = Consumed ? Consumed : 1;
        return FAT_NAME_FREE;
    }

    Dir = (const FAT_DIRENT *)(Entries + Consumed * FAT_DIRENT_SIZE);

    if (Dir->Name[0] == FAT_DIRENT_END) {
        return FAT_NAME_END;
    }

    if (Dir->Name[0] == FAT_DIRENT_FREE) {
        Info->EntryCount = Consumed + 1;
        return FAT_NAME_FREE;
    }

    /* A long-name set is only valid if it is complete and the checksum
       matches this short entry; otherwise fall back to the 8.3 name */
    if (LfnValid && LfnCount == MaxOrder && LastOrder == 1 && MaxOrder > 0 &&
        FatLongNameChecksum(Dir->Name) == Checksum) {

        Base = MaxOrder * FAT_LFN_CHARS_PER_ENTRY;
        NameLen = 0;
        for (i = 0; i < Base && i < FAT_MAX_LFN; i++) {
            if (Assembled[i] == 0) {
                break;
            }
            Info->Name[NameLen++] = Assembled[i];
        }
        Info->Name[NameLen] = 0;
        Info->NameLength = (UCHAR)NameLen;
        Info->HasLongName = TRUE;

    } else {

        NameLen = FatExpandShortName(Dir->Name, Dir->NtReserved, Info->Name);
        Info->NameLength = (UCHAR)NameLen;
        Info->HasLongName = FALSE;
        Consumed = Consumed;    /* LFN (if any) is stale; still skip it below */
    }

    for (i = 0; i < FAT_SFN_LEN; i++) {
        Info->ShortName[i] = Dir->Name[i];
    }

    Info->Attributes     = Dir->Attributes;
    Info->NtReserved     = Dir->NtReserved;
    Info->FirstCluster   = ((ULONG)Dir->FirstClusterHigh << 16) | Dir->FirstClusterLow;
    Info->FileSize       = Dir->FileSize;
    Info->CreateDate     = Dir->CreateDate;
    Info->CreateTime     = Dir->CreateTime;
    Info->CreateTimeTenth= Dir->CreateTimeTenth;
    Info->WriteDate      = Dir->WriteDate;
    Info->WriteTime      = Dir->WriteTime;
    Info->LastAccessDate = Dir->LastAccessDate;

    Info->SetOffset   = BaseOffset;
    Info->Offset      = BaseOffset + Consumed * FAT_DIRENT_SIZE;
    Info->EntryCount  = Consumed + 1;

    /* The volume label lives in the root as a special entry */
    if ((Dir->Attributes & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_VOLUME_ID) {
        return FAT_NAME_LABEL;
    }

    return FAT_NAME_OK;
}
