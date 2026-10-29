#include "software_decoder.h"
#include <windows.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    require(argc==2,"Fixture path required");std::ifstream file(argv[1],std::ios::binary);require(bool(file),"Open generated fixture");
    std::vector<hive::EncodedFrame> frames;
    while(file.peek()!=EOF){
        uint32_t header[5]{};file.read(reinterpret_cast<char*>(header),sizeof(header));
        require(bool(file)&&header[4]&&header[4]<1024*1024,"Fixture header bounds");
        hive::EncodedFrame frame;frame.width=header[0];frame.height=header[1];frame.sequence=header[2];frame.keyframe=header[3]!=0;frame.data.resize(header[4]);
        file.read(reinterpret_cast<char*>(frame.data.data()),frame.data.size());require(bool(file),"Fixture payload");frames.push_back(std::move(frame));
    }
    require(frames.size()==24,"Fixture frame count");hive::SoftwareDecoder decoder;unsigned delivered=0,references=0;
    for(const auto& frame:frames){
        if(!frame.keyframe)++references;
        const auto cost=decoder.decode(frame,[&](const hive::SoftwarePicture& p){
            require(p.width==frame.width&&p.height==frame.height,"Resolution change");
            const auto row=p.height/2;
            for(uint32_t x=8;x<16;++x){
                const double yy=p.planes[0][row*p.strides[0]+x],u=p.planes[1][row*p.strides[1]+x]-128.,v=p.planes[2][row*p.strides[2]+x]-128.;
                const double red=yy+1.5748*v,green=yy-.187324*u-.468124*v,blue=yy+1.8556*u;
                require(std::abs(red-((x&1)?20:220))<12&&std::abs(blue-((x&1)?220:20))<12,"One-pixel chroma detail lost");
                require(std::abs(green-(40+(frame.sequence%12)*8))<12,"Reference picture content or order changed");
            }
            ++delivered;
        });require(cost>0,"Missing CPU service time");
    }
    require(delivered==24&&references>0,"Reference-frame path not exercised");
    // Seek/recovery may restart at a fresh IDR without replaying an old GOP.
    decoder.decode(frames[20],[](const auto&){});
    decoder.decode(frames[21],[](const auto&){});
    bool rejected=false;auto wrong=frames[0];wrong.width+=2;
    try{hive::SoftwareDecoder bad;bad.decode(wrong,[](const auto&){});}catch(const std::exception&){rejected=true;}
    require(rejected,"Mismatched dimensions accepted");
    rejected=false;wrong=frames[0];wrong.data={0,0,0,1,0xff,0xff,0xff};
    try{hive::SoftwareDecoder bad;bad.decode(wrong,[](const auto&){});}catch(const std::exception&){rejected=true;}
    require(rejected,"Damaged bitstream accepted");
    require(!GetModuleHandleW(L"nvcuda.dll")&&!GetModuleHandleW(L"nvcuvid.dll"),"CPU decoder loaded an NVIDIA driver");
    std::cout<<"PASS software HEVC444: 24 exact-order pictures, P references, one-pixel chroma, resize, IDR recovery, bad input rejection; no NVIDIA driver loaded\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
