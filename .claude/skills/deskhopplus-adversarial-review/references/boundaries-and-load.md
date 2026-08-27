# Boundaries and Load

Apply this checklist for communication, routing, transfers, queues, and timing changes.

## Boundary matrix

Inspect the relevant links in both directions:

```text
USB peripheral <-> RP2040 USB stack <-> firmware routing <-> channel/queue
    <-> inter-board protocol <-> peer firmware <-> host-facing USB
    <-> host helper <-> OS input / clipboard / cursor
```

Distinguish keyboard, mouse, bulk/clipboard, control, heartbeat, and configuration paths. A correction in one traffic class does not establish behavior in another.

When a change affects communication, inspect the changed direction, the reverse direction, and any helper-to-device or device-to-peer path it makes reachable.

## Load escalation

For relevant paths, reason through:

1. one message;
2. two messages;
3. a burst;
4. sustained traffic;
5. competing traffic classes;
6. traffic during reconnect or reset; and
7. traffic at queue capacity.

Favor defects caused by interactions such as bulk traffic plus a bounded priority queue plus heartbeat timeout, or reset plus a stale message plus ID reuse. These emergent failures are especially valuable when independently defensible.
