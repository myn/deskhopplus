/*
 * Where each thing lives in the board's 2 MB flash, as plain arithmetic.
 *
 * The authority on placement is `misc/memory_map.ld`; these constants mirror
 * it, and the linker script carries ASSERTs tying its own ORIGINs to the same
 * numbers, so the two cannot drift silently. What lives *here* rather than
 * there is the part a test can reach: which ranges each writer touches, and
 * whether any of them overlaps a range it must not.
 *
 * That question is not academic. Peer propagation copies board A's running
 * image onto board B (#91, measured working 2026-08-18), and the upgrade path
 * (#90, #104) writes every page of it. **An identity key inside that range
 * would give both boards the same identity** — which is exactly the failure
 * ADR-0008 put the identity in its own sector to prevent, and exactly the sort
 * of arithmetic that shipped wrong once already behind flash I/O no test could
 * reach (#74).
 *
 * Pure C11 — stdint and stddef only. Keep it that way, or the host test goes
 * with it. tests/flash_layout_test.c is the gate.
 */
#pragma once

#include <stdint.h>

#define FLASH_LAYOUT_KB(n) ((uint32_t)(n) * 1024u)

/* The memory-mapped base of flash (XIP_BASE), and the part fitted. */
#define FLASH_LAYOUT_BASE 0x10000000u
#define FLASH_LAYOUT_TOTAL FLASH_LAYOUT_KB(2048)

/* The erase granule and the program granule the SDK works in. Named here so
   the arithmetic below does not need hardware/flash.h, which is not pure. */
#define FLASH_LAYOUT_SECTOR 4096u
#define FLASH_LAYOUT_PAGE 256u

/*
 * The running image: executable, then the FAT disk image the config-mode mass
 * storage device serves, then the metadata page the post-build checksum is
 * stamped into. 256 KB in total, and the whole of it is what a peer
 * propagation writes — page by page from address 0 to the end.
 */
#define FLASH_LAYOUT_IMAGE_OFFSET 0u
#define FLASH_LAYOUT_IMAGE_LEN FLASH_LAYOUT_KB(256)

/* The staging slot an upgrade is received into, immediately behind it. */
#define FLASH_LAYOUT_STAGING_OFFSET FLASH_LAYOUT_KB(256)
#define FLASH_LAYOUT_STAGING_LEN FLASH_LAYOUT_KB(256)

/*
 * The board's identity (#111): its P-256 private key, generated on first boot.
 * Its own sector, beside the configuration and part of neither it nor the
 * image — so it survives a config wipe, a firmware update, and the peer
 * propagation above. A config wipe unpairs; it does not change who this board
 * is, because a wipe that took the identity too would make every helper report
 * "this board changed" on a routine action.
 */
#define FLASH_LAYOUT_IDENTITY_OFFSET (FLASH_LAYOUT_TOTAL - FLASH_LAYOUT_KB(8))
#define FLASH_LAYOUT_IDENTITY_LEN FLASH_LAYOUT_SECTOR

/* The stored configuration, in the last sector. Holds the registration — the
   one helper key this board has paired with — which a wipe is meant to clear. */
#define FLASH_LAYOUT_CONFIG_OFFSET (FLASH_LAYOUT_TOTAL - FLASH_LAYOUT_KB(4))
#define FLASH_LAYOUT_CONFIG_LEN FLASH_LAYOUT_SECTOR

/* Do two [offset, offset+len) ranges share a byte? */
#define FLASH_LAYOUT_OVERLAPS(a_off, a_len, b_off, b_len) \
    ((a_off) < (b_off) + (b_len) && (b_off) < (a_off) + (a_len))

_Static_assert(FLASH_LAYOUT_IDENTITY_OFFSET % FLASH_LAYOUT_SECTOR == 0,
               "the identity must start on an erase boundary, or saving it erases a neighbour");
_Static_assert(!FLASH_LAYOUT_OVERLAPS(FLASH_LAYOUT_IDENTITY_OFFSET, FLASH_LAYOUT_IDENTITY_LEN,
                                      FLASH_LAYOUT_IMAGE_OFFSET, FLASH_LAYOUT_IMAGE_LEN),
               "a peer propagation would overwrite the identity, giving both boards one identity");
_Static_assert(!FLASH_LAYOUT_OVERLAPS(FLASH_LAYOUT_IDENTITY_OFFSET, FLASH_LAYOUT_IDENTITY_LEN,
                                      FLASH_LAYOUT_STAGING_OFFSET, FLASH_LAYOUT_STAGING_LEN),
               "a staged upgrade would overwrite the identity");
_Static_assert(!FLASH_LAYOUT_OVERLAPS(FLASH_LAYOUT_IDENTITY_OFFSET, FLASH_LAYOUT_IDENTITY_LEN,
                                      FLASH_LAYOUT_CONFIG_OFFSET, FLASH_LAYOUT_CONFIG_LEN),
               "a config wipe would unpair the board and change who it is");
_Static_assert(FLASH_LAYOUT_CONFIG_OFFSET + FLASH_LAYOUT_CONFIG_LEN == FLASH_LAYOUT_TOTAL,
               "the configuration is the last sector of flash");
