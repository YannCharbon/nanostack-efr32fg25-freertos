/*
 * Power and activity statistics (HW5). See include/ns_pstats.h.
 * Compiled only with FW_POWER_STATS=ON.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "em_device.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "rail.h"
#include "sl_power_manager.h"

#include "ns_pstats.h"

#ifndef FW_PSTATS_GPIO_PORT
#define FW_PSTATS_GPIO_PORT gpioPortC   /* EXP header pin 6 on BRD4001A + BRD4271A */
#endif
#ifndef FW_PSTATS_GPIO_PIN
#define FW_PSTATS_GPIO_PIN  1
#endif

static struct ps_snapshot st;
static uint32_t clear_time_us;
static uint32_t cyc_last;            /* for cycles_total (32-bit DWT wraps every 44 s) */

/* Energy modes. */
static uint32_t em_since;
static unsigned em_cur;              /* 0, 1, 2 */
static sl_power_manager_em_transition_event_handle_t em_handle;

/* Current wake. */
static bool in_wake;
static uint32_t wake_c0;
static uint32_t wake_work;
static uint32_t wake_cbt_mask;
static int wake_first_cbt = -1;

/* Radio. */
static enum ps_radio radio_cur = PS_R_IDLE;
static uint32_t radio_since;

static inline uint32_t irq_save(void)
{
    uint32_t m = __get_PRIMASK();
    __disable_irq();
    return m;
}

static inline void irq_restore(uint32_t m)
{
    if (!m) {
        __enable_irq();
    }
}

static void acc_add(struct ps_acc *a, uint32_t cycles)
{
    a->count++;
    a->cycles += cycles;
    if (cycles > a->max) {
        a->max = cycles;
    }
}

static void cycles_fold(void)
{
    uint32_t c = ns_pstats_cyc();
    st.cycles_total += (uint32_t)(c - cyc_last);
    cyc_last = c;
}

static void em_cb(sl_power_manager_em_t from, sl_power_manager_em_t to)
{
    uint32_t now = RAIL_GetTime();
    unsigned f = from > 2 ? 2 : (unsigned)from;
    unsigned t = to > 2 ? 2 : (unsigned)to;

    (void)f;
    st.em_us[em_cur] += (uint32_t)(now - em_since);
    em_since = now;
    em_cur = t;
    st.em_entries[t]++;
    if (t == 0) {
        GPIO_PinOutSet(FW_PSTATS_GPIO_PORT, FW_PSTATS_GPIO_PIN);
    } else {
        GPIO_PinOutClear(FW_PSTATS_GPIO_PORT, FW_PSTATS_GPIO_PIN);
        cycles_fold();   /* often enough to never miss a DWT wrap while active */
    }
}

static const sl_power_manager_em_transition_event_info_t em_info = {
    .event_mask = SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM0 | SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM1
                  | SL_POWER_MANAGER_EVENT_TRANSITION_ENTERING_EM2,
    .on_event = em_cb,
};

void ns_pstats_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    CMU_ClockEnable(cmuClock_GPIO, true);
    GPIO_PinModeSet(FW_PSTATS_GPIO_PORT, FW_PSTATS_GPIO_PIN, gpioModePushPull, 1);
    ns_pstats_clear();
    sl_power_manager_subscribe_em_transition_event(&em_handle, &em_info);
}

void ns_pstats_clear(void)
{
    uint32_t m = irq_save();
    enum ps_radio r = radio_cur;
    void *fns[PS_MAX_CBT];

    memcpy(fns, st.cbt_fn, sizeof(fns));
    memset(&st, 0, sizeof(st));
    memcpy(st.cbt_fn, fns, sizeof(fns));
    clear_time_us = RAIL_GetTime();
    em_since = clear_time_us;
    em_cur = 0;                      /* called from a running task */
    radio_since = clear_time_us;
    radio_cur = r;
    cyc_last = ns_pstats_cyc();
    st.cpu_hz = SystemCoreClock;
    irq_restore(m);
}

void ns_pstats_get(struct ps_snapshot *s)
{
    uint32_t m = irq_save();
    uint32_t now = RAIL_GetTime();

    cycles_fold();
    st.em_us[em_cur] += (uint32_t)(now - em_since);
    em_since = now;
    st.radio_us[radio_cur] += (uint32_t)(now - radio_since);
    radio_since = now;
    st.elapsed_us = now - clear_time_us;
    st.cpu_hz = SystemCoreClock;
    *s = st;
    irq_restore(m);
}

/* ---------------------------------------------------------------- wakes */

void ns_pstats_wake_begin(void)
{
    in_wake = true;
    wake_c0 = ns_pstats_cyc();
    wake_work = 0;
    wake_cbt_mask = 0;
    wake_first_cbt = -1;
}

void ns_pstats_mark(uint32_t work)
{
    wake_work |= work;
}

void ns_pstats_cbt(int id, void *fn, uint32_t cycles)
{
    if (id < 0 || id >= PS_MAX_CBT) {
        return;
    }
    st.cbt_fn[id] = fn;
    acc_add(&st.cbt[id], cycles);
    wake_work |= PS_W_CBT;
    wake_cbt_mask |= 1u << id;
    if (wake_first_cbt < 0) {
        wake_first_cbt = id;
    }
}

void ns_pstats_kind(enum ps_kind k, uint32_t cycles)
{
    if (k < PS_K_COUNT) {
        acc_add(&st.kind[k], cycles);
    }
}

void ns_pstats_wake_end(void)
{
    uint32_t cyc;
    enum ps_cause c;

    if (!in_wake) {
        return;
    }
    in_wake = false;
    cyc = ns_pstats_cyc() - wake_c0;
    if (wake_work & PS_W_RX) {
        c = PS_C_RX;
    } else if (wake_work & PS_W_TXEV) {
        c = PS_C_TXEV;
    } else if (wake_work & PS_W_PREPARE) {
        c = PS_C_PREPARE;
    } else if (wake_work & PS_W_FHSS) {
        c = PS_C_FHSS;
    } else if (wake_work & PS_W_CBT) {
        c = PS_C_CBT;
    } else if (wake_work & PS_W_CLI) {
        c = PS_C_CLI;
    } else if (wake_work & PS_W_EVENT) {
        c = PS_C_EVENT;
    } else {
        c = PS_C_NONE;
    }
    st.wakes++;
    acc_add(&st.wake, cyc);
    acc_add(&st.by_cause[c], cyc);
    for (unsigned b = 0; b < 8; b++) {
        if (wake_work & (1u << b)) {
            st.work_count[b]++;
        }
    }
    if (c == PS_C_CBT && wake_first_cbt >= 0) {
        st.cbt_wake_cause[wake_first_cbt]++;
        if (wake_work == PS_W_CBT && (wake_cbt_mask & (wake_cbt_mask - 1)) == 0) {
            st.cbt_only_empty[wake_first_cbt]++;
        }
    }
}

/* ---------------------------------------------------------------- radio */

void ns_pstats_radio_at(enum ps_radio r, uint32_t when)
{
    uint32_t m = irq_save();
    int32_t d = (int32_t)(when - radio_since);

    if (r != radio_cur) {
        /* A change before the (future) start of the previous state: the
         * previous state never really began, count nothing for it. */
        if (d > 0) {
            st.radio_us[radio_cur] += (uint32_t)d;
            radio_since = when;
        }
        radio_cur = r;
        st.radio_entries[r]++;
    }
    irq_restore(m);
}

void ns_pstats_radio(enum ps_radio r)
{
    ns_pstats_radio_at(r, RAIL_GetTime());
}

void ns_pstats_rx_frame(uint32_t air_us)
{
    st.rx_frames++;
    st.rx_frame_us += air_us;
}

void ns_pstats_tx_frame(uint32_t air_us)
{
    st.tx_frames++;
    st.tx_air_us += air_us;
}

void ns_pstats_start_rx_call(void)
{
    st.start_rx_calls++;
}

void ns_pstats_idle_call(void)
{
    st.idle_calls++;
}

/* ------------------------------------------------- crypto (link wraps) */

#include "mbedtls/aes.h"
#include "mbedtls/ecp.h"

int __real_mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode, const unsigned char in[16],
                                 unsigned char out[16]);
int __wrap_mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode, const unsigned char in[16],
                                 unsigned char out[16])
{
    uint32_t c0 = ns_pstats_cyc();
    int r = __real_mbedtls_aes_crypt_ecb(ctx, mode, in, out);
    ns_pstats_kind(PS_K_AES, ns_pstats_cyc() - c0);
    return r;
}

int __real_mbedtls_ecp_mul_restartable(mbedtls_ecp_group *grp, mbedtls_ecp_point *R, const mbedtls_mpi *m,
                                       const mbedtls_ecp_point *P,
                                       int (*f_rng)(void *, unsigned char *, size_t), void *p_rng,
                                       mbedtls_ecp_restart_ctx *rs_ctx);
int __wrap_mbedtls_ecp_mul_restartable(mbedtls_ecp_group *grp, mbedtls_ecp_point *R, const mbedtls_mpi *m,
                                       const mbedtls_ecp_point *P,
                                       int (*f_rng)(void *, unsigned char *, size_t), void *p_rng,
                                       mbedtls_ecp_restart_ctx *rs_ctx)
{
    uint32_t c0 = ns_pstats_cyc();
    int r = __real_mbedtls_ecp_mul_restartable(grp, R, m, P, f_rng, p_rng, rs_ctx);
    ns_pstats_kind(PS_K_ECP, ns_pstats_cyc() - c0);
    return r;
}

int __real_mbedtls_ecp_muladd_restartable(mbedtls_ecp_group *grp, mbedtls_ecp_point *R, const mbedtls_mpi *m,
                                          const mbedtls_ecp_point *P, const mbedtls_mpi *n,
                                          const mbedtls_ecp_point *Q, mbedtls_ecp_restart_ctx *rs_ctx);
int __wrap_mbedtls_ecp_muladd_restartable(mbedtls_ecp_group *grp, mbedtls_ecp_point *R, const mbedtls_mpi *m,
                                          const mbedtls_ecp_point *P, const mbedtls_mpi *n,
                                          const mbedtls_ecp_point *Q, mbedtls_ecp_restart_ctx *rs_ctx)
{
    uint32_t c0 = ns_pstats_cyc();
    int r = __real_mbedtls_ecp_muladd_restartable(grp, R, m, P, n, Q, rs_ctx);
    ns_pstats_kind(PS_K_ECP, ns_pstats_cyc() - c0);
    return r;
}
