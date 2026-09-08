#ifndef CHANNEL_LIFECYCLE_H_
#define CHANNEL_LIFECYCLE_H_

#include "dh_outq.h"
#include "dh_session.h"
#include "dh_txq.h"

/* Firmware lifecycle ownership, using the existing protocol and queue storage.
   Core 0 owns the session and pairing; the adapter serializes outbound access.
   Entropy, persistence and USB callbacks remain in channel.c. */
typedef struct {
    dh_session session;
    dh_pair pair;
    dh_outq out;
    dh_txq_stats tx;
} channel_lifecycle;

/* Hardware adapter: same outbound critical section as transport peek/advance. */
void channel_lifecycle_lock(void);
void channel_lifecycle_unlock(void);

bool channel_lifecycle_queue(channel_lifecycle *c, const uint8_t *frame, size_t len,
                             uint32_t now);
/* Session-band frame only. Owns successful hello -> fresh stream -> reply.
   Registration changes remain visible to the persistence adapter via pair. */
void channel_lifecycle_on_frame(channel_lifecycle *c, const dh_frame_view *frame,
                                uint32_t now);

#endif
