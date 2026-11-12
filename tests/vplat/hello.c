// hello.c - the smallest program: print through the 16550 and pass.
#include "rt.h"

void exit(int code) { exit_(code); }

int main(int hart) {
    (void)hart;
    puts_("hello from mvplat\n");
    return 0;
}
