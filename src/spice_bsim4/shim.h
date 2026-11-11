/* shim.h - what the BSIM4 model files (third_party/bsim4, unmodified) expect
 * from their host simulator, provided by mspice.
 *
 * The model files were written for SPICE3 and include "ngspice/...h"
 * headers. The headers of that name next to this file all include this one.
 * It declares the handful of types, constants and functions the model code
 * uses; spice_bsim4.c implements the functions on top of mspice.
 *
 * Only names and layouts the model files rely on are given; everything here
 * is mspice code, not a copy of the simulator the files came from.
 */
#ifndef MSPICE_BSIM4_SHIM_H
#define MSPICE_BSIM4_SHIM_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The model's per-instance load function and the serial matrix pass: the
 * split mspice's deterministic threads need. No OpenMP is involved; the
 * pragma in the model file is ignored by the compiler. */
#define USE_OMP 1

/* ------------------------------------------------------------ small things */
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define OK 0
#define E_PANIC 1
#define E_EXISTS 2
#define E_NODEV 3
#define E_NOMOD 4
#define E_NOANAL 5
#define E_NOTERM 6
#define E_BADPARM 7
#define E_NOMEM 8
#define E_NODECON 9
#define E_UNSUPP 10
#define E_PARMVAL 11
#define E_BAD_DOMAIN 100
#define ERR_WARNING 1
#define ERR_FATAL 2
#define ERR_PANIC 4
#define ERR_INFO 8

#define NUMELEMS(a) (sizeof(a) / sizeof(*(a)))
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define NG_IGNORE(x) (void)(x)
#define NG_IGNOREABLE(x) (void)(x)

#define TMALLOC(type, n) ((type*)calloc((size_t)(n), sizeof(type)))
#define TREALLOC(type, p, n) ((type*)realloc((p), (size_t)(n) * sizeof(type)))
#define FREE(p) do { free(p); (p) = NULL; } while (0)
#define tfree(p) do { free(p); (p) = NULL; } while (0)
char* copy(const char* s);
char* tprintf(const char* fmt, ...);

/* ------------------------------------------------------------ constants */
#define CONSTsqrt2 1.4142135623730950488016887242097
#define CONSTpi 3.1415926535897932384626433832795
#define CONSTnap 2.7182818284590452353602874713527
#define CONSTCtoK 273.15
#define CONSTKtoC (-CONSTCtoK)
#define CHARGE 1.6021766208e-19
#define CONSTboltz 1.38064852e-23
#define CONSTplanck 6.626070040e-34
#define CONSTc 299792458
#define CONSTmuZero (4.0 * CONSTpi * 1E-7)
#define CONSTepsZero (1.0 / (CONSTmuZero * CONSTc * CONSTc))
#define CONSTepsrSiO2 3.9
#define CONSTepsSiO2 (CONSTepsrSiO2 * CONSTepsZero)
#define REFTEMP (27.0 + CONSTCtoK)
extern double CONSTroot2, CONSTvt0, CONSTKoverQ, CONSTe;

/* ------------------------------------------------------------ parameters */
typedef char* IFuid;
typedef struct { double real, imag; } IFcomplex;
typedef struct CKTnode CKTnode;
typedef struct CKTnode* IFnode;
typedef union IFvalue {
    int iValue;
    double rValue;
    IFcomplex cValue;
    char* sValue;
    IFuid uValue;
    CKTnode* nValue;
    struct {
        int numValue;
        union { int* iVec; double* rVec; char** sVec; } vec;
    } v;
} IFvalue;
typedef struct IFparm {
    char* keyword;
    int id;
    int dataType;
    char* description;
} IFparm;

#define IF_FLAG 0x1
#define IF_INTEGER 0x2
#define IF_REAL 0x4
#define IF_COMPLEX 0x8
#define IF_NODE 0x10
#define IF_STRING 0x20
#define IF_INSTANCE 0x40
#define IF_PARSETREE 0x80
#define IF_VECTOR 0x8000
#define IF_REALVEC (IF_REAL | IF_VECTOR)
#define IF_VARTYPES 0x80ff
#define IF_SET 0x2000
#define IF_ASK 0x1000
#define IF_REDUNDANT 0x0010000
#define IOP(a, b, c, d) { a, b, c | IF_SET | IF_ASK, d }
#define IOPR(a, b, c, d) { a, b, c | IF_SET | IF_ASK | IF_REDUNDANT, d }
#define IOPU(a, b, c, d) { a, b, c | IF_SET | IF_ASK, d }
#define IP(a, b, c, d) { a, b, c | IF_SET, d }
#define OP(a, b, c, d) { a, b, c | IF_ASK, d }

/* ------------------------------------------------------------ devices */
typedef struct GENmodel GENmodel;
typedef struct GENinstance GENinstance;
struct GENinstance {
    GENmodel* GENmodPtr;
    GENinstance* GENnextInstance;
    IFuid GENname;
    int GENstate;   /* index of the instance's first state */
};
struct GENmodel {
    int GENmodType;
    GENmodel* GENnextModel;
    GENinstance* GENinstances;
    IFuid GENmodName;
    struct wordlist* defaults;
};

/* ------------------------------------------------------------ the circuit */
#define MODE 0x3
#define MODETRAN 0x1
#define MODEAC 0x2
#define MODEACNOISE 0x8
#define MODEDC 0x70
#define MODEDCOP 0x10
#define MODETRANOP 0x20
#define MODEDCTRANCURVE 0x40
#define INITF 0x3f00
#define MODEINITFLOAT 0x100
#define MODEINITJCT 0x200
#define MODEINITFIX 0x400
#define MODEINITSMSIG 0x800
#define MODEINITTRAN 0x1000
#define MODEINITPRED 0x2000
#define MODEUIC 0x10000l

#define TRAPEZOIDAL 1
#define GEAR 2

struct CKTnode {
    IFuid name;
    int type;
    int number;
    double ic, nodeset;
    unsigned icGiven : 1;
    unsigned nsGiven : 1;
};

typedef struct SMPmatrix SMPmatrix;
typedef struct JOB {
    int JOBtype;
    struct JOB* JOBnextJob;
    IFuid JOBname;
} JOB;
typedef struct TSKtask { JOB* jobs; } TSKtask;
struct circ { void* ci_curTask; };
extern struct circ* ft_curckt;

typedef struct CKTcircuit {
    double* CKTstates[8];
#define CKTstate0 CKTstates[0]
#define CKTstate1 CKTstates[1]
#define CKTstate2 CKTstates[2]
#define CKTstate3 CKTstates[3]
    double CKTtime, CKTdelta, CKTdeltaOld[7];
    double CKTtemp, CKTnomTemp, CKTvt;
    double CKTag[7];
    int CKTorder, CKTmaxOrder, CKTintegrateMethod;
    double* CKTrhs;
    double* CKTrhsOld;
    double* CKTirhs;
    double* CKTirhsOld;
    SMPmatrix* CKTmatrix;
    long CKTmode;
    int CKTbypass, CKTnoncon;
    double CKTabstol, CKTreltol, CKTvoltTol, CKTchgtol, CKTtrtol;
    double CKTgmin, CKTgshunt, CKTomega, CKTsrcFact;
    int CKTcopyNodesets;
    int CKTnumStates;
    JOB* CKTcurJob;
    GENinstance* CKTtroubleElt;
    int CKTsoaCheck;
    /* mspice: the adapter that owns this circuit view */
    void* host;
} CKTcircuit;

int CKTmkVolt(CKTcircuit*, CKTnode**, IFuid, char*);
int CKTinst2Node(CKTcircuit*, void*, int, CKTnode**, IFuid*);
int CKTdltNNum(CKTcircuit*, int);
double* SMPmakeElt(SMPmatrix*, int row, int col);
int NIintegrate(CKTcircuit*, double* geq, double* ceq, double cap, int qcap);
void CKTterr(int qcap, CKTcircuit*, double* timeStep);
double DEVfetlim(double vnew, double vold, double vto);
double DEVpnjlim(double vnew, double vold, double vt, double vcrit, int* icheck);
double DEVlimvds(double vnew, double vold);

/* Noise analysis (not in mspice yet): only what the declarations need. */
#define NSTATVARS 3
#define LNLSTDENS 0
#define OUTNOIZ 1
#define INNOIZ 2
typedef struct { double real, imag; } SPcomplex;
typedef struct Ndata Ndata;
#define BSIZE_SP 512

/* ------------------------------------------------------------ messages */
typedef struct IFfrontEnd {
    void (*IFerrorf)(int flags, const char* fmt, ...);
} IFfrontEnd;
extern IFfrontEnd* SPfrontEnd;

typedef struct wordlist {
    char* wl_word;
    struct wordlist* wl_next;
    struct wordlist* wl_prev;
} wordlist;
void wl_append_word(wordlist** first, wordlist** last, char* word);
void wl_free(wordlist*);
enum cp_types { CP_BOOL, CP_NUM, CP_REAL, CP_STRING, CP_LIST };
int cp_getvar(char* name, enum cp_types type, void* retval, size_t rsize);

#ifdef __cplusplus
}
#endif
#endif
