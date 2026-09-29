// Native settings panel; included after the application's calibration callbacks.
static UINT calibrationDpi=96;
static HFONT calibrationFont=nullptr;
static bool syncingCalibration=false;
static constexpr int C_MODE=501,C_WIDTH=502,C_HEIGHT=503,C_DISTANCE=504,C_EYEX=505,C_EYEY=506,C_YAW=507,C_PITCH=508,C_CURVE=509,C_AXIS=510,C_MESH=511,C_APPLY=512,C_CLEAR=513,C_CLOSE=514,C_INFO=515;
static int CS(int n){return MulDiv(n,calibrationDpi,96);}
static HWND CalControl(const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int w,int h){
    HWND control=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,CS(x),CS(y),CS(w),CS(h),calibrationWindow,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),appInstance,nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(calibrationFont),TRUE);return control;
}
static void SetCalNumber(int id,double value){wchar_t text[64]{};swprintf_s(text,L"%.2f",value);SetWindowTextW(GetDlgItem(calibrationWindow,id),text);}
static double CalNumber(int id){wchar_t text[100]{};GetWindowTextW(GetDlgItem(calibrationWindow,id),text,100);std::wistringstream in(text);in.imbue(std::locale::classic());double value{};if(!(in>>value)||!std::isfinite(value))throw std::runtime_error("Invalid number");in>>std::ws;if(!in.eof())throw std::runtime_error("Invalid trailing text");return value;}
static void UpdateCalValues(){
    struct Slider{int id;const wchar_t* label;};
    for(auto slider:std::array<Slider,3>{{{C_YAW,L"左右倾斜：%d°  （正值：右边更远）"},{C_PITCH,L"上下倾斜：%d°  （正值：下边更远）"},{C_CURVE,L"曲率：%d%%  （0 为平面，支持反向弯曲）"}}}){
        wchar_t text[120]{};swprintf_s(text,slider.label,int(SendMessageW(GetDlgItem(calibrationWindow,slider.id),TBM_GETPOS,0,0)));SetWindowTextW(GetDlgItem(calibrationWindow,slider.id+100),text);
    }
    int mode=int(SendMessageW(GetDlgItem(calibrationWindow,C_MODE),CB_GETCURSEL,0,0));
    for(int id:{C_WIDTH,C_HEIGHT,C_DISTANCE,C_EYEX,C_EYEY,C_YAW,C_PITCH,C_APPLY})EnableWindow(GetDlgItem(calibrationWindow,id),mode!=0);
    EnableWindow(GetDlgItem(calibrationWindow,C_CURVE),mode==2);EnableWindow(GetDlgItem(calibrationWindow,C_AXIS),mode==2);
}
static void SyncCalibration(){
    if(!calibrationWindow||monitors.empty())return;syncingCalibration=true;
    const auto& p=profiles[selected];SetWindowTextW(calibrationWindow,(L"曲面与视点 · "+monitors[selected].device).c_str());
    SendMessageW(GetDlgItem(calibrationWindow,C_MODE),CB_SETCURSEL,int(p.mode),0);
    SetCalNumber(C_WIDTH,p.widthCm);SetCalNumber(C_HEIGHT,p.heightCm);SetCalNumber(C_DISTANCE,p.distanceCm);SetCalNumber(C_EYEX,p.eyeX);SetCalNumber(C_EYEY,p.eyeY);
    SendMessageW(GetDlgItem(calibrationWindow,C_YAW),TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(p.yaw)));
    SendMessageW(GetDlgItem(calibrationWindow,C_PITCH),TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(p.pitch)));
    SendMessageW(GetDlgItem(calibrationWindow,C_CURVE),TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(p.curvature)));
    SendMessageW(GetDlgItem(calibrationWindow,C_AXIS),CB_SETCURSEL,p.horizontalAxis?1:0,0);
    SendMessageW(GetDlgItem(calibrationWindow,C_MESH),BM_SETCHECK,editMesh?BST_CHECKED:BST_UNCHECKED,0);
    SetWindowTextW(GetDlgItem(calibrationWindow,C_INFO),L"尺寸默认为示例值，请按实物测量。滑条实时生效；尺寸和偏移输入后点“应用”。");
    UpdateCalValues();syncingCalibration=false;
}
static void ApplyCalibrationControls(bool save){
    if(syncingCalibration||monitors.empty())return;
    try{
        auto p=profiles[selected];p.mode=static_cast<WarpMode>(SendMessageW(GetDlgItem(calibrationWindow,C_MODE),CB_GETCURSEL,0,0));
        p.widthCm=CalNumber(C_WIDTH);p.heightCm=CalNumber(C_HEIGHT);p.distanceCm=CalNumber(C_DISTANCE);p.eyeX=CalNumber(C_EYEX);p.eyeY=CalNumber(C_EYEY);
        p.yaw=double(SendMessageW(GetDlgItem(calibrationWindow,C_YAW),TBM_GETPOS,0,0));p.pitch=double(SendMessageW(GetDlgItem(calibrationWindow,C_PITCH),TBM_GETPOS,0,0));
        p.curvature=double(SendMessageW(GetDlgItem(calibrationWindow,C_CURVE),TBM_GETPOS,0,0));p.horizontalAxis=SendMessageW(GetDlgItem(calibrationWindow,C_AXIS),CB_GETCURSEL,0,0)==1;
        if(ApplyProfile(p)){
            if(save)SaveQuad(selected);
            std::wstring info=L"已应用。完整画面缩放 "+std::to_wstring(int(mappings[selected]->fit*100))+L"%；原四角参数仍保留，可随时切回。";
            SetWindowTextW(GetDlgItem(calibrationWindow,C_INFO),info.c_str());SetStatus(L"视点／曲面参数已更新。可开启校准网格，坐在平时的位置观察。");
        }else SetWindowTextW(GetDlgItem(calibrationWindow,C_INFO),L"未应用：画面会超出屏幕或发生折叠。请减小参数；如果改过局部网格，可先清除微调。");
    }catch(...){SetWindowTextW(GetDlgItem(calibrationWindow,C_INFO),L"请输入有效数字：宽高 5–200 cm，距离 15–300 cm，眼睛偏移 −150–150 cm。");}
    UpdateCalValues();
}
static LRESULT CALLBACK CalibrationProc(HWND h,UINT message,WPARAM wp,LPARAM lp){
    switch(message){
    case WM_CREATE:{
        calibrationWindow=h;calibrationDpi=GetDpiForWindow(h);
        calibrationFont=CreateFontW(-CS(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        CalControl(L"STATIC",L"校正模式",0,600,20,16,440,22);
        auto mode=CalControl(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,C_MODE,20,42,450,180);
        for(const wchar_t* name:{L"四角透视（兼容已有校准）",L"视点标定 · 正对的平面",L"视点标定 · 可调圆柱曲面"})SendMessageW(mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        const wchar_t* labels[]={L"屏幕宽度 / cm",L"屏幕高度 / cm",L"前后距离 / cm",L"眼睛左右偏移 / cm",L"眼睛上下偏移 / cm"};
        for(int i=0;i<5;++i){int x=i<3?20+i*152:20+(i-3)*228,y=i<3?87:149,w=i<3?140:216;
            CalControl(L"STATIC",labels[i],0,620+i,x,y,w,22);CalControl(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,C_WIDTH+i,x,y+25,w,28);}
        CalControl(L"STATIC",L"偏移正值：眼睛在屏幕中心的右方／下方。",0,625,20,209,450,23);
        for(int i=0;i<3;++i){int id=C_YAW+i,y=242+i*67;
            CalControl(L"STATIC",L"",0,id+100,20,y,450,24);auto slider=CalControl(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP,id,16,y+25,458,30);
            int range=i==0?65:i==1?45:100;SendMessageW(slider,TBM_SETRANGEMIN,FALSE,-range);SendMessageW(slider,TBM_SETRANGEMAX,FALSE,range);SendMessageW(slider,TBM_SETPAGESIZE,0,5);}
        CalControl(L"STATIC",L"圆柱弯曲方向（始终按观看方向定义）",0,626,20,445,450,23);
        auto axis=CalControl(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,C_AXIS,20,471,450,150);
        SendMessageW(axis,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"左右弯曲（绕竖轴）"));SendMessageW(axis,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"上下弯曲（绕横轴，适合长竖屏）"));
        CalControl(WC_BUTTONW,L"在主窗口编辑 4 × 4 局部曲面网格",BS_AUTOCHECKBOX|WS_TABSTOP,C_MESH,20,510,450,30);
        CalControl(WC_BUTTONW,L"应用尺寸和偏移",BS_PUSHBUTTON|WS_TABSTOP,C_APPLY,20,553,214,34);
        CalControl(WC_BUTTONW,L"清除局部微调",BS_PUSHBUTTON|WS_TABSTOP,C_CLEAR,246,553,224,34);
        CalControl(L"STATIC",L"",SS_LEFT,C_INFO,20,603,450,56);
        CalControl(WC_BUTTONW,L"关闭设置",BS_PUSHBUTTON|WS_TABSTOP,C_CLOSE,320,671,150,34);
        SyncCalibration();return 0;
    }
    case WM_HSCROLL:ApplyCalibrationControls(LOWORD(wp)!=TB_THUMBTRACK);return 0;
    case WM_COMMAND:{int id=LOWORD(wp);
        if(id==C_APPLY||((id==C_MODE||id==C_AXIS)&&HIWORD(wp)==CBN_SELCHANGE))ApplyCalibrationControls(true);
        else if(id==C_MESH){editMesh=SendMessageW(GetDlgItem(h,C_MESH),BM_GETCHECK,0,0)==BST_CHECKED;selectedCorner=0;RefreshLabels();InvalidateRect(mainWindow,nullptr,TRUE);}
        else if(id==C_CLEAR&&!monitors.empty()){auto p=profiles[selected];p.offsets={};if(ApplyProfile(p)){SaveQuad(selected);SetWindowTextW(GetDlgItem(h,C_INFO),L"局部网格已清除，保留模式、四角和观看参数。");}}
        else if(id==C_CLOSE)DestroyWindow(h);return 0;}
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:calibrationWindow=nullptr;if(calibrationFont){DeleteObject(calibrationFont);calibrationFont=nullptr;}return 0;
    case WM_DPICHANGED:{
        UINT oldDpi=calibrationDpi;calibrationDpi=HIWORD(wp);RECT target=*reinterpret_cast<RECT*>(lp);
        EnumChildWindows(h,[](HWND child,LPARAM data)->BOOL{auto oldDpi=static_cast<UINT>(data);RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,calibrationWindow,reinterpret_cast<POINT*>(&r),2);MoveWindow(child,MulDiv(r.left,calibrationDpi,oldDpi),MulDiv(r.top,calibrationDpi,oldDpi),MulDiv(r.right-r.left,calibrationDpi,oldDpi),MulDiv(r.bottom-r.top,calibrationDpi,oldDpi),TRUE);return TRUE;},oldDpi);
        if(calibrationFont)DeleteObject(calibrationFont);calibrationFont=CreateFontW(-CS(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        EnumChildWindows(h,[](HWND child,LPARAM)->BOOL{SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(calibrationFont),TRUE);return TRUE;},0);
        SetWindowPos(h,nullptr,target.left,target.top,target.right-target.left,target.bottom-target.top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;
    }
    }
    return DefWindowProcW(h,message,wp,lp);
}
static void OpenCalibration(){
    if(calibrationWindow){ShowWindow(calibrationWindow,SW_RESTORE);SetForegroundWindow(calibrationWindow);return;}
    WNDCLASSW wc{};wc.hInstance=appInstance;wc.lpfnWndProc=CalibrationProc;wc.lpszClassName=L"SelfWindowCalibration";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&wc);
    RECT r{};GetWindowRect(mainWindow,&r);UINT dpi=GetDpiForWindow(mainWindow);RECT desired{0,0,MulDiv(490,dpi,96),MulDiv(725,dpi,96)};AdjustWindowRectExForDpi(&desired,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_TOOLWINDOW,dpi);
    HWND h=CreateWindowExW(WS_EX_TOOLWINDOW,L"SelfWindowCalibration",L"曲面与视点",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,r.left+S(36),r.top+S(24),desired.right-desired.left,desired.bottom-desired.top,mainWindow,nullptr,appInstance,nullptr);
    if(!h)winrt::throw_last_error();ShowWindow(h,SW_SHOW);UpdateWindow(h);
}
