// ext.hpp - the interface between mvplat and an extension library.
//
// mvplat is the platform engine: harts, the memory map, the bundled devices,
// the platform file. An extension library adds options and device types to
// it without a second copy of the engine: it is built against these same
// headers and loaded with dlopen the first time the command line or the
// platform file asks for something mvplat does not know itself. Nothing is
// loaded for a run that does not need it.
//
// The library is found at $MVPLAT_EXT, else at
// <prefix>/lib/minisim/libmvplat_ext.so beside the installed mvplat, and
// exports one C function, MVPLAT_EXT_ENTRY, which returns its Ext.
#pragma once

#include <filesystem>
#include <string>

#include "json.hpp"
#include "machine.hpp"

namespace vp {

// Changes whenever Ext, Machine, Hart or Device change shape, so a library
// built against other headers is refused rather than misread.
constexpr int EXT_ABI = 2;

struct Ext {
    virtual ~Ext() = default;
    // The option at argv[i], which mvplat does not know: the number of
    // arguments it takes (itself included), or 0 if it is not the library's
    // either. Throws std::runtime_error for a bad value.
    virtual int option(int argc, char** argv, int i) = 0;
    // True if the options given mean the run needs no program (it starts
    // from a saved state, say).
    virtual bool needsNoProgram() const { return false; }
    // A device of a type mvplat does not build: add it (and anything it
    // brings) to m.bus and return true, or return false if the type is not
    // the library's either. `dir` is the platform file's directory.
    virtual bool device(Machine& m, const json::Value& d, const std::string& type, const std::string& name,
                        const std::filesystem::path& dir) = 0;
    // The platform is built and the program loaded; Machine::start() comes
    // next. Set m.profile, m.dbg and the like here.
    virtual void prepare(Machine& m) = 0;
    // After Machine::start(), before Machine::run().
    virtual void started(Machine& m) = 0;
    // After the run, with the exit code it would end with; returns the one
    // it ends with. Throws std::runtime_error to fail with exit code 2.
    virtual int finished(Machine& m, int code) = 0;
    // Lines for mvplat -h.
    virtual std::string usage() const { return ""; }
};

}  // namespace vp

// extern "C" vp::Ext* mvplat_ext(int abi, int argc, char** argv);
// Returns nullptr if `abi` is not EXT_ABI. May exit (a missing licence).
#define MVPLAT_EXT_ENTRY "mvplat_ext"
using MvplatExtEntry = vp::Ext* (*)(int abi, int argc, char** argv);
