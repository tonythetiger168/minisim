// Counter fixture C ABI for external-driver validation, not a cocotb GPI backend.
#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include "design.hpp"
GeneratedDesign external_design;
bool initialized = false;
std::string bridge_error;
template<class F> int checked(F f) noexcept {
    try { bridge_error.clear(); f(); return 0; }
    catch (const std::exception& e) { bridge_error=e.what(); return -1; }
    catch (...) { bridge_error="unknown bridge error"; return -1; }
}
int lookup(const char* name) {
    if (!initialized) throw std::runtime_error("bridge not initialized");
    if (!name) throw std::runtime_error("null signal name");
    return external_design.signals.at(name);
}
extern "C" {
const char* ms_error() { return bridge_error.c_str(); }
int ms_init() { return checked([] {
    if (initialized) throw std::runtime_error("bridge already initialized");
    external_design=build_generated_design(); initialized=true;
    for (auto& p:external_design.programs) S.spawn(exec(&p));
    S.advance_to(0);
}); }
int ms_write(const char* name, uint64_t v, uint64_t x, uint64_t z) {
    return checked([&] {
        int id=lookup(name);
        if (std::string(name)!="clk" && std::string(name)!="rst")
            throw std::runtime_error("counter output is read only");
        S.set(id,make(v,x,S.sigs[id].w,z));
    });
}
int ms_read(const char* name,uint64_t* v,uint64_t* x,uint64_t* z) {
    return checked([&] {
        if (!v || !x || !z) throw std::runtime_error("null read buffer");
        auto value=S.get(lookup(name)); *v=value.v; *x=value.x; *z=value.z;
    });
}
int ms_advance(uint64_t end) { return checked([&] {
    if (!initialized) throw std::runtime_error("bridge not initialized");
    S.advance_to(end); S.vcd.flush();
}); }
uint64_t ms_time() { return S.now; }
int ms_callback(int kind,const char* signal,uint64_t delay,int edge,
                void (*fn)(uint64_t),uint64_t token,uint64_t* id) {
    return checked([&] {
        if (!initialized || !fn || !id) throw std::runtime_error("invalid callback registration");
        auto invoke=[fn,token] { fn(token); };
        switch(kind) {
            case 0: *id=S.on_timer(delay,invoke); break;
            case 1:
                if (edge < 0 || edge > 2) throw std::runtime_error("invalid edge kind");
                *id=S.on_edge(lookup(signal),static_cast<Edge>(edge),invoke); break;
            case 2: *id=S.on_readwrite(invoke); break;
            case 3: *id=S.on_readonly(invoke); break;
            default: throw std::runtime_error("invalid callback kind");
        }
    });
}
int ms_cancel(uint64_t id) { return S.cancel_callback(id) ? 1 : 0; }
}
