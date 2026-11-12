#define main minisim_embedded_vpi_main
#include "vpi_generic_main.cpp"
#undef main
int main(int argc,char**argv){
    if(argc!=2)return 3;
    auto design=build_generated_design();root=object(VH::Root);root->text=dut_top;
    for(auto&[name,id]:design.signals){auto h=object(VH::Signal);h->signal=id;h->text=name;signals.push_back(h);}
    std::string mode=argv[1];auto input=vpi_handle_by_name("tinyalu.a",nullptr);
    s_vpi_value value{};value.format=vpiIntVal;value.value.integer=1;
    s_vpi_time time{};time.type=vpiSimTime;time.low=1;
    if(mode=="output")vpi_put_value(vpi_handle_by_name("tinyalu.result",nullptr),&value,nullptr,vpiNoDelay);
    else if(mode=="delay")vpi_put_value(input,&value,&time,vpiInertialDelay);
    else if(mode=="force")vpi_put_value(input,&value,nullptr,vpiForceFlag);
    else if(mode=="bad-binary"){value.format=vpiBinStrVal;char invalid[]="00000002";value.value.str=invalid;vpi_put_value(input,&value,nullptr,vpiNoDelay);}
    else if(mode=="index")vpi_handle_by_index(input,0);
    else if(mode=="read-format"){value.format=vpiRealVal;vpi_get_value(input,&value);}
    else if(mode=="null-vector"){value.format=vpiVectorVal;value.value.vector=nullptr;vpi_put_value(input,&value,nullptr,vpiNoDelay);}
    else if(mode=="module-read"){vpi_get_value(handle(root),&value);if(adapter_error.empty())std::cout<<"UNEXPECTED_MODULE_VALUE "<<value.value.integer<<'\n';}
    else if(mode=="missing-name")return vpi_handle_by_name("tinyalu.missing",nullptr)==nullptr && adapter_error.empty()?0:4;
    else return 3;
    if(adapter_error.empty() || !S.finished)return 4;
    std::cout<<"EXPECTED_REJECTION "<<adapter_error<<'\n';return 0;
}
