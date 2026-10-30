#include "../src/intel_system_surfaces.h"
#include <cstdio>
int main(){
    try{
        mfxFrameInfo info{};info.FourCC=MFX_FOURCC_AYUV;info.ChromaFormat=MFX_CHROMAFORMAT_YUV444;
        info.BitDepthLuma=info.BitDepthChroma=8;info.Width=640;info.Height=368;info.CropW=640;info.CropH=360;
        IntelSystemSurfaces pool;pool.allocate(info,2);auto* first=pool.available();
        if(first->Data.U!=first->Data.V+1||first->Data.Y!=first->Data.V+2||first->Data.A!=first->Data.V+3||first->Data.PitchLow!=2560||first->FrameInterface)return 1;
        first->Data.Locked=1;auto* second=pool.available();if(first==second)return 1;second->Data.Locked=1;
        bool exhausted=false;try{pool.available();}catch(const std::runtime_error&){exhausted=true;}if(!exhausted)return 1;
        first->Data.Locked=0;if(pool.available()!=first)return 1;
        first->Data.A[367*2560+639*4]=255;
        bool rejected=false;try{IntelSystemSurfaces invalid;invalid.allocate(info,33);}catch(const std::runtime_error&){rejected=true;}if(!rejected)return 1;
        rejected=false;info.CropW=641;try{IntelSystemSurface invalid(info);}catch(const std::runtime_error&){rejected=true;}if(!rejected)return 1;
        rejected=false;info.CropW=640;info.FourCC=MFX_FOURCC_NV12;try{IntelSystemSurface invalid(info);}catch(const std::runtime_error&){rejected=true;}if(!rejected)return 1;
        std::puts("PASS legacy AYUV layout, padded allocation, locked-surface ownership/reuse, bounds and no chroma downgrade");return 0;
    }catch(...){return 1;}
}
