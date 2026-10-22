/*
 * Nanostack phy_device_driver_s over Silicon Labs RAIL (2.x API) for the
 * EFR32FG25, Wi-SUN FAN 1.0 FSK PHYs. See rail_phy.c and
 * nanostack-generic-standalone docs/GENERICITY_PLAN.md §3.2 / §5.5.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RAIL_PHY_H
#define RAIL_PHY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring RAIL up (RAIL_Init, channel config from the generated rail_config.c,
 * IEEE 802.15.4 SUN-FSK options, PA, FIFOs, events). Must run in the stack
 * task before ns_port_init(), which needs the RAIL timebase. */
void rail_phy_hw_init(void);

/* Register the driver with Nanostack (arm_net_phy_register +
 * arm_net_phy_mac64_set) and install its bottom half as a port poll hook.
 * Returns the driver id (>= 0) or < 0 on error. */
int8_t rail_phy_register(const uint8_t eui64[8]);

/* The factory EUI-64 of this chip (DEVINFO), MSB first. */
void rail_phy_get_eui64(uint8_t eui64[8]);

/* Warn (via the returned count) about RAIL IRQs that FreeRTOS critical
 * sections cannot mask (priority numerically < configMAX_SYSCALL_INTERRUPT_PRIORITY). */
int rail_phy_check_irq_priorities(void);

struct rail_phy_stats {
    uint32_t rx_ok;
    uint32_t rx_ring_full;       /* dropped: bottom half too slow */
    uint32_t rx_crc_err;
    uint32_t rx_fifo_overflow;
    uint32_t rx_aborted;
    uint32_t tx_requests;
    uint32_t tx_ok;
    uint32_t tx_cca_busy;        /* CCA failed (RAIL CSMA or RESTART_CSMA RSSI check) */
    uint32_t tx_cca_busy_rx;     /* CCA skipped: frame being received */
    uint32_t tx_fail;            /* aborted / underflow / start error */
    uint32_t tx_sched_missed;
    uint32_t tx_late_start;      /* prepare ran after the requested TX time */
    uint32_t tx_aborted_by_mac;
    uint32_t ack_tx;
    uint32_t ack_latency_max_us; /* end of received frame -> Enh-ACK TX start */
    uint32_t prepare_lateness_max_us;
    uint32_t cal_runs;
    uint16_t channel;
    int8_t cca_threshold_dbm;
    int16_t tx_power_ddbm;
};
void rail_phy_get_stats(struct rail_phy_stats *st);
void rail_phy_clear_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* RAIL_PHY_H */
