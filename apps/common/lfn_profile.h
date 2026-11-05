/*
 * FW_LFN_PROFILE: Silicon Labs' LFN profiles (Test, Balanced, Eco) as our
 * LFN settings, for kit runs that compare with Silicon Labs' LFN and with
 * the emulator's profile runs. The values are those of the emulator's
 * sim_utils/vtime/lfn_profile.sh (the stack's docs/LFN_STACK.md §11):
 * LFN side from Simplicity SDK sl_wisun_lfn_params_api.h
 * (SL_WISUN_PARAMS_LFN_*), parent side from
 * border_router/sl_wisun_br_lfn_params_api.h (SL_WISUN_BR_PARAMS_LFN_*).
 * Eco's longest unicast interval is 2 147 483 ms, as in the emulator.
 *
 * Without FW_LFN_PROFILE nothing is defined here and the images keep the
 * stack's defaults.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_LFN_PROFILE_H
#define APP_LFN_PROFILE_H

#include <stdbool.h>
#include "nanostack/ws_management_api.h"

#if defined(APP_LFN_PROFILE_TEST) || defined(APP_LFN_PROFILE_BALANCED) || defined(APP_LFN_PROFILE_ECO)
#define APP_LFN_PROFILE 1

#if defined(APP_LFN_PROFILE_TEST)
#define APP_LFN_PROFILE_NAME "test"
#define LP_LI 6000u
#define LP_LIMAX 60000u
#define LP_MPT 5
#define LP_MISMATCH 120
#define LP_REG 2160
#define LP_SYNCONLY false
#define LP_BCI 10000u
#define LP_BCSP 5
#define LP_NAW 300u
#define LP_DHD 10
#define LP_DHI 10
#define LP_IKM 3
#define LP_IKR 18
#define LP_IKX 42
#define LP_IKL 3
#elif defined(APP_LFN_PROFILE_BALANCED)
#define APP_LFN_PROFILE_NAME "balanced"
#define LP_LI 60000u
#define LP_LIMAX 300000u
#define LP_MPT 20
#define LP_MISMATCH 3600
#define LP_REG 3600
#define LP_SYNCONLY false
#define LP_BCI 60000u
#define LP_BCSP 15
#define LP_NAW 3600u
#define LP_DHD 300
#define LP_DHI 900
#define LP_IKM 30
#define LP_IKR 180
#define LP_IKX 420
#define LP_IKL 3
#else
#define APP_LFN_PROFILE_NAME "eco"
#define LP_LI 300000u
#define LP_LIMAX 2147483u
#define LP_MPT 60
#define LP_MISMATCH 3600
#define LP_REG 5040
#define LP_SYNCONLY true
#define LP_BCI 600000u
#define LP_BCSP 60
#define LP_NAW 7200u
#define LP_DHD 600
#define LP_DHI 1800
#define LP_IKM 30
#define LP_IKR 300
#define LP_IKX 900
#define LP_IKL 5
#endif

/* The parent's side: its LFN broadcast schedule and the registration
 * lifetime it grants (border router or router parenting LFNs) */
static inline void app_lfn_profile_parent(ws_lfn_settings_t *s)
{
    s->bc_interval_ms = LP_BCI;
    s->bc_sync_period = LP_BCSP;
    s->registration_lifetime_min = LP_REG;
}

/* The LFN's side */
static inline void app_lfn_profile_lfn(ws_lfn_settings_t *s)
{
    s->listen_interval_ms = LP_LI;
    s->listen_interval_min_ms = 1000;
    s->listen_interval_max_ms = LP_LIMAX;
    s->eapol_listen_interval_ms = 1300;
    s->maintain_parent_time = LP_MPT;
    s->lgtk_max_mismatch_s = LP_MISMATCH;
    s->registration_lifetime_min = LP_REG;
    s->bc_sync_only = LP_SYNCONLY;
    s->na_wait_s = LP_NAW;
    s->dhcp_max_delay_s = LP_DHD;
    s->dhcp_irt_s = LP_DHI;
    s->dhcp_mrt_s = 3600;
    s->initial_key_max_s = LP_IKM;
    s->initial_key_retry_min_s = LP_IKR;
    s->initial_key_retry_max_limit_s = LP_IKX;
    s->initial_key_retry_limit = LP_IKL;
}
#endif

#endif /* APP_LFN_PROFILE_H */
