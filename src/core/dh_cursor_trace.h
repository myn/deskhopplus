/* Bounded cursor-transition evidence preserved across a config-mode reboot (#28). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Covers the complete 30 ms re-anchor window at the 2 kHz mouse task rate,
   plus the query and terminal lifecycle records that bracket it. */
#define DH_CURSOR_TRACE_CAPACITY 64u
#define DH_CURSOR_TRACE_MAGIC 0x44544331u /* "DTC1" */

/* dh_cursor_trace_record_t.state wire layout; mirrored by the config page. */
#define DH_CURSOR_TRACE_OUTPUT_SHIFT 0u
#define DH_CURSOR_TRACE_SCREEN_SHIFT 1u
#define DH_CURSOR_TRACE_DIRECTION_SHIFT 4u
#define DH_CURSOR_TRACE_PHASE_SHIFT 7u
#define DH_CURSOR_TRACE_RELATIVE_SHIFT 10u
#define DH_CURSOR_TRACE_TRANSITION_SHIFT 11u

typedef enum {
    DH_CURSOR_TRACE_INPUT = 1,
    DH_CURSOR_TRACE_DECISION,
    DH_CURSOR_TRACE_QUERY,
    DH_CURSOR_TRACE_RESPONSE,
    DH_CURSOR_TRACE_PLACE,
    DH_CURSOR_TRACE_SWITCH,
    DH_CURSOR_TRACE_CANCEL,
    DH_CURSOR_TRACE_TIMEOUT,
    /*
     * Not cursor events: the board's own USB host bringing up the keyboard
     * and mouse plugged into it (#102). The ring survives a reboot, which is
     * the one thing a dead-after-boot keyboard needs a log to do. Fields:
     * BOOT      query_id = 1 when booting into config mode.
     * HID_MOUNT query_id = dev_addr, move_x = instance, move_y = itf protocol
     *           (1 keyboard, 2 mouse, 0 none); direction bit0 = keyboard seen,
     *           bit1 = mouse seen; transition 1 = report polling started,
     *           2 = it refused, 3 = rejected by the bounds guard.
     * HID_UNMOUNT the same three, no outcome.
     * DEV_MOUNT / DEV_UNMOUNT query_id = dev_addr: the device itself, after
     *           enumeration and before any class driver claims it. A DEV_MOUNT
     *           with no HID_MOUNT behind it is a descriptor the HID driver
     *           refused; a boot with neither is an enumeration that never
     *           finished.
     * HOST_REPLUG query_id = pulls this run, this one included: the firmware
     *           emulated a cable pull because the port read attached and
     *           nothing had mounted (dh_host_replug.h).
     */
    DH_CURSOR_TRACE_BOOT,
    DH_CURSOR_TRACE_HID_MOUNT,
    DH_CURSOR_TRACE_HID_UNMOUNT,
    DH_CURSOR_TRACE_DEV_MOUNT,
    DH_CURSOR_TRACE_DEV_UNMOUNT,
    DH_CURSOR_TRACE_HOST_REPLUG,
} dh_cursor_trace_event_t;

/* Twelve bytes so one config response can carry either six-byte half. */
typedef struct {
    uint8_t event;
    uint8_t query_id;
    int16_t move_x;
    int16_t move_y;
    int16_t pointer_x;
    int16_t pointer_y;
    uint16_t state;
} dh_cursor_trace_record_t;

typedef struct {
    uint32_t magic;
    uint8_t head;
    uint8_t count;
    uint16_t reserved;
    dh_cursor_trace_record_t records[DH_CURSOR_TRACE_CAPACITY];
} dh_cursor_trace_t;

void dh_cursor_trace_init(dh_cursor_trace_t *trace, bool preserve);
void dh_cursor_trace_append(dh_cursor_trace_t *trace, dh_cursor_trace_record_t record);
size_t dh_cursor_trace_count(const dh_cursor_trace_t *trace);
bool dh_cursor_trace_read(const dh_cursor_trace_t *trace, size_t index,
                          dh_cursor_trace_record_t *record);
