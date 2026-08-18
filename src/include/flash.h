/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <hardware/flash.h>

/*==============================================================================
 *  Firmware Metadata
 *==============================================================================*/

/*
 * Packed, because this struct is not what the compiler lays out — it is what
 * `misc/crc32.py` writes into `.section_metadata` post-build, and that is ten
 * bytes with no padding: magic, version, checksum, `struct.pack('<IHI', ...)`.
 *
 * Unpacked, the compiler puts two bytes of padding after `version` and reads
 * `checksum` from offset 8, where the file has only the top half of it and
 * then whatever follows the section. `_running_fw.checksum` was therefore
 * never the firmware CRC, and config field 79 reported that. Nothing caught
 * it because `magic` and `version` sit at the same offset either way, and
 * they are what everything else reads.
 *
 * Found while putting the checksum on the heartbeat (#91), where a field that
 * is quietly wrong stops board B ever pulling correctly.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint32_t checksum;
} __attribute__((packed)) firmware_metadata_t;

/* The layout is a wire format shared with misc/crc32.py and read back by
   tools/build.sh. Neither can see this header, so assert what they assume. */
_Static_assert(sizeof(firmware_metadata_t) == 10,
               "firmware_metadata_t must match misc/crc32.py's <IHI: 10 bytes, no padding");
_Static_assert(offsetof(firmware_metadata_t, checksum) == 6,
               "firmware_metadata_t: checksum follows version immediately (see misc/crc32.py)");

extern firmware_metadata_t _firmware_metadata;
#define FIRMWARE_METADATA_MAGIC   0xf00d

/*==============================================================================
 *  Firmware Transfer Packet
 *==============================================================================*/

typedef struct {
    uint8_t cmd;          // Byte 0 = command
    uint16_t page_number; // Bytes 1-2 = page number
    union {
        uint8_t offset;   // Byte 3 = offset
        uint8_t checksum; // In write packets, it's checksum
    };
    uint8_t data[4]; // Bytes 4-7 = data
} fw_packet_t;

/*==============================================================================
 *  Flash Memory Layout
 *==============================================================================*/

#define RUNNING_FIRMWARE_SLOT     0
#define STAGING_FIRMWARE_SLOT     1
#define STAGING_PAGES_CNT         1024
#define STAGING_IMAGE_SIZE        (STAGING_PAGES_CNT * FLASH_PAGE_SIZE)

/*==============================================================================
*  Lookup Tables
*==============================================================================*/

extern const uint32_t crc32_lookup_table[];

/*==============================================================================
 *  UF2 Firmware Format Structure
 *==============================================================================*/
typedef struct {
    uint32_t magicStart0;
    uint32_t magicStart1;
    uint32_t flags;
    uint32_t targetAddr;
    uint32_t payloadSize;
    uint32_t blockNo;
    uint32_t numBlocks;
    uint32_t fileSize;
    uint8_t data[476];
    uint32_t magicEnd;
} uf2_t;

#define UF2_MAGIC_START0 0x0A324655
#define UF2_MAGIC_START1 0x9E5D5157
#define UF2_MAGIC_END    0x0AB16F30
