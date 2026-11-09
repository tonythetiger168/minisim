/*
 * sv_vpi_user.h: the SystemVerilog additions to VPI (IEEE 1800-2023 annex M)
 * that C code written for other simulators includes. The object types are
 * the standard's numbers. This file was written for minisim and is MIT
 * licensed like the rest of it.
 */
#ifndef SV_VPI_USER_H
#define SV_VPI_USER_H

#include "vpi_user.h"

#define vpiPackage 600
#define vpiInterface 601
#define vpiProgram 602
#define vpiInterfaceArray 603
#define vpiProgramArray 604
#define vpiTypespec 605
#define vpiModport 606
#define vpiInterfaceTfDecl 607
#define vpiRefObj 608
#define vpiTypeParameter 609

#define vpiLongIntVar 610
#define vpiShortIntVar 611
#define vpiIntVar 612
#define vpiShortRealVar 613
#define vpiByteVar 614
#define vpiClassVar 615
#define vpiStringVar 616
#define vpiEnumVar 617
#define vpiStructVar 618
#define vpiUnionVar 619
#define vpiBitVar 620
#define vpiClassObj 621
#define vpiChandleVar 622
#define vpiPackedArrayVar 623
#define vpiVirtualInterfaceVar 728

#define vpiArrayVar vpiRegArray
#define vpiArrayNet vpiNetArray

#endif
