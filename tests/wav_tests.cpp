#include "WavWriter.h"
#include <cassert>
#include <vector>
#include <iterator>
#include <iostream>
static uint32_t u32(const std::vector<uint8_t>& b,size_t n) {
    return uint32_t(b[n])|(uint32_t(b[n+1])<<8)|(uint32_t(b[n+2])<<16)|(uint32_t(b[n+3])<<24);
}
int main() {
    auto p=std::filesystem::temp_directory_path()/"connect-lab-wav-test.wav";
    uint8_t data[]={1,2,3,4};
    {WavWriter w(p);w.append(data,4);w.append(data,4);w.finish();}
    std::ifstream in(p,std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)),{});in.close();
    assert(b.size()==52 && u32(b,4)==44 && u32(b,24)==16000 && u32(b,28)==32000 && u32(b,40)==8);
    assert(std::string(b.begin(),b.begin()+4)=="RIFF" && b[44]==1 && b[51]==4);
    {WavWriter w(p);bool threw=false;try{w.append(data,3);}catch(...){threw=true;}assert(threw);w.finish();}
    {std::ifstream empty(p,std::ios::binary|std::ios::ate);assert(empty.tellg()==44);}
    std::filesystem::remove(p);std::cout<<"WAV structure, payload, empty recording and invalid alignment passed\n";
}
