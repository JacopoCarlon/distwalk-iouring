#ifndef __DW_EVENT_H__
#define __DW_EVENT_H__

#include <stdint.h>

/*
 * Encoding of the opaque `aux` token carried through the dw_poll layer.
 *
 *   [63..60] reserved - uring uses this for opcode
 *   [59..32] event_t  - what kind of fd produced the event
 *   [31.. 0] id       - conn_id for SOCKET/CONNECT events, raw fd otherwise
 */
typedef enum {
    LISTEN,
    STORAGE,
    TIMER,
    CONNECT,
    SOCKET,
    DISPATCH,
    STATS,
    TERMINATION,
    EVENT_NUMBER
} event_t;

static inline uint64_t i2l(uint32_t ln, uint32_t rn) {
    return ((uint64_t) ln) << 32 | rn;
}

static inline void l2i(uint64_t n, uint32_t *ln, uint32_t *rn) {
    if (ln)
        *ln = n >> 32;

    if (rn)
        *rn = (uint32_t) n;
}

#endif /* __DW_EVENT_H__ */
