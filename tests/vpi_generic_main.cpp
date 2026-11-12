// Experimental flat-counter VPI host. Uses real installed cocotb, not mocks.
#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include "design.hpp"
#include <vpi_user.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#if __has_include("vpi_manifest.hpp")
#include "vpi_manifest.hpp"
#else
const std::string dut_top="counter";
const std::vector<std::string> dut_outputs={"cnt"};
#endif
struct VH {
    enum Kind {Root,Signal,Constant,Iterator,Callback} kind;
    int signal=-1, number=0; size_t index=0; bool live=true; uint64_t native_id=0;
    std::string text; std::vector<VH*> entries;
    s_cb_data cb{}; s_vpi_time time{}; s_vpi_value value{};
};
std::vector<std::unique_ptr<VH>> handles;
std::vector<VH*> start_callbacks,end_callbacks,signals;
VH* root=nullptr;std::string adapter_error;int sim_argc;char** sim_argv;
VH* object(VH::Kind kind) { auto h=std::make_unique<VH>();h->kind=kind;auto p=h.get();handles.push_back(std::move(h));return p; }
vpiHandle handle(VH* h){return reinterpret_cast<vpiHandle>(h);}
VH* obj(vpiHandle h){return reinterpret_cast<VH*>(h);}
void unsupported(std::string what){adapter_error="VPI unsupported: "+what;std::cerr<<adapter_error<<'\n';S.finished=true;}
void stamp(s_vpi_time* t){if(t){t->type=vpiSimTime;t->high=S.now>>32;t->low=S.now;}}
void arm(VH* h);
void invoke(VH* h){
    if(!h->live)return;stamp(&h->time);
    h->cb.cb_rtn(&h->cb);
    if(h->live && h->cb.reason==cbValueChange && !S.finished)arm(h);
}
void arm(VH* h){
    auto fn=[h]{invoke(h);};
    switch(h->cb.reason){
    case cbValueChange:h->native_id=S.on_edge(obj(h->cb.obj)->signal,ANY,fn);break;
    case cbReadWriteSynch:h->native_id=S.on_readwrite(fn);break;
    case cbReadOnlySynch:h->native_id=S.on_readonly(fn);break;
    case cbAfterDelay:h->native_id=S.on_timer((uint64_t(h->time.high)<<32)|h->time.low,fn);break;
    default:unsupported("callback reason "+std::to_string(h->cb.reason));
    }
}
extern "C" {
PLI_INT32 vpi_chk_error(p_vpi_error_info info){if(info)*info={};return 0;}
void vpi_control(PLI_INT32 operation,...){
    if(operation!=vpiFinish && operation!=vpiStop)unsupported("control "+std::to_string(operation));
    S.finished=true;
}
PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info info){
    static char product[]="Minisim",version[]="experimental-vpi-counter-v1";
    info->argc=sim_argc;info->argv=sim_argv;info->product=product;info->version=version;return 1;
}
void vpi_get_time(vpiHandle,p_vpi_time time){stamp(time);}
vpiHandle vpi_handle_by_name(const char* name,vpiHandle scope){
    if(!name)return nullptr;std::string n=name;
    if(n==dut_top)return handle(root);
    if(n.rfind(dut_top+".",0)==0)n=n.substr(dut_top.size()+1);
    if(scope && obj(scope)!=root)return nullptr;
    for(auto h:signals)if(h->text==n)return handle(h);
    return nullptr;
}
vpiHandle vpi_handle_by_index(vpiHandle,PLI_INT32){unsupported("indexed handles");return nullptr;}
vpiHandle vpi_handle(PLI_INT32 type,vpiHandle ref){
    VH* h=obj(ref);if(!h)return nullptr;
    if(type==vpiParent || type==vpiScope)return h==root?nullptr:handle(root);
    if(type==vpiLeftRange || type==vpiRightRange){
        if(h->kind!=VH::Signal)return nullptr;
        auto c=object(VH::Constant);c->number=type==vpiLeftRange?S.sigs[h->signal].w-1:0;return handle(c);
    }
    unsupported("handle relation "+std::to_string(type));return nullptr;
}
PLI_INT32 vpi_get(PLI_INT32 property,vpiHandle ref){
    VH* h=obj(ref);
    switch(property){
    case vpiTimePrecision:case vpiTimeUnit:return -9;
    case vpiType:return !h?0:h->kind==VH::Root?vpiModule:h->kind==VH::Constant?vpiConstant:vpiReg;
    case vpiSize:return h && h->kind==VH::Signal?S.sigs[h->signal].w:32;
    case vpiVector:return h && h->kind==VH::Signal && S.sigs[h->signal].w>1;
    case vpiSigned:return 0;
    case vpiConstType:return vpiDecConst;
    case vpiDirection:return h && h->kind==VH::Signal && std::find(dut_outputs.begin(),dut_outputs.end(),h->text)!=dut_outputs.end()?vpiOutput:vpiInput;
    case vpiScalar:return h && h->kind==VH::Signal && S.sigs[h->signal].w==1;
    default:unsupported("property "+std::to_string(property));return 0;
    }
}
PLI_BYTE8* vpi_get_str(PLI_INT32 property,vpiHandle ref){
    static std::string value;VH* h=obj(ref);value.clear();if(!h)return nullptr;
    switch(property){
    case vpiName:case vpiDefName:value=h->text;break;
    case vpiFullName:value=h==root?h->text:dut_top+"."+h->text;break;
    case vpiType:value=h==root?"vpiModule":"vpiReg";break;
    default:unsupported("string property "+std::to_string(property));return nullptr;
    }
    return value.data();
}
void vpi_get_value(vpiHandle ref,p_vpi_value out){
    auto h=obj(ref);if(!h){unsupported("null value handle");return;}
    if(h->kind!=VH::Signal && h->kind!=VH::Constant){unsupported("read requires signal or constant");return;}
    Val v=h->kind==VH::Constant?mk(h->number,32):S.get(h->signal);
    static std::string binary;static s_vpi_vecval vector[2];
    switch(out->format){
    case vpiIntVal:out->value.integer=v.v;break;
    case vpiBinStrVal:
        binary.clear();for(int i=v.w-1;i>=0;--i)binary+=((v.x>>i)&1)?(((v.z>>i)&1)?'z':'x'):(((v.v>>i)&1)?'1':'0');
        out->value.str=binary.data();break;
    case vpiVectorVal:
        for(int i=0;i<2;++i){vector[i].aval=(v.v|(v.x&~v.z))>>(32*i);vector[i].bval=v.x>>(32*i);}
        out->value.vector=vector;break;
    case vpiSuppressVal:break;
    default:unsupported("read format "+std::to_string(out->format));
    }
}
vpiHandle vpi_put_value(vpiHandle ref,p_vpi_value in,p_vpi_time time,PLI_INT32 flags){
    try{
        VH* h=obj(ref);if(!h || h->kind!=VH::Signal || std::find(dut_outputs.begin(),dut_outputs.end(),h->text)!=dut_outputs.end())throw std::runtime_error("write requires input signal");
        if((flags!=vpiNoDelay && flags!=vpiInertialDelay) || (time && (time->high || time->low)))
            throw std::runtime_error("only zero-delay deposit supported");
        uint64_t v=0,x=0,z=0;int width=S.sigs[h->signal].w;
        if(in->format==vpiIntVal)v=uint32_t(in->value.integer);
        else if(in->format==vpiBinStrVal){
            for(char c:std::string(in->value.str)){v<<=1;x<<=1;z<<=1;if(c=='1')v|=1;else if(c=='x'||c=='X')x|=1;else if(c=='z'||c=='Z'){x|=1;z|=1;}else if(c!='0')throw std::runtime_error("invalid binary value");}
        }else if(in->format==vpiVectorVal){
            if(!in->value.vector)throw std::runtime_error("null vector value");
            uint64_t a=0,b=0;for(int i=0;i<(width+31)/32;++i){a|=uint64_t(uint32_t(in->value.vector[i].aval))<<(32*i);b|=uint64_t(uint32_t(in->value.vector[i].bval))<<(32*i);}
            v=a&~b;x=b;z=b&~a;
        }else throw std::runtime_error("write format "+std::to_string(in->format));
        S.set(h->signal,make(v,x,width,z));
    }catch(const std::exception& e){unsupported(e.what());}
    return nullptr;
}
vpiHandle vpi_iterate(PLI_INT32 type,vpiHandle scope){
    auto i=object(VH::Iterator);
    if(type==vpiModule && !scope)i->entries.push_back(root);
    else if(type==vpiReg && obj(scope)==root)i->entries=signals;
    if(i->entries.empty())return nullptr;return handle(i);
}
vpiHandle vpi_scan(vpiHandle iterator){auto i=obj(iterator);if(!i || i->index>=i->entries.size())return nullptr;return handle(i->entries[i->index++]);}
PLI_INT32 vpi_free_object(vpiHandle ref){auto h=obj(ref);if(h && h->kind==VH::Iterator)h->live=false;return 1;}
vpiHandle vpi_register_cb(p_cb_data data){
    auto h=object(VH::Callback);h->cb=*data;
    if(data->time)h->time=*data->time;h->cb.time=&h->time;
    if(data->value)h->value=*data->value;h->cb.value=&h->value;
    if(data->reason==cbStartOfSimulation)start_callbacks.push_back(h);
    else if(data->reason==cbEndOfSimulation)end_callbacks.push_back(h);
    else try{arm(h);}catch(const std::exception& e){unsupported(e.what());return nullptr;}
    return handle(h);
}
PLI_INT32 vpi_remove_cb(vpiHandle cb){auto h=obj(cb);if(!h)return 0;h->live=false;if(h->native_id)S.cancel_callback(h->native_id);return 1;}
}
int main(int argc,char** argv){
    sim_argc=argc;sim_argv=argv;
    try{
        S.collect_coverage=std::getenv("MINISIM_COVERAGE")!=nullptr;
        auto design=build_generated_design();root=object(VH::Root);root->text=dut_top;
        for(const auto& [name,id]:design.signals){auto h=object(VH::Signal);h->signal=id;h->text=name;signals.push_back(h);}
        for(auto& p:design.programs)S.spawn(exec(&p));S.advance_to(0);
        const char* lib=std::getenv("MINISIM_COCOTB_VPI");if(!lib)throw std::runtime_error("MINISIM_COCOTB_VPI missing");
        void* loaded=dlopen(lib,RTLD_NOW|RTLD_GLOBAL);if(!loaded)throw std::runtime_error(dlerror());
        auto routines=reinterpret_cast<void(**)()>(dlsym(loaded,"vlog_startup_routines"));
        if(!routines)throw std::runtime_error("cocotb startup routines missing");
        for(auto f=routines;*f;++f)(*f)();
        for(auto h:start_callbacks)invoke(h);
        S.run(1000000);
        for(auto h:end_callbacks)invoke(h);
        if(S.collect_coverage){
            std::ofstream line("line.json"),toggle("toggle.json");
            if(!line || !toggle)throw std::runtime_error("coverage output open failed");
            S.line_coverage.write_json(line);S.toggle_coverage.write_json(toggle);
            line.flush();toggle.flush();
            if(!line || !toggle)throw std::runtime_error("coverage output write failed");
        }
        if(!adapter_error.empty())return 2;
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
