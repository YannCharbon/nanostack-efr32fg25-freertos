/*
 * Compile-time tuning of the RAIL radio driver. All values can be overridden
 * with -D. Values marked (estimate) are starting points for bench bring-up.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RAIL_PHY_CONFIG_H
#define RAIL_PHY_CONFIG_H

/* PHY data rate and modulation index of the radio config (the channel plan
 * itself is read from rail_config.c). NA/1b: 50 kbit/s, h = 1.0;
 * EU/1a: 50 kbit/s, h = 0.5. FW_REGION_EU comes from the build (FW_REGION). */
#ifndef RAIL_PHY_DATARATE
#define RAIL_PHY_DATARATE 50000
#endif
#ifndef RAIL_PHY_MOD_INDEX
#ifdef FW_REGION_EU
#define RAIL_PHY_MOD_INDEX MODULATION_INDEX_0_5
#else
#define RAIL_PHY_MOD_INDEX MODULATION_INDEX_1_0
#endif
#endif

/* FCS length written in the PHR (FCS Type bit): 4 (CRC-32, Wi-SUN FAN) or 2. */
#ifndef RAIL_PHY_FCS_LEN
#define RAIL_PHY_FCS_LEN 4
#endif

/* Data whitening (PHR DW bit); the stack may change it via
 * PHY_EXTENSION_SET_DATA_WHITENING. */
#ifndef RAIL_PHY_WHITENING
#define RAIL_PHY_WHITENING 1
#endif

/* The prepare step (CCA_PREPARE callback + FIFO load + RAIL scheduling)
 * starts this long before the MAC's requested TX time (estimate). */
#ifndef RAIL_PHY_PREPARE_US
#define RAIL_PHY_PREPARE_US 800
#endif

/* Below this lead time a RAIL scheduled TX is not used: transmit now.
 * On EFR32FG25 hardware RAIL refused scheduled CCA TX ~260 µs ahead
 * (SL_STATUS_INVALID_PARAMETER); 400 µs leaves margin (the driver also falls
 * back to an immediate start if RAIL refuses). */
#ifndef RAIL_PHY_MIN_SCHED_AHEAD_US
#define RAIL_PHY_MIN_SCHED_AHEAD_US 400
#endif

/* CCA length: 8 symbols at 50 kbit/s = 160 µs. */
#ifndef RAIL_PHY_CCA_DURATION_US
#define RAIL_PHY_CCA_DURATION_US 160
#endif

/* CCA threshold until the MAC sets one (estimate). */
#ifndef RAIL_PHY_CCA_THRESHOLD_DBM
#define RAIL_PHY_CCA_THRESHOLD_DBM (-85)
#endif

/* TX power: Nanostack gives a percentage; map it linearly onto this range
 * (deci-dBm, estimate), or force a fixed value (non-zero) for the bench. */
#ifndef RAIL_PHY_TX_POWER_MIN_DDBM
#define RAIL_PHY_TX_POWER_MIN_DDBM (-100)
#endif
#ifndef RAIL_PHY_TX_POWER_MAX_DDBM
#define RAIL_PHY_TX_POWER_MAX_DDBM 140
#endif
#ifndef RAIL_PHY_TX_POWER_FIXED_DDBM
#define RAIL_PHY_TX_POWER_FIXED_DDBM 100   /* +10 dBm on the bench */
#endif

/* RX ring slots (each ~2 kB) and RAIL FIFOs. */
#ifndef RAIL_PHY_RX_SLOTS
#define RAIL_PHY_RX_SLOTS 4
#endif
#ifndef RAIL_PHY_TX_FIFO_SIZE
#define RAIL_PHY_TX_FIFO_SIZE 4096
#endif
#ifndef RAIL_PHY_RX_FIFO_SIZE
#define RAIL_PHY_RX_FIFO_SIZE 4096
#endif

#endif /* RAIL_PHY_CONFIG_H */
