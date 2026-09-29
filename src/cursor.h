#pragma once
#include "graphics.h"
#include <magnification.h>
#include <functional>

namespace sw {
// The OS keeps its original input coordinates. Only the displayed cursor is
// transformed alongside the desktop, so native clicking/dragging stays aligned.
// No mouse injection, hooks, registry edits, or replacement system cursors.
class Cursor {
public:
    Cursor() {
        initialized=MagInitialize()!=FALSE;
        window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,
            L"STATIC",L"SelfWindow Pointer",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window||!SetWindowDisplayAffinity(window,WDA_EXCLUDEFROMCAPTURE)) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()),L"无法创建鼠标显示层。");
    }
    ~Cursor() {restore();clearShape();if(window) DestroyWindow(window);if(initialized) MagUninitialize();}
    void restore() noexcept {
        if(hidden) {MagShowSystemCursor(TRUE);hidden=false;}
        if(window) ShowWindow(window,SW_HIDE);
    }
    void update(bool active,Point output) {
        if(!active) {restore();return;}
        CURSORINFO ci{sizeof(ci)};
        if(!GetCursorInfo(&ci)) {restore();return;}
        if(!initialized) throw winrt::hresult_error(E_FAIL,L"系统不支持鼠标校正。");
        if(!hidden) {
            if(!MagShowSystemCursor(FALSE)) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()),L"无法隐藏原始鼠标。");
            hidden=true;
        }
        if(!ci.hCursor || (ci.flags&CURSOR_SUPPRESSED)) {ShowWindow(window,SW_HIDE);return;}
        if(ci.hCursor!=lastCursor || GetTickCount64()-lastShapeTime>150) createShape(ci.hCursor);
        POINT pos{static_cast<LONG>(std::lround(output.x))-hotspot.x,static_cast<LONG>(std::lround(output.y))-hotspot.y};
        SIZE size{cursorSize,cursorSize};POINT origin{};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(window,nullptr,&pos,&size,shapeDC,&origin,0,&blend,ULW_ALPHA)) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()),L"鼠标显示失败。");
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
private:
    HWND window{};bool initialized{},hidden{};HCURSOR lastCursor{};ULONGLONG lastShapeTime{};
    HDC shapeDC{};HBITMAP shapeBitmap{};HGDIOBJ shapeOld{};POINT hotspot{};int cursorSize=96;
    void clearShape() {
        if(shapeDC) {SelectObject(shapeDC,shapeOld);DeleteObject(shapeBitmap);DeleteDC(shapeDC);shapeDC=nullptr;}
    }
    void createShape(HCURSOR cursor) {
        clearShape();lastCursor=cursor;lastShapeTime=GetTickCount64();
        ICONINFO ii{};if(!GetIconInfo(cursor,&ii)) return;
        hotspot={static_cast<LONG>(ii.xHotspot),static_cast<LONG>(ii.yHotspot)};
        BITMAP bitmap{};GetObjectW(ii.hbmColor?ii.hbmColor:ii.hbmMask,sizeof(bitmap),&bitmap);
        int w=bitmap.bmWidth,h=ii.hbmColor?bitmap.bmHeight:bitmap.bmHeight/2;
        cursorSize=std::clamp(std::max(w,h),16,256);
        if(ii.hbmColor) DeleteObject(ii.hbmColor);if(ii.hbmMask) DeleteObject(ii.hbmMask);
        BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=cursorSize;bi.bmiHeader.biHeight=-cursorSize;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
        void* blackBits{};void* whiteBits{};
        shapeDC=CreateCompatibleDC(nullptr);shapeBitmap=CreateDIBSection(shapeDC,&bi,DIB_RGB_COLORS,&blackBits,nullptr,0);shapeOld=SelectObject(shapeDC,shapeBitmap);
        HDC whiteDC=CreateCompatibleDC(nullptr);HBITMAP whiteBitmap=CreateDIBSection(whiteDC,&bi,DIB_RGB_COLORS,&whiteBits,nullptr,0);auto old=SelectObject(whiteDC,whiteBitmap);
        size_t bytes=static_cast<size_t>(cursorSize)*cursorSize*4;memset(blackBits,0,bytes);memset(whiteBits,255,bytes);
        DrawIconEx(shapeDC,0,0,cursor,w,h,0,nullptr,DI_NORMAL);DrawIconEx(whiteDC,0,0,cursor,w,h,0,nullptr,DI_NORMAL);
        GdiFlush();auto b=static_cast<unsigned char*>(blackBits);auto v=static_cast<unsigned char*>(whiteBits);
        for(size_t i=0;i<bytes;i+=4) {
            int diff=std::max({int(v[i])-int(b[i]),int(v[i+1])-int(b[i+1]),int(v[i+2])-int(b[i+2])});
            b[i+3]=static_cast<unsigned char>(255-std::clamp(diff,0,255));
        }
        SelectObject(whiteDC,old);DeleteObject(whiteBitmap);DeleteDC(whiteDC);
    }
public:
    void disposeShape() {clearShape();}
};
}
