#ifndef HOST_SL_POWER_MANAGER_H
#define HOST_SL_POWER_MANAGER_H
typedef enum { SL_POWER_MANAGER_EM0, SL_POWER_MANAGER_EM1, SL_POWER_MANAGER_EM2 } sl_power_manager_em_t;
void sl_power_manager_add_em_requirement(sl_power_manager_em_t em);
#endif
