/* msim_upf.h - power state of the running power intent, for C and C++
 * models (IEEE 1801-2024 5.14: system-level models, such as SystemC or TLM
 * models called through DPI-C, follow the power intent for power analysis).
 * msim and simv export these functions when run with -upf.
 */
#ifndef MSIM_UPF_H
#define MSIM_UPF_H
#ifdef __cplusplus
extern "C" {
#endif
/* The current power state of a power domain, supply set or power state
 * group, by its UPF name ("PD_CORE", "SS_AON", "u_sub/PD_X"): "ON", "OFF",
 * or a state of add_power_state; "" when the object has no state yet or is
 * not in the power intent, NULL when the simulation has no power intent.
 * The string stays valid until the next call. */
const char* msim_upf_power_state(const char* name);
/* The current simstate of a power domain or supply set (4.8): 0 NORMAL,
 * 1 CORRUPT_STATE_ON_CHANGE, 2 CORRUPT_STATE_ON_ACTIVITY, 3 CORRUPT_ON_CHANGE,
 * 4 CORRUPT_ON_ACTIVITY, 5 CORRUPT; -1 when there is no such object. */
int msim_upf_simstate(const char* name);
#ifdef __cplusplus
}
#endif
#endif
