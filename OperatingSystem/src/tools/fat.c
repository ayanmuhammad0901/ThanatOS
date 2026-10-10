#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct
{
    uint8_t  BootJumpInstruction[3];
    uint8_t  OemIdentifier[8];
    uint16_t BytesPerSector;        // FIX: was uint8_t
    uint8_t  SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t  FatCount;
    uint16_t DirEntriesCount;
    uint16_t TotalSectors;
    uint8_t  MediaDescriptorType;
    uint16_t SectorsPerFAT;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t LargeSectorCount;

    // Extended boot record
    uint8_t  DriveNumber;
    uint8_t  _Reserved;
    uint8_t  Signature;
    uint32_t VolumeId;              // FIX: was uint8_t (it is 4 bytes)
    uint8_t  VolumeLabel[11];
    uint8_t  SystemId[8];

} __attribute__((packed)) BootSector;

typedef struct
{
    uint8_t  Name[11];
    uint8_t  Attributes;
    uint8_t  _Reserved;
    uint8_t  CreatedTimeTenths;
    uint16_t CreatedTime;
    uint16_t CreatedDate;
    uint16_t AccessedDate;
    uint16_t FirstClusterHigh;
    uint16_t ModifiedTime;
    uint16_t ModifiedDate;
    uint16_t FirstClusterLow;
    uint32_t Size;

} __attribute__((packed)) DirectoryEntry;

BootSector g_BootSector;
uint8_t* g_Fat = NULL;
DirectoryEntry* g_RootDirectory = NULL;
uint32_t g_RootDirectoryEnd;

bool readBootSector(FILE* disk)
{
    return fread(&g_BootSector, sizeof(g_BootSector), 1, disk) > 0;
}

bool readSectors(FILE* disk, uint32_t lba, uint32_t count, void* bufferOut)
{
    bool ok = true;
    // FIX: 64-bit offset so large images don't overflow 32-bit math
    ok = ok && (fseek(disk, (long)((uint64_t)lba * g_BootSector.BytesPerSector), SEEK_SET) == 0);
    ok = ok && (fread(bufferOut, g_BootSector.BytesPerSector, count, disk) == count);
    return ok;
}

bool readFat(FILE* disk)
{
    g_Fat = (uint8_t*) malloc(g_BootSector.SectorsPerFAT * g_BootSector.BytesPerSector);
    if (!g_Fat) return false;
    return readSectors(disk, g_BootSector.ReservedSectors, g_BootSector.SectorsPerFAT, g_Fat);
}

bool readRootDirectory(FILE* disk)
{
    uint32_t lba = g_BootSector.ReservedSectors + g_BootSector.SectorsPerFAT * g_BootSector.FatCount;
    uint32_t size = sizeof(DirectoryEntry) * g_BootSector.DirEntriesCount;
    uint32_t sectors = (size / g_BootSector.BytesPerSector);
    if (size % g_BootSector.BytesPerSector > 0)
        sectors++;

    g_RootDirectoryEnd = lba + sectors;
    g_RootDirectory = (DirectoryEntry*) malloc(sectors * g_BootSector.BytesPerSector);
    if (!g_RootDirectory) return false;
    return readSectors(disk, lba, sectors, g_RootDirectory);
}

// Convert "test.txt" -> "TEST    TXT" (11 bytes, space padded, no terminator).
// An already-formatted 11-char name is passed through unchanged.
static void toFatName(const char* in, char out[11])
{
    memset(out, ' ', 11);

    if (strlen(in) == 11 && !strchr(in, '.')) {
        memcpy(out, in, 11);
        return;
    }

    const char* dot = strrchr(in, '.');
    size_t nameLen = dot ? (size_t)(dot - in) : strlen(in);
    if (nameLen > 8) nameLen = 8;

    for (size_t i = 0; i < nameLen; i++)
        out[i] = (char) toupper((unsigned char) in[i]);

    if (dot)
        for (size_t i = 0; i < 3 && dot[1 + i]; i++)
            out[8 + i] = (char) toupper((unsigned char) dot[1 + i]);
}

DirectoryEntry* findFile(const char* name)
{
    char fatName[11];
    toFatName(name, fatName);   // FIX: memcmp of 11 bytes against a short argv string read out of bounds

    for (uint32_t i = 0; i < g_BootSector.DirEntriesCount; i++)
    {
        if (memcmp(fatName, g_RootDirectory[i].Name, 11) == 0)
            return &g_RootDirectory[i];
    }

    return NULL;
}

bool readFileData(DirectoryEntry* fileEntry, FILE* disk, uint8_t* outputBuffer)
{
    bool ok = true;
    uint16_t currentCluster = fileEntry->FirstClusterLow;

    do {
        uint32_t lba = g_RootDirectoryEnd + (currentCluster - 2) * g_BootSector.SectorsPerCluster;
        ok = ok && readSectors(disk, lba, g_BootSector.SectorsPerCluster, outputBuffer);
        outputBuffer += g_BootSector.SectorsPerCluster * g_BootSector.BytesPerSector;

        uint32_t fatIndex = currentCluster * 3 / 2;
        uint16_t entry;
        memcpy(&entry, g_Fat + fatIndex, sizeof(entry));   // avoids unaligned deref

        if (currentCluster % 2 == 0)
            currentCluster = entry & 0x0FFF;
        else
            currentCluster = entry >> 4;
    } while (ok && currentCluster < 0xFF8);

    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("Syntax: %s <disk image> <file name>\n", argv[0]);
        return -1;
    }

    FILE* disk = fopen(argv[1], "rb");
    if (!disk) {
        fprintf(stderr, "Cannot open disk image %s!\n", argv[1]);
        return -1;
    }

    if (!readBootSector(disk)) {
        fprintf(stderr, "Could not read boot sector!\n");
        fclose(disk);
        return -2;
    }
    
    fprintf(stderr, "BytesPerSector=%u SectorsPerCluster=%u Reserved=%u FatCount=%u DirEntries=%u SectorsPerFAT=%u\n",
        g_BootSector.BytesPerSector, g_BootSector.SectorsPerCluster,
        g_BootSector.ReservedSectors, g_BootSector.FatCount,
        g_BootSector.DirEntriesCount, g_BootSector.SectorsPerFAT);

    if (!readFat(disk)) {
        fprintf(stderr, "Could not read FAT!\n");          // FIX: message
        free(g_Fat);
        fclose(disk);
        return -3;
    }

    if (!readRootDirectory(disk)) {
        fprintf(stderr, "Could not read root directory!\n"); // FIX: message
        free(g_Fat);
        free(g_RootDirectory);
        fclose(disk);
        return -4;
    }

    DirectoryEntry* fileEntry = findFile(argv[2]);
    if (!fileEntry) {
        fprintf(stderr, "Could not find file %s!\n", argv[2]);
        free(g_Fat);
        free(g_RootDirectory);
        fclose(disk);
        return -5;
    }

    // FIX: round up to a whole number of clusters, since readFileData reads
    // full clusters and could overrun a Size + one-sector buffer.
    uint32_t clusterBytes = g_BootSector.SectorsPerCluster * g_BootSector.BytesPerSector;
    uint32_t bufSize = ((fileEntry->Size + clusterBytes - 1) / clusterBytes) * clusterBytes;
    if (bufSize == 0) bufSize = clusterBytes;

    uint8_t* buffer = (uint8_t*) malloc(bufSize);
    if (!buffer || !readFileData(fileEntry, disk, buffer)) {   // FIX: was readFile (undefined)
        fprintf(stderr, "Could not read file %s!\n", argv[2]);
        free(g_Fat);
        free(g_RootDirectory);
        free(buffer);
        fclose(disk);
        return -6;
    }

    for (size_t i = 0; i < fileEntry->Size; i++)
    {
        if (isprint(buffer[i])) fputc(buffer[i], stdout);
        else printf("<%02x>", buffer[i]);
    }
    printf("\n");

    free(buffer);
    free(g_Fat);
    free(g_RootDirectory);
    fclose(disk);
    return 0;
}