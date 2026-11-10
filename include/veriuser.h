/*
 * veriuser.h: the few IEEE 1364 TF routines that C code written for other
 * simulators still calls. minisim's VPI provides tf_dofinish and
 * tf_dostop. This file was written for minisim and is MIT licensed like the
 * rest of it.
 */
#ifndef VERIUSER_H
#define VERIUSER_H

#include "vpi_user.h"

#ifdef __cplusplus
extern "C" {
#endif

int tf_dofinish(void);   /* $finish */
int tf_dostop(void);     /* $stop */

#ifdef __cplusplus
}
#endif

#endif
