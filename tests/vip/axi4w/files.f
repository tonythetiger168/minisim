// AXI4 VIP self-tests on a wide bus: 128-bit data, 40-bit address, 4-bit
// ID, 16-bit xUSER (same benches as tests/vip/amba; run from the repository root)
//! top: vip_tb_top
//! test: vip_axi4_loopback_test
//! test: vip_axi4_outstanding_test
//! test: vip_axi4_excl_test
+define+AXI4_DATA_W=128
+define+AXI4_ADDR_W=40
+define+AXI4_ID_W=4
+define+AXI4_USER_W=16
+incdir+vip/mem
+incdir+vip/axi4
+incdir+vip/apb
vip/axi4/axi4_if.sv
vip/apb/apb_if.sv
vip/axi4/axi4_sva.sv
vip/apb/apb_sva.sv
vip/mem/mem_vip_pkg.sv
vip/axi4/axi4_pkg.sv
vip/apb/apb_pkg.sv
tests/vip/amba/vip_test_pkg.sv
tests/vip/amba/vip_tb_top.sv
