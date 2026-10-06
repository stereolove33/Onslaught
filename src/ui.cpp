#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <memory>
#include <cstring>
#include <algorithm>
#include <utility>
#include <thread>
#include <mutex>
#include <winhttp.h>
#include "release.h"
#include "target_selection.h"
#include "instance_lock.h"
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;
constexpr COLORREF Background=RGB(9,9,9),Card=RGB(20,20,20),Line=RGB(48,48,48);
constexpr COLORREF Text=RGB(255,255,255),Muted=RGB(163,163,163);
enum {AppList=100,Refresh,MonitorList,InputList,FpsList,SizeList,Resize,KeepBackground,Start,Stop,Logs,Settings,Language,AutoRun,CloseTray,MinimizeStart,RememberApp,EmergencyHotkey,SaveHotkey,Repository,CheckUpdates,OpenUpdate,AutoCheck};
struct Target {HWND window{};std::wstring title;DWORD pid{};std::wstring executable;};
struct Display {std::wstring device,label;};
HWND mainWindow{},controls[140]{};
HINSTANCE instance{};
std::vector<Target> targets;
std::vector<Display> displays;
HFONT normalFont{},smallFont{},titleFont{},headingFont{};
HBRUSH backgroundBrush{},cardBrush{};
ULONG_PTR gdiplusToken{};IStream* iconStream{};std::unique_ptr<Image> iconImage;
HANDLE engineProcess{},engineWait{};
std::wstring appDirectory,sessionDirectory,settingsPath;
std::wstring statusText,ratesText;
bool running=false,quitting=false,uiTest=false,settingsPage=false,portuguese=false,trayAvailable=false;
HICON bigIcon{},smallIcon{};
constexpr UINT TrayMessage=WM_APP+20;
std::wstring tr(const wchar_t* en,const wchar_t* pt) {return portuguese?pt:en;}
void applyLanguage();
void layout();
float dpiScale=1;
int contentWidth=960;
UINT emergencyKey=VK_F10,emergencyModifiers=MOD_CONTROL|MOD_ALT;
std::wstring lastExecutable,lastTitle,releaseUrl,updateStatus;
bool restoreLastSelection=true,checkingUpdate=false;
struct UpdateResult {std::mutex mutex;int code=0;std::wstring tag,url;};
std::shared_ptr<UpdateResult> updateResult;
std::wstring emergencyLabel() {
    std::wstring text;if(emergencyModifiers&MOD_CONTROL) text+=L"Ctrl + ";if(emergencyModifiers&MOD_ALT) text+=L"Alt + ";if(emergencyModifiers&MOD_SHIFT) text+=L"Shift + ";
    UINT scan=MapVirtualKeyW(emergencyKey,MAPVK_VK_TO_VSC);wchar_t key[64]{};GetKeyNameTextW(LONG(scan<<16),key,64);text+=*key?key:L"?";return text;
}
std::wstring profileString(const wchar_t* key) {wchar_t value[32768]{};GetPrivateProfileStringW(L"interface",key,wcscmp(key,L"repository")==0?L"stereolove33/Onslaught":L"",value,32768,settingsPath.c_str());return value;}
void checkForUpdates();
LRESULT CALLBACK centeredHotkey(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(message==WM_NCPAINT) return 0;
    if(message==WM_ERASEBKGND) {RECT bounds{};GetClientRect(window,&bounds);FillRect(reinterpret_cast<HDC>(w),&bounds,cardBrush);return 1;}
    if(message==WM_PAINT) {
        PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);RECT bounds{};GetClientRect(window,&bounds);
        FillRect(dc,&bounds,cardBrush);HBRUSH border=CreateSolidBrush(Line);FrameRect(dc,&bounds,border);DeleteObject(border);
        WORD value=WORD(SendMessageW(window,HKM_GETHOTKEY,0,0));BYTE mods=HIBYTE(value),key=LOBYTE(value);
        std::wstring text;if(mods&HOTKEYF_CONTROL) text+=L"Ctrl + ";if(mods&HOTKEYF_ALT) text+=L"Alt + ";if(mods&HOTKEYF_SHIFT) text+=L"Shift + ";
        wchar_t name[64]{};UINT scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC);GetKeyNameTextW(LONG(scan<<16)|((mods&HOTKEYF_EXT)?(1<<24):0),name,64);
        text+=key?name:L"None";SelectObject(dc,normalFont);SetTextColor(dc,IsWindowEnabled(window)?Text:Muted);SetBkMode(dc,TRANSPARENT);
        bounds.left+=int(10*dpiScale+.5f);
        DrawTextW(dc,text.c_str(),int(text.size()),&bounds,DT_LEFT|DT_VCENTER|DT_SINGLELINE);EndPaint(window,&ps);HideCaret(window);return 0;
    }
    auto result=DefSubclassProc(window,message,w,l);
    if(message==WM_KEYDOWN || message==WM_KEYUP || message==HKM_SETHOTKEY || message==WM_SETFOCUS || message==WM_KILLFOCUS) InvalidateRect(window,nullptr,FALSE);
    if(message==WM_NCDESTROY) RemoveWindowSubclass(window,centeredHotkey,id);
    return result;
}

int scale(int v) {return int(v*dpiScale+.5f);}
std::wstring wide(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);
    std::wstring result(n,L' ');MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),result.data(),n);return result;
}
std::wstring label(HWND h) {int n=GetWindowTextLengthW(h);std::wstring s(n+1,L' ');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
int selected(int id) {return int(SendMessageW(controls[id],CB_GETCURSEL,0,0));}
void setStatus(std::wstring text) {statusText=std::move(text);InvalidateRect(mainWindow,nullptr,FALSE);}
void enableControls() {
    for(int id : {AppList,Refresh,MonitorList,InputList,FpsList,SizeList,Resize,KeepBackground,Start})
        EnableWindow(controls[id],!running);
    EnableWindow(controls[Stop],running);EnableWindow(controls[EmergencyHotkey],!running);EnableWindow(controls[SaveHotkey],!running);
}
void addCombo(int id,const std::vector<std::wstring>& values,int selection=0) {
    SendMessageW(controls[id],CB_RESETCONTENT,0,0);
    for(auto& text:values) SendMessageW(controls[id],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    if(!values.empty()) SendMessageW(controls[id],CB_SETCURSEL,std::min(selection,int(values.size()-1)),0);
}
void refreshTargets() {
    HWND previous=nullptr;
    int current=selected(AppList);if(current>=0 && current<int(targets.size())) previous=targets[current].window;
    targets.clear();
    EnumWindows([](HWND window,LPARAM)->BOOL {
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        if(window==mainWindow || pid==GetCurrentProcessId() || !IsWindowVisible(window) ||
           GetWindowTextLengthW(window)==0 || (GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)) return TRUE;
        wchar_t text[1024]{};GetWindowTextW(window,text,1024);
        std::wstring title=text;
        if(title==L"Program Manager" || title.find(L"Onslaught - Presentation")!=std::wstring::npos) return TRUE;
        wchar_t path[32768]{};DWORD length=32768;
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(process) {if(!QueryFullProcessImageNameW(process,0,path,&length)) path[0]=0;CloseHandle(process);}
        targets.push_back({window,title,pid,path});return TRUE;
    },0);
    std::vector<std::wstring> names;int selection=0;int remembered=-1;
    for(size_t i=0;i<targets.size();++i) {
        names.push_back(targets[i].title+L"  ·  "+std::to_wstring(targets[i].pid));
        if(targets[i].window==previous) selection=int(i);
    }
    if(restoreLastSelection && GetPrivateProfileIntW(L"interface",L"remember_app",1,settingsPath.c_str()))
        remembered=onslaught::rememberedTarget(targets,lastExecutable,lastTitle);
    if(remembered>=0) {selection=remembered;restoreLastSelection=false;}
    addCombo(AppList,names,selection);InvalidateRect(mainWindow,nullptr,FALSE);
}
void refreshMonitors() {
    displays.clear();
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM)->BOOL {
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(!GetMonitorInfoW(monitor,&info)) return TRUE;
        DEVMODEW mode{};mode.dmSize=sizeof(mode);EnumDisplaySettingsW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode);
        auto name=std::wstring(info.szDevice)+L"  ·  "+std::to_wstring(info.rcMonitor.right-info.rcMonitor.left)+
            L" × "+std::to_wstring(info.rcMonitor.bottom-info.rcMonitor.top)+L"  ·  "+std::to_wstring(mode.dmDisplayFrequency)+L" Hz";
        if(info.dwFlags&MONITORINFOF_PRIMARY) name+=tr(L"  ·  primary",L"  ·  principal");
        displays.push_back({info.szDevice,name});return TRUE;
    },0);
    std::vector<std::wstring> names;int primary=0;
    for(size_t i=0;i<displays.size();++i) {names.push_back(displays[i].label);if(names.back().find(portuguese?L"principal":L"primary")!=std::wstring::npos) primary=int(i);}
    addCombo(MonitorList,names,primary);
}
void saveSettings() {
    WritePrivateProfileStringW(L"interface",L"repository",nullptr,settingsPath.c_str());
    WritePrivateProfileStringW(L"interface",L"emergency_key",std::to_wstring(emergencyKey).c_str(),settingsPath.c_str());
    WritePrivateProfileStringW(L"interface",L"emergency_modifiers",std::to_wstring(emergencyModifiers).c_str(),settingsPath.c_str());
    WritePrivateProfileStringW(L"interface",L"last_executable",lastExecutable.c_str(),settingsPath.c_str());
    WritePrivateProfileStringW(L"interface",L"last_title",lastTitle.c_str(),settingsPath.c_str());
    WritePrivateProfileStringW(L"interface",L"language",portuguese?L"pt-BR":L"en",settingsPath.c_str());
    for(auto item : {std::pair<const wchar_t*,int>{L"close_tray",CloseTray},{L"minimize_start",MinimizeStart},{L"remember_app",RememberApp},{L"auto_check",AutoCheck}})
        WritePrivateProfileStringW(L"interface",item.first,SendMessageW(controls[item.second],BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",settingsPath.c_str());
    for(auto pair : {std::pair<const wchar_t*,int>{L"input",InputList},{L"fps",FpsList},{L"size",SizeList},{L"monitor",MonitorList}}) {
        auto value=std::to_wstring(selected(pair.second));WritePrivateProfileStringW(L"interface",pair.first,value.c_str(),settingsPath.c_str());
    }
    WritePrivateProfileStringW(L"interface",L"background",SendMessageW(controls[KeepBackground],BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",settingsPath.c_str());
}
void stopEngine() {
    if(engineProcess && WaitForSingleObject(engineProcess,0)==WAIT_TIMEOUT) TerminateProcess(engineProcess,70);
}
VOID CALLBACK engineFinished(PVOID context,BOOLEAN) {
    PostMessageW(reinterpret_cast<HWND>(context),WM_APP+10,0,0);
}
void cleanupEngine() {
    if(engineWait) {UnregisterWaitEx(engineWait,INVALID_HANDLE_VALUE);engineWait=nullptr;}
    if(engineProcess) {CloseHandle(engineProcess);engineProcess=nullptr;}
}
void startEngine() {
    if(running || uiTest) {if(uiTest) setStatus(tr(L"UI test mode: capture disabled.",L"Modo de verificação da interface: captura desativada."));return;}
    int target=selected(AppList),monitor=selected(MonitorList);
    if(target<0 || target>=int(targets.size()) || monitor<0 || monitor>=int(displays.size())) {
        setStatus(tr(L"Select an app and a monitor.",L"Selecione um aplicativo e um monitor."));return;
    }
    HWND source=targets[target].window;
    if(!IsWindow(source)) {setStatus(tr(L"The window closed. Refresh the list.",L"A janela fechou. Atualize a lista."));return;}
    const wchar_t* input=selected(InputList)==0?L"absolute":selected(InputList)==1?L"relative":L"visual";
    const int fpsValues[]={0,60,120,144,165,180,240};
    int fps=selected(FpsList);if(fps<0 || fps>6) fps=0;
    SYSTEMTIME now{};GetLocalTime(&now);wchar_t stamp[100]{};
    swprintf_s(stamp,L"%04u%02u%02u-%02u%02u%02u-%lu",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId());
    CreateDirectoryW((appDirectory+L"\\logs").c_str(),nullptr);
    sessionDirectory=appDirectory+L"\\logs\\"+stamp;
    if(!CreateDirectoryW(sessionDirectory.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
        setStatus(tr(L"Cannot create session logs in this folder.",L"Não foi possível criar o registro da sessão nesta pasta."));return;
    }
    std::wstring executable=appDirectory+L"\\OnslaughtEngine.exe";
    std::wstring command=L"\""+executable+L"\" --window "+std::to_wstring(reinterpret_cast<ULONG_PTR>(source))+
        L" --monitor-device \""+displays[monitor].device+L"\" --input "+input+L" --fps "+std::to_wstring(fpsValues[fps]);
    command+=L" --emergency-key "+std::to_wstring(emergencyKey)+L" --emergency-modifiers "+std::to_wstring(emergencyModifiers);
    if(SendMessageW(controls[KeepBackground],BM_GETCHECK,0,0)!=BST_CHECKED) command+=L" --hide-on-blur";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION info{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
        sessionDirectory.c_str(),&startup,&info)) {
        setStatus(tr(L"Cannot start the engine. Code ",L"Não foi possível iniciar o motor. Código ")+std::to_wstring(GetLastError())+L".");return;
    }
    engineProcess=info.hProcess;CloseHandle(info.hThread);
    if(!RegisterWaitForSingleObject(&engineWait,engineProcess,engineFinished,mainWindow,INFINITE,WT_EXECUTEONLYONCE)) {
        stopEngine();cleanupEngine();setStatus(tr(L"Cannot monitor the session.",L"Não foi possível acompanhar a sessão."));return;
    }
    lastExecutable=targets[target].executable;lastTitle=targets[target].title;
    ratesText.clear();running=true;saveSettings();enableControls();
    setStatus(tr(L"Session started. Emergency exit: ",L"Sessão iniciada. Saída de emergência: ")+emergencyLabel());
    if(SendMessageW(controls[MinimizeStart],BM_GETCHECK,0,0)==BST_CHECKED) ShowWindow(mainWindow,SW_MINIMIZE);
}
void resizeTarget() {
    int target=selected(AppList),size=selected(SizeList);
    if(target<0 || target>=int(targets.size()) || size<=0) {setStatus(tr(L"Choose a size before resizing the window.",L"Escolha um tamanho antes de ajustar a janela."));return;}
    const SIZE sizes[]={{0,0},{1680,1050},{1600,900},{1280,720},{1024,768},{800,600}};
    if(size>5) return;
    HWND source=targets[target].window;
    if(IsIconic(source)) ShowWindowAsync(source,SW_RESTORE);
    RECT client{},outer{};
    if(!GetClientRect(source,&client)||!GetWindowRect(source,&outer)||IsIconic(source)) {
        setStatus(tr(L"Restore the window and try again.",L"Restaure a janela e tente novamente."));return;
    }
    int width=sizes[size].cx+(outer.right-outer.left)-(client.right-client.left);
    int height=sizes[size].cy+(outer.bottom-outer.top)-(client.bottom-client.top);
    if(!SetWindowPos(source,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS)) {
        setStatus(tr(L"The app refused the resize request.",L"O aplicativo recusou o ajuste da janela."));return;
    }
    setStatus(tr(L"Size requested. Also select the resolution inside your game.",L"Tamanho solicitado. Em jogos, confirme também a resolução nas opções do jogo."));
}
void readMetrics() {
    if(!running || IsIconic(mainWindow) || sessionDirectory.empty()) return;
    std::ifstream file(sessionDirectory+L"\\diagnostico.txt",std::ios::binary);
    if(!file) return;
    file.seekg(0,std::ios::end);auto end=file.tellg();file.seekg(end>8192?end-std::streamoff(8192):std::streamoff(0));
    std::string data((std::istreambuf_iterator<char>(file)),{});
    auto p=data.rfind("RATES ");
    if(p!=std::string::npos) {
        ratesText=wide(data.substr(p,data.find('\n',p)-p));InvalidateRect(mainWindow,nullptr,FALSE);
    }
}
void positionControl(int id,int x,int y,int width,int height) {
    SetWindowPos(controls[id],nullptr,scale(x),scale(y),scale(width),scale(height),SWP_NOZORDER|SWP_NOACTIVATE);
}
void layout() {
    RECT client{};GetClientRect(mainWindow,&client);contentWidth=int(client.right/dpiScale);
    int leftWidth=contentWidth-390,rightX=contentWidth-340;
    positionControl(AppList,52,171,leftWidth-40,240);positionControl(Refresh,32+leftWidth-110,129,90,30);
    positionControl(MonitorList,52,286,leftWidth-40,240);
    positionControl(SizeList,52,383,leftWidth-240,200);positionControl(Resize,52+leftWidth-224,383,184,36);
    positionControl(InputList,rightX+20,171,268,210);positionControl(FpsList,rightX+20,267,268,240);
    positionControl(KeepBackground,rightX+20,337,268,48);
    positionControl(Start,32,560,contentWidth-388,48);positionControl(Stop,rightX,560,308,48);
    positionControl(Logs,contentWidth-159,473,107,30);
    positionControl(Settings,contentWidth-274,33,116,34);
    positionControl(Language,52,181,360,180);
    positionControl(AutoRun,52,229,480,30);
    positionControl(CloseTray,52,265,480,30);
    positionControl(RememberApp,52,301,480,30);
    positionControl(MinimizeStart,52,337,480,30);
    positionControl(EmergencyHotkey,52,419,230,34);positionControl(SaveHotkey,294,419,118,34);
    positionControl(Repository,rightX+20,192,268,34);
    positionControl(CheckUpdates,rightX+20,174,268,36);positionControl(OpenUpdate,rightX+20,222,268,36);
    positionControl(AutoCheck,rightX+20,270,268,42);
    for(int id : {AppList,Refresh,MonitorList,InputList,FpsList,SizeList,Resize,KeepBackground,Start,Stop,Logs})
        ShowWindow(controls[id],settingsPage?SW_HIDE:SW_SHOW);
    for(int id : {Language,AutoRun,CloseTray,MinimizeStart,RememberApp,EmergencyHotkey,SaveHotkey,Repository,CheckUpdates,OpenUpdate,AutoCheck}) ShowWindow(controls[id],settingsPage?SW_SHOW:SW_HIDE);
}
void fonts() {
    for(HFONT font : {normalFont,smallFont,titleFont,headingFont}) if(font) DeleteObject(font);
    normalFont=CreateFontW(-scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    smallFont=CreateFontW(-scale(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    titleFont=CreateFontW(-scale(26),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    headingFont=CreateFontW(-scale(11),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    for(auto control:controls) if(control) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(normalFont),TRUE);
}
void drawText(HDC dc,const std::wstring& text,int x,int y,int width,int height,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_WORDBREAK) {
    SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    RECT r{scale(x),scale(y),scale(x+width),scale(y+height)};DrawTextW(dc,text.c_str(),int(text.size()),&r,flags);
}
void rounded(HDC dc,int x,int y,int width,int height,COLORREF fill,COLORREF stroke,int radius=12) {
    HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,stroke);
    auto oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
    RoundRect(dc,scale(x),scale(y),scale(x+width),scale(y+height),scale(radius*2),scale(radius*2));
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(brush);DeleteObject(pen);
}
void paint(HDC supplied=nullptr) {
    PAINTSTRUCT ps{};HDC output=supplied ? supplied : BeginPaint(mainWindow,&ps);RECT bounds{};GetClientRect(mainWindow,&bounds);
    HDC dc=CreateCompatibleDC(output);HBITMAP bitmap=CreateCompatibleBitmap(output,bounds.right,bounds.bottom);
    auto old=SelectObject(dc,bitmap);FillRect(dc,&bounds,backgroundBrush);
    int leftWidth=contentWidth-390,rightX=contentWidth-340;
    if(iconImage) {Graphics g(dc);g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(iconImage.get(),Rect(scale(26),scale(20),scale(76),scale(62)));}
    drawText(dc,L"Onslaught",112,23,400,35,titleFont,Text);
    drawText(dc,tr(L"Stretch your screen. Expand your view.",L"Estique a tela. Amplie sua visão."),114,61,440,25,smallFont,Muted);
    rounded(dc,contentWidth-148,33,116,34,Card,Line,7);
    drawText(dc,running?tr(L"Running",L"Em execução"):tr(L"Ready",L"Pronto"),contentWidth-138,33,96,34,normalFont,Text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(settingsPage) {
        rounded(dc,32,108,leftWidth,437,Card,Line);
        rounded(dc,rightX,108,308,437,Card,Line);
        drawText(dc,tr(L"SETTINGS",L"CONFIGURAÇÕES"),52,130,400,24,headingFont,Muted);
        drawText(dc,tr(L"Language",L"Idioma"),52,155,400,22,normalFont,Text);
        drawText(dc,tr(L"EMERGENCY EXIT",L"SAÍDA DE EMERGÊNCIA"),52,390,420,24,headingFont,Muted);
        drawText(dc,tr(L"Ctrl + Alt + F12 remains as a backup.",L"Ctrl + Alt + F12 continua como alternativa."),52,474,leftWidth-40,35,smallFont,Muted);
        drawText(dc,tr(L"UPDATES",L"ATUALIZAÇÕES"),rightX+20,130,268,24,headingFont,Muted);
        drawText(dc,updateStatus.empty()?tr(L"Updates come from the official Onslaught GitHub Releases.",L"As atualizações vêm das Releases oficiais do Onslaught no GitHub."):updateStatus,rightX+20,474,268,51,smallFont,Muted);
        drawText(dc,tr(L"Preferences are saved automatically.",L"As preferências são salvas automaticamente."),32,566,contentWidth-64,28,smallFont,Muted);
    } else {
    rounded(dc,32,108,leftWidth,329,Card,Line);
    rounded(dc,rightX,108,308,329,Card,Line);
    drawText(dc,tr(L"APPLICATION",L"APLICATIVO"),52,135,300,20,headingFont,Muted);
    std::wstring dimensions=tr(L"Refresh the list if your app is missing.",L"Atualize a lista se o aplicativo não aparecer.");
    int index=selected(AppList);
    if(index>=0 && index<int(targets.size())) {
        RECT client{};GetClientRect(targets[index].window,&client);
        dimensions=IsIconic(targets[index].window)?tr(L"Minimized window · restored when starting",L"Janela minimizada · será restaurada ao iniciar"):
            std::to_wstring(client.right)+L" × "+std::to_wstring(client.bottom)+tr(L" · capture area",L" · área capturada");
    }
    drawText(dc,dimensions,52,213,leftWidth-40,32,smallFont,Muted);
    drawText(dc,tr(L"OUTPUT MONITOR",L"MONITOR DE APRESENTAÇÃO"),52,256,leftWidth-40,20,headingFont,Muted);
    drawText(dc,tr(L"WINDOW SIZE · OPTIONAL",L"TAMANHO DA JANELA · OPCIONAL"),52,354,leftWidth-40,20,headingFont,Muted);
    drawText(dc,tr(L"INPUT",L"ENTRADA"),rightX+20,135,260,20,headingFont,Muted);
    drawText(dc,tr(L"Mapped cursor for apps · Relative mouse for FPS",L"Cursor mapeado para apps · Mouse relativo para FPS"),rightX+20,214,268,36,smallFont,Muted);
    drawText(dc,tr(L"PRESENTATION LIMIT",L"LIMITE DE APRESENTAÇÃO"),rightX+20,244,268,20,headingFont,Muted);
    drawText(dc,tr(L"The source app controls its own FPS.",L"O app de origem controla seu próprio FPS."),rightX+20,309,268,22,smallFont,Muted);
    drawText(dc,tr(L"Alt+Tab releases the app input.",L"Alt+Tab libera a entrada do aplicativo."),rightX+20,395,268,25,smallFont,Muted);
    rounded(dc,32,457,contentWidth-64,86,Card,Line);
    drawText(dc,statusText,52,474,contentWidth-245,32,normalFont,Text);
    drawText(dc,ratesText.empty()?tr(L"Emergency exit: ",L"Saída de emergência: ")+emergencyLabel():ratesText,52,510,contentWidth-104,22,smallFont,Muted,DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    drawText(dc,std::wstring(onslaught::Version)+L" · Experimental",32,625,220,20,smallFont,Muted);
    drawText(dc,tr(L"No injection · desktop resolution preserved",L"Sem injeção · desktop na resolução atual"),contentWidth-350,625,318,20,smallFont,Muted,DT_RIGHT|DT_SINGLELINE);
    BitBlt(output,0,0,bounds.right,bounds.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);if(!supplied) EndPaint(mainWindow,&ps);
}
void checkForUpdates() {
    if(checkingUpdate) return;
    const std::wstring repository=onslaught::OfficialRepository;
    saveSettings();
    checkingUpdate=true;releaseUrl.clear();EnableWindow(controls[OpenUpdate],FALSE);EnableWindow(controls[CheckUpdates],FALSE);
    updateStatus=tr(L"Checking GitHub Releases…",L"Verificando Releases do GitHub…");InvalidateRect(mainWindow,nullptr,FALSE);
    auto result=std::make_shared<UpdateResult>();updateResult=result;HWND window=mainWindow;
    std::thread([result,repository,window] {
        int code=0;std::wstring tag,url;
        HINTERNET session=WinHttpOpen((std::wstring(L"Onslaught/")+onslaught::Version).c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
        HINTERNET connection=nullptr,request=nullptr;
        if(session) {
            WinHttpSetTimeouts(session,4000,4000,4000,4000);
            connection=WinHttpConnect(session,L"api.github.com",INTERNET_DEFAULT_HTTPS_PORT,0);
            auto path=L"/repos/"+repository+L"/releases/latest";
            if(connection) request=WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
            if(request) {
                DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
                const wchar_t* headers=L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2026-03-10\r\n";
                if(WinHttpSendRequest(request,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request,nullptr)) {
                    DWORD status=0,bytes=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&bytes,WINHTTP_NO_HEADER_INDEX);
                    if(status==404) code=3;else if(status==403 || status==429) code=4;
                    else if(status==200) {
                        std::string json;bool complete=false;ULONGLONG started=GetTickCount64();
                        while(json.size()<2*1024*1024 && GetTickCount64()-started<15000) {
                            char buffer[8192];DWORD read=0;if(!WinHttpReadData(request,buffer,sizeof(buffer),&read)) break;
                            if(!read) {complete=true;break;}json.append(buffer,read);
                        }
                        if(complete) {
                            tag=onslaught::releaseTag(json);int comparison=onslaught::compareVersion(tag,onslaught::Version);
                            if(comparison==-2) code=5;
                            else if(comparison>0) {code=1;url=L"https://github.com/"+repository+L"/releases/tag/"+tag;}
                            else code=2;
                        }
                    }
                }
            }
        }
        if(request) WinHttpCloseHandle(request);if(connection) WinHttpCloseHandle(connection);if(session) WinHttpCloseHandle(session);
        {std::lock_guard<std::mutex> lock(result->mutex);result->code=code;result->tag=tag;result->url=url;}
        PostMessageW(window,WM_APP+30,0,0);
    }).detach();
}
HWND control(int id,const wchar_t* className,const std::wstring& title,DWORD style) {
    HWND h=CreateWindowExW(0,className,title.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,10,10,mainWindow,
        reinterpret_cast<HMENU>(INT_PTR(id)),instance,nullptr);
    controls[id]=h;SetWindowTheme(h,L"DarkMode_Explorer",nullptr);return h;
}
void applyLanguage() {
    for(auto item : {std::pair<int,std::wstring>{Refresh,tr(L"Refresh",L"Atualizar")},
        {Resize,tr(L"Resize window",L"Ajustar janela")},{Start,tr(L"Start stretching",L"Iniciar apresentação")},
        {Stop,tr(L"Stop",L"Parar")},{Logs,tr(L"Logs",L"Registros")},
        {Settings,settingsPage?tr(L"Back",L"Voltar"):tr(L"Settings",L"Configurações")},
        {KeepBackground,tr(L"Keep full image\nwhen using Alt+Tab",L"Manter imagem completa\nno Alt+Tab")},
        {AutoRun,tr(L"Start with Windows",L"Iniciar com o Windows")},
        {CloseTray,tr(L"Close to system tray",L"Fechar para a bandeja")},
        {MinimizeStart,tr(L"Minimize when stretching starts",L"Minimizar ao iniciar a apresentação")},
        {RememberApp,tr(L"Remember last application",L"Lembrar último aplicativo")},
        {SaveHotkey,tr(L"Save shortcut",L"Salvar atalho")},
        {CheckUpdates,tr(L"Check for updates",L"Verificar atualizações")},
        {OpenUpdate,tr(L"View update",L"Ver atualização")},
        {AutoCheck,tr(L"Check on startup",L"Verificar ao iniciar")}})
        SetWindowTextW(controls[item.first],item.second.c_str());
    int input=selected(InputList),fps=selected(FpsList),size=selected(SizeList),monitor=selected(MonitorList);
    addCombo(InputList,{tr(L"Mapped cursor · Apps",L"Cursor mapeado · Apps"),tr(L"Relative mouse · FPS",L"Mouse relativo · FPS"),tr(L"Image only",L"Somente imagem")},std::max(0,input));
    addCombo(FpsList,{tr(L"Automatic · monitor refresh rate",L"Automático · frequência do monitor"),L"60 FPS",L"120 FPS",L"144 FPS",L"165 FPS",L"180 FPS",L"240 FPS"},std::max(0,fps));
    addCombo(SizeList,{tr(L"Keep current size",L"Manter tamanho atual"),L"1680 × 1050",L"1600 × 900",L"1280 × 720",L"1024 × 768",L"800 × 600"},std::max(0,size));
    refreshMonitors();SendMessageW(controls[MonitorList],CB_SETCURSEL,std::max(0,monitor),0);
    if(!running) statusText=tr(L"Select an app to get started.",L"Selecione um aplicativo para começar.");
}
LRESULT CALLBACK windowProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE: {
        mainWindow=h;dpiScale=GetDpiForWindow(h)/96.0f;
        backgroundBrush=CreateSolidBrush(Background);cardBrush=CreateSolidBrush(Card);
        DWORD combo=CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL;
        for(int id : {AppList,MonitorList,InputList,FpsList,SizeList,Language}) control(id,L"COMBOBOX",L"",combo);
        control(Refresh,L"BUTTON",tr(L"Refresh",L"Atualizar"),BS_OWNERDRAW);control(Resize,L"BUTTON",tr(L"Resize window",L"Ajustar janela"),BS_OWNERDRAW);
        control(Start,L"BUTTON",tr(L"Start stretching",L"Iniciar apresentação"),BS_OWNERDRAW);control(Stop,L"BUTTON",tr(L"Stop",L"Parar"),BS_OWNERDRAW);
        control(Logs,L"BUTTON",tr(L"Logs",L"Registros"),BS_OWNERDRAW);
        control(KeepBackground,L"BUTTON",tr(L"Keep full image\non Alt+Tab",L"Manter imagem completa\nno Alt+Tab"),BS_AUTOCHECKBOX|BS_MULTILINE);
        control(Settings,L"BUTTON",L"Settings",BS_OWNERDRAW);
        control(AutoRun,L"BUTTON",L"Start with Windows",BS_AUTOCHECKBOX);
        control(CloseTray,L"BUTTON",L"Close to system tray",BS_AUTOCHECKBOX);
        control(MinimizeStart,L"BUTTON",L"Minimize when stretching starts",BS_AUTOCHECKBOX);
        addCombo(Language,{L"English",L"Português (Brasil)"},portuguese?1:0);
        HKEY key{};DWORD bytes=0;
        bool startup=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS;
        if(startup) {startup=RegQueryValueExW(key,L"Onslaught",nullptr,nullptr,nullptr,&bytes)==ERROR_SUCCESS;RegCloseKey(key);}
        SendMessageW(controls[AutoRun],BM_SETCHECK,startup?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(controls[CloseTray],BM_SETCHECK,GetPrivateProfileIntW(L"interface",L"close_tray",1,settingsPath.c_str())?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(controls[MinimizeStart],BM_SETCHECK,GetPrivateProfileIntW(L"interface",L"minimize_start",1,settingsPath.c_str())?BST_CHECKED:BST_UNCHECKED,0);
        control(RememberApp,L"BUTTON",L"Remember last application",BS_AUTOCHECKBOX);
        control(AutoCheck,L"BUTTON",L"Check on startup",BS_AUTOCHECKBOX|BS_MULTILINE);
        control(EmergencyHotkey,HOTKEY_CLASSW,L"",0);
        SetWindowSubclass(controls[EmergencyHotkey],centeredHotkey,1,0);
        SetWindowTheme(controls[EmergencyHotkey],L"",L"");
        SetWindowLongPtrW(controls[EmergencyHotkey],GWL_EXSTYLE,GetWindowLongPtrW(controls[EmergencyHotkey],GWL_EXSTYLE)&~(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE));
        SetWindowPos(controls[EmergencyHotkey],nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        WORD hotmods=WORD((emergencyModifiers&MOD_CONTROL?HOTKEYF_CONTROL:0)|(emergencyModifiers&MOD_ALT?HOTKEYF_ALT:0)|(emergencyModifiers&MOD_SHIFT?HOTKEYF_SHIFT:0));
        SendMessageW(controls[EmergencyHotkey],HKM_SETHOTKEY,MAKEWORD(BYTE(emergencyKey),BYTE(hotmods)),0);
        control(SaveHotkey,L"BUTTON",L"Save shortcut",BS_OWNERDRAW);
    
        control(CheckUpdates,L"BUTTON",L"Check for updates",BS_OWNERDRAW);control(OpenUpdate,L"BUTTON",L"View update",BS_OWNERDRAW);EnableWindow(controls[OpenUpdate],FALSE);
        SendMessageW(controls[RememberApp],BM_SETCHECK,GetPrivateProfileIntW(L"interface",L"remember_app",1,settingsPath.c_str())?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(controls[AutoCheck],BM_SETCHECK,GetPrivateProfileIntW(L"interface",L"auto_check",0,settingsPath.c_str())?BST_CHECKED:BST_UNCHECKED,0);
        fonts();layout();refreshTargets();refreshMonitors();
        addCombo(InputList,{tr(L"Mapped cursor · Apps",L"Cursor mapeado · Apps"),tr(L"Relative mouse · FPS",L"Mouse relativo · FPS"),tr(L"Image only",L"Somente imagem")},GetPrivateProfileIntW(L"interface",L"input",0,settingsPath.c_str()));
        addCombo(FpsList,{tr(L"Automatic · monitor refresh rate",L"Automático · frequência do monitor"),L"60 FPS",L"120 FPS",L"144 FPS",L"165 FPS",L"180 FPS",L"240 FPS"},GetPrivateProfileIntW(L"interface",L"fps",0,settingsPath.c_str()));
        addCombo(SizeList,{tr(L"Keep current size",L"Manter tamanho atual"),L"1680 × 1050",L"1600 × 900",L"1280 × 720",L"1024 × 768",L"800 × 600"},GetPrivateProfileIntW(L"interface",L"size",0,settingsPath.c_str()));
        SendMessageW(controls[KeepBackground],BM_SETCHECK,GetPrivateProfileIntW(L"interface",L"background",1,settingsPath.c_str())?BST_CHECKED:BST_UNCHECKED,0);
        applyLanguage();statusText=tr(L"Select an app to get started.",L"Selecione um aplicativo para começar.");
        enableControls();SetTimer(h,1,1500,nullptr);
        BOOL dark=TRUE;DwmSetWindowAttribute(h,20,&dark,sizeof(dark));
        break;
    }
    case WM_MEASUREITEM: {auto* item=reinterpret_cast<MEASUREITEMSTRUCT*>(l);item->itemHeight=UINT(scale(30));return TRUE;}
    case WM_DRAWITEM: {
        auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(l);HDC dc=item->hDC;
        bool disabled=(item->itemState&ODS_DISABLED)!=0;
        bool primary=item->CtlID==Start;
        bool pressed=(item->itemState&ODS_SELECTED)!=0;
        COLORREF fill=primary && !disabled?(pressed?RGB(205,205,205):Text):(pressed?RGB(42,42,42):Card);
        HBRUSH brush=CreateSolidBrush(item->CtlType==ODT_COMBOBOX && pressed?RGB(55,55,55):fill);
        FillRect(dc,&item->rcItem,item->CtlType==ODT_BUTTON?backgroundBrush:cardBrush);
        if(item->CtlType==ODT_BUTTON) {
            HPEN pen=CreatePen(PS_SOLID,1,primary && !disabled?Text:Line);
            auto previousBrush=SelectObject(dc,brush),previousPen=SelectObject(dc,pen);
            RoundRect(dc,item->rcItem.left,item->rcItem.top,item->rcItem.right,item->rcItem.bottom,scale(14),scale(14));
            SelectObject(dc,previousBrush);SelectObject(dc,previousPen);DeleteObject(pen);
        } else FillRect(dc,&item->rcItem,brush);
        DeleteObject(brush);
        std::wstring text;
        if(item->CtlType==ODT_COMBOBOX && item->itemID!=UINT(-1)) {
            int n=int(SendMessageW(item->hwndItem,CB_GETLBTEXTLEN,item->itemID,0));
            text.resize(n+1);SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,reinterpret_cast<LPARAM>(text.data()));text.resize(n);
        } else text=label(item->hwndItem);
        SelectObject(dc,normalFont);SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,disabled?Muted:primary?Background:Text);
        RECT r=item->rcItem;r.left+=scale(10);r.right-=scale(8);
        DrawTextW(dc,text.c_str(),int(text.size()),&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|(item->CtlType==ODT_BUTTON?DT_CENTER:DT_LEFT));
        if(item->itemState&ODS_FOCUS) {RECT focus=item->rcItem;InflateRect(&focus,-scale(4),-scale(4));DrawFocusRect(dc,&focus);}return TRUE;
    }
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLORLISTBOX:case WM_CTLCOLOREDIT: {
        SetTextColor(reinterpret_cast<HDC>(w),Text);SetBkColor(reinterpret_cast<HDC>(w),Card);return reinterpret_cast<LRESULT>(cardBrush);
    }
    case WM_COMMAND: {
        int id=LOWORD(w);
        if(id==SaveHotkey && HIWORD(w)==BN_CLICKED) {
            WORD value=WORD(SendMessageW(controls[EmergencyHotkey],HKM_GETHOTKEY,0,0));UINT key=LOBYTE(value),mods=HIBYTE(value);
            UINT flags=(mods&HOTKEYF_CONTROL?MOD_CONTROL:0)|(mods&HOTKEYF_ALT?MOD_ALT:0)|(mods&HOTKEYF_SHIFT?MOD_SHIFT:0);
            bool valid=key>0 && flags && !(key==VK_F11 && flags==(MOD_CONTROL|MOD_ALT));
            if(valid) {valid=RegisterHotKey(h,800,flags|MOD_NOREPEAT,key)!=FALSE;if(valid) UnregisterHotKey(h,800);}
            if(!valid) MessageBoxW(h,tr(L"Use Ctrl, Alt or Shift with a free key. Ctrl+Alt+F11 is reserved.",L"Use Ctrl, Alt ou Shift com uma tecla livre. Ctrl+Alt+F11 está reservado.").c_str(),L"Onslaught",MB_OK|MB_ICONWARNING);
            else {emergencyKey=key;emergencyModifiers=flags;saveSettings();InvalidateRect(h,nullptr,FALSE);}
        }
        else if(id==CheckUpdates && HIWORD(w)==BN_CLICKED) checkForUpdates();
        else if(id==OpenUpdate && HIWORD(w)==BN_CLICKED && !releaseUrl.empty()) ShellExecuteW(h,L"open",releaseUrl.c_str(),nullptr,nullptr,SW_SHOWNORMAL);

        else if(id==Settings && HIWORD(w)==BN_CLICKED) {settingsPage=!settingsPage;applyLanguage();layout();InvalidateRect(h,nullptr,FALSE);}
        else if(id==Language && HIWORD(w)==CBN_SELCHANGE) {portuguese=selected(Language)==1;applyLanguage();saveSettings();InvalidateRect(h,nullptr,FALSE);}
        else if(id==AutoRun && HIWORD(w)==BN_CLICKED) {
            HKEY key{};LONG result=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
            if(result==ERROR_SUCCESS) {
                if(SendMessageW(controls[AutoRun],BM_GETCHECK,0,0)==BST_CHECKED) {
                    auto command=L"\""+appDirectory+L"\\Onslaught.exe\" --tray";
                    result=RegSetValueExW(key,L"Onslaught",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t)));
                } else {result=RegDeleteValueW(key,L"Onslaught");if(result==ERROR_FILE_NOT_FOUND) result=ERROR_SUCCESS;}
                RegCloseKey(key);
            }
            if(result!=ERROR_SUCCESS) {SendMessageW(controls[AutoRun],BM_SETCHECK,BST_UNCHECKED,0);MessageBoxW(h,tr(L"Could not change the startup preference.",L"Não foi possível alterar a preferência de inicialização.").c_str(),L"Onslaught",MB_OK|MB_ICONERROR);}
        }
        else if((id==CloseTray || id==MinimizeStart || id==RememberApp || id==AutoCheck) && HIWORD(w)==BN_CLICKED) saveSettings();
        else if(id==Refresh && HIWORD(w)==BN_CLICKED) {refreshTargets();refreshMonitors();setStatus(tr(L"Window list refreshed.",L"Lista de janelas atualizada."));}
        else if(id==Start && HIWORD(w)==BN_CLICKED) startEngine();
        else if(id==Stop && HIWORD(w)==BN_CLICKED) stopEngine();
        else if(id==Resize && HIWORD(w)==BN_CLICKED) resizeTarget();
        else if(id==Logs && HIWORD(w)==BN_CLICKED) {
            auto path=appDirectory+L"\\logs";CreateDirectoryW(path.c_str(),nullptr);
            ShellExecuteW(h,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        } else if(HIWORD(w)==CBN_SELCHANGE || (id==KeepBackground && HIWORD(w)==BN_CLICKED)) {saveSettings();InvalidateRect(h,nullptr,FALSE);}
        return 0;
    }
    case WM_APP+30: {
        checkingUpdate=false;EnableWindow(controls[CheckUpdates],TRUE);
        if(updateResult) {std::lock_guard<std::mutex> lock(updateResult->mutex);
            switch(updateResult->code) {
            case 1: updateStatus=tr(L"Update available: ",L"Atualização disponível: ")+updateResult->tag;releaseUrl=updateResult->url;break;
            case 2: updateStatus=tr(L"You are up to date.",L"Você está atualizado.");break;
            case 3: updateStatus=tr(L"No update published yet.",L"Nenhuma atualização publicada ainda.");break;
            case 4: updateStatus=tr(L"GitHub rate limit reached. Try later.",L"Limite do GitHub atingido. Tente mais tarde.");break;
            case 5: updateStatus=tr(L"Unsupported tag. Use vMAJOR.MINOR.PATCH.",L"Tag não suportada. Use vMAJOR.MINOR.PATCH.");break;
            default:updateStatus=tr(L"Could not check updates. Try again later.",L"Não foi possível verificar. Tente mais tarde.");
            }
        }
        EnableWindow(controls[OpenUpdate],!releaseUrl.empty());InvalidateRect(h,nullptr,FALSE);return 0;
    }
    case WM_APP+31:checkForUpdates();return 0;
    case WM_APP+10: {
        DWORD code=0;if(engineProcess) GetExitCodeProcess(engineProcess,&code);
        cleanupEngine();running=false;enableControls();
        std::ifstream file(sessionDirectory+L"\\diagnostico.txt",std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(file)),{});auto error=data.rfind("ERROR=");
        if(error!=std::string::npos) setStatus(wide(data.substr(error,data.find('\n',error)-error)));
        else setStatus(code==71?tr(L"Watchdog stopped the session. Check the logs.",L"Sessão encerrada pelo watchdog. Consulte os registros."):tr(L"Session ended. Select an app to start again.",L"Sessão encerrada. Escolha uma janela para iniciar novamente."));
        if(!quitting) ShowWindow(h,SW_RESTORE);
        return 0;
    }
    case TrayMessage:
        if(l==WM_LBUTTONDBLCLK) {ShowWindow(h,SW_RESTORE);SetForegroundWindow(h);}
        else if(l==WM_RBUTTONUP) {
            HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,tr(L"Open Onslaught",L"Abrir Onslaught").c_str());
            AppendMenuW(menu,MF_STRING|(running?0:MF_GRAYED),2,tr(L"Stop stretching",L"Parar apresentação").c_str());
            AppendMenuW(menu,MF_STRING,3,tr(L"Quit",L"Sair").c_str());
            POINT point{};GetCursorPos(&point);SetForegroundWindow(h);
            int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,h,nullptr);DestroyMenu(menu);
            if(choice==1) ShowWindow(h,SW_RESTORE);else if(choice==2) stopEngine();else if(choice==3) {quitting=true;SendMessageW(h,WM_CLOSE,0,0);}
        } return 0;
    case WM_TIMER:readMetrics();return 0;
    case WM_SHOWWINDOW:if(w && !IsIconic(h)) SetTimer(h,1,1500,nullptr);else KillTimer(h,1);break;
    case WM_SIZE:
        if(w==SIZE_MINIMIZED) KillTimer(h,1);
        else {SetTimer(h,1,1500,nullptr);layout();InvalidateRect(h,nullptr,FALSE);}
        return 0;
    case WM_DPICHANGED: {
        dpiScale=HIWORD(w)/96.0f;auto r=reinterpret_cast<RECT*>(l);
        SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        fonts();layout();return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={scale(920),scale(704)};return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:paint();return 0;
    case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(w));return 0;
    case WM_CLOSE:if(!quitting && trayAvailable && SendMessageW(controls[CloseTray],BM_GETCHECK,0,0)==BST_CHECKED) {saveSettings();KillTimer(h,1);ShowWindow(h,SW_HIDE);return 0;} quitting=true;saveSettings();stopEngine();cleanupEngine();DestroyWindow(h);return 0;
    case WM_DESTROY: {NOTIFYICONDATAW tray{sizeof(tray)};tray.hWnd=h;tray.uID=1;Shell_NotifyIconW(NIM_DELETE,&tray);} KillTimer(h,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,message,w,l);
}
int WINAPI wWinMain(HINSTANCE module,HINSTANCE,LPWSTR arguments,int show) {
    const std::wstring args=arguments;
    const bool verification=args.find(L"--preview")!=std::wstring::npos || args.find(L"--ui-self-test")!=std::wstring::npos || args.find(L"--ui-test")!=std::wstring::npos;
    std::unique_ptr<onslaught::InstanceLock> instanceLock;
    if(!verification) {
        instanceLock=std::make_unique<onslaught::InstanceLock>(L"Local\\Onslaught.UI",true);
        if(instanceLock->alreadyRunning) {
            HICON noticeIcon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,64,64,0));
            TASKDIALOGCONFIG notice{sizeof(notice)};notice.dwFlags=TDF_USE_HICON_MAIN;notice.dwCommonButtons=TDCBF_OK_BUTTON;
            notice.hMainIcon=noticeIcon;notice.pszWindowTitle=L"Onslaught";notice.pszMainInstruction=L"Onslaught is already running";
            notice.pszContent=L"Check the system tray to open it.";
            if(FAILED(TaskDialogIndirect(&notice,nullptr,nullptr,nullptr))) MessageBoxW(nullptr,notice.pszContent,notice.pszMainInstruction,MB_OK);
            if(noticeIcon) DestroyIcon(noticeIcon);
            return 0;
        }
        if(!instanceLock->valid()) {
            MessageBoxW(nullptr,L"Onslaught could not initialize. Please try again.",L"Onslaught",MB_OK|MB_ICONERROR);return 1;
        }
    }
    instance=module;uiTest=std::wstring(arguments).find(L"--ui-test")!=std::wstring::npos;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    appDirectory=executable;appDirectory=appDirectory.substr(0,appDirectory.find_last_of(L"\\/"));settingsPath=appDirectory+L"\\settings.ini";
    lastExecutable=profileString(L"last_executable");lastTitle=profileString(L"last_title");
    emergencyKey=GetPrivateProfileIntW(L"interface",L"emergency_key",VK_F10,settingsPath.c_str());
    emergencyModifiers=GetPrivateProfileIntW(L"interface",L"emergency_modifiers",MOD_CONTROL|MOD_ALT,settingsPath.c_str());
    if(emergencyKey<1 || emergencyKey>254 || !emergencyModifiers || emergencyModifiers>7 || (emergencyKey==VK_F11 && emergencyModifiers==(MOD_CONTROL|MOD_ALT))) {emergencyKey=VK_F10;emergencyModifiers=MOD_CONTROL|MOD_ALT;}
    wchar_t language[32]{};GetPrivateProfileStringW(L"interface",L"language",L"en",language,32,settingsPath.c_str());portuguese=std::wstring(language)==L"pt-BR";
    GdiplusStartupInput startup{};GdiplusStartup(&gdiplusToken,&startup,nullptr);
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(102),RT_RCDATA);
    if(resource) {
        DWORD size=SizeofResource(instance,resource);HGLOBAL raw=LoadResource(instance,resource),memory=GlobalAlloc(GMEM_MOVEABLE,size);
        if(memory) {void* ptr=GlobalLock(memory);std::memcpy(ptr,LockResource(raw),size);GlobalUnlock(memory);
            if(SUCCEEDED(CreateStreamOnHGlobal(memory,TRUE,&iconStream))) iconImage.reset(Image::FromStream(iconStream));else GlobalFree(memory);}
    }
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&common);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"OnslaughtInterface";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));RegisterClassW(&wc);
    RECT rect{0,0,960,662};AdjustWindowRectEx(&rect,WS_OVERLAPPEDWINDOW,FALSE,0);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Onslaught",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,
        rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);
    if(!window) return 1;
    bigIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),0));
    smallIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0));
    SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(bigIcon));SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(smallIcon));
    NOTIFYICONDATAW tray{sizeof(tray)};tray.hWnd=window;tray.uID=1;tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=TrayMessage;tray.hIcon=smallIcon;wcscpy_s(tray.szTip,L"Onslaught");trayAvailable=Shell_NotifyIconW(NIM_ADD,&tray)!=FALSE;
    ShowWindow(window,std::wstring(arguments).find(L"--tray")!=std::wstring::npos?SW_HIDE:show);UpdateWindow(window);
    if(std::wstring(arguments).find(L"--ui-self-test")!=std::wstring::npos) {
        bool passed=selected(InputList)>=0 && selected(FpsList)>=0 && selected(Language)>=0 && IsWindow(controls[EmergencyHotkey]) && !IsWindow(controls[Repository]);
        WORD shortcut=WORD(SendMessageW(controls[EmergencyHotkey],HKM_GETHOTKEY,0,0));passed=passed && LOBYTE(shortcut)==emergencyKey;
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(Settings,BN_CLICKED),0);
        passed=passed && settingsPage && !(GetWindowLongPtrW(controls[Start],GWL_STYLE)&WS_VISIBLE) && (GetWindowLongPtrW(controls[Language],GWL_STYLE)&WS_VISIBLE);
        for(int choice : {1,0}) {
            SendMessageW(controls[Language],CB_SETCURSEL,choice,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(Language,CBN_SELCHANGE),0);
            passed=passed && portuguese==(choice==1) && label(controls[AutoRun])==(choice?L"Iniciar com o Windows":L"Start with Windows");
        }
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(Settings,BN_CLICKED),0);
        passed=passed && !settingsPage && (GetWindowLongPtrW(controls[Start],GWL_STYLE)&WS_VISIBLE);
        RECT button{},client{};GetWindowRect(controls[Resize],&button);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2);GetClientRect(window,&client);
        passed=passed && button.right<=scale(contentWidth-390+32-20) && button.bottom<scale(437);
        GetWindowRect(controls[Refresh],&button);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2);
        passed=passed && button.right<=scale(contentWidth-390+32-20) && button.left>=scale(52);
        ShowWindow(window,SW_MINIMIZE);passed=passed && IsIconic(window);ShowWindow(window,SW_RESTORE);
        DestroyWindow(window);iconImage.reset();if(iconStream) iconStream->Release();GdiplusShutdown(gdiplusToken);
        return passed?0:1;
    }
    if(std::wstring(arguments).find(L"--settings-preview")!=std::wstring::npos) {settingsPage=true;applyLanguage();layout();}
    if(std::wstring(arguments).find(L"--preview")!=std::wstring::npos) {
        RECT client{};GetClientRect(window,&client);
        HDC screen=GetDC(window),dc=CreateCompatibleDC(screen);
        HBITMAP bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);auto old=SelectObject(dc,bitmap);
        SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
        for(auto child:controls) if(child && (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)) {
            RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
            int saved=SaveDC(dc);SetViewportOrgEx(dc,r.left,r.top,nullptr);
            SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
            int id=GetDlgCtrlID(child);
            if(id==AppList || id==MonitorList || id==InputList || id==FpsList || id==SizeList || id==Language) {
                COMBOBOXINFO info{sizeof(info)};GetComboBoxInfo(child,&info);
                DRAWITEMSTRUCT item{};item.CtlType=ODT_COMBOBOX;item.CtlID=UINT(id);item.itemID=UINT(selected(id));item.hwndItem=child;item.hDC=dc;item.rcItem=info.rcItem;
                SendMessageW(window,WM_DRAWITEM,id,reinterpret_cast<LPARAM>(&item));
            }
            if(id==Repository || id==EmergencyHotkey) {
                RECT field{0,0,r.right-r.left,r.bottom-r.top};FillRect(dc,&field,cardBrush);
                HPEN pen=CreatePen(PS_SOLID,1,Line);auto previousPen=SelectObject(dc,pen);auto previousBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
                Rectangle(dc,0,0,field.right,field.bottom);SelectObject(dc,previousPen);SelectObject(dc,previousBrush);DeleteObject(pen);
                std::wstring text=id==EmergencyHotkey?emergencyLabel():label(child);SelectObject(dc,normalFont);SetTextColor(dc,Text);SetBkMode(dc,TRANSPARENT);field.left+=scale(10);
                DrawTextW(dc,text.c_str(),int(text.size()),&field,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_LEFT);
            }
            RestoreDC(dc,saved);
        }
        UINT count=0,bytes=0;GetImageEncodersSize(&count,&bytes);std::vector<BYTE> buffer(bytes);
        auto encoders=reinterpret_cast<ImageCodecInfo*>(buffer.data());GetImageEncoders(count,bytes,encoders);
        int result=1;
        for(UINT n=0;n<count;++n) if(std::wstring(encoders[n].MimeType)==L"image/png") {
            Bitmap image(bitmap,nullptr);
            result=image.Save((appDirectory+L"\\Onslaught-preview.png").c_str(),&encoders[n].Clsid)==Ok ? 0 : 1;break;
        }
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
        DestroyWindow(window);iconImage.reset();if(iconStream) iconStream->Release();GdiplusShutdown(gdiplusToken);return result;
    }
    if(!uiTest && GetPrivateProfileIntW(L"interface",L"auto_check",0,settingsPath.c_str())) PostMessageW(window,WM_APP+31,0,0);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) {
        if(!IsDialogMessageW(window,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}
    }
    iconImage.reset();if(iconStream) iconStream->Release();GdiplusShutdown(gdiplusToken);
    for(auto font:{normalFont,smallFont,titleFont,headingFont}) if(font) DeleteObject(font);
    DeleteObject(backgroundBrush);DeleteObject(cardBrush);return 0;
}





