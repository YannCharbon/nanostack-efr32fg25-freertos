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
 *              console: "R <window> <preamble start - window start, us>"
 *              and one "W <window>" line per window
 *   FM_ROLE 2  sender: one frame every FM_TXPER_US, scheduled on the radio's
 *              clock, deep sleep in between
 */
#include <stdio.h>
#include "sl_component_catalog.h"
#include "sl_rail.h"
#include "sl_rail_util_init.h"
#include "fm_cfg.h"           /* written by build.sh */
#include "sl_sleeptimer.h"

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

static sl_rail_handle_t h;
static sl_rail_time_t t0;
static uint32_t k;
static volatile bool ev_next;
static volatile uint32_t rx_count;
static volatile int32_t rx_off;
static volatile bool rx_new;
static sl_rail_fifo_buffer_align_t fifo[64];
static const uint8_t payload[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x06, 0x60, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };

static bool started;

static sl_rail_status_t schedule(void)
{
  sl_rail_time_t now = sl_rail_get_time(h);
#if FM_ROLE == 2
  uint32_t n = (uint32_t)(now - t0) / FM_TXPER_US + 2;
  if (n <= k) n = k + 1;
  k = n;
  sl_rail_write_tx_fifo(h, payload, sizeof payload, true);
  sl_rail_scheduled_tx_config_t c = { .when = t0 + k * FM_TXPER_US, .mode = SL_RAIL_TIME_ABSOLUTE };
  return sl_rail_start_scheduled_tx(h, 0, SL_RAIL_TX_OPTIONS_DEFAULT, &c, NULL);
#else
  if (FM_WIN_US == 0) return SL_RAIL_STATUS_NO_ERROR;
  uint32_t n = (uint32_t)(now - t0) / FM_PERIOD_US + 1;
  if (n <= k) n = k + 1;
  k = n;
  sl_rail_scheduled_rx_config_t c = {
    .start = t0 + k * FM_PERIOD_US, .start_mode = SL_RAIL_TIME_ABSOLUTE,
    .end = t0 + k * FM_PERIOD_US + FM_WIN_US, .end_mode = SL_RAIL_TIME_ABSOLUTE,
    .rx_transition_end_schedule = 1, .hard_window_end = 0,
  };
  return sl_rail_start_scheduled_rx(h, 0, &c, NULL);
#endif
}

static void radio_setup(void)
{
  h = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
  sl_rail_config_events(h, SL_RAIL_EVENTS_ALL,
                        SL_RAIL_EVENT_RX_PACKET_RECEIVED | SL_RAIL_EVENT_RX_SCHEDULED_RX_END
                        | SL_RAIL_EVENT_RX_SCHEDULED_RX_MISSED | SL_RAIL_EVENT_TX_PACKET_SENT
                        | SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED);
  sl_rail_set_tx_fifo(h, fifo, sizeof fifo, 0, 0);
}

#if FM_ROLE == 3
/* FM_ROLE 3: the processor only, woken every FM_PERIOD_US by the sleep timer */
static sl_sleeptimer_timer_handle_t wt;
static void wake_cb(sl_sleeptimer_timer_handle_t *t, void *d) { (void)t; (void)d; }
#endif

void app_init(void)
{
#if FM_ROLE == 3
  sl_sleeptimer_start_periodic_timer_ms(&wt, FM_PERIOD_US / 1000, wake_cb, NULL, 0, 0);
#endif
#if FM_ROLE == 1
  printf("fmmeas role %d window %d period %d\n", FM_ROLE, FM_WIN_US, FM_PERIOD_US);
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
#if FM_ROLE == 1
    static bool said;
    if (!said || started) { printf("schedule status %d\n", (int)st); said = true; }
#endif
    return;
  }
#if FM_ROLE == 1
  if (rx_new) { rx_new = false; printf("R %lu %ld\n", (unsigned long)k, (long)rx_off); }
#endif
  if (ev_next) {
    ev_next = false;
#if FM_ROLE == 1
    printf("W %lu\n", (unsigned long)k);
#endif
    schedule();
  }
}

void sl_rail_util_on_event(sl_rail_handle_t rail_handle, sl_rail_events_t events)
{
  if (events & SL_RAIL_EVENT_RX_PACKET_RECEIVED) {
    sl_rail_rx_packet_info_t info;
    sl_rail_rx_packet_handle_t ph = sl_rail_get_rx_packet_info(rail_handle, SL_RAIL_RX_PACKET_HANDLE_NEWEST, &info);
    sl_rail_rx_packet_details_t d = { 0 };
    d.time_received.time_position = SL_RAIL_PACKET_TIME_AT_PREAMBLE_START;
    d.time_received.total_packet_bytes = info.packet_bytes;
    if (sl_rail_get_rx_packet_details(rail_handle, ph, &d) == SL_RAIL_STATUS_NO_ERROR && d.crc_passed) {
      rx_count++;
      rx_off = (int32_t)(d.time_received.packet_time - (t0 + k * FM_PERIOD_US));
      rx_new = true;
    }
  }
  if (events & (SL_RAIL_EVENT_RX_SCHEDULED_RX_END | SL_RAIL_EVENT_RX_SCHEDULED_RX_MISSED
                | SL_RAIL_EVENT_TX_PACKET_SENT | SL_RAIL_EVENT_TX_SCHEDULED_TX_MISSED))
    ev_next = true;
}
