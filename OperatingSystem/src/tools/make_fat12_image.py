#!/usr/bin/env python3
"""Build a 1.44MB FAT12 floppy image containing TEST.TXT in the root directory.

Usage:
    python make_fat12_image.py <out.img> [boot.bin]

If boot.bin is given, its jump instruction and boot code (bytes 0-2 and 62-509)
are copied into the image. The BPB fields are always written by this script and
match the standard 1.44MB floppy layout.
"""
import struct
import sys

BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 1
RESERVED = 1
FAT_COUNT = 2
ROOT_ENTRIES = 224
TOTAL_SECTORS = 2880
SECTORS_PER_FAT = 9

ROOT_SECTORS = ROOT_ENTRIES * 32 // BYTES_PER_SECTOR            # 14
ROOT_LBA = RESERVED + FAT_COUNT * SECTORS_PER_FAT                # 19
DATA_LBA = ROOT_LBA + ROOT_SECTORS                               # 33
CLUSTER_BYTES = SECTORS_PER_CLUSTER * BYTES_PER_SECTOR


def build_boot_sector(boot_bin=None):
    bpb = struct.pack(
        "<3s8sHBHBHHBHHHIIBBBI11s8s",
        b"\xEB\x3C\x90",
        b"MSWIN4.1",
        BYTES_PER_SECTOR,
        SECTORS_PER_CLUSTER,
        RESERVED,
        FAT_COUNT,
        ROOT_ENTRIES,
        TOTAL_SECTORS,
        0xF0,              # media descriptor
        SECTORS_PER_FAT,
        18,                # sectors per track
        2,                 # heads
        0,                 # hidden sectors
        0,                 # large sector count
        0,                 # drive number
        0,                 # reserved
        0x29,              # extended boot signature
        0x12345678,        # volume id
        b"NBOS       ",
        b"FAT12   ",
    )
    assert len(bpb) == 62, len(bpb)
    sector = bytearray(bpb) + bytearray(BYTES_PER_SECTOR - 62)

    if boot_bin:
        with open(boot_bin, "rb") as f:
            code = f.read(BYTES_PER_SECTOR)
        code = code.ljust(BYTES_PER_SECTOR, b"\x00")
        sector[0:3] = code[0:3]
        sector[62:510] = code[62:510]

    sector[510:512] = b"\x55\xAA"
    return bytes(sector)


def set_fat12(fat, n, value):
    i = n * 3 // 2
    if n % 2 == 0:
        fat[i] = value & 0xFF
        fat[i + 1] = (fat[i + 1] & 0xF0) | ((value >> 8) & 0x0F)
    else:
        fat[i] = (fat[i] & 0x0F) | ((value & 0x0F) << 4)
        fat[i + 1] = (value >> 4) & 0xFF


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    out_path = sys.argv[1]
    boot_bin = sys.argv[2] if len(sys.argv) > 2 else None

    content = b"hello from fat12\n"

    image = bytearray(TOTAL_SECTORS * BYTES_PER_SECTOR)
    image[0:512] = build_boot_sector(boot_bin)

    # FAT: entries 0 and 1 are reserved, then the file's cluster chain.
    fat = bytearray(SECTORS_PER_FAT * BYTES_PER_SECTOR)
    set_fat12(fat, 0, 0xFF0)
    set_fat12(fat, 1, 0xFFF)

    clusters_needed = max(1, -(-len(content) // CLUSTER_BYTES))
    first_cluster = 2
    for i in range(clusters_needed):
        c = first_cluster + i
        set_fat12(fat, c, 0xFFF if i == clusters_needed - 1 else c + 1)

    for n in range(FAT_COUNT):
        start = RESERVED * BYTES_PER_SECTOR + n * len(fat)
        image[start:start + len(fat)] = fat

    # Root directory entry for TEST.TXT
    entry = struct.pack(
        "<11sBBBHHHHHHHI",
        b"TEST    TXT",
        0x20,              # archive attribute
        0, 0,              # reserved, created time tenths
        0, 0x5821, 0x5821, # created time, created date, accessed date
        0,                 # first cluster high
        0, 0x5821,         # modified time, modified date
        first_cluster,
        len(content),
    )
    assert len(entry) == 32
    root_start = ROOT_LBA * BYTES_PER_SECTOR
    image[root_start:root_start + 32] = entry

    # File data
    data_start = (DATA_LBA + (first_cluster - 2) * SECTORS_PER_CLUSTER) * BYTES_PER_SECTOR
    image[data_start:data_start + len(content)] = content

    with open(out_path, "wb") as f:
        f.write(image)

    print(f"Wrote {out_path} ({len(image)} bytes) containing TEST.TXT")
    return 0


if __name__ == "__main__":
    sys.exit(main())
