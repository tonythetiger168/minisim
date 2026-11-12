/* IEEE 1800-2023 37.17, 37.18, 37.29 (tests/spec/37-vpi-containers.sv). */
#include <stdio.h>
#include "sv_vpi_user.h"

static int geti(vpiHandle h) { s_vpi_value v = {vpiIntVal}; vpi_get_value(h, &v); return v.value.integer; }
static void puti(vpiHandle h, int x) { s_vpi_value v = {vpiIntVal}; v.value.integer = x; vpi_put_value(h, &v, NULL, vpiNoDelay); }
static const char* nm(vpiHandle h) { static char b[4][128]; static int n; const char* s = vpi_get_str(vpiFullName, h); n = (n + 1) % 4; snprintf(b[n], 128, "%s", s ? s : "(null)"); return b[n]; }

static void walkMembers(vpiHandle h, int depth) {
    vpiHandle it = vpi_iterate(vpiMember, h), m;
    while (it && (m = vpi_scan(it))) {
        vpi_printf("%*s%s type %d size %d", depth * 2, "", nm(m), vpi_get(vpiType, m), vpi_get(vpiSize, m));
        if (vpi_get(vpiType, m) == vpiStructVar) { vpi_printf("\n"); walkMembers(m, depth + 1); }
        else vpi_printf(" = %d\n", geti(m));
    }
}
static void array(const char* name) {
    vpiHandle a = vpi_handle_by_name((PLI_BYTE8*)name, NULL);
    vpi_printf("%s: type %d array type %d size %d\n", nm(a), vpi_get(vpiType, a), vpi_get(vpiArrayType, a), vpi_get(vpiSize, a));
    vpiHandle it = vpi_iterate(vpiReg, a), e;
    while (it && (e = vpi_scan(it))) vpi_printf("  %s = %d\n", nm(e), geti(e));
}
static PLI_INT32 show(PLI_BYTE8* u) {
    (void)u;
    vpiHandle s = vpi_handle_by_name("top.s", NULL);
    vpi_printf("%s: type %d size %d\n", nm(s), vpi_get(vpiType, s), vpi_get(vpiSize, s));
    walkMembers(s, 1);
    vpiHandle it = vpi_iterate(vpiMember, s), b = NULL, m;
    while ((m = vpi_scan(it))) b = m;          /* the last member: in */
    vpiHandle d = NULL;
    it = vpi_iterate(vpiMember, b);
    while ((m = vpi_scan(it))) d = m;          /* in.d */
    puti(d, 99);
    it = vpi_iterate(vpiMember, s);
    puti(vpi_scan(vpi_scan(it) ? it : it), -5); /* b (the second member) */
    vpi_free_object(it);

    array("top.dyn");
    array("top.q");
    array("top.aa");
    puti(vpi_handle_by_index(vpi_handle_by_name("top.dyn", NULL), 1), 21);
    puti(vpi_handle_by_index(vpi_handle_by_name("top.q", NULL), 1), 22);
    puti(vpi_handle_by_index(vpi_handle_by_name("top.aa", NULL), 5), 55);
    vpi_printf("dyn[7]: %s, aa[6]: %s\n", vpi_handle_by_index(vpi_handle_by_name("top.dyn", NULL), 7) ? "found" : "none",
               vpi_handle_by_index(vpi_handle_by_name("top.aa", NULL), 6) ? "found" : "none");

    vpiHandle p = vpi_handle_by_name("top.p", NULL);
    vpiHandle obj = vpi_handle(vpiClassObj, p);
    vpi_printf("%s: type %d, object type %d\n", nm(p), vpi_get(vpiType, p), obj ? vpi_get(vpiType, obj) : 0);
    it = vpi_iterate(vpiVariables, obj);
    vpiHandle len = NULL;
    while ((m = vpi_scan(it))) {
        vpi_printf("  %s type %d = %d\n", vpi_get_str(vpiName, m), vpi_get(vpiType, m), geti(m));
        if (!len) len = m;
    }
    puti(len, 128);
    vpi_printf("null handle object: %s\n", vpi_handle(vpiClassObj, vpi_handle_by_name("top.none", NULL)) ? "found" : "none");
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$show", show, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
