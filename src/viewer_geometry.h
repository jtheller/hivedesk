#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace hive {
// Constrain the client, not the caption/borders, to the source aspect ratio.
inline void constrain_viewer(RECT& r,UINT edge,LONG frame_x,LONG frame_y,double ratio,LONG minimum=240,const RECT* previous=nullptr) {
    ratio=std::clamp(ratio,1.0/32,32.0);
    LONG width=std::max(minimum,r.right-r.left-frame_x);
    LONG height=std::max(1L,r.bottom-r.top-frame_y);
    const bool corner=edge==WMSZ_TOPLEFT||edge==WMSZ_TOPRIGHT||edge==WMSZ_BOTTOMLEFT||edge==WMSZ_BOTTOMRIGHT;
    const bool vertical_corner=corner&&previous&&std::abs((r.bottom-r.top)-(previous->bottom-previous->top))>
        std::abs((r.right-r.left)-(previous->right-previous->left))/ratio;
    if(edge==WMSZ_TOP||edge==WMSZ_BOTTOM||vertical_corner)width=std::max(minimum,LONG(std::lround(height*ratio)));
    height=std::max(1L,LONG(std::lround(width/ratio)));
    width+=frame_x;height+=frame_y;
    if(edge==WMSZ_LEFT||edge==WMSZ_TOPLEFT||edge==WMSZ_BOTTOMLEFT)r.left=r.right-width;else r.right=r.left+width;
    if(edge==WMSZ_TOP||edge==WMSZ_TOPLEFT||edge==WMSZ_TOPRIGHT)r.top=r.bottom-height;else r.bottom=r.top+height;
}
}
