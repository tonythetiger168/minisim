# Forward SAIF (IEEE 1801-2024 Annex J): msim -saif with -saif_forward.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -P saif_fwd.cmake
# lib.saif (J.4) asks for state- and path-dependent toggles and leakage of
# cell AN2 (J.3.6 to J.3.10); rtl.saif (J.5) maps RTL points to gate names,
# with a virtual instance for a typed port (J.3.11). The (VERSION ..) line
# is left out of the comparison.
set(D ${SRC}/tests/saif)
set(B ${CMAKE_CURRENT_BINARY_DIR})
foreach(t lib rtl)
  execute_process(COMMAND ${MSIM} ${D}/cells.sv -top top -saif ${B}/fwd_${t}.saif -saif_forward ${D}/${t}.saif
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim -saif_forward ${t}.saif exited ${rc}\n${out}${err}")
  endif()
  file(READ ${B}/fwd_${t}.saif got)
  string(REGEX REPLACE "\\(VERSION [^\n]*\n" "" got "${got}")
  file(READ ${D}/${t}.out want)
  if(NOT got STREQUAL want)
    message(FATAL_ERROR "${t}: backward SAIF differs\n--- got\n${got}--- want\n${want}")
  endif()
endforeach()
# A bad forward file stops msim with its file, line and clause.
function(fwd_error name text want)
  file(WRITE ${B}/${name}.saif "${text}")
  execute_process(COMMAND ${MSIM} ${D}/cells.sv -top top -saif ${B}/${name}.out.saif -saif_forward ${B}/${name}.saif
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${err}" "${want}" pos)
  if(rc EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "${name}: expected the error '${want}': ${rc}\n${out}${err}")
  endif()
endfunction()
fwd_error(fe_dir "(SAIFILE (SAIFVERSION \"2.0\") (DIRECTION \"backward\"))" "not a forward SAIF file")
fwd_error(fe_default "(SAIFILE (SAIFVERSION \"2.0\" \"lib\") (DIRECTION \"forward\")\n(LIBRARY \"l\" (MODULE AN2 (PORT (A (COND_DEFAULT COND B))))))"
          "fe_default.saif:2: COND_DEFAULT must be the last item")
fwd_error(fe_leak "(SAIFILE (SAIFVERSION \"2.0\" \"lib\") (DIRECTION \"forward\")\n(LIBRARY \"l\" (MODULE AN2 (LEAKAGE (COND A RISE_FALL)))))"
          "RISE_FALL is for toggle directives, not LEAKAGE (J.4.4)")
fwd_error(fe_cond "(SAIFILE (SAIFVERSION \"2.0\" \"lib\") (DIRECTION \"forward\")\n(LIBRARY \"l\" (MODULE AN2 (PORT (A (COND (B * ))))))))"
          "expected a pin name in a COND expression")
fwd_error(fe_map "(SAIFILE (SAIFVERSION \"2.0\") (DIRECTION \"forward\")\n(INSTANCE top (NET (y)))))"
          "a net mapping is (rtl_name mapped_name) (J.5.3)")
