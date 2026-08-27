# State, Protocol, and USB

Apply this checklist when the changed area manages state, sessions, identity, messages, reset/reconnect, USB, or HID.

## State and reset

List relevant states, transitions, and valid events. Consider events arriving early, late, twice, during shutdown, after reset, or from an old session. Look for a valid state plus an unexpected ignored event yielding a permanent stall or transition without recovery.

Simulate:

```text
healthy -> active operation -> reset/disconnect -> reconnect -> old message -> new message
```

For each state item, determine whether it is reset or retained, who learns it reset, whether queued messages can outlive their creator, and whether either peer can accept stale data.

## Identifiers and messages

For every touched ID, establish its namespace: global, board, direction, connection, session, transfer, message type, or local. Check wraparound, reuse, old messages sharing a value, bidirectional collisions, and whether the receiver knows the namespace.

For every affected message, inspect creator and consumer; required receiving state; early, late, duplicate, missing, malformed, and truncated cases; compatibility; and the sender-success/receiver-rejection gap. Review both sender and receiver.

## USB and HID

For USB/HID changes, check descriptors, report sizes and IDs, enumeration, endpoint availability, host expectations, device reset/reconnect, boot behavior, malformed reports, report rate and buffering, host-specific behavior, and firmware-version compatibility.
