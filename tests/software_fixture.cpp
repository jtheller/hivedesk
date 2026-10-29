#include "hivedesk/media.h"
#include <fstream>
#include <iostream>
namespace hive {std::vector<EncodedFrame> media_generated_software_fixture();}
int main(int argc,char** argv){try{
    if(argc!=2)return 2;
    const auto frames=hive::media_generated_software_fixture();
    std::ofstream file(argv[1],std::ios::binary);file.exceptions(std::ios::failbit|std::ios::badbit);
    for(const auto& frame:frames){
        uint32_t header[]={frame.width,frame.height,uint32_t(frame.sequence),uint32_t(frame.keyframe),uint32_t(frame.data.size())};
        file.write(reinterpret_cast<const char*>(header),sizeof(header));
        file.write(reinterpret_cast<const char*>(frame.data.data()),frame.data.size());
    }
    std::cout<<"Generated "<<frames.size()<<" HEVC444 reference/IDR/resize fixture frames\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
