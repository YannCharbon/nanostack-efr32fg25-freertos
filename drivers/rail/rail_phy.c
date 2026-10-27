/*
 * Nanostack radio driver (phy_device_driver_s, "dynamic RF" mode) over RAIL
 * 2.x for the EFR32FG25 and the Wi-SUN FAN 1.0 FSK PHY of the generated
 * rail_config.c (NA / class 1 / mode 1b: 902.2 MHz + n*200 kHz, 129 channels,
 * 50 kbit/s 2-FSK, h = 1).
 *
 * Contract: GENERICITY_PLAN.md §3.2 and the emulator's reference driver
 * (mesh-emulator-orchestrator app_wshwsim/sl_rf_driver.c). Execution model
 * (Phase A, §2.3 / §5.5):
 *   - RAIL callbacks (ISR) only copy RX frames into a ring, record TX event
 *     bits, mark the "prepare" timer expired and notify the stack task;
 *   - rail_phy_process() (bottom half, a port poll hook in the stack task)
 *     calls phy_rx_cb() / phy_tx_done_cb() and runs the CCA_PREPARE step;
 *   - all extension()/tx()/state_control() calls come from the stack task.
 *
 * TX sequence for one MAC transmission:
 *   SET_CSMA_PARAMETERS(backoff_time = absolute µs, cca_enabled)  [MAC]
 *   tx(psdu)                         -> store, arm prepare timer at
 *                                       backoff_time - PREPARE_US
 *   prepare timer (stack task)       -> phy_tx_done_cb(CCA_PREPARE): the MAC
 *                                       / FHSS may switch channel here
 *     PHY_TX_ALLOWED                 -> RAIL scheduled CCA+TX (or TX) at
 *                                       backoff_time, on the current channel
 *     PHY_RESTART_CSMA               -> one CCA now (RSSI), report CCA_OK /
 *                                       CCA_FAIL; on OK re-arm for the new
 *                                       backoff_time the MAC just set
 *     PHY_TX_NOT_ALLOWED             -> drop, back to RX
 *   RAIL TX_PACKET_SENT              -> PHY_LINK_TX_SUCCESS (MAC waits for the
 *                                       ACK itself; radio is back in RX)
 *   RAIL TX_CHANNEL_BUSY             -> PHY_LINK_CCA_FAIL
 *   SET_CSMA_PARAMETERS(0)           -> abort pending/active TX, back to RX
 * Like the reference driver, CCA_OK is reported only on the RESTART_CSMA
 * path: on the TX_ALLOWED path the MAC expects TX_SUCCESS/CCA_FAIL only
 * (CCA_OK there would make FHSS return to the RX channel mid-TX).
 *
 * Frame format: 802.15.4 SUN-FSK, 2-byte PHR written by this driver, radio
 * computes/checks the FCS (RAIL_IEEE802154_G_OPTION_GB868: FCS length and
 * whitening follow the PHR bits). The MAC never sees PHR or FCS.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "em_device.h"
#include "em_system.h"
#include "rail.h"
#include "rail_ieee802154.h"
#include "rail_config.h"
#include "pa_conversions_efr32.h"

#include "nanostack/mac/platform/arm_hal_phy.h"
#include "mbed-client-libservice/common_functions.h"
#include "mbed-client-libservice/ns_trace.h"
#include "ns_port/critical.h"
#include "ns_port.h"
#include "rail_phy.h"
#include "rail_phy_config.h"

#define TRACE_GROUP "rphy"

/* Max PSDU the MAC hands us (2047-byte SUN PSDU minus 4-byte FCS). */
#define PHY_MTU         2043
#define PHR_LEN         2
#define RX_BUF_LEN      (PHR_LEN + 2047)

/* ------------------------------------------------------------------ state */

static RAIL_Handle_t rh;
static phy_device_driver_s drv;
static int8_t drv_id = -1;
static uint8_t mac64[8];

static uint16_t rail_ch_offset;      /* RAIL channel = offset + Wi-SUN channel */
static uint16_t rail_ch_count;
static uint32_t symbol_rate;

static bool radio_on;
static uint8_t cur_channel;          /* Wi-SUN channel number */
static int rx_channel = -1;          /* channel RAIL is receiving on, -1 = not in RX */

/* TX */
enum tx_state { TXS_IDLE, TXS_WAIT_PREPARE, TXS_PREPARING, TXS_ACTIVE };
static enum tx_state tx_state;
static uint8_t tx_buf[PHY_MTU];
static uint16_t tx_len;
static uint8_t tx_handle;
static uint32_t csma_backoff_time;   /* absolute µs, 0 = none */
static bool csma_cca_enabled;
static bool tx_is_ack;
static uint32_t tx_start_time;
static int8_t cca_threshold_dbm = RAIL_PHY_CCA_THRESHOLD_DBM;
static bool whitening = RAIL_PHY_WHITENING;
static RAIL_MultiTimer_t prepare_timer;
static volatile bool prepare_due;
/* TX watchdog: RAIL can accept a scheduled CCA+TX and then never report it
 * (seen when the stack masks the RAIL IRQs across the scheduled start). The
 * prepare timer, idle while TXS_ACTIVE, is re-armed as a deadline. */
static volatile bool tx_watchdog_due;
#define RAIL_PHY_TX_WATCHDOG_MARGIN_US 20000u
static volatile RAIL_Events_t pending_tx_events;
/* Channel change requested while a frame is being received (FHSS unicast hop
 * or broadcast slot edge in the middle of a long frame). RAIL_StartRx() on
 * another channel aborts the reception, so with RAIL_PHY_HOP_DEFER the
 * switch waits for the end of the frame (the emulator's reference driver
 * refuses the change in DRVSTATE_RX for the same reason). */
static volatile bool hop_pending;
static uint32_t hop_pending_since;
static struct {
    bool due;
    uint8_t from, to;
    uint16_t bytes;
    uint32_t time;
} hop_ev;
#define RAIL_PHY_HOP_DEFER_MAX_US 400000u   /* > air time of a 2049-byte frame */
static volatile bool cal_needed;

/* RX ring: filled by the RAIL ISR, drained by rail_phy_process() */
struct rx_slot {
    uint16_t len;          /* PSDU length (no PHR, no FCS) */
    int8_t rssi;
    uint8_t lqi;
    uint32_t time;         /* sync-word end, RAIL µs */
    uint32_t end;          /* estimated end of frame (FCS end), RAIL µs */
    uint16_t rail_ch;      /* RAIL channel the frame was received on */
    uint8_t buf[RX_BUF_LEN];
};
static struct rx_slot rx_ring[RAIL_PHY_RX_SLOTS];
static volatile uint32_t rx_head, rx_tail;
static uint32_t last_rx_time;        /* sync-word end of the last delivered frame */
static uint32_t last_rx_end;         /* estimated end of that frame */
static uint16_t last_rx_rail_ch = 0xffff; /* its RAIL channel (Enh-ACK goes there) */

/* RAIL FIFOs */
static uint8_t tx_fifo[RAIL_PHY_TX_FIFO_SIZE] __attribute__((aligned(4)));
static uint8_t rx_fifo[RAIL_PHY_RX_FIFO_SIZE] __attribute__((aligned(4)));

static struct rail_phy_stats stats;

/* Filled in rail_phy_hw_init() from the generated rail_config.c (channel
 * plan) and rail_phy_config.h (data rate, modulation index of the region). */
static phy_rf_channel_configuration_s phy_cfg = {
    .datarate = RAIL_PHY_DATARATE,
    .modulation = M_2FSK,
    .modulation_index = RAIL_PHY_MOD_INDEX,
};
static const phy_device_channel_page_s phy_channel_pages[] = {
    { CHANNEL_PAGE_0, &phy_cfg },
    { CHANNEL_PAGE_0, NULL },
};

#define TX_EVENTS (RAIL_EVENT_TX_PACKET_SENT | RAIL_EVENT_TX_CHANNEL_BUSY | RAIL_EVENT_TX_ABORTED \
                   | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW | RAIL_EVENT_TX_SCHEDULED_TX_MISSED)
#define RX_EVENTS (RAIL_EVENT_RX_PACKET_RECEIVED | RAIL_EVENT_RX_FRAME_ERROR | RAIL_EVENT_RX_FIFO_OVERFLOW \
                   | RAIL_EVENT_RX_PACKET_ABORTED)

/* --------------------------------------------------------------- helpers */

static inline uint8_t rbit8(uint8_t v)
{
    v = (uint8_t)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = (uint8_t)((v & 0xCC) >> 2 | (v & 0x33) << 2);
    v = (uint8_t)((v & 0xAA) >> 1 | (v & 0x55) << 1);
    return v;
}

/* 802.15.4-2015 SUN FSK PHR, as RAIL expects it in the FIFO (bit k of the
 * first byte = PHR bit k, sent first): MS(0)=0, reserved, FCS type (1 =
 * 2-octet FCS), DW, then the 11-bit length MSB first. Length includes FCS. */
static void phr_build(uint8_t phr[2], uint16_t psdu_len)
{
    uint16_t len = psdu_len + RAIL_PHY_FCS_LEN;
    uint8_t b0 = 0;

    if (RAIL_PHY_FCS_LEN == 2) {
        b0 |= 0x08;
    }
    if (whitening) {
        b0 |= 0x10;
    }
    b0 |= (uint8_t)(((len >> 10) & 1) << 5 | ((len >> 9) & 1) << 6 | ((len >> 8) & 1) << 7);
    phr[0] = b0;
    phr[1] = rbit8((uint8_t)(len & 0xFF));
}

static inline uint16_t rail_channel(uint8_t ws_channel)
{
    return rail_ch_offset + ws_channel;
}

static inline bool rx_frame_in_progress(void)
{
    return (RAIL_GetRadioState(rh) & RAIL_RF_STATE_RX_ACTIVE) == RAIL_RF_STATE_RX_ACTIVE;
}

static void start_rx(void)
{
    if (!radio_on || tx_state == TXS_ACTIVE) {
        return;
    }
    if (cur_channel >= rail_ch_count) {
        /* The MAC brings the PHY up on channel 0xff before FHSS has picked a
         * channel; any other out-of-range value is a configuration mismatch. */
        if (cur_channel != 0xff) {
            tr_err("channel %u out of radio config range (%u)", cur_channel, rail_ch_count);
        }
        return;
    }
    if (rx_channel == cur_channel && (RAIL_GetRadioState(rh) & RAIL_RF_STATE_RX)) {
        hop_pending = false;
        return;
    }
    if (rx_channel >= 0 && rx_frame_in_progress()) {
        if (!hop_pending && !hop_ev.due) {
            RAIL_RxPacketInfo_t info;
            hop_ev.from = (uint8_t)rx_channel;
            hop_ev.to = cur_channel;
            hop_ev.bytes = RAIL_GetRxIncomingPacketInfo(rh, &info) == RAIL_STATUS_NO_ERROR ? info.packetBytes : 0xffff;
            hop_ev.time = RAIL_GetTime();
            hop_ev.due = true;
        }
#if RAIL_PHY_HOP_DEFER
        if (!hop_pending) {
            hop_pending = true;
            hop_pending_since = RAIL_GetTime();
            stats.rx_hop_deferred++;
        }
        return;     /* rail_phy_process() switches at the end of the frame */
#else
        stats.rx_hop_abort++;
#endif
    }
    hop_pending = false;
    if (RAIL_StartRx(rh, rail_channel(cur_channel), NULL) == RAIL_STATUS_NO_ERROR) {
        rx_channel = cur_channel;
    } else {
        rx_channel = -1;
    }
}

static void tx_done(phy_link_tx_status_e status)
{
    if (drv.phy_tx_done_cb) {
        drv.phy_tx_done_cb(drv_id, tx_handle, status, 0, 0);
    }
}

/* ------------------------------------------------------------ RAIL (ISR) */

static void prepare_timer_isr(RAIL_MultiTimer_t *tmr, RAIL_Time_t expected, void *arg)
{
    (void)tmr;
    (void)expected;
    (void)arg;
    prepare_due = true;
    ns_port_notify_from_isr();
}

static void tx_watchdog_isr(RAIL_MultiTimer_t *tmr, RAIL_Time_t expected, void *arg)
{
    (void)tmr;
    (void)expected;
    (void)arg;
    tx_watchdog_due = true;
    ns_port_notify_from_isr();
}

static void rx_capture_isr(void)
{
    RAIL_RxPacketInfo_t info;
    RAIL_RxPacketDetails_t details;
    RAIL_RxPacketHandle_t h;
    struct rx_slot *slot;
    uint8_t fcs_len;

    h = RAIL_GetRxPacketInfo(rh, RAIL_RX_PACKET_HANDLE_NEWEST, &info);
    if (h == RAIL_RX_PACKET_HANDLE_INVALID || info.packetStatus != RAIL_RX_PACKET_READY_SUCCESS) {
        return;
    }
    if (info.packetBytes < PHR_LEN + 3 || info.packetBytes > RX_BUF_LEN) {
        return;
    }
    if (rx_head - rx_tail >= RAIL_PHY_RX_SLOTS) {
        stats.rx_ring_full++;
        return;
    }
    slot = &rx_ring[rx_head % RAIL_PHY_RX_SLOTS];
    fcs_len = (info.firstPortionBytes ? info.firstPortionData[0] : info.lastPortionData[0]) & 0x08 ? 2 : 4;
    if (RAIL_GetRxPacketDetailsAlt(rh, h, &details) != RAIL_STATUS_NO_ERROR) {
        return;
    }
    details.timeReceived.totalPacketBytes = info.packetBytes + fcs_len;
    if (RAIL_GetRxTimeSyncWordEndAlt(rh, &details) != RAIL_STATUS_NO_ERROR) {
        details.timeReceived.packetTime = RAIL_GetTime();
    }
    RAIL_CopyRxPacket(slot->buf, &info);
    slot->len = info.packetBytes - PHR_LEN;
    slot->rssi = details.rssi;
    slot->lqi = details.lqi;
    slot->time = details.timeReceived.packetTime;
    slot->end = slot->time + (uint32_t)(((uint64_t)(info.packetBytes + fcs_len) * 8u * 1000000u) / symbol_rate);
    if (RAIL_GetChannel(rh, &slot->rail_ch) != RAIL_STATUS_NO_ERROR) {
        slot->rail_ch = 0xffff;
    }
    __DMB();
    rx_head++;
}

static void rail_events_cb(RAIL_Handle_t handle, RAIL_Events_t events)
{
    (void)handle;
    bool wake = false;

    if (events & RAIL_EVENT_RX_PACKET_RECEIVED) {
        rx_capture_isr();
        wake = true;
    }
    if (events & RAIL_EVENT_RX_FRAME_ERROR) {
        stats.rx_crc_err++;
    }
    if (events & RAIL_EVENT_RX_FIFO_OVERFLOW) {
        stats.rx_fifo_overflow++;
    }
    if (events & RAIL_EVENT_RX_PACKET_ABORTED) {
        stats.rx_aborted++;
    }
    if (hop_pending && (events & RX_EVENTS)) {
        wake = true;      /* end of the frame: apply the deferred channel change */
    }
    if (events & TX_EVENTS) {
        pending_tx_events |= events & TX_EVENTS;
        wake = true;
    }
    if (events & RAIL_EVENT_CAL_NEEDED) {
        cal_needed = true;
        wake = true;
    }
    if (wake) {
        ns_port_notify_from_isr();
    }
}

/* Bigger RX FIFO than RAIL's 512-byte default (frames up to 2049 bytes). */
RAIL_Status_t RAILCb_SetupRxFifo(RAIL_Handle_t railHandle)
{
    uint16_t size = sizeof(rx_fifo);
    RAIL_Status_t st = RAIL_SetRxFifo(railHandle, rx_fifo, &size);

    return (st == RAIL_STATUS_NO_ERROR && size >= RX_BUF_LEN) ? RAIL_STATUS_NO_ERROR : RAIL_STATUS_INVALID_PARAMETER;
}

/* ------------------------------------------------------- TX state machine */

static void arm_prepare(void)
{
    uint32_t now = RAIL_GetTime();
    int32_t delta = (int32_t)(csma_backoff_time - RAIL_PHY_PREPARE_US - now);

    tx_state = TXS_WAIT_PREPARE;
    prepare_due = false;
    if (delta <= 0 || RAIL_SetMultiTimer(&prepare_timer, (uint32_t)delta, RAIL_TIME_DELAY,
                                         prepare_timer_isr, NULL) != RAIL_STATUS_NO_ERROR) {
        prepare_due = true;
        ns_port_notify();
    }
}

static void tx_abort(void)
{
    tx_watchdog_due = false;
    RAIL_CancelMultiTimer(&prepare_timer);
    prepare_due = false;
    if (tx_state == TXS_ACTIVE) {
        RAIL_Idle(rh, RAIL_IDLE_ABORT, true);
        platform_enter_critical();
        pending_tx_events = 0;
        platform_exit_critical();
        rx_channel = -1;
        stats.tx_aborted_by_mac++;
    }
    tx_state = TXS_IDLE;
    start_rx();
}

static bool frame_is_ack(const uint8_t *psdu)
{
    return (psdu[0] & 0x07) == 0x02;   /* FCF frame type = Ack */
}

static void tx_start_rail(void)
{
    uint8_t phr[2];
    RAIL_Status_t st;
    uint32_t now = RAIL_GetTime();
    int32_t ahead = (int32_t)(csma_backoff_time - now);
    uint16_t ch = rail_channel(cur_channel);
    bool scheduled = ahead > RAIL_PHY_MIN_SCHED_AHEAD_US;

#if RAIL_PHY_HOP_DEFER
    /* An Enh-ACK answers the last received frame on that frame's channel: the
     * sender waits there. FHSS may have hopped since (deferred change). */
    if (tx_is_ack && last_rx_rail_ch != 0xffff && last_rx_rail_ch != ch) {
        ch = last_rx_rail_ch;
        stats.ack_on_rx_channel++;
    }
#endif
    hop_pending = false;

    phr_build(phr, tx_len);
    RAIL_WriteTxFifo(rh, phr, PHR_LEN, true);
    RAIL_WriteTxFifo(rh, tx_buf, tx_len, false);

    if (!scheduled && ahead < -100) {
        stats.tx_late_start++;
    }
    tx_start_time = scheduled ? csma_backoff_time : now;
    tx_state = TXS_ACTIVE;
    rx_channel = ch - rail_ch_offset;  /* RAIL returns to RX on the TX channel */

    RAIL_CsmaConfig_t csma = {
        .csmaMinBoExp = 0,
        .csmaMaxBoExp = 0,
        .csmaTries = 1,
        .ccaThreshold = cca_threshold_dbm,
        .ccaBackoff = 0,
        .ccaDuration = RAIL_PHY_CCA_DURATION_US,
        .csmaTimeout = 0,
    };
    st = RAIL_STATUS_INVALID_STATE;
    if (scheduled) {
        RAIL_ScheduleTxConfig_t sched = {
            .when = csma_backoff_time,
            .mode = RAIL_TIME_ABSOLUTE,
            .txDuringRx = RAIL_SCHEDULED_TX_DURING_RX_ABORT_TX,
        };
        st = csma_cca_enabled
             ? RAIL_StartScheduledCcaCsmaTx(rh, ch, RAIL_TX_OPTIONS_DEFAULT, &sched, &csma, NULL)
             : RAIL_StartScheduledTx(rh, ch, RAIL_TX_OPTIONS_DEFAULT, &sched, NULL);
        if (st != RAIL_STATUS_NO_ERROR) {
            /* On hardware RAIL refuses (SL_STATUS_INVALID_PARAMETER) a
             * scheduled start that is too close (seen at ~260 µs ahead):
             * transmit now instead of failing the frame. */
            stats.tx_sched_refused++;
            tx_start_time = RAIL_GetTime();
        }
    }
    if (st != RAIL_STATUS_NO_ERROR) {
        st = csma_cca_enabled
             ? RAIL_StartCcaCsmaTx(rh, ch, RAIL_TX_OPTIONS_DEFAULT, &csma, NULL)
             : RAIL_StartTx(rh, ch, RAIL_TX_OPTIONS_DEFAULT, NULL);
    }
    if (st != RAIL_STATUS_NO_ERROR) {
        stats.tx_fail++;
        stats.tx_start_err++;
        stats.tx_start_err_last = st;
        stats.tx_start_err_state = RAIL_GetRadioState(rh) | (csma_cca_enabled ? 0x100u : 0) | (scheduled ? 0x200u : 0)
                                   | ((uint32_t)(ahead < 0 ? 0 : (ahead > 0xffff ? 0xffff : ahead)) << 16);
        tx_state = TXS_IDLE;
        rx_channel = -1;
        start_rx();
        tx_done(PHY_LINK_CCA_FAIL);
        return;
    }
    {
        /* Deadline: start time + air time (SHR 8 B + PHR + PSDU + FCS) + margin. */
        int32_t wait = (int32_t)(tx_start_time - RAIL_GetTime());
        uint32_t air = (uint32_t)(((uint64_t)(8u + PHR_LEN + tx_len + RAIL_PHY_FCS_LEN) * 8u * 1000000u) / symbol_rate);

        tx_watchdog_due = false;
        RAIL_SetMultiTimer(&prepare_timer, (uint32_t)(wait > 0 ? wait : 0) + air + RAIL_PHY_TX_WATCHDOG_MARGIN_US,
                           RAIL_TIME_DELAY, tx_watchdog_isr, NULL);
    }
    if (tx_is_ack) {
        uint32_t lat = tx_start_time - last_rx_end;
        stats.ack_tx++;
        if (lat > stats.ack_latency_max_us && lat < 1000000u) {
            stats.ack_latency_max_us = lat;
        }
    }
}

static void do_prepare(void)
{
    int8_t ret;
    int32_t late = (int32_t)(RAIL_GetTime() - (csma_backoff_time - RAIL_PHY_PREPARE_US));

    if (late > 0 && (uint32_t)late > stats.prepare_lateness_max_us) {
        stats.prepare_lateness_max_us = late;
    }
    tx_state = TXS_PREPARING;
    ret = drv.phy_tx_done_cb ? drv.phy_tx_done_cb(drv_id, tx_handle, PHY_LINK_CCA_PREPARE, 0, 0) : PHY_TX_ALLOWED;
    if (tx_state != TXS_PREPARING) {
        return;                       /* aborted from inside the callback */
    }
    switch (ret) {
        case PHY_TX_ALLOWED:
            /* cur_channel is now the TX channel (FHSS may have switched it
             * through PHY_EXTENSION_SET_CHANNEL inside the callback). */
            tx_start_rail();
            break;
        case PHY_RESTART_CSMA:
            /* Multi-CCA: one CCA now, the MAC already set the next backoff_time. */
            if ((RAIL_GetRadioState(rh) & RAIL_RF_STATE_RX_ACTIVE) == RAIL_RF_STATE_RX_ACTIVE) {
                stats.tx_cca_busy_rx++;
                tx_state = TXS_IDLE;
                tx_done(PHY_LINK_CCA_FAIL_RX);
            } else {
                int16_t rssi_q = RAIL_GetRssi(rh, true);
                if (rssi_q != RAIL_RSSI_INVALID && rssi_q / 4 > cca_threshold_dbm) {
                    stats.tx_cca_busy++;
                    tx_state = TXS_IDLE;
                    tx_done(PHY_LINK_CCA_FAIL);
                } else {
                    tx_done(PHY_LINK_CCA_OK);
                    if (tx_state == TXS_PREPARING && csma_backoff_time) {
                        arm_prepare();
                    }
                }
            }
            break;
        case PHY_TX_NOT_ALLOWED:
        default:
            tx_state = TXS_IDLE;
            start_rx();
            break;
    }
}

static void handle_tx_events(void)
{
    RAIL_Events_t ev;

    platform_enter_critical();
    ev = pending_tx_events;
    pending_tx_events = 0;
    platform_exit_critical();
    if (!ev || tx_state != TXS_ACTIVE) {
        return;
    }
    RAIL_CancelMultiTimer(&prepare_timer);
    tx_watchdog_due = false;
    tx_state = TXS_IDLE;
    if (ev & RAIL_EVENT_TX_PACKET_SENT) {
        stats.tx_ok++;
        tx_done(PHY_LINK_TX_SUCCESS);
    } else if (ev & RAIL_EVENT_TX_CHANNEL_BUSY) {
        stats.tx_cca_busy++;
        tx_done(PHY_LINK_CCA_FAIL);
    } else if (ev & RAIL_EVENT_TX_SCHEDULED_TX_MISSED) {
        stats.tx_sched_missed++;
        rx_channel = -1;
        tx_done(PHY_LINK_CCA_FAIL);
    } else if (ev & (RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED)) {
        /* Aborted because a frame was being received at the scheduled time. */
        stats.tx_cca_busy_rx++;
        rx_channel = -1;
        tx_done(PHY_LINK_CCA_FAIL);
    } else {
        stats.tx_fail++;
        stats.tx_underflow++;
        rx_channel = -1;
        tx_done(PHY_LINK_TX_FAIL);
    }
    /* tx_done() may have moved FHSS back to the RX channel. */
    start_rx();
}

static void deliver_rx(void)
{
    while (rx_tail != rx_head) {
        struct rx_slot *slot = &rx_ring[rx_tail % RAIL_PHY_RX_SLOTS];

        __DMB();
        last_rx_time = slot->time;
        last_rx_end = slot->end;
        last_rx_rail_ch = slot->rail_ch;
        stats.rx_ok++;
        if (drv.phy_rx_cb) {
            drv.phy_rx_cb(slot->buf + PHR_LEN, slot->len, slot->lqi, slot->rssi, drv_id);
        }
        rx_tail++;
    }
}

/* Bottom half, stack task. */
static void rail_phy_process(void)
{
    if (cal_needed) {
        cal_needed = false;
        stats.cal_runs++;
        RAIL_Calibrate(rh, NULL, RAIL_CAL_ALL_PENDING);
    }
    deliver_rx();
    handle_tx_events();
    if (hop_ev.due) {
        hop_ev.due = false;
        tr_debug("hop %u->%u during RX (%u B in) at %lu: %s", hop_ev.from, hop_ev.to, hop_ev.bytes,
                (unsigned long)hop_ev.time, RAIL_PHY_HOP_DEFER ? "deferred" : "frame aborted");
    }
    if (hop_pending) {
        uint32_t waited = RAIL_GetTime() - hop_pending_since;
        bool timeout = waited > RAIL_PHY_HOP_DEFER_MAX_US;
        if (timeout || !rx_frame_in_progress()) {
            if (waited > stats.rx_hop_defer_max_us) {
                stats.rx_hop_defer_max_us = waited;
            }
            hop_pending = false;
            if (timeout) {
                rx_channel = -1;   /* stuck RX: restart on cur_channel regardless */
            }
            start_rx();
        }
    }
    if (tx_watchdog_due) {
        tx_watchdog_due = false;
        if (tx_state == TXS_ACTIVE) {
            /* No TX event by the deadline: the TX was lost inside RAIL. */
            RAIL_Idle(rh, RAIL_IDLE_ABORT, true);
            platform_enter_critical();
            pending_tx_events = 0;
            platform_exit_critical();
            stats.tx_lost++;
            stats.tx_fail++;
            tx_state = TXS_IDLE;
            rx_channel = -1;
            tx_done(PHY_LINK_CCA_FAIL);   /* MAC retries */
            start_rx();
        }
    }
    if (prepare_due && tx_state == TXS_WAIT_PREPARE) {
        prepare_due = false;
        do_prepare();
    }
}

/* --------------------------------------------------- phy_device_driver_s */

static int8_t phy_state_control(phy_interface_state_e state, uint8_t channel)
{
    switch (state) {
        case PHY_INTERFACE_UP:
        case PHY_INTERFACE_RX_ENERGY_STATE:
        case PHY_INTERFACE_SNIFFER_STATE:
            radio_on = true;
            cur_channel = channel;
            rx_channel = -1;
            start_rx();
            break;
        case PHY_INTERFACE_DOWN:
        case PHY_INTERFACE_RESET:
        default:
            tx_abort();
            radio_on = false;
            RAIL_Idle(rh, RAIL_IDLE_ABORT, true);
            rx_channel = -1;
            break;
    }
    return 0;
}

static int8_t phy_tx(uint8_t *data, uint16_t len, uint8_t handle, data_protocol_e protocol)
{
    (void)protocol;

    if (tx_state != TXS_IDLE || len > PHY_MTU || len < 3) {
        return -1;
    }
    memcpy(tx_buf, data, len);
    tx_len = len;
    tx_handle = handle;
    tx_is_ack = frame_is_ack(tx_buf);
    stats.tx_requests++;
    if (!csma_backoff_time) {
        csma_backoff_time = RAIL_GetTime();  /* "send now" */
    }
    arm_prepare();
    return 0;
}

static int8_t phy_address_write(phy_address_type_e type, uint8_t *addr)
{
    /* Promiscuous radio: the MAC filters. Keep the EUI-64 for reference. */
    if (type == PHY_MAC_64BIT && addr) {
        memcpy(mac64, addr, 8);
    }
    return 0;
}

static __attribute__((unused)) int16_t tx_power_from_percent(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    return (int16_t)(RAIL_PHY_TX_POWER_MIN_DDBM
                     + ((int32_t)RAIL_PHY_TX_POWER_MAX_DDBM - RAIL_PHY_TX_POWER_MIN_DDBM) * pct / 100);
}

static void check_rf_config(const phy_rf_channel_configuration_s *c)
{
    if (c->channel_0_center_frequency != phy_cfg.channel_0_center_frequency
            || c->channel_spacing != phy_cfg.channel_spacing
            || c->datarate != phy_cfg.datarate
            || c->number_of_channels > rail_ch_count) {
        tr_err("stack PHY (ch0 %lu Hz, %lu Hz, %lu bps, %u ch) does not match the radio config "
               "(ch0 %lu Hz, %lu Hz, %lu bps, %u ch): wrong FW_REGION/FW_BOARD or regenerate rail_config.c",
               (unsigned long)c->channel_0_center_frequency, (unsigned long)c->channel_spacing,
               (unsigned long)c->datarate, c->number_of_channels,
               (unsigned long)phy_cfg.channel_0_center_frequency, (unsigned long)phy_cfg.channel_spacing,
               (unsigned long)phy_cfg.datarate, rail_ch_count);
    } else {
        tr_info("PHY config ok: ch0 %lu Hz, spacing %lu Hz, %lu bps, %u channels",
                (unsigned long)c->channel_0_center_frequency, (unsigned long)c->channel_spacing,
                (unsigned long)c->datarate, c->number_of_channels);
    }
}

static int8_t phy_extension(phy_extension_type_e type, uint8_t *data)
{
    if (!data) {
        return 0;
    }
    switch (type) {
        case PHY_EXTENSION_CTRL_PENDING_BIT:
            break;
        case PHY_EXTENSION_READ_LAST_ACK_PENDING_STATUS:
            *data = 0;
            break;
        case PHY_EXTENSION_DYNAMIC_RF_SUPPORTED:
            *data = true;
            break;
        case PHY_EXTENSION_GET_SYMBOLS_PER_SECOND:
            *(uint32_t *)data = symbol_rate;
            break;
        case PHY_EXTENSION_GET_TIMESTAMP:
            *(uint32_t *)data = RAIL_GetTime();
            break;
        case PHY_EXTENSION_READ_RX_TIME:
            common_write_32_bit(last_rx_time, data);
            break;
        case PHY_EXTENSION_SET_CHANNEL:
            if (tx_state == TXS_ACTIVE) {
                return -1;    /* MAC retries; same behaviour as the reference driver */
            }
            cur_channel = *data;
            start_rx();
            break;
        case PHY_EXTENSION_SET_CSMA_PARAMETERS: {
            const phy_csma_params_t *p = (const phy_csma_params_t *)data;
            if (p->backoff_time == 0) {
                csma_backoff_time = 0;
                tx_abort();
            } else {
                csma_backoff_time = p->backoff_time;
                csma_cca_enabled = p->cca_enabled;
                if (tx_state == TXS_WAIT_PREPARE) {
                    arm_prepare();
                }
            }
            break;
        }
        case PHY_EXTENSION_SET_TX_POWER: {
#if RAIL_PHY_TX_POWER_FIXED_DDBM
            int16_t ddbm = RAIL_PHY_TX_POWER_FIXED_DDBM;
#else
            int16_t ddbm = tx_power_from_percent(*data);
#endif
            if (RAIL_SetTxPowerDbm(rh, ddbm) == RAIL_STATUS_NO_ERROR) {
                stats.tx_power_ddbm = ddbm;
            }
            tr_info("TX power %u%% -> %d.%d dBm", *data, ddbm / 10, (ddbm < 0 ? -ddbm : ddbm) % 10);
            break;
        }
        case PHY_EXTENSION_SET_CHANNEL_CCA_THRESHOLD:
            cca_threshold_dbm = (int8_t)*data;
            break;
        case PHY_EXTENSION_SET_CCA_THRESHOLD:
            break;    /* percentage form: not used by Wi-SUN */
        case PHY_EXTENSION_SET_RF_CONFIGURATION:
            check_rf_config((const phy_rf_channel_configuration_s *)data);
            break;
        case PHY_EXTENSION_SET_DATA_WHITENING:
            whitening = *data;
            break;
        case PHY_EXTENSION_SET_802_15_4_MODE:
        case PHY_EXTENSION_FILTERING_SUPPORT:
        default:
            break;
    }
    return 0;
}

/* ------------------------------------------------------------------ init */

void rail_phy_get_eui64(uint8_t eui64[8])
{
    uint64_t u = SYSTEM_GetUnique();

    for (int i = 0; i < 8; i++) {
        eui64[i] = (uint8_t)(u >> (56 - 8 * i));
    }
}

int rail_phy_check_irq_priorities(void)
{
    static const IRQn_Type irqs[] = {
        AGC_IRQn, BUFC_IRQn, FRC_PRI_IRQn, FRC_IRQn, MODEM_IRQn, PROTIMER_IRQn,
        RAC_RSM_IRQn, RAC_SEQ_IRQn, HOSTMAILBOX_IRQn, SYNTH_IRQn,
    };
    const uint32_t min_prio = configMAX_SYSCALL_INTERRUPT_PRIORITY >> (8 - __NVIC_PRIO_BITS);
    int bad = 0;

    for (unsigned i = 0; i < sizeof(irqs) / sizeof(irqs[0]); i++) {
        if (NVIC_GetPriority(irqs[i]) < min_prio) {
            bad++;
        }
    }
    return bad;
}

void rail_phy_hw_init(void)
{
    static RAIL_Config_t rail_cfg = {
        .eventsCallback = rail_events_cb,
    };
    static const RAIL_IEEE802154_Config_t ieee_cfg = {
        .addresses = NULL,
        .ackConfig = {
            .enable = false,          /* the MAC builds and sends Enh-ACKs */
            .ackTimeout = 0,
            .rxTransitions = { RAIL_RF_STATE_RX, RAIL_RF_STATE_RX },
            .txTransitions = { RAIL_RF_STATE_RX, RAIL_RF_STATE_RX },
        },
        .timings = {
            .idleToRx = 100,
            .txToRx = 192 - 10,
            .idleToTx = 100,
            .rxToTx = 192,
            .rxSearchTimeout = 0,
            .txToRxSearchTimeout = 0,
            .txToTx = 0,
        },
        .framesMask = RAIL_IEEE802154_ACCEPT_STANDARD_FRAMES,
        .promiscuousMode = true,      /* no address filtering: the MAC filters */
        .isPanCoordinator = false,
        .defaultFramePendingInOutgoingAcks = false,
    };
    const RAIL_StateTransitions_t to_rx = { RAIL_RF_STATE_RX, RAIL_RF_STATE_RX };
    uint16_t sz;

    rh = RAIL_Init(&rail_cfg, NULL);
    configASSERT(rh != NULL);
    RAIL_ConfigCal(rh, RAIL_CAL_ALL);
    RAIL_ConfigChannels(rh, channelConfigs[0], NULL);
    rail_ch_offset = channelConfigs[0]->configs[0].channelNumberStart;
    rail_ch_count = channelConfigs[0]->configs[0].channelNumberEnd - rail_ch_offset + 1;
    phy_cfg.channel_0_center_frequency = channelConfigs[0]->configs[0].baseFrequency;
    phy_cfg.channel_spacing = channelConfigs[0]->configs[0].channelSpacing;
    phy_cfg.number_of_channels = rail_ch_count;
    RAIL_IEEE802154_Init(rh, &ieee_cfg);
    /* SUN FSK: 2-byte PHR, per-packet FCS length / whitening, frames > 127 B */
    RAIL_IEEE802154_ConfigGOptions(rh, RAIL_IEEE802154_G_OPTION_GB868, RAIL_IEEE802154_G_OPTION_GB868);
    RAIL_ConfigTxPower(rh, sl_rail_util_pa_get_tx_power_config_subghz());
    stats.tx_power_ddbm = RAIL_PHY_TX_POWER_FIXED_DDBM ? RAIL_PHY_TX_POWER_FIXED_DDBM : RAIL_PHY_TX_POWER_MAX_DDBM;
    RAIL_SetTxPowerDbm(rh, stats.tx_power_ddbm);
    sz = RAIL_SetTxFifo(rh, tx_fifo, 0, sizeof(tx_fifo));
    configASSERT(sz >= PHR_LEN + PHY_MTU);
    (void)sz;
    RAIL_SetRxTransitions(rh, &to_rx);
    RAIL_SetTxTransitions(rh, &to_rx);
    RAIL_ConfigEvents(rh, RAIL_EVENTS_ALL, TX_EVENTS | RX_EVENTS | RAIL_EVENT_CAL_NEEDED);
    symbol_rate = RAIL_GetSymbolRate(rh);
    if (!symbol_rate) {
        symbol_rate = phy_cfg.datarate;     /* 2-FSK: symbol rate = bit rate */
    }
    stats.cca_threshold_dbm = cca_threshold_dbm;
}

int8_t rail_phy_register(const uint8_t eui64[8])
{
    if (drv_id >= 0) {
        return drv_id;
    }
    memcpy(mac64, eui64, 8);
    memset(&drv, 0, sizeof(drv));
    drv.link_type = PHY_LINK_15_4_SUBGHZ_TYPE;
    drv.data_request_layer = PHY_LAYER_PAYLOAD_DATA_FLOW;
    drv.PHY_MAC = mac64;
    drv.phy_MTU = PHY_MTU;
    drv.driver_description = (char *)"RAIL-FG25";
    drv.phy_header_length = 0;
    drv.phy_tail_length = 0;
    drv.state_control = phy_state_control;
    drv.tx = phy_tx;
    drv.address_write = phy_address_write;
    drv.extension = phy_extension;
    drv.phy_channel_pages = phy_channel_pages;
    drv_id = arm_net_phy_register(&drv);
    if (drv_id < 0) {
        return drv_id;
    }
    arm_net_phy_mac64_set(mac64, drv_id);
    ns_port_add_poll_hook(rail_phy_process);
    return drv_id;
}

void rail_phy_get_stats(struct rail_phy_stats *st)
{
    *st = stats;
    st->channel = cur_channel;
    st->cca_threshold_dbm = cca_threshold_dbm;
}

void rail_phy_clear_stats(void)
{
    int16_t p = stats.tx_power_ddbm;

    memset(&stats, 0, sizeof(stats));
    stats.tx_power_ddbm = p;
}
