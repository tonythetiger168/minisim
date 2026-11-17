// dhrystone_rt.c - what riscv-tests' Dhrystone expects from its run time
// beyond rt.c: setStats (debug_printf is its own).
void setStats(int enable) { (void)enable; }
