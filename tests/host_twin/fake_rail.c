/*
 * Host twin: a fake RAIL (2.x API subset used by drivers/rail/rail_phy.c and
 * port/freertos/port_core.c), compiled against the real SDK RAIL headers.
 *
 * Radio medium: every node process binds a UNIX datagram socket
 * <dir>/n<i>.sock; a transmission is one datagram sent to all other nodes at
 * TX start. Timing is modelled at 50 kbit/s (160 µs/byte, 8-byte preamble +
 * 2-byte SFD + PHR + PSDU + 4-byte FCS): the receiver completes the frame
 * after the air time, only if it stayed in RX on that channel, and two
 * overlapping frames on the same channel collide. CCA = "a frame is on the
 * air on this channel". No propagation loss unless HOST_TWIN_LOSS (percent)
 * is set.
 *
 * Callbacks (RAIL events, multitimers) run in "ISR context": in the timer or
 * medium thread, with the kernel lock held (see fake_rtos.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rail.h"
#include "rail_ieee802154.h"
#include "rail_config.h"
#include "host_twin.h"

#define US_PER_BYTE   160u
#define SHR_BYTES     10u          /* preamble 8 + SFD 2 */
#define FCS_BYTES     4u
#define MAX_FRAME     2100
#define MAGIC         0x57534e31u  /* "WSN1" */

struct air_hdr {
    uint32_t magic;
    uint16_t channel;
    uint16_t len;      /* bytes incl. PHR */
    uint16_t sender;
};

/* ---------------------------------------------------------- timebase */

static uint32_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
}

RAIL_Time_t RAIL_GetTime(void)
{
    return now_us();
}

/* ------------------------------------------------------- multitimers */

static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tcond;
static RAIL_MultiTimer_t *timers;   /* running timers, unsorted list */

static void tlist_remove(RAIL_MultiTimer_t *t)
{
    for (RAIL_MultiTimer_t **pp = &timers; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            t->next = NULL;
            return;
        }
    }
}

bool RAIL_ConfigMultiTimer(bool enable)
{
    (void)enable;
    return true;
}

RAIL_Status_t RAIL_SetMultiTimer(RAIL_MultiTimer_t *tmr, RAIL_Time_t time, RAIL_TimeMode_t mode,
                                 RAIL_MultiTimerCallback_t cb, void *arg)
{
    uint32_t now = now_us();
    uint32_t exp = mode == RAIL_TIME_ABSOLUTE ? time : now + time;

    if (mode == RAIL_TIME_ABSOLUTE && (int32_t)(exp - now) < 0) {
        return RAIL_STATUS_INVALID_PARAMETER;
    }
    pthread_mutex_lock(&tlock);
    if (tmr->isRunning) {
        tlist_remove(tmr);
    }
    tmr->absOffset = exp;
    tmr->callback = cb;
    tmr->cbArg = arg;
    tmr->isRunning = true;
    tmr->next = timers;
    timers = tmr;
    pthread_cond_signal(&tcond);
    pthread_mutex_unlock(&tlock);
    return RAIL_STATUS_NO_ERROR;
}

bool RAIL_CancelMultiTimer(RAIL_MultiTimer_t *tmr)
{
    bool was;

    pthread_mutex_lock(&tlock);
    was = tmr->isRunning;
    if (was) {
        tlist_remove(tmr);
        tmr->isRunning = false;
    }
    pthread_mutex_unlock(&tlock);
    return was;
}

RAIL_Time_t RAIL_GetMultiTimer(RAIL_MultiTimer_t *tmr, RAIL_TimeMode_t mode)
{
    uint32_t r = 0;

    pthread_mutex_lock(&tlock);
    if (tmr->isRunning) {
        int32_t d = (int32_t)(tmr->absOffset - now_us());
        r = mode == RAIL_TIME_ABSOLUTE ? tmr->absOffset : (d > 0 ? (uint32_t)d : 0);
    }
    pthread_mutex_unlock(&tlock);
    return r;
}

static void *timer_thread(void *p)
{
    (void)p;
    pthread_setname_np(pthread_self(), "rail_timer");
    pthread_mutex_lock(&tlock);
    for (;;) {
        RAIL_MultiTimer_t *first = NULL;
        for (RAIL_MultiTimer_t *t = timers; t; t = t->next) {
            if (!first || (int32_t)(t->absOffset - first->absOffset) < 0) {
                first = t;
            }
        }
        if (!first) {
            pthread_cond_wait(&tcond, &tlock);
            continue;
        }
        int32_t d = (int32_t)(first->absOffset - now_us());
        if (d > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += (long)d * 1000L;
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&tcond, &tlock, &ts);
            continue;
        }
        /* Expired: take the kernel lock first (K -> T order, like the task),
         * then re-check: the task may have cancelled/re-armed it meanwhile. */
        pthread_mutex_unlock(&tlock);
        host_isr_enter();
        pthread_mutex_lock(&tlock);
        bool still = false;
        for (RAIL_MultiTimer_t *t = timers; t; t = t->next) {
            if (t == first) {
                still = true;
            }
        }
        if (still && (int32_t)(first->absOffset - now_us()) <= 0) {
            RAIL_MultiTimerCallback_t cb = first->callback;
            void *arg = first->cbArg;
            RAIL_Time_t exp = first->absOffset;
            tlist_remove(first);
            first->isRunning = false;
            pthread_mutex_unlock(&tlock);
            if (cb) {
                cb(first, exp, arg);
            }
            pthread_mutex_lock(&tlock);
        }
        pthread_mutex_unlock(&tlock);
        host_isr_exit();
        pthread_mutex_lock(&tlock);
    }
    return NULL;
}

/* ------------------------------------------------------------- radio */

static int rail_dummy;
static RAIL_Handle_t handle = &rail_dummy;
static void (*events_cb)(RAIL_Handle_t, RAIL_Events_t);
static RAIL_Events_t events_enabled;

enum rstate { R_IDLE, R_RX, R_TX };
static enum rstate state;
static uint16_t rx_channel;

static uint8_t *txfifo;
static uint16_t txfifo_size, txfifo_len;

/* reception in progress */
static bool receiving, rx_corrupt;
static uint16_t rx_ch;
static uint8_t rx_buf[MAX_FRAME];
static uint16_t rx_len;
static uint32_t rx_sync_end;
static RAIL_MultiTimer_t rx_done_timer;
/* frame handed to the event callback */
static bool rx_ready;

static uint32_t air_busy_until[1024];

/* pending / active TX */
static RAIL_MultiTimer_t tx_start_timer, tx_done_timer;
static struct {
    uint16_t channel;
    bool cca;
    int8_t cca_threshold;
    uint8_t frame[MAX_FRAME];
    uint16_t len;
} txp;

static int sock = -1;
static uint32_t loss_pct;
static int dbg;
#define DBG(...) do { if (dbg) { fprintf(stderr, "[fakerail %u] ", now_us() / 1000); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void raise_events(RAIL_Events_t ev)
{
    ev &= events_enabled;
    if (ev && events_cb) {
        events_cb(handle, ev);
    }
}

static bool channel_busy(uint16_t ch)
{
    return (receiving && rx_ch == ch) || (int32_t)(air_busy_until[ch & 1023] - now_us()) > 0;
}

static void rx_done_cb(RAIL_MultiTimer_t *t, RAIL_Time_t exp, void *arg)
{
    (void)t;
    (void)exp;
    (void)arg;
    if (!receiving) {
        return;
    }
    receiving = false;
    if (state != R_RX || rx_channel != rx_ch) {
        return;                         /* left the channel: frame lost */
    }
    if (rx_corrupt || (loss_pct && (uint32_t)(rand() % 100) < loss_pct)) {
        raise_events(RAIL_EVENT_RX_FRAME_ERROR);
        return;
    }
    rx_ready = true;
    DBG("rx done ch %u len %u", rx_ch, rx_len);
    raise_events(RAIL_EVENT_RX_PACKET_RECEIVED);
    rx_ready = false;
}

static void medium_rx(const struct air_hdr *h, const uint8_t *data)
{
    uint32_t now = now_us();
    uint32_t air = (SHR_BYTES + h->len + FCS_BYTES) * US_PER_BYTE;

    if ((int32_t)(now + air - air_busy_until[h->channel & 1023]) > 0) {
        air_busy_until[h->channel & 1023] = now + air;
    }
    if (state != R_RX || rx_channel != h->channel) {
        DBG("air ch %u len %u from %u: not listening (state %d ch %u)", h->channel, h->len, h->sender, state, rx_channel);
        return;
    }
    DBG("air ch %u len %u from %u: receiving%s", h->channel, h->len, h->sender, receiving ? " COLLISION" : "");
    if (receiving) {
        if (rx_ch == h->channel) {
            rx_corrupt = true;          /* collision */
        }
        return;
    }
    receiving = true;
    rx_corrupt = false;
    rx_ch = h->channel;
    rx_len = h->len;
    memcpy(rx_buf, data, h->len);
    rx_sync_end = now + SHR_BYTES * US_PER_BYTE;
    RAIL_SetMultiTimer(&rx_done_timer, air, RAIL_TIME_DELAY, rx_done_cb, NULL);
}

static void *medium_thread(void *p)
{
    uint8_t buf[sizeof(struct air_hdr) + MAX_FRAME];

    (void)p;
    pthread_setname_np(pthread_self(), "rail_medium");
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        const struct air_hdr *h = (const struct air_hdr *)buf;
        if (n < (ssize_t)sizeof(*h) || h->magic != MAGIC || n != (ssize_t)(sizeof(*h) + h->len)) {
            continue;
        }
        host_isr_enter();
        medium_rx(h, buf + sizeof(*h));
        host_isr_exit();
    }
    return NULL;
}

static void air_send(uint16_t ch, const uint8_t *frame, uint16_t len)
{
    uint8_t buf[sizeof(struct air_hdr) + MAX_FRAME];
    struct air_hdr *h = (struct air_hdr *)buf;
    struct sockaddr_un a = { .sun_family = AF_UNIX };

    h->magic = MAGIC;
    h->channel = ch;
    h->len = len;
    h->sender = host_node_index;
    memcpy(buf + sizeof(*h), frame, len);
    for (int i = 0; i < host_node_count; i++) {
        if (i == host_node_index) {
            continue;
        }
        snprintf(a.sun_path, sizeof(a.sun_path), "%s/n%d.sock", host_medium_dir, i);
        sendto(sock, buf, sizeof(*h) + len, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof(a));
    }
}

static void tx_done_cb(RAIL_MultiTimer_t *t, RAIL_Time_t exp, void *arg)
{
    (void)t;
    (void)exp;
    (void)arg;
    if (state != R_TX) {
        return;
    }
    state = R_RX;                       /* TX success transition: RX */
    rx_channel = txp.channel;
    raise_events(RAIL_EVENT_TX_PACKET_SENT);
}

static void tx_start_cb(RAIL_MultiTimer_t *t, RAIL_Time_t exp, void *arg)
{
    (void)t;
    (void)exp;
    bool scheduled = arg != NULL;

    if (scheduled && receiving) {
        /* RAIL_SCHEDULED_TX_DURING_RX_ABORT_TX */
        raise_events(RAIL_EVENT_TX_BLOCKED);
        return;
    }
    if (txp.cca && channel_busy(txp.channel)) {
        DBG("cca busy ch %u", txp.channel);
        state = R_RX;                   /* error transition: RX */
        rx_channel = txp.channel;
        raise_events(RAIL_EVENT_TX_CHANNEL_BUSY);
        return;
    }
    receiving = false;
    RAIL_CancelMultiTimer(&rx_done_timer);
    state = R_TX;
    DBG("tx ch %u len %u%s", txp.channel, txp.len, txp.cca ? " (cca)" : "");
    air_send(txp.channel, txp.frame, txp.len);
    RAIL_SetMultiTimer(&tx_done_timer, (SHR_BYTES + txp.len + FCS_BYTES) * US_PER_BYTE,
                       RAIL_TIME_DELAY, tx_done_cb, NULL);
}

static RAIL_Status_t start_tx(uint16_t channel, bool cca, int8_t thr, const RAIL_ScheduleTxConfig_t *sched)
{
    uint16_t len;

    if (state == R_TX || tx_start_timer.isRunning) {
        return RAIL_STATUS_INVALID_STATE;
    }
    if (txfifo_len < 2) {
        return RAIL_STATUS_INVALID_PARAMETER;
    }
    /* PHR (as written by the driver) -> length incl. FCS */
    len = (uint16_t)(((txfifo[0] >> 5) & 1) << 10 | ((txfifo[0] >> 6) & 1) << 9 | ((txfifo[0] >> 7) & 1) << 8);
    {
        uint8_t b = txfifo[1], r = 0;
        for (int i = 0; i < 8; i++) {
            r |= (uint8_t)(((b >> i) & 1) << (7 - i));
        }
        len |= r;
    }
    uint16_t fcs = (txfifo[0] & 0x08) ? 2 : 4;
    if (len < fcs || (uint16_t)(len - fcs + 2) != txfifo_len) {
        fprintf(stderr, "fake RAIL: PHR length %u (fcs %u) does not match FIFO %u\n", len, fcs, txfifo_len);
        return RAIL_STATUS_INVALID_PARAMETER;
    }
    txp.channel = channel;
    txp.cca = cca;
    txp.cca_threshold = thr;
    txp.len = txfifo_len;
    memcpy(txp.frame, txfifo, txfifo_len);
    if (sched) {
        uint32_t when = sched->mode == RAIL_TIME_ABSOLUTE ? sched->when : now_us() + sched->when;
        if ((int32_t)(when - now_us()) < 0) {
            raise_events(RAIL_EVENT_TX_SCHEDULED_TX_MISSED);
            return RAIL_STATUS_NO_ERROR;
        }
        RAIL_SetMultiTimer(&tx_start_timer, when, RAIL_TIME_ABSOLUTE, tx_start_cb, (void *)1);
    } else {
        RAIL_SetMultiTimer(&tx_start_timer, 1, RAIL_TIME_DELAY, tx_start_cb, NULL);
    }
    return RAIL_STATUS_NO_ERROR;
}

RAIL_Status_t RAIL_StartTx(RAIL_Handle_t h, uint16_t ch, RAIL_TxOptions_t o, const RAIL_SchedulerInfo_t *s)
{
    (void)h; (void)o; (void)s;
    host_kernel_lock();
    RAIL_Status_t st = start_tx(ch, false, 0, NULL);
    host_kernel_unlock();
    return st;
}

RAIL_Status_t RAIL_StartScheduledTx(RAIL_Handle_t h, uint16_t ch, RAIL_TxOptions_t o,
                                    const RAIL_ScheduleTxConfig_t *c, const RAIL_SchedulerInfo_t *s)
{
    (void)h; (void)o; (void)s;
    host_kernel_lock();
    RAIL_Status_t st = start_tx(ch, false, 0, c);
    host_kernel_unlock();
    return st;
}

RAIL_Status_t RAIL_StartCcaCsmaTx(RAIL_Handle_t h, uint16_t ch, RAIL_TxOptions_t o,
                                  const RAIL_CsmaConfig_t *csma, const RAIL_SchedulerInfo_t *s)
{
    (void)h; (void)o; (void)s;
    host_kernel_lock();
    RAIL_Status_t st = start_tx(ch, true, csma->ccaThreshold, NULL);
    host_kernel_unlock();
    return st;
}

RAIL_Status_t RAIL_StartScheduledCcaCsmaTx(RAIL_Handle_t h, uint16_t ch, RAIL_TxOptions_t o,
                                           const RAIL_ScheduleTxConfig_t *c, const RAIL_CsmaConfig_t *csma,
                                           const RAIL_SchedulerInfo_t *s)
{
    (void)h; (void)o; (void)s;
    host_kernel_lock();
    RAIL_Status_t st = start_tx(ch, true, csma->ccaThreshold, c);
    host_kernel_unlock();
    return st;
}

uint16_t RAIL_WriteTxFifo(RAIL_Handle_t h, const uint8_t *data, uint16_t n, bool reset)
{
    (void)h;
    host_kernel_lock();
    if (reset) {
        txfifo_len = 0;
    }
    if (txfifo_len + n > txfifo_size) {
        n = txfifo_size - txfifo_len;
    }
    memcpy(txfifo + txfifo_len, data, n);
    txfifo_len += n;
    host_kernel_unlock();
    return n;
}

uint16_t RAIL_SetTxFifo(RAIL_Handle_t h, uint8_t *addr, uint16_t init, uint16_t size)
{
    (void)h;
    txfifo = addr;
    txfifo_len = init;
    txfifo_size = size;
    return size;
}

RAIL_Status_t RAIL_SetRxFifo(RAIL_Handle_t h, uint8_t *addr, uint16_t *size)
{
    (void)h;
    (void)addr;
    (void)size;
    return RAIL_STATUS_NO_ERROR;
}

RAIL_Status_t RAIL_StartRx(RAIL_Handle_t h, uint16_t ch, const RAIL_SchedulerInfo_t *s)
{
    (void)h;
    (void)s;
    host_kernel_lock();
    if (state == R_TX) {
        host_kernel_unlock();
        return RAIL_STATUS_INVALID_STATE;
    }
    if (receiving && rx_ch != ch) {
        receiving = false;
        RAIL_CancelMultiTimer(&rx_done_timer);
    }
    state = R_RX;
    rx_channel = ch;
    host_kernel_unlock();
    return RAIL_STATUS_NO_ERROR;
}

RAIL_Status_t RAIL_Idle(RAIL_Handle_t h, RAIL_IdleMode_t mode, bool wait)
{
    (void)h;
    (void)mode;
    (void)wait;
    host_kernel_lock();
    RAIL_CancelMultiTimer(&tx_start_timer);
    RAIL_CancelMultiTimer(&tx_done_timer);
    RAIL_CancelMultiTimer(&rx_done_timer);
    receiving = false;
    state = R_IDLE;
    host_kernel_unlock();
    return RAIL_STATUS_NO_ERROR;
}

RAIL_RadioState_t RAIL_GetRadioState(RAIL_Handle_t h)
{
    (void)h;
    switch (state) {
        case R_RX: return receiving ? RAIL_RF_STATE_RX_ACTIVE : RAIL_RF_STATE_RX;
        case R_TX: return RAIL_RF_STATE_TX_ACTIVE;
        default:   return RAIL_RF_STATE_IDLE;
    }
}

int16_t RAIL_GetRssi(RAIL_Handle_t h, bool wait)
{
    (void)h;
    (void)wait;
    if (state != R_RX) {
        return RAIL_RSSI_INVALID;
    }
    return channel_busy(rx_channel) ? -50 * 4 : -110 * 4;
}

RAIL_RxPacketHandle_t RAIL_GetRxPacketInfo(RAIL_Handle_t h, RAIL_RxPacketHandle_t ph, RAIL_RxPacketInfo_t *info)
{
    (void)h;
    (void)ph;
    if (!rx_ready) {
        return RAIL_RX_PACKET_HANDLE_INVALID;
    }
    memset(info, 0, sizeof(*info));
    info->packetStatus = RAIL_RX_PACKET_READY_SUCCESS;
    info->packetBytes = rx_len;
    info->firstPortionBytes = rx_len;
    info->firstPortionData = rx_buf;
    info->lastPortionData = NULL;
    return (RAIL_RxPacketHandle_t)rx_buf;
}

RAIL_Status_t RAIL_GetRxPacketDetailsAlt(RAIL_Handle_t h, RAIL_RxPacketHandle_t ph, RAIL_RxPacketDetails_t *d)
{
    (void)h;
    (void)ph;
    d->timeReceived.packetTime = rx_sync_end;
    d->timeReceived.timePosition = RAIL_PACKET_TIME_AT_SYNC_END;
    d->crcPassed = true;
    d->isAck = false;
    d->rssi = -50;
    d->lqi = 200;
    return RAIL_STATUS_NO_ERROR;
}

RAIL_Status_t RAIL_GetRxTimeSyncWordEndAlt(RAIL_Handle_t h, RAIL_RxPacketDetails_t *d)
{
    (void)h;
    d->timeReceived.packetTime = rx_sync_end;
    return RAIL_STATUS_NO_ERROR;
}

/* RAIL_CopyRxPacket() is a static inline in rail.h: nothing to fake. */

/* ------------------------------------------------- init / config stubs */

RAIL_Handle_t RAIL_Init(const RAIL_Config_t *cfg, RAIL_InitCompleteCallbackPtr_t cb)
{
    extern RAIL_Status_t RAILCb_SetupRxFifo(RAIL_Handle_t railHandle);

    (void)cb;
    events_cb = cfg->eventsCallback;
    RAILCb_SetupRxFifo(handle);
    host_rail_start();
    return handle;
}

void host_rail_start(void)
{
    static bool started;
    pthread_t th;
    pthread_condattr_t ca;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    const char *loss = getenv("HOST_TWIN_LOSS");

    if (started) {
        return;
    }
    started = true;
    loss_pct = loss ? (uint32_t)atoi(loss) : 0;
    dbg = getenv("HOST_TWIN_DEBUG") != NULL;
    for (unsigned i = 0; i < sizeof(air_busy_until) / sizeof(air_busy_until[0]); i++) {
        air_busy_until[i] = now_us();   /* 32-bit time: "not busy" must be a recent value */
    }
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&tcond, &ca);
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/n%d.sock", host_medium_dir, host_node_index);
    unlink(a.sun_path);
    if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("fake RAIL: bind");
        exit(2);
    }
    pthread_create(&th, NULL, timer_thread, NULL);
    pthread_create(&th, NULL, medium_thread, NULL);
}

RAIL_Status_t RAIL_ConfigCal(RAIL_Handle_t h, RAIL_CalMask_t m) { (void)h; (void)m; return RAIL_STATUS_NO_ERROR; }
RAIL_Status_t RAIL_Calibrate(RAIL_Handle_t h, RAIL_CalValues_t *v, RAIL_CalMask_t m) { (void)h; (void)v; (void)m; return RAIL_STATUS_NO_ERROR; }

uint16_t RAIL_ConfigChannels(RAIL_Handle_t h, const RAIL_ChannelConfig_t *c, RAIL_RadioConfigChangedCallback_t cb)
{
    (void)h;
    (void)cb;
    return c->configs[0].channelNumberStart;
}

RAIL_Status_t RAIL_IEEE802154_Init(RAIL_Handle_t h, const RAIL_IEEE802154_Config_t *c) { (void)h; (void)c; return RAIL_STATUS_NO_ERROR; }
RAIL_Status_t RAIL_IEEE802154_ConfigGOptions(RAIL_Handle_t h, RAIL_IEEE802154_GOptions_t m, RAIL_IEEE802154_GOptions_t o) { (void)h; (void)m; (void)o; return RAIL_STATUS_NO_ERROR; }
RAIL_Status_t RAIL_ConfigTxPower(RAIL_Handle_t h, const RAIL_TxPowerConfig_t *c) { (void)h; (void)c; return RAIL_STATUS_NO_ERROR; }
RAIL_Status_t RAIL_SetTxPowerDbm(RAIL_Handle_t h, RAIL_TxPower_t p) { (void)h; (void)p; return RAIL_STATUS_NO_ERROR; }
RAIL_Status_t RAIL_SetRxTransitions(RAIL_Handle_t h, const RAIL_StateTransitions_t *t) { (void)h; (void)t; return RAIL_STATUS_NO_ERROR; }
RAIL_Status_t RAIL_SetTxTransitions(RAIL_Handle_t h, const RAIL_StateTransitions_t *t) { (void)h; (void)t; return RAIL_STATUS_NO_ERROR; }

RAIL_Status_t RAIL_ConfigEvents(RAIL_Handle_t h, RAIL_Events_t mask, RAIL_Events_t events)
{
    (void)h;
    events_enabled = (events_enabled & ~mask) | (events & mask);
    return RAIL_STATUS_NO_ERROR;
}

uint32_t RAIL_GetSymbolRate(RAIL_Handle_t h) { (void)h; return 50000; }

RAIL_Status_t RAIL_GetVersion(RAIL_Version_t *v, bool verbose)
{
    (void)verbose;
    memset(v, 0, sizeof(*v));
    v->major = 2;
    v->minor = 99;   /* fake */
    return RAIL_STATUS_NO_ERROR;
}

/* The generated rail_config.c holds 32-bit register/pointer tables that do
 * not compile for a 64-bit host; the driver only needs the channel range,
 * copied here from sdk/gen/autogen/rail_config.c
 * (WiSunConf_Protocol_Configuration_1_channels). */
static const RAIL_ChannelConfigEntry_t host_channels[] = {
    {
        .phyConfigDeltaAdd = NULL,
        .baseFrequency = 902200000,
        .channelSpacing = 200000,
        .physicalChannelOffset = 512,
        .channelNumberStart = 512,
        .channelNumberEnd = 640,
        .maxPower = RAIL_TX_POWER_MAX,
    },
};
static const RAIL_ChannelConfig_t host_channel_config = {
    .configs = host_channels,
    .length = 1U,
};
const RAIL_ChannelConfig_t *channelConfigs[] = { &host_channel_config, NULL };
