/* Array values in one call (IEEE 1800-2023 38.16 vpi_get_value_array,
 * 38.35 vpi_put_value_array). */
#include <string.h>
#include "vpi_user.h"

static PLI_INT32 arrays(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle it = vpi_iterate(vpiArgument, call);
    vpiHandle mem = vpi_scan(it), tiny = vpi_scan(it);
    vpi_free_object(it);
    PLI_INT32 idx;

    /* Integers, in the simulator's buffer. An x or z bit reads as 0. */
    s_vpi_arrayvalue av = {vpiIntVal, 0, {0}};
    idx = 1;
    vpi_get_value_array(mem, &av, &idx, 4);
    vpi_printf("ints from 1: %d %d %d %d\n", av.value.integers[0], av.value.integers[1], av.value.integers[2],
               av.value.integers[3]);

    /* The caller's buffer; past the last element it goes on from the first. */
    PLI_INT32 mine[3];
    s_vpi_arrayvalue uv = {vpiIntVal, vpiUserAllocFlag, {0}};
    uv.value.integers = mine;
    idx = 4;
    vpi_get_value_array(mem, &uv, &idx, 3);
    vpi_printf("user buffer from 4: %d %d %d\n", mine[0], mine[1], mine[2]);

    /* Four-state vectors: aval/bval per element. */
    s_vpi_arrayvalue vv = {vpiVectorVal, 0, {0}};
    idx = 2;
    vpi_get_value_array(mem, &vv, &idx, 2);
    vpi_printf("vectors from 2: %03x/%03x %03x/%03x\n", (unsigned)vv.value.vectors[0].aval,
               (unsigned)vv.value.vectors[0].bval, (unsigned)vv.value.vectors[1].aval, (unsigned)vv.value.vectors[1].bval);

    /* Raw bytes: two per 12-bit element, lowest byte first. */
    s_vpi_arrayvalue rv = {vpiRawTwoStateVal, 0, {0}};
    idx = 0;
    vpi_get_value_array(mem, &rv, &idx, 2);
    unsigned char *b = (unsigned char *)rv.value.rawvals;
    vpi_printf("raw two-state from 0: %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3]);
    s_vpi_arrayvalue fv = {vpiRawFourStateVal, 0, {0}};
    idx = 3;
    vpi_get_value_array(mem, &fv, &idx, 1);
    b = (unsigned char *)fv.value.rawvals;
    vpi_printf("raw four-state from 3: %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3]);

    /* A signed array declared [3:0]: from index 3 toward index 0. */
    s_vpi_arrayvalue lv = {vpiLongIntVal, 0, {0}};
    idx = 3;
    vpi_get_value_array(tiny, &lv, &idx, 4);
    vpi_printf("longs of tiny from 3: %d %d %d %d\n", (int)lv.value.longints[0], (int)lv.value.longints[1],
               (int)lv.value.longints[2], (int)lv.value.longints[3]);

    /* Writes: two elements; one value for two elements; one without propagation. */
    PLI_INT32 two[2] = {0xaa, 0xbb};
    s_vpi_arrayvalue pv = {vpiIntVal, 0, {0}};
    pv.value.integers = two;
    idx = 4;
    vpi_put_value_array(mem, &pv, &idx, 2);
    PLI_INT32 one = 0x777;
    s_vpi_arrayvalue ov = {vpiIntVal, vpiOneValue, {0}};
    ov.value.integers = &one;
    idx = 0;
    vpi_put_value_array(mem, &ov, &idx, 2);
    PLI_INT16 sh[2] = {-5, 6};
    s_vpi_arrayvalue sv = {vpiShortIntVal, vpiPropagateOff, {0}};
    sv.value.shortints = sh;
    idx = 3;
    vpi_put_value_array(tiny, &sv, &idx, 2);

    /* Errors are reported through vpi_chk_error. */
    idx = 9;
    vpi_get_value_array(mem, &av, &idx, 1);
    if (!vpi_chk_error(NULL)) vpi_printf("no error for an index outside the array\n");
    return 0;
}

static void boot(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$arrays";
    d.calltf = arrays;
    vpi_register_systf(&d);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
