#include "graphics.h"
#include "cursor.h"
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <locale>
#include <limits>
#include "../tests/warp-tests.h"

using namespace sw;
static HINSTANCE appInstance;
static HWND mainWindow;
static HFONT font, titleFont, smallFont;
static std::vector<Monitor> monitors;
static std::vector<WarpProfile> profiles;
static std::vector<std::shared_ptr<WarpMapping>> mappings;
static bool editMesh=false;
static HWND calibrationWindow=nullptr,selectionInfo=nullptr;
static int selected=0, selectedCorner=0;
static HWND combo, toggleButton, gridCheck, statusLabel, cornerLabels[4];
static std::wstring status=L"选择显示器，开启校正，再拖动四角。配置按屏幕和方向分别保存。";
static std::wstring settingsFile;
static std::unique_ptr<Graphics> graphics;
static std::unique_ptr<Cursor> cursor;
static bool dragging=false, grid=false, refreshing=false;
static RECT previewRect{};
static ULONGLONG lastTopmost=0;
static UINT uiDpi=96;
static HANDLE watchdogProcess=nullptr;
static bool timerResolution=false;
static bool emergencyReady=false;
static constexpr int ID_MONITOR=101,ID_TOGGLE=102,ID_GRID=103,ID_RESET=104,ID_SAVE=105,ID_REFRESH=106,ID_STOP=107,ID_IDENTIFY=108,ID_BACKGROUND=109,ID_CALIBRATION=110;
static constexpr int ID_TRAY_SETTINGS=401,ID_TRAY_QUIT=402;
static constexpr UINT WM_TRAY=WM_APP+1,WM_RESTORE_PROFILES=WM_APP+2,WM_SHOW_SETTINGS=WM_APP+3;
static NOTIFYICONDATAW trayIcon{};
static bool trayAdded=false;
static UINT taskbarCreatedMessage=0;
static HICON appIcon=nullptr;
static void RefreshTray();
static void ShowSettings();
static void HideSettings();
static void ShowTrayMenu();
static void OpenCalibration();
static void SyncCalibration();

struct Running {
    Monitor monitor;
    std::shared_ptr<WarpMapping> mapping;
    HWND window{};
    std::unique_ptr<Capture> capture;
    std::unique_ptr<Renderer> renderer;
    ULONGLONG started{};
    bool shown{},dirty=true;
    ~Running() {if(window) ShowWindow(window,SW_HIDE);renderer.reset();capture.reset();if(window) DestroyWindow(window);}
};
static std::vector<std::unique_ptr<Running>> running;
static int S(int value) {return MulDiv(value,uiDpi,96);}
static Running* FindRunning(const std::wstring& key) {for(auto& r:running) if(r->monitor.key==key) return r.get();return nullptr;}
static std::wstring ProfileKey(const Monitor& m) {
    unsigned long long hash=14695981039346656037ull;
    for(wchar_t c:m.key) {hash^=static_cast<unsigned short>(c);hash*=1099511628211ull;}
    std::wostringstream out;out<<L"Monitor_"<<std::hex<<hash;return out.str();
}
static void InitializeSettings() {
    PWSTR path{};winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path));
    std::wstring dir=std::wstring(path)+L"\\SelfWindow";CoTaskMemFree(path);
    if(!CreateDirectoryW(dir.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS) winrt::throw_last_error();
    settingsFile=dir+L"\\settings.ini";
}
static Quad LoadQuad(const Monitor& m) {
    wchar_t text[512]{};GetPrivateProfileStringW(ProfileKey(m).c_str(),L"corners",L"",text,512,settingsFile.c_str());
    Quad q;std::wistringstream in(text);in.imbue(std::locale::classic());
    for(auto& p:q) if(!(in>>p.x>>p.y)) return IdentityQuad();
    return ValidQuad(q)?q:IdentityQuad();
}
static WarpProfile LoadProfile(const Monitor& m){
    WarpProfile p;p.corners=LoadQuad(m);p.heightCm=p.widthCm*m.height()/m.width();
    wchar_t text[2048]{};auto key=ProfileKey(m);
    GetPrivateProfileStringW(key.c_str(),L"view_v2",L"",text,2048,settingsFile.c_str());
    if(text[0]){
        std::wistringstream in(text);in.imbue(std::locale::classic());int mode{},axis{};
        if(!(in>>mode>>p.widthCm>>p.heightCm>>p.distanceCm>>p.yaw>>p.pitch>>p.eyeX>>p.eyeY>>p.curvature>>axis)||mode<0||mode>2||axis<0||axis>1)
            throw std::runtime_error("Invalid saved view profile");
        p.mode=static_cast<WarpMode>(mode);p.horizontalAxis=axis!=0;
    }
    GetPrivateProfileStringW(key.c_str(),L"mesh_v2",L"",text,2048,settingsFile.c_str());
    if(text[0]){std::wistringstream in(text);in.imbue(std::locale::classic());for(auto& point:p.offsets)if(!(in>>point.x>>point.y))throw std::runtime_error("Invalid saved mesh");}
    ValidateProfile(p);return p;
}
static void SaveQuad(int index) {
    if(index<0||index>=static_cast<int>(monitors.size())) return;
    std::wostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(10);
    if(GetPrivateProfileIntW(L"App",L"schema",1,settingsFile.c_str())<2&&GetFileAttributesW(settingsFile.c_str())!=INVALID_FILE_ATTRIBUTES){
        std::wstring backup=settingsFile+L".v1-backup.ini";
        if(!CopyFileW(settingsFile.c_str(),backup.c_str(),TRUE)&&GetLastError()!=ERROR_FILE_EXISTS)winrt::throw_last_error();
    }
    for(auto p:profiles[index].corners) out<<p.x<<L' '<<p.y<<L' ';
    if(!WritePrivateProfileStringW(ProfileKey(monitors[index]).c_str(),L"corners",out.str().c_str(),settingsFile.c_str())) winrt::throw_last_error();
    const auto& p=profiles[index];std::wostringstream view,mesh;view.imbue(std::locale::classic());mesh.imbue(std::locale::classic());view<<std::setprecision(12);mesh<<std::setprecision(12);
    view<<int(p.mode)<<L' '<<p.widthCm<<L' '<<p.heightCm<<L' '<<p.distanceCm<<L' '<<p.yaw<<L' '<<p.pitch<<L' '<<p.eyeX<<L' '<<p.eyeY<<L' '<<p.curvature<<L' '<<int(p.horizontalAxis);
    for(auto d:p.offsets)mesh<<d.x<<L' '<<d.y<<L' ';
    auto key=ProfileKey(monitors[index]);
    winrt::check_bool(WritePrivateProfileStringW(key.c_str(),L"view_v2",view.str().c_str(),settingsFile.c_str()));
    winrt::check_bool(WritePrivateProfileStringW(key.c_str(),L"mesh_v2",mesh.str().c_str(),settingsFile.c_str()));
    winrt::check_bool(WritePrivateProfileStringW(L"App",L"schema",L"2",settingsFile.c_str()));
}
static bool LoadEnabled(int index) {
    wchar_t value[16]{};GetPrivateProfileStringW(ProfileKey(monitors[index]).c_str(),L"enabled",L"",value,16,settingsFile.c_str());
    if(value[0]) return wcscmp(value,L"1")==0;
    // Migrate a calibrated v1 display, which saved corners but no enabled flag.
    auto identity=IdentityQuad();
    for(int i=0;i<4;++i) if(std::hypot(profiles[index].corners[i].x-identity[i].x,profiles[index].corners[i].y-identity[i].y)>1e-7) return true;
    return false;
}
static void SaveEnabled(int index,bool enabled) {
    if(index<0||index>=static_cast<int>(monitors.size())) return;
    winrt::check_bool(WritePrivateProfileStringW(ProfileKey(monitors[index]).c_str(),L"enabled",enabled?L"1":L"0",settingsFile.c_str()));
}
static void SetStatus(const std::wstring& text) {status=text;if(statusLabel) SetWindowTextW(statusLabel,text.c_str());}
static void RefreshLabels() {
    if(mainWindow) SetTimer(mainWindow,1,running.empty()?250:16,nullptr);
    if(monitors.empty()) return;
    const auto& q=profiles[selected].corners;const wchar_t* names[]={L"① 左上",L"② 右上",L"③ 右下",L"④ 左下"};
    bool cornersEditable=!editMesh&&profiles[selected].mode==WarpMode::Corners;
    for(int i=0;i<4;++i) {
        wchar_t text[100]{};swprintf_s(text,L"%s  X %.1f%%   Y %.1f%%",names[i],q[i].x*100,q[i].y*100);
        SetWindowTextW(cornerLabels[i],text);SendMessageW(cornerLabels[i],BM_SETCHECK,i==selectedCorner?BST_CHECKED:BST_UNCHECKED,0);
        ShowWindow(cornerLabels[i],cornersEditable?SW_SHOW:SW_HIDE);
    }
    EnableWindow(GetDlgItem(mainWindow,ID_RESET),cornersEditable);
    const wchar_t* modeNames[]={L"四角透视",L"视点标定 · 平面",L"视点标定 · 圆柱"};
    std::wstring info=modeNames[int(profiles[selected].mode)];
    if(profiles[selected].mode!=WarpMode::Corners)info+=L"\r\n完整画面缩放 "+std::to_wstring(int(mappings[selected]->fit*100))+L"%";
    if(editMesh)info+=L"\r\n网格点 "+std::to_wstring(selectedCorner+1)+L" / 16\r\n拖动圆点或用方向键微调。";
    else if(!cornersEditable)info+=L"\r\n点击“曲面与视点设置”调节。";
    SetWindowTextW(selectionInfo,info.c_str());ShowWindow(selectionInfo,cornersEditable?SW_HIDE:SW_SHOW);
    SetWindowTextW(toggleButton,FindRunning(monitors[selected].key)?L"停止此屏校正":L"开启此屏校正");
    InvalidateRect(mainWindow,&previewRect,FALSE);
    RefreshTray();
}
static void StopAll() noexcept {
    if(cursor) cursor->restore();
    running.clear();
    if(timerResolution) {timeEndPeriod(1);timerResolution=false;}
    if(mainWindow) RefreshLabels();
}
static void DisableAll() {
    StopAll();
    for(int i=0;i<static_cast<int>(monitors.size());++i) SaveEnabled(i,false);
}
static bool ApplyProfile(const WarpProfile& p){
    try{
        auto mapping=std::make_shared<WarpMapping>(p,monitors[selected].width(),monitors[selected].height());
        profiles[selected]=p;mappings[selected]=mapping;
        if(auto r=FindRunning(monitors[selected].key)){r->mapping=mapping;r->dirty=true;}
        RefreshLabels();return true;
    }catch(const std::exception&){SetStatus(L"未应用此次调整：范围超出屏幕或出现折叠。请减小角度、偏移或网格移动量。");return false;}
}
static void ApplyQuad(const Quad& q) {
    if(!ValidQuad(q)||monitors.empty()) return;
    auto profile=profiles[selected];profile.corners=q;ApplyProfile(profile);
}
static void MoveMeshPoint(Point delta){
    auto p=profiles[selected];int x=selectedCorner%4,y=selectedCorner/4;
    double weight=Bernstein(x/3.)[x]*Bernstein(y/3.)[y];
    p.offsets[selectedCorner].x+=delta.x/weight;p.offsets[selectedCorner].y+=delta.y/weight;
    ApplyProfile(p);
}
static void StartSelected() {
    if(monitors.empty()) return;
    const auto& monitor=monitors[selected];
    if(auto active=FindRunning(monitor.key)) {
        (void)active;cursor->restore();
        running.erase(std::remove_if(running.begin(),running.end(),[&](const auto& r){return r->monitor.key==monitor.key;}),running.end());
        SaveEnabled(selected,false);
        if(running.empty()&&timerResolution) {timeEndPeriod(1);timerResolution=false;}
        RefreshLabels();SetStatus(L"已恢复此屏原画面。校准参数仍保留。");return;
    }
    if(!watchdogProcess||!emergencyReady) throw winrt::hresult_error(E_FAIL,L"恢复保护或紧急快捷键未就绪，无法开启校正。请释放 Ctrl+Alt+F12 后重新打开软件。");
    auto r=std::make_unique<Running>();r->monitor=monitor;r->mapping=mappings[selected];
    r->window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        L"SelfWindowOutput",L"SelfWindow Output",WS_POPUP,monitor.bounds.left,monitor.bounds.top,monitor.width(),monitor.height(),nullptr,nullptr,appInstance,nullptr);
    if(!r->window) winrt::throw_last_error();
    winrt::check_bool(SetLayeredWindowAttributes(r->window,0,255,LWA_ALPHA));
    winrt::check_bool(SetWindowDisplayAffinity(r->window,WDA_EXCLUDEFROMCAPTURE));
    r->renderer=std::make_unique<Renderer>(*graphics,r->window,monitor.width(),monitor.height());
    r->capture=std::make_unique<Capture>(*graphics);r->capture->start(monitor);r->started=GetTickCount64();
    running.push_back(std::move(r));
    if(!timerResolution) {timerResolution=timeBeginPeriod(1)==TIMERR_NOERROR;}
    SaveQuad(selected);SaveEnabled(selected,true);RefreshLabels();SetStatus(L"正在启动捕获…  Ctrl + Alt + F12 可随时恢复全部屏幕。");
}
static void RestoreProfiles() {
    int previous=selected;
    try {
        for(int i=0;i<static_cast<int>(monitors.size());++i) if(LoadEnabled(i)&&!FindRunning(monitors[i].key)) {selected=i;StartSelected();}
    }catch(...) {selected=previous;throw;}
    selected=previous;RefreshLabels();
}
static HICON CreateAppIcon() {
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=32;bi.bmiHeader.biHeight=-32;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    void* bits{};HDC dc=CreateCompatibleDC(nullptr);HBITMAP color=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);auto old=SelectObject(dc,color);
    memset(bits,0,32*32*4);HBRUSH brush=CreateSolidBrush(RGB(25,56,76));auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));RoundRect(dc,1,1,31,31,8,8);
    HPEN pen=CreatePen(PS_SOLID,2,RGB(79,222,187));SelectObject(dc,pen);SelectObject(dc,GetStockObject(NULL_BRUSH));POINT points[]={{7,7},{25,11},{24,25},{7,25},{7,7}};Polyline(dc,points,5);MoveToEx(dc,15,9,nullptr);LineTo(dc,15,25);MoveToEx(dc,7,16,nullptr);LineTo(dc,25,17);GdiFlush();
    auto pixels=static_cast<unsigned char*>(bits);for(int i=0;i<32*32;++i) if(pixels[i*4]||pixels[i*4+1]||pixels[i*4+2]) pixels[i*4+3]=255;
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);SelectObject(dc,old);DeleteObject(pen);DeleteObject(brush);DeleteDC(dc);
    HBITMAP mask=CreateBitmap(32,32,1,1,nullptr);ICONINFO info{};info.fIcon=TRUE;info.hbmMask=mask;info.hbmColor=color;HICON icon=CreateIconIndirect(&info);DeleteObject(mask);DeleteObject(color);return icon;
}
static bool AddTray() {
    trayIcon={};trayIcon.cbSize=sizeof(trayIcon);trayIcon.hWnd=mainWindow;trayIcon.uID=1;
    trayIcon.guidItem={0x4d7d9802,0x6cda,0x49b1,{0x98,0x4b,0xc5,0x61,0x9d,0x1a,0x76,0x83}};
    trayIcon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP|NIF_GUID;trayIcon.uCallbackMessage=WM_TRAY;
    trayIcon.hIcon=appIcon?appIcon:LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(trayIcon.szTip,L"SelfWindow · 双击打开设置");
    trayAdded=Shell_NotifyIconW(NIM_ADD,&trayIcon)!=FALSE;
    if(trayAdded) {trayIcon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&trayIcon);RefreshTray();}
    return trayAdded;
}
static void RemoveTray() noexcept {if(trayAdded) {Shell_NotifyIconW(NIM_DELETE,&trayIcon);trayAdded=false;}}
static void RefreshTray() {
    if(!trayAdded) return;
    auto data=trayIcon;data.uFlags=NIF_GUID|NIF_TIP;
    std::wstring tip=running.empty()?L"SelfWindow · 校正已暂停":L"SelfWindow · 正在校正 "+std::to_wstring(running.size())+L" 块屏幕";
    tip+=L"\n双击设置 / 右键退出";wcsncpy_s(data.szTip,tip.c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&data);
}
static void NotifyBackground() {
    if(!trayAdded) return;
    auto data=trayIcon;data.uFlags=NIF_GUID|NIF_INFO;data.dwInfoFlags=NIIF_INFO|NIIF_RESPECT_QUIET_TIME;
    wcscpy_s(data.szInfoTitle,L"SelfWindow 已在后台运行");
    wcscpy_s(data.szInfo,L"双击通知区域的 SelfWindow 图标打开设置；右键可恢复屏幕或退出。再次运行程序也可打开设置。");
    Shell_NotifyIconW(NIM_MODIFY,&data);
}
static void ShowSettings() {if(mainWindow) {ShowWindow(mainWindow,SW_RESTORE);SetForegroundWindow(mainWindow);}}
static void HideSettings() {
    if(!trayAdded) {ShowSettings();SetStatus(L"托盘暂不可用，设置窗口保持打开以便恢复屏幕。");return;}
    if(grid) {grid=false;SendMessageW(gridCheck,BM_SETCHECK,BST_UNCHECKED,0);for(auto& r:running) r->dirty=true;}
    ShowWindow(mainWindow,SW_HIDE);
}
static void ShowTrayMenu() {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,ID_TRAY_SETTINGS,L"打开设置");SetMenuDefaultItem(menu,ID_TRAY_SETTINGS,FALSE);
    bool active=!monitors.empty()&&FindRunning(monitors[selected].key);
    AppendMenuW(menu,MF_STRING,ID_TOGGLE,active?L"停止当前屏幕校正":L"开启当前屏幕校正");
    AppendMenuW(menu,MF_STRING,ID_STOP,L"恢复全部屏幕   Ctrl+Alt+F12");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,ID_TRAY_QUIT,L"退出 SelfWindow");
    POINT p{};GetCursorPos(&p);SetForegroundWindow(mainWindow);
    UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,p.x,p.y,0,mainWindow,nullptr);
    DestroyMenu(menu);PostMessageW(mainWindow,WM_NULL,0,0);
    if(command) PostMessageW(mainWindow,WM_COMMAND,command,0);
}
static void Tick() {
    if(running.empty()) return;
    for(auto& r:running) {
        bool changed=r->capture->update();
        if(!r->capture->view) {
            if(GetTickCount64()-r->started>4000) throw winrt::hresult_error(E_FAIL,L"4 秒内没有收到桌面画面。请检查屏幕捕获权限或关闭冲突的显示工具。");
            continue;
        }
        if(changed||r->dirty) {r->renderer->draw(r->capture->view.get(),*r->mapping,grid);r->dirty=false;}
        if(!r->shown) {
            ShowWindow(r->window,SW_SHOWNOACTIVATE);r->shown=true;
            SetStatus(L"校正已开启。拖动预览四角可实时调整；关闭校准网格后即可操作桌面。");
        }
    }
    if(GetTickCount64()-lastTopmost>1000) {
        for(auto& r:running) if(r->shown) SetWindowPos(r->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        lastTopmost=GetTickCount64();
    }
    POINT p{};
    if(!GetCursorPos(&p)) {cursor->restore();return;}
    bool warped=false;Point mapped{};
    for(auto& r:running) if(r->shown&&PtInRect(&r->monitor.bounds,p)) {
        auto uv=r->mapping->map({double(p.x-r->monitor.bounds.left)/r->monitor.width(),double(p.y-r->monitor.bounds.top)/r->monitor.height()});
        mapped={r->monitor.bounds.left+uv.x*r->monitor.width(),r->monitor.bounds.top+uv.y*r->monitor.height()};warped=true;break;
    }
    cursor->update(warped,mapped);
}
static void ReloadMonitors() {
    StopAll();refreshing=true;
    auto previous=monitors.empty()?L"":monitors[selected].key;
    monitors=EnumerateMonitors();profiles.clear();mappings.clear();selected=0;selectedCorner=0;bool resetInvalid=false;
    SendMessageW(combo,CB_RESETCONTENT,0,0);
    for(int i=0;i<static_cast<int>(monitors.size());++i) {
        auto& m=monitors[i];WarpProfile p;p.corners=LoadQuad(m);p.heightCm=p.widthCm*m.height()/m.width();
        try{p=LoadProfile(m);mappings.push_back(std::make_shared<WarpMapping>(p,m.width(),m.height()));}
        catch(...){p.mode=WarpMode::Corners;p.offsets={};p.widthCm=30;p.heightCm=30.*m.height()/m.width();p.distanceCm=60;p.eyeX=p.eyeY=p.pitch=0;p.yaw=30;p.curvature=100;
            mappings.push_back(std::make_shared<WarpMapping>(p,m.width(),m.height()));resetInvalid=true;}
        profiles.push_back(p);
        std::wstring name=m.device+L"   "+std::to_wstring(m.width())+L" × "+std::to_wstring(m.height())+(m.height()>m.width()?L"   竖屏":L"   横屏")+(m.primary?L"  · 主屏":L"");
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        if(m.key==previous) selected=i;
    }
    if(previous.empty()&&!monitors.empty()) selected=static_cast<int>(monitors.size())-1;
    SendMessageW(combo,CB_SETCURSEL,selected,0);refreshing=false;RefreshLabels();SyncCalibration();InvalidateRect(mainWindow,nullptr,TRUE);
    if(resetInvalid)SetStatus(L"某个已保存的曲面参数无效，已回到该屏的四角模式；原配置文件仍保留。");
}
static HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int id) {
    HWND h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,mainWindow,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),appInstance,nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;
}
static void SetFonts() {
    if(font) DeleteObject(font);if(titleFont) DeleteObject(titleFont);if(smallFont) DeleteObject(smallFont);
    font=CreateFontW(-S(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    titleFont=CreateFontW(-S(28),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    smallFont=CreateFontW(-S(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    if(mainWindow) EnumChildWindows(mainWindow,[](HWND h,LPARAM)->BOOL {SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return TRUE;},0);
}
static void Layout() {
    RECT client{};GetClientRect(mainWindow,&client);int w=client.right,h=client.bottom;
    auto move=[&](int id,int x,int y,int width,int height){MoveWindow(GetDlgItem(mainWindow,id),S(x),S(y),S(width),S(height),TRUE);};
    int logicalW=MulDiv(w,96,uiDpi);
    move(ID_MONITOR,24,93,logicalW-230,32);move(ID_REFRESH,logicalW-194,92,76,32);move(ID_IDENTIFY,logicalW-108,92,84,32);
    move(ID_TOGGLE,24,143,148,36);move(ID_GRID,188,145,148,32);move(ID_RESET,350,143,94,36);move(ID_SAVE,454,143,94,36);move(ID_STOP,logicalW-174,143,150,36);
    int sideX=logicalW-286;
    for(int i=0;i<4;++i) move(201+i,sideX,254+i*45,262,36);
    move(ID_CALIBRATION,sideX,568,262,36);move(ID_BACKGROUND,sideX,612,262,36);
    move(302,sideX,254,262,175);
    int bottom=std::max(S(530),h-S(92));
    previewRect={S(24),S(210),S(sideX-24),bottom};
    MoveWindow(statusLabel,S(24),h-S(66),w-S(48),S(52),TRUE);
    InvalidateRect(mainWindow,nullptr,TRUE);
}
static RECT ScreenRect() {
    RECT r=previewRect;InflateRect(&r,-S(26),-S(34));
    if(monitors.empty()) return r;
    const auto& m=monitors[selected];double scale=std::min(double(r.right-r.left)/m.width(),double(r.bottom-r.top)/m.height());
    int w=static_cast<int>(m.width()*scale),h=static_cast<int>(m.height()*scale),cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
    return {cx-w/2,cy-h/2,cx-w/2+w,cy-h/2+h};
}
static POINT ToPreview(Point uv,RECT r) {return {r.left+static_cast<LONG>(std::lround(uv.x*(r.right-r.left))),r.top+static_cast<LONG>(std::lround(uv.y*(r.bottom-r.top)))};}
static void Text(HDC dc,int x,int y,const std::wstring& s,COLORREF color=RGB(35,48,64),HFONT f=nullptr) {
    SelectObject(dc,f?f:font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);TextOutW(dc,x,y,s.c_str(),static_cast<int>(s.size()));
}
static void PaintPreview(HDC dc) {
    HBRUSH panel=CreateSolidBrush(RGB(234,239,244));FillRect(dc,&previewRect,panel);DeleteObject(panel);
    if(monitors.empty()) return;
    RECT r=ScreenRect();HBRUSH bg=CreateSolidBrush(RGB(12,19,28));FillRect(dc,&r,bg);DeleteObject(bg);
    const auto& tr=*mappings[selected];
    HPEN pen=CreatePen(PS_SOLID,1,RGB(73,109,127));auto oldPen=SelectObject(dc,pen);
    auto line=[&](Point a,Point b){for(int i=0;i<=64;++i){double t=i/64.;auto p=ToPreview(tr.map({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t}),r);if(i==0)MoveToEx(dc,p.x,p.y,nullptr);else LineTo(dc,p.x,p.y);}};
    for(int i=0;i<=10;++i) {line({i/10.,0},{i/10.,1});line({0,i/10.},{1,i/10.});}
    SelectObject(dc,oldPen);DeleteObject(pen);
    pen=CreatePen(PS_SOLID,S(2),RGB(60,209,165));oldPen=SelectObject(dc,pen);
    auto edges=IdentityQuad();for(int i=0;i<4;++i)line(edges[i],edges[(i+1)%4]);
    SelectObject(dc,oldPen);DeleteObject(pen);
    const auto& m=monitors[selected];double radius=std::min(m.width(),m.height())*0.23;
    pen=CreatePen(PS_SOLID,S(2),RGB(66,189,226));oldPen=SelectObject(dc,pen);
    for(int i=0;i<=80;++i) {double a=i*6.283185307179586/80;auto p=ToPreview(tr.map({.5+cos(a)*radius/m.width(),.5+sin(a)*radius/m.height()}),r);if(i==0) MoveToEx(dc,p.x,p.y,nullptr);else LineTo(dc,p.x,p.y);}
    SelectObject(dc,oldPen);DeleteObject(pen);
    COLORREF colors[]={RGB(245,87,79),RGB(71,199,125),RGB(79,129,239),RGB(244,180,62)};
    int handles=editMesh?16:(profiles[selected].mode==WarpMode::Corners?4:0);
    for(int i=0;i<handles;++i) {
        Point uv=editMesh?Point{(i%4)/3.,(i/4)/3.}:edges[i];
        POINT p=ToPreview(editMesh?tr.analytic(uv):profiles[selected].corners[i],r);int size=S(i==selectedCorner?8:6);
        HBRUSH b=CreateSolidBrush(editMesh?RGB(67,154,214):colors[i]);auto ob=SelectObject(dc,b);auto op=SelectObject(dc,GetStockObject(WHITE_PEN));Ellipse(dc,p.x-size,p.y-size,p.x+size+1,p.y+size+1);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);
        Text(dc,p.x+S(13),p.y-S(10),std::to_wstring(i+1),RGB(80,103,120),smallFont);
    }
    Text(dc,previewRect.left+S(14),previewRect.top+S(8),editMesh?L"局部曲面微调 · 拖动蓝色圆点":L"实际方向预览 · 曲面与视点可单独设置",RGB(85,104,120),smallFont);
    std::wstring label=std::to_wstring(m.width())+L" × "+std::to_wstring(m.height())+L"  /  "+std::to_wstring(m.rotation*90)+L"°";
    Text(dc,previewRect.left+S(14),previewRect.bottom-S(24),label,RGB(85,104,120),smallFont);
}
static void PaintMain(HWND hwnd) {
    PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT client{};GetClientRect(hwnd,&client);
    HDC mem=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);auto old=SelectObject(mem,bitmap);
    HBRUSH bg=CreateSolidBrush(RGB(248,250,252));FillRect(mem,&client,bg);DeleteObject(bg);
    Text(mem,S(24),S(17),L"SelfWindow  屏幕透视校正",RGB(25,43,61),titleFont);
    Text(mem,S(26),S(59),L"横屏 / 竖屏独立校准，让侧面的画面更方正。",RGB(88,107,123),smallFont);
    PaintPreview(mem);int x=previewRect.right+S(24);
    Text(mem,x,S(215),editMesh?L"局部网格":L"当前校准",RGB(25,43,61));
    Text(mem,x,S(448),L"选中圆点后，用方向键微调。",RGB(88,107,123),smallFont);
    Text(mem,x,S(473),L"每次 1 像素；Shift 加速到 10 像素。",RGB(88,107,123),smallFont);
    Text(mem,x,S(508),L"Ctrl + Alt + F10   开关此屏",RGB(51,99,124),smallFont);
    Text(mem,x,S(534),L"Ctrl + Alt + F12   恢复全部",RGB(51,99,124),smallFont);
    BitBlt(dc,0,0,client.right,client.bottom,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bitmap);DeleteDC(mem);EndPaint(hwnd,&ps);
}
static void Identify() {
    for(size_t i=0;i<monitors.size();++i) {
        auto& m=monitors[i];HWND h=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"SelfWindowIdentify",m.device.c_str(),WS_POPUP,
            m.bounds.left+40,m.bounds.top+40,420,100,nullptr,nullptr,appInstance,nullptr);
        SetWindowDisplayAffinity(h,WDA_EXCLUDEFROMCAPTURE);ShowWindow(h,SW_SHOWNOACTIVATE);SetTimer(h,1,2200,nullptr);
    }
}
#include "view_controls.h"
static void HandleFailure(const std::wstring& message) {
    StopAll();SetStatus(L"已恢复全部屏幕。"+message);ShowSettings();
}
static LRESULT CALLBACK MainProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    try {
        if(taskbarCreatedMessage&&msg==taskbarCreatedMessage) {trayAdded=false;if(!AddTray()) ShowSettings();return 0;}
        switch(msg) {
        case WM_CREATE: {
            mainWindow=h;uiDpi=GetDpiForWindow(h);SetFonts();
            combo=Control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,ID_MONITOR);
            Control(WC_BUTTONW,L"刷新",BS_PUSHBUTTON|WS_TABSTOP,ID_REFRESH);Control(WC_BUTTONW,L"识别屏幕",BS_PUSHBUTTON|WS_TABSTOP,ID_IDENTIFY);
            toggleButton=Control(WC_BUTTONW,L"开启此屏校正",BS_PUSHBUTTON|WS_TABSTOP,ID_TOGGLE);
            gridCheck=Control(WC_BUTTONW,L"显示校准网格",BS_AUTOCHECKBOX|WS_TABSTOP,ID_GRID);
            Control(WC_BUTTONW,L"重置四角",BS_PUSHBUTTON|WS_TABSTOP,ID_RESET);Control(WC_BUTTONW,L"保存参数",BS_PUSHBUTTON|WS_TABSTOP,ID_SAVE);
            Control(WC_BUTTONW,L"恢复全部屏幕",BS_PUSHBUTTON|WS_TABSTOP,ID_STOP);
            Control(WC_BUTTONW,L"曲面与视点设置…",BS_PUSHBUTTON|WS_TABSTOP,ID_CALIBRATION);
            Control(WC_BUTTONW,L"隐藏到托盘，继续校正",BS_PUSHBUTTON|WS_TABSTOP,ID_BACKGROUND);
            for(int i=0;i<4;++i) cornerLabels[i]=Control(WC_BUTTONW,L"",BS_RADIOBUTTON|WS_TABSTOP,201+i);
            statusLabel=Control(WC_STATICW,status.c_str(),SS_LEFT,301);
            selectionInfo=Control(WC_STATICW,L"",SS_LEFT,302);
            ReloadMonitors();Layout();SetTimer(h,1,16,nullptr);
            emergencyReady=RegisterHotKey(h,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F12)!=FALSE;
            if(!emergencyReady) throw winrt::hresult_error(E_FAIL,L"Ctrl+Alt+F12 被其他程序占用。请释放快捷键后重启。");
            if(!RegisterHotKey(h,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F10)) SetStatus(L"Ctrl+Alt+F10 已被占用，请使用按钮开启；F12 恢复键已就绪。");
            return 0;
        }
        case WM_SIZE: if(wp==SIZE_MINIMIZED) HideSettings();else Layout();return 0;
        case WM_GETMINMAXINFO: {auto p=reinterpret_cast<MINMAXINFO*>(lp);p->ptMinTrackSize={S(910),S(800)};return 0;}
        case WM_DPICHANGED: {uiDpi=HIWORD(wp);SetFonts();auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);Layout();return 0;}
        case WM_PAINT: PaintMain(h);return 0;
        case WM_ERASEBKGND:return 1;
        case WM_CTLCOLORSTATIC: {auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,RGB(248,250,252));SetDCBrushColor(dc,RGB(248,250,252));SetTextColor(dc,RGB(80,100,116));return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));}
        case WM_TIMER:if(cursor) Tick();return 0;
        case WM_TRAY:
            if(LOWORD(lp)==WM_LBUTTONDBLCLK||LOWORD(lp)==NIN_KEYSELECT||LOWORD(lp)==NIN_BALLOONUSERCLICK) ShowSettings();
            else if(LOWORD(lp)==WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case WM_SHOW_SETTINGS:ShowSettings();return 0;
        case WM_RESTORE_PROFILES:RestoreProfiles();return 0;
        case WM_DISPLAYCHANGE: ReloadMonitors();SetStatus(L"显示器配置已改变，已停止校正并重新检测。请确认显示器后再开启。");return 0;
        case WM_POWERBROADCAST:if(wp==PBT_APMSUSPEND) {StopAll();SetStatus(L"休眠前已恢复屏幕；唤醒后可重新开启。");}return TRUE;
        case WM_HOTKEY:if(wp==1) {DisableAll();SetStatus(L"已通过快捷键恢复全部屏幕。");}else if(wp==2) StartSelected();return 0;
        case WM_COMMAND: {
            int id=LOWORD(wp);
            if(id==ID_MONITOR&&HIWORD(wp)==CBN_SELCHANGE&&!refreshing) {selected=static_cast<int>(SendMessageW(combo,CB_GETCURSEL,0,0));selectedCorner=0;RefreshLabels();SyncCalibration();InvalidateRect(h,nullptr,TRUE);}
            else if(id==ID_TOGGLE) StartSelected();
            else if(id==ID_GRID) {grid=SendMessageW(gridCheck,BM_GETCHECK,0,0)==BST_CHECKED;for(auto& r:running) r->dirty=true;}
            else if(id==ID_RESET) {ApplyQuad(IdentityQuad());SaveQuad(selected);SetStatus(L"此屏四角已重置。");}
            else if(id==ID_SAVE) {SaveQuad(selected);SetStatus(L"参数已保存。下次手动启动程序会在后台恢复已开启的屏幕。");}
            else if(id==ID_REFRESH) ReloadMonitors();
            else if(id==ID_IDENTIFY) Identify();
            else if(id==ID_STOP) {DisableAll();SetStatus(L"已恢复全部屏幕。");}
            else if(id==ID_BACKGROUND) HideSettings();
            else if(id==ID_CALIBRATION) OpenCalibration();
            else if(id==ID_TRAY_SETTINGS) ShowSettings();
            else if(id==ID_TRAY_QUIT) DestroyWindow(h);
            else if(id>=201&&id<=204) {selectedCorner=id-201;RefreshLabels();SetFocus(h);}
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if(monitors.empty()) break;
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};RECT r=ScreenRect();int nearest=-1;double distance=S(22)*S(22);
            int handles=editMesh?16:(profiles[selected].mode==WarpMode::Corners?4:0);
            for(int i=0;i<handles;++i) {Point uv{(i%4)/3.,(i/4)/3.};auto c=ToPreview(editMesh?mappings[selected]->analytic(uv):profiles[selected].corners[i],r);double d=double(c.x-p.x)*(c.x-p.x)+double(c.y-p.y)*(c.y-p.y);if(d<distance) {distance=d;nearest=i;}}
            if(nearest>=0) {selectedCorner=nearest;dragging=true;SetCapture(h);SetFocus(h);RefreshLabels();}return 0;
        }
        case WM_MOUSEMOVE:if(dragging&&!monitors.empty()) {
            RECT r=ScreenRect();Point desired{std::clamp(double(GET_X_LPARAM(lp)-r.left)/(r.right-r.left),0.,1.),std::clamp(double(GET_Y_LPARAM(lp)-r.top)/(r.bottom-r.top),0.,1.)};
            if(editMesh){Point uv{(selectedCorner%4)/3.,(selectedCorner/4)/3.};auto current=mappings[selected]->analytic(uv);MoveMeshPoint({desired.x-current.x,desired.y-current.y});}
            else{Quad q=profiles[selected].corners;q[selectedCorner]=desired;ApplyQuad(q);}
        }return 0;
        case WM_LBUTTONUP:if(dragging) {dragging=false;ReleaseCapture();SaveQuad(selected);}return 0;
        case WM_CAPTURECHANGED:if(dragging) {dragging=false;SaveQuad(selected);}return 0;
        case WM_KEYDOWN: {
            if(wp==VK_ESCAPE) {DisableAll();SetStatus(L"已恢复全部屏幕。");return 0;}
            if(!monitors.empty()&&(wp==VK_LEFT||wp==VK_RIGHT||wp==VK_UP||wp==VK_DOWN)) {
                if(!editMesh&&profiles[selected].mode!=WarpMode::Corners)break;
                double step=(GetKeyState(VK_SHIFT)&0x8000)?10:1;
                if(editMesh){Point d{};if(wp==VK_LEFT)d.x=-step/monitors[selected].width();if(wp==VK_RIGHT)d.x=step/monitors[selected].width();if(wp==VK_UP)d.y=-step/monitors[selected].height();if(wp==VK_DOWN)d.y=step/monitors[selected].height();MoveMeshPoint(d);SaveQuad(selected);return 0;}
                Quad q=profiles[selected].corners;
                if(wp==VK_LEFT) q[selectedCorner].x-=step/monitors[selected].width();
                if(wp==VK_RIGHT) q[selectedCorner].x+=step/monitors[selected].width();
                if(wp==VK_UP) q[selectedCorner].y-=step/monitors[selected].height();
                if(wp==VK_DOWN) q[selectedCorner].y+=step/monitors[selected].height();
                q[selectedCorner].x=std::clamp(q[selectedCorner].x,0.,1.);q[selectedCorner].y=std::clamp(q[selectedCorner].y,0.,1.);
                ApplyQuad(q);SaveQuad(selected);return 0;
            }break;
        }
        case WM_QUERYENDSESSION:StopAll();return TRUE;
        case WM_CLOSE:if(trayAdded) HideSettings();else DestroyWindow(h);return 0;
        case WM_DESTROY:RemoveTray();StopAll();KillTimer(h,1);UnregisterHotKey(h,1);UnregisterHotKey(h,2);mainWindow=nullptr;PostQuitMessage(0);return 0;
        }
    } catch(const winrt::hresult_error& e) {HandleFailure(std::wstring(e.message()));}
      catch(const std::exception& e) {HandleFailure(std::wstring(winrt::to_hstring(e.what())));}
    return DefWindowProcW(h,msg,wp,lp);
}
static LRESULT CALLBACK OutputProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_NCHITTEST) return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(msg==WM_ERASEBKGND) return 1;
    return DefWindowProcW(h,msg,wp,lp);
}
static LRESULT CALLBACK IdentifyProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_TIMER) {DestroyWindow(h);return 0;}
    if(msg==WM_PAINT) {PAINTSTRUCT ps{};auto dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));wchar_t text[128]{};GetWindowTextW(h,text,128);Text(dc,24,26,text,RGB(255,255,255),titleFont);EndPaint(h,&ps);return 0;}
    return DefWindowProcW(h,msg,wp,lp);
}
static void RegisterClasses() {
    WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=appInstance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=appIcon?appIcon:LoadIconW(nullptr,IDI_APPLICATION);wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName=L"SelfWindowMain";wc.lpfnWndProc=MainProc;winrt::check_bool(RegisterClassExW(&wc));
    wc.lpszClassName=L"SelfWindowOutput";wc.lpfnWndProc=OutputProc;winrt::check_bool(RegisterClassExW(&wc));
    wc.lpszClassName=L"SelfWindowIdentify";wc.lpfnWndProc=IdentifyProc;winrt::check_bool(RegisterClassExW(&wc));
}
static int Watchdog(DWORD pid,HWND window) {
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE,FALSE,pid);if(!process) return 1;
    MagInitialize();int missed=0;
    while(WaitForSingleObject(process,1000)==WAIT_TIMEOUT) {
        DWORD_PTR result{};
        if(IsWindow(window)&&!SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1500,&result)) {
            if(++missed>=3) {TerminateProcess(process,3);break;}
        }else missed=0;
    }
    MagShowSystemCursor(TRUE);MagUninitialize();CloseHandle(process);return 0;
}
static void StartWatchdog() {
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    std::wstring command=L"\""+std::wstring(path)+L"\" --watch "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow));
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    winrt::check_bool(CreateProcessW(path,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));
    CloseHandle(pi.hThread);watchdogProcess=pi.hProcess;
}
static void Pump() {MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);}}
static void Require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
static int BackgroundTest(const std::wstring& outputDir) {
    std::ofstream log(outputDir+L"\\background-tests.txt");
    try {
        Require(trayAdded,"Tray icon not registered");Require(!IsWindowVisible(mainWindow),"Background launch showed settings");
        NOTIFYICONIDENTIFIER id{sizeof(id)};id.hWnd=mainWindow;id.uID=1;id.guidItem=trayIcon.guidItem;RECT rect{};
        winrt::check_hresult(Shell_NotifyIconGetRect(&id,&rect));
        Require(rect.right>rect.left,"No tray icon rectangle");
        log<<"PASS: hidden startup; Windows shell confirms tray icon exists.\n";
        Require(!monitors.empty(),"No test monitor");
        SaveEnabled(selected,true);RestoreProfiles();
        ULONGLONG start=GetTickCount64();while(GetTickCount64()-start<500) {Pump();Tick();Sleep(16);}
        Require(running.size()==1&&running[0]->shown,"Saved enabled display did not start");
        log<<"PASS: saved display enabled state restored; actual capture/output running while hidden.\n";
        auto savedProfile=profiles[selected];auto curved=savedProfile;curved.mode=WarpMode::Cylinder;curved.horizontalAxis=true;curved.yaw=32;curved.curvature=72;curved.eyeX=-5;curved.offsets[5]={.005,-.003};
        Require(ApplyProfile(curved),"Could not apply cylinder profile");SaveQuad(selected);auto reloaded=LoadProfile(monitors[selected]);
        Require(reloaded.mode==WarpMode::Cylinder&&reloaded.horizontalAxis&&std::abs(reloaded.curvature-72)<1e-10&&std::abs(reloaded.offsets[5].x-.005)<1e-10,"Curve/mesh profile did not round-trip");
        Require(ApplyProfile(savedProfile),"Could not switch back to legacy profile");SaveQuad(selected);
        Require(profiles[selected].corners[0].x==savedProfile.corners[0].x,"Switching modes damaged old corners");
        log<<"PASS: cylinder, viewing parameters and Bezier offsets persist; original corners preserved.\n";
        grid=true;HideSettings();Require(!grid,"Calibration grid remained active in background");
        SendMessageW(mainWindow,WM_CLOSE,0,0);Require(IsWindow(mainWindow)&&!IsWindowVisible(mainWindow),"Close did not hide to tray");
        Require(running.size()==1,"Close stopped active correction");
        log<<"PASS: close hides settings, keeps correction running, and exits grid mode.\n";
        RemoveTray();Require(AddTray(),"Tray re-registration failed");
        log<<"PASS: tray can be re-created using Explorer restart path.\n";
        StopAll();Require(LoadEnabled(selected),"Stopping runtime erased resume preference");
        RestoreProfiles();Require(running.size()==1,"Resume after runtime stop failed");
        DisableAll();Require(running.empty()&&!LoadEnabled(selected),"Emergency stop did not disable future auto-resume");
        log<<"PASS: normal stop retains resume; explicit restore-all clears resume.\n";
        SendMessageW(mainWindow,WM_COMMAND,ID_TRAY_QUIT,0);
        Require(!mainWindow&&!trayAdded&&running.empty(),"Exit did not clean up tray and overlays");
        log<<"PASS: tray exit destroys window, removes icon, and restores screens.\n";return 0;
    }catch(const winrt::hresult_error& e) {log<<"FAIL: "<<winrt::to_string(e.message())<<"\n";}
     catch(const std::exception& e) {log<<"FAIL: "<<e.what()<<"\n";}
    if(mainWindow) DestroyWindow(mainWindow);return 1;
}
static int SelfTest(const std::wstring& outputDir) {
    CreateDirectoryW(outputDir.c_str(),nullptr);std::ofstream log(outputDir+L"\\geometry-tests.txt");
    try {
        std::vector<Quad> cases{IdentityQuad(),{{{.14,.04},{.92,.19},{.91,.82},{.03,.96}}},{{{.03,.22},{.98,.03},{.91,.99},{.12,.80}}}};
        int checks=0;
        for(auto& q:cases) {
            auto h=FromQuad(q),inv=h.inverse();auto src=IdentityQuad();
            for(int i=0;i<4;++i) {auto p=h.map(src[i]);Require(std::hypot(p.x-q[i].x,p.y-q[i].y)<1e-8,"Corner mapping mismatch");++checks;}
            for(int y=0;y<=20;++y)for(int x=0;x<=20;++x) {Point p{x/20.,y/20.};auto r=inv.map(h.map(p));Require(std::hypot(r.x-p.x,r.y-p.y)<1e-8,"Inverse / cursor round trip mismatch");++checks;}
            for(auto dims:std::vector<POINT>{{2560,1440},{1440,2560},{1080,1920},{1920,1080}}) {
                for(POINT origin:std::vector<POINT>{{0,0},{-1440,86},{2560,86}}) {
                    Point logical{.37,.61};auto visual=h.map(logical);
                    auto back=inv.map({(origin.x+visual.x*dims.x-origin.x)/dims.x,(origin.y+visual.y*dims.y-origin.y)/dims.y});
                    Require(std::hypot(back.x-logical.x,back.y-logical.y)<1e-8,"Portrait or negative-origin mapping mismatch");++checks;
                }
            }
        }
        Quad crossed={{{0,0},{1,1},{1,0},{0,1}}};Require(!ValidQuad(crossed),"Crossed quad accepted");
        Quad flat={{{0,0},{1,0},{1,0},{0,0}}};Require(!ValidQuad(flat),"Degenerate quad accepted");
        auto invalid=IdentityQuad();invalid[0].x=std::numeric_limits<double>::quiet_NaN();Require(!ValidQuad(invalid),"NaN accepted");
        log<<"PASS: "<<checks+3<<" assertions; projective mapping, inverse hit alignment, portrait/landscape, negative origins, invalid quads.\n";
        RunWarpTests(log);return 0;
    }catch(const std::exception& e) {log<<"FAIL: "<<e.what()<<"\n";return 1;}
}
static int Diagnostics(const std::wstring& outputDir) {
    CreateDirectoryW(outputDir.c_str(),nullptr);std::ofstream log(outputDir+L"\\capture-tests.txt");
    try {
        Graphics g;auto displays=EnumerateMonitors();Require(!displays.empty(),"No monitors");
        for(const auto& m:displays) {
            Capture capture(g);capture.start(m);ULONGLONG start=GetTickCount64();
            while(!capture.frames&&GetTickCount64()-start<4000) {capture.update();Pump();Sleep(10);}
            Require(capture.frames>0,"No captured frames");
            D3D11_TEXTURE2D_DESC d{};capture.texture->GetDesc(&d);
            Require(d.Width==static_cast<UINT>(m.width())&&d.Height==static_cast<UINT>(m.height()),"Portrait capture surface mismatch");
            log<<winrt::to_string(m.device)<<" bounds="<<m.bounds.left<<","<<m.bounds.top<<" "<<m.width()<<"x"<<m.height()<<" rotation="<<m.rotation*90<<" captured="<<d.Width<<"x"<<d.Height<<" PASS\n";
        }
        MagInitialize();CURSORINFO before{sizeof(before)},after{sizeof(after)};GetCursorInfo(&before);
        bool hide=MagShowSystemCursor(FALSE)!=FALSE;GetCursorInfo(&after);MagShowSystemCursor(TRUE);MagUninitialize();
        Require(hide,"Cursor visibility API failed");
        log<<"Cursor API PASS; before flags="<<before.flags<<", hidden flags="<<after.flags<<", cursor handle preserved="<<(before.hCursor==after.hCursor)<<"\n";
        log<<"PASS: all "<<displays.size()<<" connected monitors; no screenshots saved.\n";return 0;
    }catch(const winrt::hresult_error& e) {MagShowSystemCursor(TRUE);log<<"FAIL: "<<winrt::to_string(e.message())<<" HRESULT="<<std::hex<<e.code().value<<"\n";return 1;}
      catch(const std::exception& e) {MagShowSystemCursor(TRUE);log<<"FAIL: "<<e.what()<<"\n";return 1;}
}
static LRESULT CALLBACK PatternProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps{};auto dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);
        COLORREF colors[]={RGB(220,40,50),RGB(30,200,75),RGB(40,80,225),RGB(235,180,30)};
        RECT cells[]={{0,0,r.right/2,r.bottom/2},{r.right/2,0,r.right,r.bottom/2},{r.right/2,r.bottom/2,r.right,r.bottom},{0,r.bottom/2,r.right/2,r.bottom}};
        for(int i=0;i<4;++i) {auto brush=CreateSolidBrush(colors[i]);FillRect(dc,&cells[i],brush);DeleteObject(brush);}
        SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);
        const wchar_t* label=L"SelfWindow display test — automatically restores";
        TextOutW(dc,30,30,label,static_cast<int>(wcslen(label)));EndPaint(h,&ps);return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
static int IntegrationTest(const std::wstring& outputDir) {
    CreateDirectoryW(outputDir.c_str(),nullptr);std::ofstream log(outputDir+L"\\integration-tests.txt");
    HWND pattern=nullptr,overlay=nullptr;
    try {
        WNDCLASSW wc{};wc.hInstance=appInstance;wc.lpfnWndProc=PatternProc;wc.lpszClassName=L"SelfWindowPattern";RegisterClassW(&wc);
        wc.lpfnWndProc=OutputProc;wc.lpszClassName=L"SelfWindowTestOutput";RegisterClassW(&wc);
        Graphics g;auto displays=EnumerateMonitors();
        auto checkColor=[](const Pixels& p,Point uv,int index) {
            unsigned x=std::clamp(static_cast<unsigned>(uv.x*p.width),0u,p.width-1),y=std::clamp(static_cast<unsigned>(uv.y*p.height),0u,p.height-1);
            size_t offset=(static_cast<size_t>(y)*p.width+x)*4;
            int expected[4][3]={{50,40,220},{75,200,30},{225,80,40},{30,180,235}};
            for(int c=0;c<3;++c) if(std::abs(int(p.bgra[offset+c])-expected[index][c])>12) {
                std::ostringstream error;error<<"Color / orientation mismatch at "<<uv.x<<","<<uv.y<<" expected channel "<<expected[index][c]<<" got "<<int(p.bgra[offset+c]);throw std::runtime_error(error.str());
            }
        };
        Point centers[]={{.25,.25},{.75,.25},{.75,.75},{.25,.75}};
        Quad q={{{.18,.03},{.94,.17},{.90,.84},{.04,.97}}};auto homography=FromQuad(q);
        for(const auto& m:displays) {
            pattern=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"SelfWindowPattern",L"SelfWindow display test",WS_POPUP,m.bounds.left,m.bounds.top,m.width(),m.height(),nullptr,nullptr,appInstance,nullptr);
            ShowWindow(pattern,SW_SHOWNOACTIVATE);UpdateWindow(pattern);
            Capture capture(g);capture.start(m);
            ULONGLONG start=GetTickCount64();while(GetTickCount64()-start<500) {Pump();capture.update();Sleep(10);}
            Require(capture.frames>0,"Pattern not captured");auto before=g.readTexture(capture.texture.get());
            for(int i=0;i<4;++i) checkColor(before,centers[i],i);
            overlay=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"SelfWindowTestOutput",L"SelfWindow test output",WS_POPUP,m.bounds.left,m.bounds.top,m.width(),m.height(),nullptr,nullptr,appInstance,nullptr);
            winrt::check_bool(SetLayeredWindowAttributes(overlay,0,255,LWA_ALPHA));winrt::check_bool(SetWindowDisplayAffinity(overlay,WDA_EXCLUDEFROMCAPTURE));
            {
                Renderer renderer(g,overlay,m.width(),m.height());renderer.draw(capture.view.get(),homography,false);ShowWindow(overlay,SW_SHOWNOACTIVATE);
                start=GetTickCount64();while(GetTickCount64()-start<650) {Pump();capture.update();renderer.draw(capture.view.get(),homography,false);Sleep(16);}
                auto output=renderer.snapshot();for(int i=0;i<4;++i) checkColor(output,homography.map(centers[i]),i);
                Require(output.bgra[0]<3&&output.bgra[1]<3&&output.bgra[2]<3,"Warp border is not black");
                auto behind=g.readTexture(capture.texture.get());for(int i=0;i<4;++i) checkColor(behind,centers[i],i);
                renderer.draw(capture.view.get(),homography,true);renderer.draw(capture.view.get(),homography,true);
                SaveBitmap(renderer.snapshot(),outputDir+L"\\grid-"+std::to_wstring(m.width())+L"x"+std::to_wstring(m.height())+L".bmp");
                std::vector<unsigned char> ramp(256*256*4);
                for(int y=0;y<256;++y)for(int x=0;x<256;++x){size_t offset=size_t(y*256+x)*4;ramp[offset]=0;ramp[offset+1]=static_cast<unsigned char>(y);ramp[offset+2]=static_cast<unsigned char>(x);ramp[offset+3]=255;}
                D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=256;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.Usage=D3D11_USAGE_IMMUTABLE;
                D3D11_SUBRESOURCE_DATA data{};data.pSysMem=ramp.data();data.SysMemPitch=256*4;
                winrt::com_ptr<ID3D11Texture2D> rampTexture;winrt::com_ptr<ID3D11ShaderResourceView> rampView;
                winrt::check_hresult(g.device->CreateTexture2D(&desc,&data,rampTexture.put()));winrt::check_hresult(g.device->CreateShaderResourceView(rampTexture.get(),nullptr,rampView.put()));
                for(int scenario=0;scenario<4;++scenario){
                    WarpProfile p;p.mode=scenario==0?WarpMode::ViewPlane:WarpMode::Cylinder;p.widthCm=30;p.heightCm=30.*m.height()/m.width();p.yaw=32;p.pitch=7;p.eyeX=-5;p.eyeY=4;p.horizontalAxis=scenario>=2;
                    if(scenario==3){p.curvature=-85;p.offsets[5]={.008,-.005};p.offsets[10]={-.007,.005};}
                    WarpMapping mapping(p,m.width(),m.height());
                    renderer.draw(rampView.get(),mapping,false);renderer.draw(rampView.get(),mapping,false);auto pixels=renderer.snapshot();
                    for(int iy=0;iy<7;++iy)for(int ix=0;ix<7;++ix){
                        Point uv{.12+ix*.12,.12+iy*.12};auto out=mapping.map(uv);int x=int(out.x*m.width()),y=int(out.y*m.height());size_t offset=(size_t(y)*pixels.width+x)*4;
                        Require(std::abs(int(pixels.bgra[offset+2])-(uv.x*256-.5))<3&&std::abs(int(pixels.bgra[offset+1])-(uv.y*256-.5))<3,"GPU sampling and software cursor mapping disagree");
                    }
                    for(int i=0;i<2;++i)renderer.draw(capture.view.get(),mapping,true);
                    if(scenario==2)SaveBitmap(renderer.snapshot(),outputDir+L"\\cylinder-"+std::to_wstring(m.width())+L"x"+std::to_wstring(m.height())+L".bmp");
                    log<<winrt::to_string(m.device)<<" scenario="<<scenario<<" PASS: GPU ramp/cursor alignment, subdivisions="<<mapping.divisions<<"\n";
                }
            }
            DestroyWindow(overlay);overlay=nullptr;capture.stop();DestroyWindow(pattern);pattern=nullptr;
            log<<winrt::to_string(m.device)<<" "<<m.width()<<"x"<<m.height()<<" rotation="<<m.rotation*90<<" PASS: oriented capture, GPU homography, corner colors, black borders, overlay excluded without recursion.\n";
        }
        log<<"PASS: integration checks on all connected monitors. Generated images contain only synthetic calibration patterns.\n";return 0;
    }catch(const winrt::hresult_error& e) {log<<"FAIL: "<<winrt::to_string(e.message())<<" HRESULT="<<std::hex<<e.code().value<<"\n";}
     catch(const std::exception& e) {log<<"FAIL: "<<e.what()<<"\n";}
    if(overlay) DestroyWindow(overlay);if(pattern) DestroyWindow(pattern);return 1;
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,PWSTR,int) {
    appInstance=hi;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc>=4&&std::wstring(argv[1])==L"--watch") {DWORD pid=wcstoul(argv[2],nullptr,10);HWND hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[3],nullptr,10)));LocalFree(argv);return Watchdog(pid,hwnd);}
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if(argc>=2&&std::wstring(argv[1])==L"--self-test") {std::wstring dir=argc>=3?argv[2]:L"test-output";LocalFree(argv);return SelfTest(dir);}
    if(argc>=2&&std::wstring(argv[1])==L"--diagnostics") {std::wstring dir=argc>=3?argv[2]:L"test-output";LocalFree(argv);return Diagnostics(dir);}
    if(argc>=2&&std::wstring(argv[1])==L"--integration-test") {std::wstring dir=argc>=3?argv[2]:L"test-output";LocalFree(argv);return IntegrationTest(dir);}
    bool openSettings=argc>=2&&std::wstring(argv[1])==L"--settings";
    bool quitRequested=argc>=2&&std::wstring(argv[1])==L"--quit";
    bool backgroundTest=argc>=2&&std::wstring(argv[1])==L"--background-test";
    bool uiTest=argc>=2&&std::wstring(argv[1])==L"--ui-test";if(uiTest)openSettings=true;
    std::wstring testDirectory=argc>=3?argv[2]:L"test-output";
    LocalFree(argv);
    if(quitRequested) {if(auto h=FindWindowW(L"SelfWindowMain",nullptr)) PostMessageW(h,WM_COMMAND,ID_TRAY_QUIT,0);return 0;}
    HANDLE single=CreateMutexW(nullptr,FALSE,L"Local\\SelfWindow.Perspective.SingleInstance");
    if(GetLastError()==ERROR_ALREADY_EXISTS) {if(!backgroundTest) {if(auto h=FindWindowW(L"SelfWindowMain",nullptr)) PostMessageW(h,WM_SHOW_SETTINGS,0,0);}if(single) CloseHandle(single);return backgroundTest?2:0;}
    int result=0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES};InitCommonControlsEx(&controls);
        InitializeSettings();
        if(backgroundTest||uiTest) {CreateDirectoryW(testDirectory.c_str(),nullptr);settingsFile=testDirectory+L"\\isolated-test-"+std::to_wstring(GetCurrentProcessId())+L".ini";}
        graphics=std::make_unique<Graphics>();cursor=std::make_unique<Cursor>();appIcon=CreateAppIcon();RegisterClasses();
        taskbarCreatedMessage=RegisterWindowMessageW(L"TaskbarCreated");
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);
        UINT initialDpi=GetDpiForSystem();
        HWND h=CreateWindowExW(0,L"SelfWindowMain",L"SelfWindow · 屏幕透视校正",WS_OVERLAPPEDWINDOW,
            mi.rcWork.left+60,mi.rcWork.top+50,MulDiv(980,initialDpi,96),MulDiv(800,initialDpi,96),nullptr,nullptr,appInstance,nullptr);
        winrt::check_bool(h!=nullptr);StartWatchdog();
        bool haveTray=AddTray();
        if(backgroundTest) result=BackgroundTest(testDirectory);
        else {
            if(openSettings||!haveTray||!emergencyReady||IsWindowVisible(h)) ShowSettings();
            else {HideSettings();NotifyBackground();}
            if(emergencyReady) PostMessageW(h,WM_RESTORE_PROFILES,0,0);
            MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
        }
    }catch(const winrt::hresult_error& e) {StopAll();MessageBoxW(nullptr,e.message().c_str(),L"SelfWindow",MB_ICONERROR);result=1;}
     catch(const std::exception& e) {StopAll();MessageBoxW(nullptr,winrt::to_hstring(e.what()).c_str(),L"SelfWindow",MB_ICONERROR);result=1;}
    StopAll();if(cursor) cursor->disposeShape();cursor.reset();graphics.reset();
    if(font) DeleteObject(font);if(titleFont) DeleteObject(titleFont);if(smallFont) DeleteObject(smallFont);
    RemoveTray();if(appIcon) DestroyIcon(appIcon);
    if(watchdogProcess) CloseHandle(watchdogProcess);if(single) CloseHandle(single);return result;
}
