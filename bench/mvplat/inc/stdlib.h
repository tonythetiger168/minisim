// stdlib.h - the part rt.c provides.
#pragma once
#include <stddef.h>
void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
