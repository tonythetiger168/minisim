/* 35.5.6: string members of an unpacked struct argument (const char*). */
#include <stdio.h>
#include <string.h>
#include "svdpi.h"

typedef struct {
    const char* name;
    int n;
    short tag;
    const char* note;
} item_t;

int c_show(const item_t* it) {
    printf("C: %s n=%d tag=%d note=%s\n", it->name, it->n, it->tag, it->note);
    return (int)strlen(it->name) + (int)strlen(it->note);
}

static char buf[64];
void c_fill(int k, item_t* it) {
    snprintf(buf, sizeof buf, "item%d", k);
    it->name = buf;
    it->n = -k;
    it->tag = (short)(k * 100);
    it->note = k % 2 ? "odd" : "even";
}

void c_upper(item_t* it) {
    static char up[64];
    size_t i = 0;
    for (; it->name[i] && i + 1 < sizeof up; i++) up[i] = (char)(it->name[i] >= 'a' && it->name[i] <= 'z' ? it->name[i] - 32 : it->name[i]);
    up[i] = 0;
    it->name = up;
    it->n += 1;
}

int c_count(const item_t* its, int n) {   /* a sized array of such structs */
    int total = 0;
    for (int k = 0; k < n; k++) total += (int)strlen(its[k].name) * its[k].n;
    return total;
}
