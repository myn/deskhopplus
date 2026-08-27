# Reliability and Liveness

Apply this checklist when the change affects a queue, buffer, retry, timeout, heartbeat, transfer, resource limit, or execution cadence.

## Queues and resources

Treat a bounded queue as a reliability boundary. Determine what happens if it is full, a producer outruns a consumer, an insertion or retry is refused, traffic classes compete, or teardown/reconnect begins. Identify who detects loss, who retries, and whether control, bulk, or priority traffic can starve another class.

Inspect fixed arrays, static buffers, transfer slots, outstanding requests, packet accumulation, stack/heap use, retry accumulation, counters, and state expected to eventually clear. Evaluate occupancy for 10 ms, 100 ms, 1 second, and indefinitely—not only one successful event.

## Retransmission

Trace the full loop:

```text
data -> loss -> detection -> retransmit request -> delivery -> acceptance -> completion
```

Determine who detects loss; how missing data is identified; whether IDs are unambiguous; whether sender data remains available; whether requests and retransmissions can themselves be lost; how duplicates are handled; and whether old data can apply to a new transfer. Check for sender/receiver disagreement about transfer state.

## Timeouts and timing

For each timer, identify its start event, reset event, and whether that reset observation can be dropped or delayed. Ask whether the system can stay healthy while the observation is absent, whether timeout races recovery, and whether recovery traffic amplifies the problem.

Check assumptions about polling cadence, interrupts between state updates, timer callbacks observing partial state, reversed event ordering, delayed consumers, and tests that accidentally guarantee hardware-unrealistic ordering. Code correct only at the expected 1 kHz task cadence is a high-priority candidate.
