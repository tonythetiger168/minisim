/*
 * svdpi.h - SystemVerilog DPI-C interface for minisim (IEEE 1800-2023
 * clause 35 and annex H).
 *
 * C code called from SystemVerilog through `import "DPI-C"`, or calling
 * `export "DPI-C"` subroutines, includes this header. The types and function
 * names are the ones the standard defines, so the same C sources build
 * against other simulators' svdpi.h. This file was written for minisim and
 * is MIT licensed like the rest of the project.
 */
#ifndef INCLUDED_SVDPI
#define INCLUDED_SVDPI

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__CYGWIN__)
#define DPI_DLLISPEC __declspec(dllimport)
#define DPI_DLLESPEC __declspec(dllexport)
#else
#define DPI_DLLISPEC
#define DPI_DLLESPEC
#endif

#ifndef XXTERN
#define XXTERN DPI_EXTERN DPI_DLLESPEC
#endif
#ifndef DPI_EXTERN
#define DPI_EXTERN extern
#endif
#ifndef DPI_PROTOTYPES
#define DPI_PROTOTYPES
#endif

/* Scalars of type bit and logic (H.7.1). */
typedef uint8_t svScalar;
typedef svScalar svBit;    /* sv_0 or sv_1 */
typedef svScalar svLogic;  /* sv_0, sv_1, sv_z or sv_x */

#define sv_0 0
#define sv_1 1
#define sv_z 2
#define sv_x 3

/* Packed arrays in canonical form (H.7.7): 32 bits per word, bit 0 of the
 * array in bit 0 of the first word. svLogicVecVal holds a 4-state word:
 * aval/bval are 0/0 for 0, 1/0 for 1, 0/1 for z and 1/1 for x. */
#ifndef VPI_VECVAL
#define VPI_VECVAL
typedef struct t_vpi_vecval {
    uint32_t aval;
    uint32_t bval;
} s_vpi_vecval, *p_vpi_vecval;
#endif
typedef s_vpi_vecval svLogicVecVal;
typedef uint32_t svBitVecVal;

/* Words needed for a packed array of WIDTH bits. */
#define SV_PACKED_DATA_NELEMS(WIDTH) (((WIDTH) + 31) >> 5)

/* The low N bits of a word, N from 1 to 32. */
#define SV_MASK(N) (~(~0u << (N)))
#define SV_GET_UNSIGNED_BITS(VALUE, N) \
    ((N) == 32 ? (VALUE) : ((VALUE) & SV_MASK(N)))
#define SV_GET_SIGNED_BITS(VALUE, N) \
    ((N) == 32 ? (VALUE) : \
     (((VALUE) & (1u << ((N) - 1))) ? ((VALUE) | ~SV_MASK(N)) : ((VALUE) & SV_MASK(N))))

/* An instance scope (35.5.3) and an open array argument (35.5.6.1). */
typedef void* svScope;
typedef void* svOpenArrayHandle;

/* Version of the DPI this simulator implements: "1800-2023". */
XXTERN const char* svDpiVersion(void);

/* Bit selects and part selects of canonical packed arrays (H.10.1). */
XXTERN svBit svGetBitselBit(const svBitVecVal* s, int i);
XXTERN svLogic svGetBitselLogic(const svLogicVecVal* s, int i);
XXTERN void svPutBitselBit(svBitVecVal* d, int i, svBit s);
XXTERN void svPutBitselLogic(svLogicVecVal* d, int i, svLogic s);
XXTERN void svGetPartselBit(svBitVecVal* d, const svBitVecVal* s, int i, int w);
XXTERN void svGetPartselLogic(svLogicVecVal* d, const svLogicVecVal* s, int i, int w);
XXTERN void svPutPartselBit(svBitVecVal* d, const svBitVecVal s, int i, int w);
XXTERN void svPutPartselLogic(svLogicVecVal* d, const svLogicVecVal s, int i, int w);

/* Open array queries (H.12.2). Dimension 0 is the packed dimension and
 * dimensions 1.. are the unpacked ones, leftmost first. */
XXTERN int svLeft(const svOpenArrayHandle h, int d);
XXTERN int svRight(const svOpenArrayHandle h, int d);
XXTERN int svLow(const svOpenArrayHandle h, int d);
XXTERN int svHigh(const svOpenArrayHandle h, int d);
XXTERN int svIncrement(const svOpenArrayHandle h, int d);
XXTERN int svSize(const svOpenArrayHandle h, int d);
XXTERN int svDimensions(const svOpenArrayHandle h);

/* Open array data (H.12.3, H.12.4). Indices are SystemVerilog index values. */
XXTERN void* svGetArrayPtr(const svOpenArrayHandle h);
XXTERN int svSizeOfArray(const svOpenArrayHandle h);
XXTERN void* svGetArrElemPtr(const svOpenArrayHandle h, int indx1, ...);
XXTERN void* svGetArrElemPtr1(const svOpenArrayHandle h, int indx1);
XXTERN void* svGetArrElemPtr2(const svOpenArrayHandle h, int indx1, int indx2);
XXTERN void* svGetArrElemPtr3(const svOpenArrayHandle h, int indx1, int indx2, int indx3);

/* Copy one packed element of an open array in or out (H.12.5). */
XXTERN void svPutBitArrElemVecVal(const svOpenArrayHandle d, const svBitVecVal* s, int indx1, ...);
XXTERN void svPutBitArrElem1VecVal(const svOpenArrayHandle d, const svBitVecVal* s, int indx1);
XXTERN void svPutBitArrElem2VecVal(const svOpenArrayHandle d, const svBitVecVal* s, int indx1, int indx2);
XXTERN void svPutBitArrElem3VecVal(const svOpenArrayHandle d, const svBitVecVal* s, int indx1, int indx2,
                                   int indx3);
XXTERN void svPutLogicArrElemVecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int indx1, ...);
XXTERN void svPutLogicArrElem1VecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int indx1);
XXTERN void svPutLogicArrElem2VecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int indx1, int indx2);
XXTERN void svPutLogicArrElem3VecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int indx1, int indx2,
                                     int indx3);
XXTERN void svGetBitArrElemVecVal(svBitVecVal* d, const svOpenArrayHandle s, int indx1, ...);
XXTERN void svGetBitArrElem1VecVal(svBitVecVal* d, const svOpenArrayHandle s, int indx1);
XXTERN void svGetBitArrElem2VecVal(svBitVecVal* d, const svOpenArrayHandle s, int indx1, int indx2);
XXTERN void svGetBitArrElem3VecVal(svBitVecVal* d, const svOpenArrayHandle s, int indx1, int indx2, int indx3);
XXTERN void svGetLogicArrElemVecVal(svLogicVecVal* d, const svOpenArrayHandle s, int indx1, ...);
XXTERN void svGetLogicArrElem1VecVal(svLogicVecVal* d, const svOpenArrayHandle s, int indx1);
XXTERN void svGetLogicArrElem2VecVal(svLogicVecVal* d, const svOpenArrayHandle s, int indx1, int indx2);
XXTERN void svGetLogicArrElem3VecVal(svLogicVecVal* d, const svOpenArrayHandle s, int indx1, int indx2,
                                     int indx3);

/* Scalar elements of an open array of bit or logic (H.12.6). */
XXTERN svBit svGetBitArrElem(const svOpenArrayHandle s, int indx1, ...);
XXTERN svBit svGetBitArrElem1(const svOpenArrayHandle s, int indx1);
XXTERN svBit svGetBitArrElem2(const svOpenArrayHandle s, int indx1, int indx2);
XXTERN svBit svGetBitArrElem3(const svOpenArrayHandle s, int indx1, int indx2, int indx3);
XXTERN svLogic svGetLogicArrElem(const svOpenArrayHandle s, int indx1, ...);
XXTERN svLogic svGetLogicArrElem1(const svOpenArrayHandle s, int indx1);
XXTERN svLogic svGetLogicArrElem2(const svOpenArrayHandle s, int indx1, int indx2);
XXTERN svLogic svGetLogicArrElem3(const svOpenArrayHandle s, int indx1, int indx2, int indx3);
XXTERN void svPutBitArrElem(const svOpenArrayHandle d, svBit value, int indx1, ...);
XXTERN void svPutBitArrElem1(const svOpenArrayHandle d, svBit value, int indx1);
XXTERN void svPutBitArrElem2(const svOpenArrayHandle d, svBit value, int indx1, int indx2);
XXTERN void svPutBitArrElem3(const svOpenArrayHandle d, svBit value, int indx1, int indx2, int indx3);
XXTERN void svPutLogicArrElem(const svOpenArrayHandle d, svLogic value, int indx1, ...);
XXTERN void svPutLogicArrElem1(const svOpenArrayHandle d, svLogic value, int indx1);
XXTERN void svPutLogicArrElem2(const svOpenArrayHandle d, svLogic value, int indx1, int indx2);
XXTERN void svPutLogicArrElem3(const svOpenArrayHandle d, svLogic value, int indx1, int indx2, int indx3);

/* Context (35.5.3, H.9): scopes, user data, and the caller's location. */
XXTERN svScope svGetScope(void);
XXTERN svScope svSetScope(const svScope scope);
XXTERN const char* svGetNameFromScope(const svScope);
XXTERN svScope svGetScopeFromName(const char* scopeName);
XXTERN int svPutUserData(const svScope scope, void* userKey, void* userData);
XXTERN void* svGetUserData(const svScope scope, void* userKey);
XXTERN int svGetCallerInfo(const char** fileName, int* lineNumber);

/* Disable protocol (35.9). */
XXTERN int svIsDisabledState(void);
XXTERN void svAckDisabledState(void);

#undef DPI_EXTERN
#undef XXTERN

#ifdef __cplusplus
}
#endif

#endif /* INCLUDED_SVDPI */
