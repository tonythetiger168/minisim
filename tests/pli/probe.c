/* The C side of tests/pli_protect.sh: what VPI and DPI may and may not do
 * with protected IP (IEEE 1800-2023 37.3.6, 38.x; IEEE 1735-2023 9.7).
 *
 * $ip_task and $ip_func are called by the protected IP itself and must work
 * (model fidelity). $probe is called by the IP user's testbench and tries to
 * get at the IP. */
#include <stdio.h>
#include <string.h>
#include "svdpi.h"
#include "vpi_user.h"

static vpiHandle kept[4];   /* argument handles of $ip_task, kept for $probe */

static const char *str(const char *s) { return s ? s : "(null)"; }
static int err(void) { return vpi_chk_error(NULL) ? 1 : 0; }
/* The call is made first (as the argument), the error state read after it. */
static void show_h(const char *what, vpiHandle h, const char *kind) {
    int e = err();
    vpi_printf("%s: %s error=%d\n", what, h ? kind : "null", e);
}
static void show_s(const char *what, const char *s) {
    int e = err();
    vpi_printf("%s=%s error=%d\n", what, str(s), e);
}
static void show_i(const char *what, int i) {
    int e = err();
    vpi_printf("%s=%d error=%d\n", what, i, e);
}

static PLI_INT32 ip_task(PLI_BYTE8 *ud) {
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL), it, a;
    int n = 0;
    (void)ud;
    vpi_printf("ip_task: call type=%d protected=%d\n", (int)vpi_get(vpiType, call) == vpiSysTaskCall,
               (int)vpi_get(vpiIsProtected, call));
    it = vpi_iterate(vpiArgument, call);          /* allowed for a protected call (38.36) */
    while (it && (a = vpi_scan(it))) {
        s_vpi_value v = {vpiHexStrVal};
        int e;
        vpi_get_value(a, &v);                      /* its own arguments: allowed while it runs */
        e = err();
        vpi_printf("ip_task: arg%d protected=%d size=%d value=%s\n", n, (int)vpi_get(vpiIsProtected, a),
                   (int)vpi_get(vpiSize, a), e ? "(error)" : v.value.str);
        if (n < 4) kept[n] = a;
        n++;
    }
    {   /* write an argument back: the IP asked for it by passing the variable */
        s_vpi_value v = {vpiIntVal};
        v.value.integer = 0x33;
        vpi_put_value(kept[0], &v, NULL, vpiNoDelay);
        vpi_printf("ip_task: put error=%d\n", err());
    }
    /* Even from inside the IP's call, the design around it stays closed. */
    show_h("ip_task: scope of the call", vpi_handle(vpiScope, call), "handle");
    return 0;
}
static PLI_INT32 ip_func(PLI_BYTE8 *ud) {
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle a = vpi_scan(vpi_iterate(vpiArgument, call));
    s_vpi_value v = {vpiIntVal};
    (void)ud;
    vpi_get_value(a, &v);
    v.value.integer += 1;
    vpi_put_value(call, &v, NULL, vpiNoDelay);     /* the function's result */
    return 0;
}
static PLI_INT32 ip_func_size(PLI_BYTE8 *ud) { (void)ud; return 8; }

static PLI_INT32 on_change(p_cb_data d) { (void)d; vpi_printf("probe: a callback fired on a protected object\n"); return 0; }

static void by_name(const char *n) {
    vpiHandle h = vpi_handle_by_name((PLI_BYTE8 *)n, NULL);
    vpi_printf("probe: by_name %s: %s error=%d\n", n, h ? "handle" : "null", err());
}

static PLI_INT32 probe(PLI_BYTE8 *ud) {
    vpiHandle top = vpi_handle_by_name((PLI_BYTE8 *)"pli_tb", NULL), it, m, u = NULL, h;
    s_vpi_value v = {vpiHexStrVal};
    s_cb_data cb;
    (void)ud;
    /* Navigation reaches the protected instance (37.3.6 NOTE). */
    it = vpi_iterate(vpiModule, top);
    while (it && (m = vpi_scan(it))) u = m;
    vpi_printf("probe: instance type=%d protected=%d\n", (int)vpi_get(vpiType, u) == vpiModule, (int)vpi_get(vpiIsProtected, u));
    show_s("probe: instance name", vpi_get_str(vpiName, u));
    show_s("probe: instance fullname", vpi_get_str(vpiFullName, u));
    show_s("probe: instance defname", vpi_get_str(vpiDefName, u));
    show_s("probe: instance type string", vpi_get_str(vpiType, u));
    show_i("probe: instance line", (int)vpi_get(vpiLineNo, u));
    show_h("probe: iterate nets", vpi_iterate(vpiNet, u), "iterator");
    show_h("probe: iterate regs", vpi_iterate(vpiReg, u), "iterator");
    show_h("probe: iterate ports", vpi_iterate(vpiPort, u), "iterator");
    show_h("probe: iterate modules", vpi_iterate(vpiModule, u), "iterator");
    show_h("probe: iterate parameters", vpi_iterate(vpiParameter, u), "iterator");
    show_h("probe: handle parent", vpi_handle(vpiParent, u), "handle");
    h = vpi_handle_by_name((PLI_BYTE8 *)"hidden", u);
    show_h("probe: by_name hidden in the protected scope", h, "handle");

    /* Disclosed names find nothing (38.20, IEEE 1735 9.7.1). */
    by_name("pli_tb.u");
    by_name("pli_tb.u.hidden");
    by_name("pli_tb.u.mem");
    by_name("pli_tb.u.SECRET_P");
    by_name("pli_tb.u.c");
    by_name("pli_tb.u.c.cc");
    by_name("pli_tb.u.q");
    /* What a viewport makes visible is found, and is not protected. */
    h = vpi_handle_by_name((PLI_BYTE8 *)"pli_tb.u.seen", NULL);
    vpi_get_value(h, &v);
    vpi_printf("probe: viewport object %s protected=%d value=%s\n", str(vpi_get_str(vpiFullName, h)),
               (int)vpi_get(vpiIsProtected, h), v.value.str);
    /* The testbench's own objects are untouched. */
    h = vpi_handle_by_name((PLI_BYTE8 *)"pli_tb.d", NULL);
    vpi_printf("probe: clear object %s protected=%d\n", str(vpi_get_str(vpiName, h)), (int)vpi_get(vpiIsProtected, h));

    /* A handle to a protected variable, kept from the IP's own call. */
    h = kept[0];
    vpi_printf("probe: kept type=%d protected=%d\n", (int)vpi_get(vpiType, h) == vpiReg, (int)vpi_get(vpiIsProtected, h));
    show_s("probe: kept name", vpi_get_str(vpiName, h));
    show_i("probe: kept size", (int)vpi_get(vpiSize, h));
    v.format = vpiHexStrVal;
    vpi_get_value(h, &v);
    vpi_printf("probe: kept get_value error=%d\n", err());
    v.format = vpiIntVal;
    v.value.integer = 0x77;
    vpi_put_value(h, &v, NULL, vpiNoDelay);
    vpi_printf("probe: kept put_value error=%d\n", err());
    vpi_put_value(h, &v, NULL, vpiForceFlag);
    vpi_printf("probe: kept force error=%d\n", err());
    show_h("probe: kept bit", vpi_handle_by_index(h, 0), "handle");
    show_h("probe: kept bits", vpi_iterate(vpiBit, h), "iterator");
    show_h("probe: kept scope", vpi_handle(vpiScope, h), "handle");
    memset(&cb, 0, sizeof cb);
    cb.reason = cbValueChange;
    cb.cb_rtn = on_change;
    cb.obj = h;
    show_h("probe: kept callback", vpi_register_cb(&cb), "handle");
    /* The expression argument of the IP's call. */
    h = kept[3];
    v.format = vpiHexStrVal;
    vpi_get_value(h, &v);
    {
        int e = err();
        vpi_printf("probe: kept expression protected=%d size=%d get_value error=%d\n", (int)vpi_get(vpiIsProtected, h),
                   (int)vpi_get(vpiSize, h), e);
    }
    return 0;
}

static void reg(const char *name, int type, PLI_INT32 (*calltf)(PLI_BYTE8 *), PLI_INT32 (*sizetf)(PLI_BYTE8 *)) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.sysfunctype = type == vpiSysFunc ? vpiSizedFunc : 0;
    d.tfname = (PLI_BYTE8 *)name;
    d.calltf = calltf;
    d.sizetf = sizetf;
    vpi_register_systf(&d);
}
static void startup(void) {
    reg("$ip_task", vpiSysTask, ip_task, NULL);
    reg("$ip_func", vpiSysFunc, ip_func, ip_func_size);
    reg("$probe", vpiSysTask, probe, NULL);
}
void (*vlog_startup_routines[])(void) = {startup, 0};

/* ---- DPI (IEEE 1735-2023 9.7.2.3) ---- */
void dpi_where(int tag) {
    const char *f = "?";
    int line = -1;
    svScope s = svGetScope();
    int ok = svGetCallerInfo(&f, &line);
    const char *base = f ? strrchr(f, '/') : NULL;
    printf("dpi %d: scope=%s caller=%s:%d ok=%d\n", tag, svGetNameFromScope(s), base ? base + 1 : f, line, ok);
    fflush(stdout);
}
void dpi_lookup(void) {
    static int key, data = 42;
    /* The disclosed name still sets up a context in the protected instance. */
    svScope s = svGetScopeFromName("pli_tb.u");
    int *back;
    svPutUserData(s, &key, &data);
    back = (int *)svGetUserData(s, &key);
    printf("dpi lookup: found=%d name=%s userdata=%d\n", s != NULL, s ? svGetNameFromScope(s) : "(null)", back ? *back : -1);
    printf("dpi lookup: child name=%s\n", svGetNameFromScope(svGetScopeFromName("pli_tb.u.c")));
    printf("dpi lookup: own name=%s\n", svGetNameFromScope(svGetScopeFromName("pli_tb")));
    fflush(stdout);
}
