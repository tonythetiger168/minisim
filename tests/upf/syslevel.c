/* The C model of syslevel.sv: one unit of energy per step while the supply
 * set of its domain is ON (IEEE 1801-2024 5.14). */
#include <string.h>
#include "msim_upf.h"
static int energy;
int sys_step(int* sim) {
    const char* st = msim_upf_power_state("SS_SYS");
    if (st && strcmp(st, "ON") == 0) energy++;
    *sim = msim_upf_simstate("PD_SYS");
    return energy;
}
