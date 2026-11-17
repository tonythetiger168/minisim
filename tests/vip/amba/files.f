// AXI4 and APB VIP loopback self-tests (run from the repository root)
//! top: vip_tb_top
//! test: vip_axi4_loopback_test
//! test: vip_axi4_outstanding_test
//! test: vip_axi4_excl_test
//! test: vip_apb_loopback_test
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
