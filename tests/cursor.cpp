#include "cursor_image.h"
#include <iostream>
int main() {
    for(auto name : {IDC_ARROW,IDC_CROSS,IDC_IBEAM,IDC_HAND}) {
        CursorPixels image;
        HCURSOR cursor=LoadCursorW(nullptr,name);
        if(!rasterCursor(cursor,image)) {
            std::cerr<<"Native cursor raster failed: handle="<<cursor<<" Win32="<<GetLastError()<<"\n";return 1;
        }
        if(image.width<=0 || image.height<=0 || image.hotspotX>=DWORD(image.width) ||
           image.hotspotY>=DWORD(image.height)) return 2;
        bool clear=false,solid=false;
        for(size_t i=0;i<image.bgra.size();i+=4) {
            BYTE alpha=image.bgra[i+3];clear|=alpha==0;solid|=alpha>0;
            for(int channel=0;channel<3;++channel) if(image.bgra[i+channel]>alpha) return 3;
        }
        if(!clear || !solid) return 4;
    }
    auto coloredCursor=[](BYTE red,BYTE green,CursorPixels& image) {
        CursorDib color(8,8,0);
        if(!color.bits) return false;
        for(int y=2;y<6;++y) for(int x=2;x<6;++x) {
            size_t offset=size_t(y*8+x)*4;
            color.bits[offset+1]=green;color.bits[offset+2]=red;color.bits[offset+3]=255;
        }
        BYTE maskBytes[16]{};
        HBITMAP mask=CreateBitmap(8,8,1,1,maskBytes);
        ICONINFO info{};info.fIcon=FALSE;info.xHotspot=1;info.yHotspot=2;
        info.hbmColor=color.bitmap;info.hbmMask=mask;
        HCURSOR cursor=reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
        DeleteObject(mask);
        if(!cursor) return false;
        bool ok=rasterCursor(cursor,image);DestroyCursor(cursor);return ok;
    };
    CursorPixels red,normal;
    if(!coloredCursor(255,0,red) || !coloredCursor(255,255,normal)) return 5;
    size_t sample=size_t(3*8+3)*4;
    if(red.hotspotX!=1 || red.hotspotY!=2 || red.bgra[sample+2]!=255 ||
       red.bgra[sample+1]!=0 || normal.bgra[sample+1]!=255 || red.bgra==normal.bgra) return 6;
    std::cout<<"Cursor rasterization, hotspot and premultiplied alpha passed\n";
    return 0;
}
