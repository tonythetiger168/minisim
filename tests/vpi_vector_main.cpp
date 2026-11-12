#define main minisim_embedded_vpi_main
#include "vpi_generic_main.cpp"
#undef main
#include "vector_cases.hpp"
int main(){
    auto design=build_generated_design();root=object(VH::Root);root->text=dut_top;
    for(auto&[name,id]:design.signals){auto h=object(VH::Signal);h->signal=id;h->text=name;signals.push_back(h);}
    auto input=vpi_handle_by_name("vpi_wide.din",nullptr);if(!input)return 3;
    for(auto& row:vector_cases){
        s_vpi_vecval words[2];words[0].aval=row[0];words[0].bval=row[1];words[1].aval=row[2];words[1].bval=row[3];
        s_vpi_value value{};value.format=vpiVectorVal;value.value.vector=words;vpi_put_value(input,&value,nullptr,vpiNoDelay);
        if(!adapter_error.empty())return 4;
        s_vpi_value got{};got.format=vpiVectorVal;vpi_get_value(input,&got);
        for(int i=0;i<2;++i)if(uint32_t(got.value.vector[i].aval)!=row[2*i] || uint32_t(got.value.vector[i].bval)!=row[2*i+1])return 5;
        got.format=vpiBinStrVal;vpi_get_value(input,&got);std::cout<<"ROW "<<got.value.str<<'\n';
    }
}
