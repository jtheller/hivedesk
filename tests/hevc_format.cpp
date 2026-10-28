#include "hevc_format.h"
#include "hivedesk/media.h"
#include <cstdio>
#include <stdexcept>
namespace hive {EncodedFrame media_generated_test_frame();}
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
std::vector<uint8_t> fixture(unsigned chroma,unsigned depth,unsigned sublayers=0){
    std::vector<uint8_t> rbsp;size_t bits=0;
    const auto put=[&](uint32_t value,unsigned n){for(unsigned i=n;i>0;--i){if(bits%8==0)rbsp.push_back(0);rbsp.back()|=((value>>(i-1))&1)<<(7-bits%8);++bits;}};
    const auto zero=[&](unsigned n){for(unsigned i=0;i<n;++i)put(0,1);};
    const auto ue=[&](unsigned v){++v;unsigned n=0;for(auto x=v;x>1;x>>=1)++n;zero(n);put(v,n+1);};
    put(0,4);put(sublayers,3);put(1,1);zero(96);
    for(unsigned i=0;i<sublayers;++i){put(1,1);put(1,1);}if(sublayers)zero((8-sublayers)*2);
    for(unsigned i=0;i<sublayers;++i)zero(96);
    ue(0);ue(chroma);if(chroma==3)put(0,1);ue(640);ue(368);put(1,1);ue(0);ue(0);ue(0);ue(chroma==1?4:8);ue(depth-8);ue(depth-8);put(1,1);
    std::vector<uint8_t> out{0,0,0,1,66,1};unsigned zeros=0;
    for(auto b:rbsp){if(zeros>=2&&b<=3){out.push_back(3);zeros=0;}out.push_back(b);zeros=b==0?zeros+1:0;}return out;
}
}
int main(){try{
    for(auto chroma:{1u,2u,3u})for(auto depth:{8u,10u})for(auto layers:{0u,2u}){
        const auto data=fixture(chroma,depth,layers);const auto f=hive::hevc_annexb_format(data);
        require(f&&f->width==640&&f->height==360&&f->chroma==chroma&&f->luma_bits==depth,"SPS format/crop/profile sublayers parsed incorrectly");
        require(f->full_chroma8()==(chroma==3&&depth==8),"Nonmatching chroma/depth qualified");
    }
    auto valid=fixture(3,8);auto conflicting=valid;auto other=fixture(1,8);conflicting.insert(conflicting.end(),other.begin(),other.end());
    require(!hive::hevc_annexb_format(conflicting),"Conflicting SPS accepted");
    valid[4]|=0x80;require(!hive::hevc_annexb_format(valid),"Forbidden NAL bit accepted");
    valid=fixture(3,8);for(size_t n=0;n<valid.size()-1;++n)require(!hive::hevc_annexb_format(std::span(valid).first(n)),"Truncated SPS accepted");
    const auto encoded=hive::media_generated_test_frame();const auto actual=hive::hevc_annexb_format(encoded.data);
    require(actual&&actual->full_chroma8()&&actual->width==encoded.width&&actual->height==encoded.height,"Real NVENC SPS did not validate HEVC444");
    std::puts("PASS SPS metadata: real NVENC 4:4:4, 4:2:0/4:2:2 and 10-bit distinction, cropping, sublayers, malformed/conflicting/truncated input");return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
