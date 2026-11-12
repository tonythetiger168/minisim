/*
 * vpi_compatibility.h - VPI compatibility modes for minisim (IEEE 1800-2023
 * 36.12 and annex L).
 *
 * An application written for an earlier standard defines one of
 *
 *   VPI_COMPATIBILITY_VERSION_1364v1995   VPI_COMPATIBILITY_VERSION_1364v2001
 *   VPI_COMPATIBILITY_VERSION_1364v2005   VPI_COMPATIBILITY_VERSION_1800v2005
 *   VPI_COMPATIBILITY_VERSION_1800v2009
 *
 * before it includes vpi_user.h (which includes this file). The routines
 * whose results differ between the standards are then bound to the variant
 * for that standard, so applications written for different standards can be
 * loaded together. With none defined the routines behave as the simulator's
 * default mode says: vpi_get(vpiCompatibilityMode, NULL).
 *
 * This file was written for minisim and is under the project's license.
 */
#ifndef VPI_COMPATIBILITY_H
#define VPI_COMPATIBILITY_H

#if defined(VPI_COMPATIBILITY_VERSION_1364v1995)
#if defined(VPI_COMPATIBILITY_VERSION_1364v2001) || defined(VPI_COMPATIBILITY_VERSION_1364v2005) || defined(VPI_COMPATIBILITY_VERSION_1800v2005) || defined(VPI_COMPATIBILITY_VERSION_1800v2009)
#error "define only one VPI_COMPATIBILITY_VERSION_ macro"
#endif
#define vpi_compare_objects vpi_compare_objects_1364v1995
#define vpi_control vpi_control_1364v1995
#define vpi_get vpi_get_1364v1995
#define vpi_get_str vpi_get_str_1364v1995
#define vpi_get_value vpi_get_value_1364v1995
#define vpi_handle vpi_handle_1364v1995
#define vpi_handle_by_index vpi_handle_by_index_1364v1995
#define vpi_handle_by_multi_index vpi_handle_by_multi_index_1364v1995
#define vpi_handle_by_name vpi_handle_by_name_1364v1995
#define vpi_handle_multi vpi_handle_multi_1364v1995
#define vpi_iterate vpi_iterate_1364v1995
#define vpi_put_value vpi_put_value_1364v1995
#define vpi_register_cb vpi_register_cb_1364v1995
#define vpi_scan vpi_scan_1364v1995
#elif defined(VPI_COMPATIBILITY_VERSION_1364v2001)
#if defined(VPI_COMPATIBILITY_VERSION_1364v2005) || defined(VPI_COMPATIBILITY_VERSION_1800v2005) || defined(VPI_COMPATIBILITY_VERSION_1800v2009)
#error "define only one VPI_COMPATIBILITY_VERSION_ macro"
#endif
#define vpi_compare_objects vpi_compare_objects_1364v2001
#define vpi_control vpi_control_1364v2001
#define vpi_get vpi_get_1364v2001
#define vpi_get_str vpi_get_str_1364v2001
#define vpi_get_value vpi_get_value_1364v2001
#define vpi_handle vpi_handle_1364v2001
#define vpi_handle_by_index vpi_handle_by_index_1364v2001
#define vpi_handle_by_multi_index vpi_handle_by_multi_index_1364v2001
#define vpi_handle_by_name vpi_handle_by_name_1364v2001
#define vpi_handle_multi vpi_handle_multi_1364v2001
#define vpi_iterate vpi_iterate_1364v2001
#define vpi_put_value vpi_put_value_1364v2001
#define vpi_register_cb vpi_register_cb_1364v2001
#define vpi_scan vpi_scan_1364v2001
#elif defined(VPI_COMPATIBILITY_VERSION_1364v2005)
#if defined(VPI_COMPATIBILITY_VERSION_1800v2005) || defined(VPI_COMPATIBILITY_VERSION_1800v2009)
#error "define only one VPI_COMPATIBILITY_VERSION_ macro"
#endif
#define vpi_compare_objects vpi_compare_objects_1364v2005
#define vpi_control vpi_control_1364v2005
#define vpi_get vpi_get_1364v2005
#define vpi_get_str vpi_get_str_1364v2005
#define vpi_get_value vpi_get_value_1364v2005
#define vpi_handle vpi_handle_1364v2005
#define vpi_handle_by_index vpi_handle_by_index_1364v2005
#define vpi_handle_by_multi_index vpi_handle_by_multi_index_1364v2005
#define vpi_handle_by_name vpi_handle_by_name_1364v2005
#define vpi_handle_multi vpi_handle_multi_1364v2005
#define vpi_iterate vpi_iterate_1364v2005
#define vpi_put_value vpi_put_value_1364v2005
#define vpi_register_cb vpi_register_cb_1364v2005
#define vpi_scan vpi_scan_1364v2005
#elif defined(VPI_COMPATIBILITY_VERSION_1800v2005)
#if defined(VPI_COMPATIBILITY_VERSION_1800v2009)
#error "define only one VPI_COMPATIBILITY_VERSION_ macro"
#endif
#define vpi_compare_objects vpi_compare_objects_1800v2005
#define vpi_control vpi_control_1800v2005
#define vpi_get vpi_get_1800v2005
#define vpi_get_str vpi_get_str_1800v2005
#define vpi_get_value vpi_get_value_1800v2005
#define vpi_handle vpi_handle_1800v2005
#define vpi_handle_by_index vpi_handle_by_index_1800v2005
#define vpi_handle_by_multi_index vpi_handle_by_multi_index_1800v2005
#define vpi_handle_by_name vpi_handle_by_name_1800v2005
#define vpi_handle_multi vpi_handle_multi_1800v2005
#define vpi_iterate vpi_iterate_1800v2005
#define vpi_put_value vpi_put_value_1800v2005
#define vpi_register_cb vpi_register_cb_1800v2005
#define vpi_scan vpi_scan_1800v2005
#elif defined(VPI_COMPATIBILITY_VERSION_1800v2009)
#define vpi_compare_objects vpi_compare_objects_1800v2009
#define vpi_control vpi_control_1800v2009
#define vpi_get vpi_get_1800v2009
#define vpi_get_str vpi_get_str_1800v2009
#define vpi_get_value vpi_get_value_1800v2009
#define vpi_handle vpi_handle_1800v2009
#define vpi_handle_by_index vpi_handle_by_index_1800v2009
#define vpi_handle_by_multi_index vpi_handle_by_multi_index_1800v2009
#define vpi_handle_by_name vpi_handle_by_name_1800v2009
#define vpi_handle_multi vpi_handle_multi_1800v2009
#define vpi_iterate vpi_iterate_1800v2009
#define vpi_put_value vpi_put_value_1800v2009
#define vpi_register_cb vpi_register_cb_1800v2009
#define vpi_scan vpi_scan_1800v2009
#endif

#endif /* VPI_COMPATIBILITY_H */
