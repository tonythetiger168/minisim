// Compile-only ABI surface check; this does not implement VPI or start cocotb.
#include <vpi_user.h>
static_assert(sizeof(PLI_UINT32)==4);
static_assert(sizeof(s_vpi_time{}.high)==4 && sizeof(s_vpi_time{}.low)==4);
static_assert(sizeof(s_vpi_vecval{}.aval)==4 && sizeof(s_vpi_vecval{}.bval)==4);
PLI_INT32 probe_callback(p_cb_data) { return 0; }
void probe_types() {
    s_cb_data callback{}; callback.reason=cbReadOnlySynch; callback.cb_rtn=probe_callback;
    s_vpi_time time{};time.type=vpiSimTime;callback.time=&time;
    s_vpi_value value{};value.format=vpiBinStrVal;
    auto register_cb=&vpi_register_cb; auto put_value=&vpi_put_value;
    (void)callback;(void)value;(void)register_cb;(void)put_value;
}
