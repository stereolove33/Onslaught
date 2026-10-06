#pragma once
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstring>

struct CursorPixels {
    int width{},height{};
    DWORD hotspotX{},hotspotY{};
    std::vector<BYTE> bgra;
};
struct CursorDib {
    HDC dc{};HBITMAP bitmap{};HGDIOBJ old{};BYTE* bits{};
    CursorDib(int w,int h,BYTE background) {
        dc=CreateCompatibleDC(nullptr);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&bits),nullptr,0);
        if(dc&&bitmap&&bits) {
            old=SelectObject(dc,bitmap);
            std::memset(bits,background,size_t(w)*h*4);
        }
    }
    ~CursorDib() {
        if(old) SelectObject(dc,old);
        if(bitmap) DeleteObject(bitmap);if(dc) DeleteDC(dc);
    }
};
inline bool rasterCursor(HCURSOR cursor,CursorPixels& output) {
    HICON copy=CopyIcon(reinterpret_cast<HICON>(cursor));
    if(!copy) return false;
    ICONINFO info{};
    if(!GetIconInfo(copy,&info)) {DestroyIcon(copy);return false;}
    BITMAP dimensions{};
    bool valid=GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask,sizeof(dimensions),&dimensions)!=0;
    int width=dimensions.bmWidth;
    int height=info.hbmColor ? dimensions.bmHeight : dimensions.bmHeight/2;
    DWORD hotspotX=info.xHotspot,hotspotY=info.yHotspot;
    if(info.hbmColor) DeleteObject(info.hbmColor);
    if(info.hbmMask) DeleteObject(info.hbmMask);
    if(!valid || width<=0 || height<=0 || width>512 || height>512) {DestroyIcon(copy);return false;}
    CursorDib black(width,height,0),white(width,height,255);
    if(!black.bits || !white.bits || !black.dc || !white.dc) {DestroyIcon(copy);return false;}
    valid=DrawIconEx(black.dc,0,0,copy,width,height,0,nullptr,DI_NORMAL) &&
          DrawIconEx(white.dc,0,0,copy,width,height,0,nullptr,DI_NORMAL);
    GdiFlush();DestroyIcon(copy);
    if(!valid) return false;
    output={width,height,hotspotX,hotspotY,std::vector<BYTE>(size_t(width)*height*4)};
    bool visible=false;
    // Recover coverage by drawing over black/white. Handles ARGB and legacy
    // masks without losing opaque black pixels; output is premultiplied BGRA.
    for(size_t i=0;i<output.bgra.size();i+=4) {
        int transparency=0;
        for(int channel=0;channel<3;++channel)
            transparency=std::max(transparency,int(white.bits[i+channel])-int(black.bits[i+channel]));
        BYTE alpha=BYTE(255-std::clamp(transparency,0,255));
        for(int channel=0;channel<3;++channel)
            output.bgra[i+channel]=std::min(black.bits[i+channel],alpha);
        output.bgra[i+3]=alpha;visible=visible||alpha!=0;
    }
    return visible;
}
