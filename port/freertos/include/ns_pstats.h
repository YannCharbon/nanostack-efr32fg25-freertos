/*
 * Power and activity statistics for the AEM measurements (workspace HW5,
 * Idle_Sleep_Study.md §5.2 and §8.3). Port and driver only, no stack change.
 *
 * Built only with the CMake option FW_POWER_STATS=ON (defines
 * FW_POWER_STATS=1); otherwise every hook below is an empty inline and the
 * image is unchanged.
 *
 * What is recorded:
 *  - energy modes: time and transitions in EM0/EM1/EM2 from the power
 *    manager's transition events (RAIL µs clock);
 *  - wakes of the stack task: one wake = one return from the run loop's
 *    blocking wait. Per wake: DWT cycles until it blocks again, the work
 *    done (frame RX, TX event, TX prepare, FHSS timer, callback timer id,
 *    events dispatched) and a primary cause;
 *  - cycles per kind of work (callback timer by id, FHSS callback, MAC RX,
 *    TX done/prepare callbacks, RAIL TX start, each event, AES block, ECC);
 *  - radio state times (off/idle, RX, TX) and the time spent receiving
 *    frames, from the driver;
 *  - a GPIO (default PC1 = EXP header pin 6) high in EM0, low in EM1/EM2.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PSTATS_H
#define NS_PSTATS_H

#include <stdint.h>
#include <stdbool.h>

#ifndef FW_POWER_STATS
#define FW_POWER_STATS 0
#endif

/* Work kinds marked during a wake (bit mask). */
enum {
    PS_W_RX      = 1u << 0,   /* driver delivered >= 1 frame to the MAC */
    PS_W_TXEV    = 1u << 1,   /* driver handled a RAIL TX event (sent, busy, ...) */
    PS_W_PREPARE = 1u << 2,   /* driver ran the CCA_PREPARE step */
    PS_W_FHSS    = 1u << 3,   /* an FHSS timer callback ran */
    PS_W_CBT     = 1u << 4,   /* a callback timer ran (ids in a separate mask) */
    PS_W_EVENT   = 1u << 5,   /* the event loop dispatched >= 1 event */
    PS_W_CLI     = 1u << 6,   /* console input handled */
};

/* Primary cause of a wake, by priority: the first that applies. */
enum ps_cause {
    PS_C_RX, PS_C_TXEV, PS_C_PREPARE, PS_C_FHSS, PS_C_CBT, PS_C_EVENT, PS_C_CLI, PS_C_NONE,
    PS_C_COUNT
};

/* Cycle accounting slots. */
enum ps_kind {
    PS_K_FHSS, PS_K_RX, PS_K_TXDONE, PS_K_TXSTART, PS_K_EVENT, PS_K_POLL, PS_K_AES, PS_K_ECP,
    PS_K_COUNT
};

/* Radio states, from the driver. */
enum ps_radio {
    PS_R_IDLE,   /* RAIL idle (radio off from the stack's view, or between operations) */
    PS_R_RX,     /* RX on (listening or receiving) */
    PS_R_TX,     /* from the TX start (CCA included) to the TX event */
    PS_R_COUNT
};

#define PS_MAX_CBT 16

struct ps_acc {
    uint32_t count;
    uint64_t cycles;
    uint32_t max;
};

struct ps_snapshot {
    uint32_t elapsed_us;            /* since the last clear */
    uint32_t cpu_hz;
    uint64_t em_us[3];              /* EM0, EM1, EM2 */
    uint32_t em_entries[3];
    uint64_t cycles_total;          /* DWT cycles since the clear (stops in sleep) */
    uint32_t wakes;
    struct ps_acc wake;             /* cycles per wake */
    struct ps_acc by_cause[PS_C_COUNT];
    uint32_t cbt_only_empty[PS_MAX_CBT];   /* wakes where only that timer ran, no event */
    struct ps_acc cbt[PS_MAX_CBT];         /* per callback timer id */
    void *cbt_fn[PS_MAX_CBT];              /* its handler (resolve with nm) */
    uint32_t cbt_wake_cause[PS_MAX_CBT];   /* wakes whose primary cause is this timer */
    struct ps_acc kind[PS_K_COUNT];
    uint32_t work_count[8];                /* wakes with each PS_W_* bit */
    uint64_t radio_us[PS_R_COUNT];
    uint32_t radio_entries[PS_R_COUNT];
    uint64_t rx_frame_us;           /* air time of received frames (SHR to FCS end) */
    uint32_t rx_frames;
    uint64_t tx_air_us;             /* computed air time of transmitted frames */
    uint32_t tx_frames;
    uint32_t start_rx_calls;
    uint32_t idle_calls;
};

#if FW_POWER_STATS

void ns_pstats_init(void);
void ns_pstats_clear(void);
void ns_pstats_get(struct ps_snapshot *s);

static inline uint32_t ns_pstats_cyc(void)
{
    return *(volatile uint32_t *)0xE0001004u;   /* DWT->CYCCNT */
}

/* Run loop (port_core.c). */
void ns_pstats_wake_begin(void);
void ns_pstats_wake_end(void);
void ns_pstats_mark(uint32_t work);
void ns_pstats_cbt(int id, void *fn, uint32_t cycles);
void ns_pstats_kind(enum ps_kind k, uint32_t cycles);

/* Driver (rail_phy.c). */
void ns_pstats_radio(enum ps_radio st);
/* State change at a RAIL time that may lie slightly in the future (scheduled TX start). */
void ns_pstats_radio_at(enum ps_radio st, uint32_t when);
void ns_pstats_rx_frame(uint32_t air_us);
void ns_pstats_tx_frame(uint32_t air_us);
void ns_pstats_start_rx_call(void);
void ns_pstats_idle_call(void);

#define PS_CYC_BEGIN()        uint32_t _ps_c0 = ns_pstats_cyc()
#define PS_CYC_END(k)         ns_pstats_kind((k), ns_pstats_cyc() - _ps_c0)

#else

static inline void ns_pstats_init(void) {}
static inline void ns_pstats_clear(void) {}
static inline void ns_pstats_wake_begin(void) {}
static inline void ns_pstats_wake_end(void) {}
static inline void ns_pstats_mark(uint32_t work) { (void)work; }
static inline void ns_pstats_radio(enum ps_radio st) { (void)st; }
static inline void ns_pstats_radio_at(enum ps_radio st, uint32_t when) { (void)st; (void)when; }
static inline void ns_pstats_rx_frame(uint32_t air_us) { (void)air_us; }
static inline void ns_pstats_tx_frame(uint32_t air_us) { (void)air_us; }
static inline void ns_pstats_start_rx_call(void) {}
static inline void ns_pstats_idle_call(void) {}
#define PS_CYC_BEGIN()        do {} while (0)
#define PS_CYC_END(k)         do {} while (0)

#endif

#endif /* NS_PSTATS_H */
