#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hive {
struct HevcFormat {
    uint32_t width=0,height=0,chroma=0,luma_bits=0,chroma_bits=0;
    bool separate_planes=false;
    bool operator==(const HevcFormat&)const=default;
    bool full_chroma8()const{return chroma==3&&!separate_planes&&luma_bits==8&&chroma_bits==8;}
};
// Bounded metadata reader for codec qualification, not a complete HEVC decoder
// or bitstream validator. H.265 sections 7.3.2.2.1 and 7.3.3: inspect actual SPS
// chroma/depth/crop, rather than trusting RGB input or a driver capability query.
inline std::optional<HevcFormat> hevc_sps_format(std::span<const uint8_t> nal){
    if(nal.size()<3||nal.size()>4096||(nal[0]&0x81)||((nal[0]>>1)&63)!=33||(nal[1]&0xf8)||!(nal[1]&7))return {};
    std::vector<uint8_t> rbsp;unsigned zeros=0;
    for(size_t i=2;i<nal.size();++i){
        const auto b=nal[i];
        if(zeros>=2&&b==3){if(i+1==nal.size()||nal[i+1]>3)return {};zeros=0;continue;}
        rbsp.push_back(b);zeros=b==0?zeros+1:0;
    }
    struct Bits {
        std::span<const uint8_t> bytes;size_t bit=0;bool valid=true;
        uint32_t read(unsigned n){
            if(n>32||bit+n>bytes.size()*8){valid=false;return 0;}
            uint32_t v=0;while(n--){v=(v<<1)|((bytes[bit/8]>>(7-bit%8))&1);++bit;}return v;
        }
        void skip(unsigned n){while(n&&valid){const auto part=n>32?32:n;read(part);n-=part;}}
        uint32_t ue(){unsigned zeros=0;while(valid&&!read(1))if(++zeros>30){valid=false;return 0;}return valid?((1u<<zeros)-1+read(zeros)):0;}
    } bits{rbsp};
    bits.skip(4);const auto layers=bits.read(3);bits.skip(1);if(layers>6)return {};
    bits.skip(96);std::array<bool,7> profile{},level{};
    for(unsigned i=0;i<layers;++i){profile[i]=bits.read(1)!=0;level[i]=bits.read(1)!=0;}
    if(layers)bits.skip((8-layers)*2);
    for(unsigned i=0;i<layers;++i){if(profile[i])bits.skip(88);if(level[i])bits.skip(8);}
    if(bits.ue()>15)return {};
    HevcFormat result;result.chroma=bits.ue();if(result.chroma>3)return {};
    if(result.chroma==3)result.separate_planes=bits.read(1)!=0;
    result.width=bits.ue();result.height=bits.ue();
    if(!result.width||!result.height||result.width>65536||result.height>65536)return {};
    if(bits.read(1)){
        const uint64_t left=bits.ue(),right=bits.ue(),top=bits.ue(),bottom=bits.ue();
        const auto chroma=result.separate_planes?0:result.chroma;
        const auto crop_x=(left+right)*(chroma==1||chroma==2?2:1),crop_y=(top+bottom)*(chroma==1?2:1);
        if(crop_x>=result.width||crop_y>=result.height)return {};
        result.width-=uint32_t(crop_x);result.height-=uint32_t(crop_y);
    }
    const auto luma=bits.ue(),chroma=bits.ue();if(!bits.valid||luma>8||chroma>8)return {};
    result.luma_bits=luma+8;result.chroma_bits=chroma+8;return result;
}
inline std::optional<HevcFormat> hevc_annexb_format(std::span<const uint8_t> bytes){
    if(bytes.size()>64ull*1024*1024)return {};
    const auto prefix=[&](size_t i)->size_t{
        if(i+3<=bytes.size()&&bytes[i]==0&&bytes[i+1]==0){
            if(bytes[i+2]==1)return 3;
            if(i+4<=bytes.size()&&bytes[i+2]==0&&bytes[i+3]==1)return 4;
        }return 0;
    };
    std::optional<HevcFormat> found;
    for(size_t i=0;i<bytes.size();){
        const auto n=prefix(i);if(!n){++i;continue;}
        const auto begin=i+n;size_t end=begin;while(end<bytes.size()&&!prefix(end))++end;
        if(end>begin&&((bytes[begin]>>1)&63)==33){
            const auto parsed=hevc_sps_format(bytes.subspan(begin,end-begin));
            if(!parsed||(found&&*found!=*parsed))return {};found=parsed;
        }
        i=end;
    }
    return found;
}
}
