/*
 * FG25 measurement firmware for the fire alarm mesh stack's radio (internal
 * measuring equipment, never delivered). The PHY is the specification's:
 * 300 kbit/s 2-FSK, 8-byte preamble, 16-bit sync word, CRC-32 (IEEE 802.3),
 * here with a fixed 16-byte payload (radio_settings.radioconf).
 *
 * Built with -D options (build.sh):
 *   FM_ROLE 0  listener for current: a receive window of FM_WIN_US every
 *              FM_PERIOD_US, deep sleep (EM2) in between; FM_WIN_US 0: no
 *              radio at all (the floor)
 *   FM_ROLE 1  the same windows, and one line per received frame on the
 *              console (fields below) and one "W <window>" line per window
 *   FM_ROLE 2  sender: one frame every FM_TXPER_US, scheduled on the radio's
 *              clock, deep sleep in between (FM_EM1 1: never below EM1, so
 *              the radio's clock stays on the high-frequency crystal)
 *   FM_ROLE 3  the processor only, woken every FM_PERIOD_US by the sleep
 *              timer (FM_WAKE_FLAGS: the sleep timer's option flags; 1: no
 *              high-frequency crystal needed at the wake; FM_NORAILPM 1:
 *              RAIL's power-manager hooks removed)
 *   FM_ROLE 4  continuous receiver: one line per received frame (below)
 *   FM_ROLE 5  responder: continuous receiver that answers each frame
 *              FM_TURN_US after its end (radio timestamp), optionally after
 *              a single carrier sense of FM_CCA_US at FM_CCA_DBM
 *   FM_ROLE 6  initiator: a frame every FM_TXPER_US, then receive until the
 *              next one; one line per answer received
 *   FM_ROLE 8  interferer: a continuous PN9-modulated carrier on FM_CHAN
 *   FM_ROLE 9  sender timed by the sleep timer alone: a periodic timer of
 *              FM_TICKS ticks (32768 Hz), a frame sent at once after each
 *              wake, deep sleep in between (the sleep clock, without RAIL's
 *              time across deep sleep)
 *   FM_ROLE 2 with FM_CCA_US > 0: each frame after a single carrier sense;
 *              one line "B <st>" per attempt (0 sent, 2 channel busy)
 *
 * Options for every role: FM_TSYNC 1: RAIL keeps its timebase across deep
 * sleep by synchronising to the low-frequency clock (sl_rail_config_sleep);
 * FM_PMOVH: added to the power manager's early wake-up before a timer that
 * needs the high-frequency crystal (sleep-timer ticks of 30.5 us, may be
 * negative); FM_CHAN: the channel (0..10, 600 kHz apart from 863.5 MHz); FM_GUARD_US: the
 * shortest lead for scheduling the next window or frame (a slot closer than
 * that is skipped).
 *
 * Console lines (all times in us of the radio's clock):
 *   R <win> <E-W> <pos> <dur> <S-W> <C-W>      role 1, W = window start
 *   W <win> <h>                                role 1, after each window; h: the
 *                                              crystal's start-up time as the HFXO
 *                                              manager estimates it (sleep-timer ticks)
 *   F <E> <pos> <dur> <S-E> <C-E> <N-E> <L>    role 4
 *   A <E> <st> <P-E> <T-E> <S-E> <C-E>         role 5, one per answer
 *   I <Tx> <E-Tx> <dur> <S-Tx> <pos> <txpos>    role 6, one per answer
 * E: the received frame's default timestamp (position code pos, 6 = frame
 * end) as RAIL returns it; dur: RAIL's on-air duration from the sync word's
 * end (EFR32xG25); S: software time of the sync-word event; C: software time
 * of the frame-received event; N/L: the radio's time and the sleep timer's
 * tick count read together; P/T: this kit's answer, preamble start and end
 * as its TX timestamps give them; st: 0 sent, 1 missed, 2 channel busy,
 * 10 + RAIL's status when the transmit could not be scheduled;
 * Tx: the initiator's frame end (TX timestamp).
 */
#include <stdio.h>
#include "sl_component_catalog.h"
#include "sl_rail.h"
#include "sl_rail_util_init.h"
#include "fm_cfg.h"           /* written by build.sh */
#include "sl_sleeptimer.h"
#include "sl_power_manager.h"
#include "em_core.h"
#include "sli_hfxo_manager.h"

#ifndef FM_ROLE
#define FM_ROLE 0
#endif
#ifndef FM_WIN_US
#define FM_WIN_US 300
#endif
#ifndef FM_PERIOD_US
#define FM_PERIOD_US 200000
#endif
#ifndef FM_TXPER_US
#define FM_TXPER_US 1013
#endif
#ifndef FM_EM1
#define FM_EM1 0
#endif
#ifndef FM_WAKE_FLAGS
#define FM_WAKE_FLAGS 0
#endif
#ifndef FM_NORAILPM
#define FM_NORAILPM 0
#endif
#ifndef FM_TSYNC
#define FM_TSYNC 0
#endif
#ifndef FM_CHAN
#define FM_CHAN 0
#endif
#ifndef FM_TURN_US
#define FM_TURN_US 400
#endif
#ifndef FM_CCA_US
#define FM_CCA_US 0
#endif
#ifndef FM_CCA_DBM
#define FM_CCA_DBM (-75)
#endif
#ifndef FM_GUARD_US
#define FM_GUARD_US 200
#endif
#ifndef FM_PAYLOAD
#define FM_PAYLOAD 16
#endif

#define CONSOLE (FM_ROLE == 1 || (FM_ROLE >= 4 && FM_ROLE <= 6) || (FM_ROLE == 2 && FM_CCA_US > 0))

static sl_rail_handle_t h;
static sl_rail_time_t t0;
static uint32_t k;
static volatile bool ev_next;
static sl_rail_fifo_buffer_align_t fifo[64];
static const uint8_t payload[FM_PAYLOAD] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x06, 0x60, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
static bool started;

#if CONSOLE
/* records filled in the radio's callbacks, printed from the main loop */
typedef struct { char tag; int32_t v[7]; } rec_t;
static rec_t ring[32];
static volatile uint32_t r_in, r_out;
static void put(char tag, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, int32_t g)
{
  if (r_in - r_out >= 32) return;
  rec_t *r = &ring[r_in % 32];
  r->tag = tag; r->v[0] = a; r->v[1] = b; r->v[2] = c; r->v[3] = d; r->v[4] = e; r->v[5] = f; r->v[6] = g;
  r_in++;
}
static volatile sl_rail_time_t t_sync;     /* software time of the last sync-word event */
#endif

#if FM_ROLE == 5 || FM_ROLE == 2
static const sl_rail_csma_config_t cca = {
  .csma_min_bo_exp = 0, .csma_max_bo_exp = 0, .csma_tries = 1,
  .cca_threshold_dbm = FM_CCA_DBM, .cca_backoff_us = 0, .cca_duration_us = FM_CCA_US, .csma_timeout_us = 0,
};
#endif
#if FM_ROLE == 5
static volatile sl_rail_time_t a_e, a_s, a_c;
#endif
#if FM_ROLE == 6
static volatile sl_rail_time_t i_tx;
static volatile int32_t i_txpos;
#endif

static sl_rail_status_t schedule(void)
{
  sl_rail_time_t now = sl_rail_get_time(h);
#if FM_ROLE == 9
  (void)now;
  return SL_RAIL_STATUS_NO_ERROR;
#elif FM_ROLE == 2 || FM_ROLE == 6
  /* the next slot at least FM_GUARD_US ahead (RAIL's time wraps; differences are signed) */
  do { k++; } while ((int32_t)(t0 + k * FM_TXPER_US - now) < FM_GUARD_US);
  sl_rail_write_tx_fifo(h, payload, sizeof payload, true);
  sl_rail_scheduled_tx_config_t c = { .when = t0 + k * FM_TXPER_US, .mode = SL_RAIL_TIME_ABSOLUTE,
                                      .tx_during_rx = SL_RAIL_SCHEDULED_TX_DURING_RX_ABORT_TX };
#if FM_ROLE == 2 && FM_CCA_US > 0
  return sl_rail_start_scheduled_cca_csma_tx(h, FM_CHAN, SL_RAIL_TX_OPTIONS_DEFAULT, &c, &cca, NULL);
#else
  return sl_rail_start_scheduled_tx(h, FM_CHAN, SL_RAIL_TX_OPTIONS_DEFAULT, &c, NULL);
#endif
#elif FM_ROLE == 8
  (void)now;
  return sl_rail_start_tx_stream(h, FM_CHAN, SL_RAIL_STREAM_PN9_STREAM, SL_RAIL_TX_OPTIONS_DEFAULT);
#elif FM_ROLE == 4 || FM_ROLE == 5
  (void)now;
  return sl_rail_start_rx(h, FM_CHAN, NULL);
#else
  if (FM_WIN_US == 0) return SL_RAIL_STATUS_NO_ERROR;
  do { k++; } while ((int32_t)(t0 + k * FM_PERIOD_US - now) < FM_GUARD_US);
  sl_rail_scheduled_rx_config_t c = {
    .start = t0 + k * FM_PERIOD_US, .start_mode = SL_RAIL_TIME_ABSOLUTE,
    .end = t0 + k * FM_PERIOD_US + FM_WIN_US, .end_mode = SL_RAIL_TIME_ABSOLUTE,
    .rx_transition_end_schedule = 1, .hard_window_end = 0,
  };
  return sl_rail_start_scheduled_rx(h, FM_CHAN, &c, NULL);
#endif
}

static void radio_setup(void)
{
  h = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
#if FM_TSYNC
  sl_rail_deinit_power_manager();
  sl_rail_timer_sync_config_t ts = SL_RAIL_TIMER_SYNC_DEFAULT;
  sl_rail_config_sleep(h, &ts);
  sl_rail_init_power_manager();
#endif
  sl_rail_config_events(h, SL_RAIL_EVENTS_ALL,
                        SL_RAIL_EVENT_RX_PACKET_RECEIVED | SL_RAIL_EVENT_RX_SCHEDULED_RX_END
                        | SL_RAIL_EVENT_RX_SCHEDULED_RX_MISSED | SL_RAIL_EVENT_TX_PACKET_SENT
                        | SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED | SL_RAIL_EVENT_TX_CHANNEL_BUSY
                        | SL_RAIL_EVENT_TX_BLOCKED | SL_RAIL_EVENT_TX_UNDERFLOW
#if CONSOLE
                        | SL_RAIL_EVENT_RX_SYNC_0_DETECT
#endif
                        );
  sl_rail_set_tx_fifo(h, fifo, sizeof fifo, 0, 0);
#if FM_ROLE == 4 || FM_ROLE == 5 || FM_ROLE == 6
  /* stay in receive after a frame; the initiator and the responder receive after sending */
  sl_rail_state_transitions_t rx = { .success = SL_RAIL_RF_STATE_RX, .error = SL_RAIL_RF_STATE_RX };
  sl_rail_set_rx_transitions(h, &rx);
#endif
#if FM_ROLE == 5 || FM_ROLE == 6
  sl_rail_state_transitions_t tx = { .success = SL_RAIL_RF_STATE_RX, .error = SL_RAIL_RF_STATE_RX };
  sl_rail_set_tx_transitions(h, &tx);
#endif
}

#if FM_ROLE == 3
static sl_sleeptimer_timer_handle_t wt;
static void wake_cb(sl_sleeptimer_timer_handle_t *t, void *d) { (void)t; (void)d; }
#endif
#if FM_ROLE == 9
/* sender timed by the sleep timer alone: a frame sent right after each wake */
#ifndef FM_TICKS
#define FM_TICKS 3277
#endif
static sl_sleeptimer_timer_handle_t wt;
static volatile bool tx_due;
static void wake_cb(sl_sleeptimer_timer_handle_t *t, void *d) { (void)t; (void)d; tx_due = true; }
#endif

void app_init(void)
{
#ifdef FM_PMOVH
  /* the power manager's early wake before a timer that needs the crystal, in sleep-timer ticks (may be negative) */
  sl_power_manager_schedule_wakeup_set_restore_overhead_tick(FM_PMOVH);
#endif
#if FM_ROLE == 9
  sl_sleeptimer_start_periodic_timer(&wt, FM_TICKS, wake_cb, NULL, 0, 0);
#endif
#if FM_ROLE == 3
  sl_sleeptimer_start_periodic_timer_ms(&wt, FM_PERIOD_US / 1000, wake_cb, NULL, 0, FM_WAKE_FLAGS);
#if FM_NORAILPM
  sl_rail_deinit_power_manager();
#endif
#endif
#if FM_EM1
  sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
#endif
#if CONSOLE
  printf("fmmeas role %d window %d period %d txper %d turn %d cca %d tsync %d\n", FM_ROLE, FM_WIN_US,
         FM_PERIOD_US, FM_TXPER_US, FM_TURN_US, FM_CCA_US, FM_TSYNC);
#endif
}

void app_process_action(void)
{
#if FM_ROLE == 3
  return;
#endif
  if (!started) {
    /* the radio may not be ready at app_init: retry until it accepts */
    radio_setup();
    t0 = sl_rail_get_time(h) + 100000;
    k = 0;
    sl_rail_status_t st = schedule();
    started = st == SL_RAIL_STATUS_NO_ERROR;
#if CONSOLE
    static bool said;
    if (!said || started) { printf("schedule status %d\n", (int)st); said = true; }
#endif
    return;
  }
#if CONSOLE
  while (r_out != r_in) {
    rec_t *r = &ring[r_out % 32];
    if (r->tag == 'W') printf("W %ld %ld\n", (long)r->v[0], (long)r->v[1]);
    else printf("%c %ld %ld %ld %ld %ld %ld %ld\n", r->tag, (long)r->v[0], (long)r->v[1], (long)r->v[2],
                (long)r->v[3], (long)r->v[4], (long)r->v[5], (long)r->v[6]);
    r_out++;
  }
#endif
#if FM_ROLE == 9
  if (tx_due) {
    tx_due = false;
    sl_rail_write_tx_fifo(h, payload, sizeof payload, true);
    sl_rail_start_tx(h, FM_CHAN, SL_RAIL_TX_OPTIONS_DEFAULT, NULL);
  }
#endif
  if (ev_next) {
    ev_next = false;
#if FM_ROLE == 1
    put('W', (int32_t)k, (int32_t)sli_hfxo_manager_get_startup_time(), 0, 0, 0, 0, 0);
#endif
#if FM_ROLE != 4 && FM_ROLE != 5 && FM_ROLE != 8 && FM_ROLE != 9
    schedule();
#endif
  }
}

#if CONSOLE
/* the received frame's default timestamp, its position and RAIL's duration */
static bool rx_frame(sl_rail_handle_t rh, sl_rail_time_t *e, int32_t *pos, int32_t *dur)
{
  sl_rail_rx_packet_info_t info;
  sl_rail_rx_packet_handle_t ph = sl_rail_get_rx_packet_info(rh, SL_RAIL_RX_PACKET_HANDLE_NEWEST, &info);
  sl_rail_rx_packet_details_t d = { 0 };
  if (sl_rail_get_rx_packet_details(rh, ph, &d) != SL_RAIL_STATUS_NO_ERROR || !d.crc_passed) return false;
  *e = d.time_received.packet_time;
  *pos = d.time_received.time_position;
  *dur = (int32_t)d.time_received.packet_duration_us;
  return true;
}
#endif

void sl_rail_util_on_event(sl_rail_handle_t rail_handle, sl_rail_events_t events)
{
#if CONSOLE
  sl_rail_time_t now = sl_rail_get_time(rail_handle);
  if (events & SL_RAIL_EVENT_RX_SYNC_0_DETECT) t_sync = now;
#endif
  if (events & SL_RAIL_EVENT_RX_PACKET_RECEIVED) {
#if CONSOLE
    sl_rail_time_t e; int32_t pos, dur;
    if (rx_frame(rail_handle, &e, &pos, &dur)) {
#if FM_ROLE == 1
      sl_rail_time_t w = t0 + k * FM_PERIOD_US;
      put('R', (int32_t)k, (int32_t)(e - w), pos, dur, (int32_t)(t_sync - w), (int32_t)(now - w), 0);
#elif FM_ROLE == 4
      CORE_DECLARE_IRQ_STATE;
      CORE_ENTER_CRITICAL();
      sl_rail_time_t n = sl_rail_get_time(rail_handle);
      uint32_t l = sl_sleeptimer_get_tick_count();
      CORE_EXIT_CRITICAL();
      put('F', (int32_t)e, pos, dur, (int32_t)(t_sync - e), (int32_t)(now - e), (int32_t)(n - e), (int32_t)l);
#elif FM_ROLE == 5
      sl_rail_write_tx_fifo(rail_handle, payload, sizeof payload, true);
      sl_rail_scheduled_tx_config_t c = { .when = e + FM_TURN_US, .mode = SL_RAIL_TIME_ABSOLUTE,
                                          .tx_during_rx = SL_RAIL_SCHEDULED_TX_DURING_RX_POSTPONE_TX };
      sl_rail_status_t ts;
      if (FM_CCA_US > 0) ts = sl_rail_start_scheduled_cca_csma_tx(rail_handle, FM_CHAN, SL_RAIL_TX_OPTIONS_DEFAULT, &c, &cca, NULL);
      else ts = sl_rail_start_scheduled_tx(rail_handle, FM_CHAN, SL_RAIL_TX_OPTIONS_DEFAULT, &c, NULL);
      a_e = e; a_s = t_sync; a_c = now;
      if (ts != SL_RAIL_STATUS_NO_ERROR) put('A', (int32_t)e, 10 + (int32_t)ts, 0, 0, (int32_t)(t_sync - e), (int32_t)(now - e), 0);
      (void)pos; (void)dur;
#elif FM_ROLE == 6
      put('I', (int32_t)i_tx, (int32_t)(e - i_tx), dur, (int32_t)(t_sync - i_tx), pos, i_txpos, 0);
#endif
    }
#endif
  }
#if FM_ROLE == 5
  if (events & (SL_RAIL_EVENT_TX_PACKET_SENT | SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED | SL_RAIL_EVENT_TX_CHANNEL_BUSY
                | SL_RAIL_EVENT_TX_BLOCKED | SL_RAIL_EVENT_TX_UNDERFLOW)) {
    int32_t st = (events & SL_RAIL_EVENT_TX_PACKET_SENT) ? 0 : (events & SL_RAIL_EVENT_TX_CHANNEL_BUSY) ? 2 : 1;
    int32_t p = 0, t = 0;
    if (st == 0) {
      sl_rail_tx_packet_details_t d = { 0 };
      if (sl_rail_get_tx_packet_details(rail_handle, &d) == SL_RAIL_STATUS_NO_ERROR) {
        t = (int32_t)(d.time_sent.packet_time - a_e);
        p = t - (int32_t)d.time_sent.packet_duration_us;
      }
    }
    put('A', (int32_t)a_e, st, p, t, (int32_t)(a_s - a_e), (int32_t)(a_c - a_e), 0);
  }
#endif
#if FM_ROLE == 6
  if (events & SL_RAIL_EVENT_TX_PACKET_SENT) {
    sl_rail_tx_packet_details_t d = { 0 };
    if (sl_rail_get_tx_packet_details(rail_handle, &d) == SL_RAIL_STATUS_NO_ERROR) {
      i_tx = d.time_sent.packet_time; i_txpos = d.time_sent.time_position;
    }
    ev_next = true;
  }
  if (events & (SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED | SL_RAIL_EVENT_TX_BLOCKED | SL_RAIL_EVENT_TX_UNDERFLOW)) ev_next = true;
#elif FM_ROLE != 4 && FM_ROLE != 5 && FM_ROLE != 8
#if FM_ROLE == 2 && FM_CCA_US > 0
  if (events & (SL_RAIL_EVENT_TX_PACKET_SENT | SL_RAIL_EVENT_TX_CHANNEL_BUSY | SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED))
    put('B', (events & SL_RAIL_EVENT_TX_PACKET_SENT) ? 0 : (events & SL_RAIL_EVENT_TX_CHANNEL_BUSY) ? 2 : 1, 0, 0, 0, 0, 0, 0);
  if (events & SL_RAIL_EVENT_TX_CHANNEL_BUSY) ev_next = true;
#endif
  if (events & (SL_RAIL_EVENT_RX_SCHEDULED_RX_END | SL_RAIL_EVENT_RX_SCHEDULED_RX_MISSED
                | SL_RAIL_EVENT_TX_PACKET_SENT | SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED))
    ev_next = true;
#endif
}
