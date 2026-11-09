// UVM's DPI library (IEEE 1800.2-2020) for minisim.
//
// The Accellera uvm-core C code (src/dpi) picks an HDL backdoor backend per
// vendor in uvm_hdl.c. This file builds the same library with a backdoor
// written against plain VPI (IEEE 1800-2023 clauses 36-38), which minisim
// provides: compile it with uvm-core's src/dpi on the include path, as
// `msim -uvm` does.
//
//   regular expressions   uvm_regex.cc (POSIX regcomp / regexec)
//   command line          uvm_svcmd_dpi.c (vpi_get_vlog_info)
//   HDL backdoor          uvm_hdl_* below (vpi_handle_by_name, vpi_get_value,
//                         vpi_put_value with vpiNoDelay / force / release)
//   polling               uvm_hdl_polling.c (value-change callbacks)

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include "uvm_dpi.h"

#include "uvm_common.c"
#include "uvm_regex.cc"
#include "uvm_svcmd_dpi.c"

// ---- HDL backdoor access (uvm_hdl.svh) over VPI ----

static void uvm_hdl_error(const char* id, const char* msg) {
    m_uvm_report_dpi(M_UVM_ERROR, (char*)id, (char*)msg, M_UVM_NONE, (char*)__FILE__, __LINE__);
}

static vpiHandle uvm_hdl_handle(const char* path, const char* id, int report) {
    vpiHandle r = vpi_handle_by_name((PLI_BYTE8*)path, 0);
    if (!r && report) {
        const char* fmt = "unable to locate hdl path (%s)\n Either the name is incorrect, or the object is not visible to VPI";
        char* msg = (char*)malloc(strlen(fmt) + strlen(path) + 1);
        sprintf(msg, fmt, path);
        uvm_hdl_error(id, msg);
        free(msg);
    }
    return r;
}

// uvm_hdl_data_t: UVM_HDL_MAX_WIDTH bits (uvm_pkg parameter, 1024 by default).
static int uvm_hdl_max_width() {
    static int w = -1;
    if (w < 0) {
        w = 1024;
        if (vpiHandle ms = vpi_handle_by_name((PLI_BYTE8*)"uvm_pkg::UVM_HDL_MAX_WIDTH", 0)) {
            s_vpi_value v = {vpiIntVal, {0}};
            vpi_get_value(ms, &v);
            if (v.value.integer > 0) w = v.value.integer;
            vpi_release_handle(ms);
        }
    }
    return w;
}

static int uvm_hdl_set(const char* path, p_vpi_vecval value, PLI_INT32 flag) {
    vpiHandle r = uvm_hdl_handle(path, "UVM/DPI/HDL_SET", 1);
    if (!r) return 0;
    s_vpi_value v;
    v.format = vpiVectorVal;
    v.value.vector = value;
    s_vpi_time t = {vpiSimTime, 0, 0, 0.0};
    vpi_put_value(r, &v, &t, flag);
    int bad = vpi_chk_error(NULL);
    vpi_release_handle(r);
    if (bad) {
        const char* fmt = "set: unable to write to hdl path (%s)";
        char* msg = (char*)malloc(strlen(fmt) + strlen(path) + 1);
        sprintf(msg, fmt, path);
        uvm_hdl_error("UVM/DPI/HDL_SET", msg);
        free(msg);
        return 0;
    }
    return 1;
}

static int uvm_hdl_get(const char* path, p_vpi_vecval value) {
    vpiHandle r = uvm_hdl_handle(path, "UVM/DPI/HDL_GET", 1);
    if (!r) return 0;
    int maxw = uvm_hdl_max_width();
    int size = vpi_get(vpiSize, r);
    if (size > maxw) {
        const char* fmt = "uvm_reg : hdl path '%s' is %0d bits, but the maximum size is %0d.  You can increase the maximum via a compile-time flag: +define+UVM_HDL_MAX_WIDTH=<value>";
        char* msg = (char*)malloc(strlen(fmt) + strlen(path) + 32);
        sprintf(msg, fmt, path, size, maxw);
        uvm_hdl_error("UVM/DPI/HDL_SET", msg);
        free(msg);
        vpi_release_handle(r);
        return 0;
    }
    s_vpi_value v;
    v.format = vpiVectorVal;
    vpi_get_value(r, &v);
    int chunks = (maxw - 1) / 32 + 1, have = size > 0 ? (size - 1) / 32 + 1 : 0;
    for (int i = 0; i < chunks; i++) {
        if (i < have && v.value.vector) {
            value[i].aval = v.value.vector[i].aval;
            value[i].bval = v.value.vector[i].bval;
            if (i == have - 1 && size % 32) {   // only the object's own bits
                PLI_UINT32 m = (1u << (size % 32)) - 1;
                value[i].aval &= m;
                value[i].bval &= m;
            }
        } else {
            value[i].aval = 0;
            value[i].bval = 0;
        }
    }
    vpi_release_handle(r);
    return 1;
}

int uvm_hdl_check_path(char* path) {
    vpiHandle r = uvm_hdl_handle(path, "UVM/DPI/HDL_CHECK", 0);
    if (!r) return 0;
    vpi_release_handle(r);
    return 1;
}

int uvm_hdl_read(char* path, p_vpi_vecval value) { return uvm_hdl_get(path, value); }

int uvm_hdl_deposit(char* path, p_vpi_vecval value) { return uvm_hdl_set(path, value, vpiNoDelay); }

int uvm_hdl_force(char* path, p_vpi_vecval value) { return uvm_hdl_set(path, value, vpiForceFlag); }

int uvm_hdl_release(char* path) {
    s_vpi_vecval z[1] = {{0, 0}};
    return uvm_hdl_set(path, z, vpiReleaseFlag);
}

int uvm_hdl_release_and_read(char* path, p_vpi_vecval value) {
    if (!uvm_hdl_release(path)) return 0;
    return uvm_hdl_get(path, value);
}

#include "uvm_hdl_polling.c"

#ifdef __cplusplus
}
#endif
