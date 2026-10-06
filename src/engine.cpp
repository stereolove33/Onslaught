#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <chrono>
#include <mmsystem.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <atomic>
#include "mapping.h"
#include "cursor_guard.h"
#include "cursor_image.h"

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr ULONG_PTR InputTag = 0x4C535452;
HWND source{}, overlay{};
HHOOK mouseHook{};
bool active=false, stopped=false;
bool paused=false;
HWND backgroundForeground{};
bool remapEnabled=true;
bool cursorDirty=false;
unsigned long long rawEvents=0, capturedFrames=0;
unsigned long long presentedFrames=0,presentBusy=0;
double monitorHz=180;
bool borderConsent=false;
HWND requestedWindow{};
std::wstring requestedMonitor;
double requestedFps=0;
bool hideOnBlur=false;
int engineExitCode=0;
extern std::ofstream diagnostics;

void requestBorderless() {
    using winrt::Windows::Foundation::Metadata::ApiInformation;
    using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
    if(!ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess")) {
        diagnostics<<"BORDERLESS_UNSUPPORTED\n";return;
    }
    try {
        std::wcout<<L"Solicitando captura sem borda ao Windows (pode aparecer uma permissao)...\n";
        auto status=GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
        borderConsent=status==AppCapabilityAccessStatus::Allowed;
        diagnostics<<"BORDERLESS_ACCESS_STATUS="<<int(status)<<" allowed="<<borderConsent<<"\n";
        std::wcout<<(borderConsent ? L"Windows permitiu captura sem borda.\n" :
            L"Windows nao permitiu captura sem borda neste pacote portatil.\n");
    } catch(hresult_error const& e) {
        engineExitCode=1;
        diagnostics<<"BORDERLESS_ERROR="<<unsigned(e.code())<<"\n";
        std::wcout<<L"Permissao sem borda indisponivel; resultado registrado.\n";
    }
    diagnostics.flush();
}
double minCursorX=960,maxCursorX=960,minCursorY=540,maxCursorY=540;
std::ofstream diagnostics;
CursorGuard cursorGuard;
std::atomic<bool> emergencyRunning{true},watchdogArmed{false};
std::atomic<ULONGLONG> mainHeartbeat{0};
std::atomic<HWND> watchedSource{nullptr};
std::atomic<DWORD> emergencyError{0};
std::atomic<bool> emergencyF12{false},emergencyF10{false};
HANDLE emergencyReady{},emergencyThread{};
UINT emergencyKey=VK_F10,emergencyModifiers=MOD_CONTROL|MOD_ALT;

DWORD WINAPI emergencyProc(void*) {
    // Independent message queue: no D3D/WGC calls, app locks or UI callbacks.
    emergencyF12=RegisterHotKey(nullptr,100,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F12)!=FALSE;
    emergencyF10=emergencyKey==VK_F12 && emergencyModifiers==(MOD_CONTROL|MOD_ALT) ? emergencyF12.load() : RegisterHotKey(nullptr,101,emergencyModifiers|MOD_NOREPEAT,emergencyKey)!=FALSE;
    const bool registered=emergencyF10.load() && emergencyF12.load();
    if(!registered) emergencyError=ERROR_HOTKEY_ALREADY_REGISTERED;
    SetEvent(emergencyReady);
    if(!registered) return 1;
    while(emergencyRunning.load()) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_HOTKEY && (message.wParam==100||message.wParam==101))
                TerminateProcess(GetCurrentProcess(),70);
        }
        if(watchdogArmed.load()) {
            HWND target=watchedSource.load();
            if((target && !IsWindow(target)) || GetTickCount64()-mainHeartbeat.load()>5000)
                TerminateProcess(GetCurrentProcess(),71);
        }
        MsgWaitForMultipleObjectsEx(0,nullptr,250,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    if(emergencyF12.load()) UnregisterHotKey(nullptr,100);
    if(emergencyF10.load() && !(emergencyKey==VK_F12 && emergencyModifiers==(MOD_CONTROL|MOD_ALT))) UnregisterHotKey(nullptr,101);
    return 0;
}
double cursorX=960, cursorY=540;
RECT monitorRect{}, originalClient{}, captureBounds{};
POINT clientOrigin{};
int outputW{},outputH{};
std::wstring stopReason;

void stop(std::wstring reason) {
    if(stopped) return;
    active=false; stopped=true; stopReason=std::move(reason);
    cursorGuard.hide(false);
    if(mouseHook) { UnhookWindowsHookEx(mouseHook); mouseHook=nullptr; }
    if(overlay) ShowWindow(overlay,SW_HIDE);
    // Never alter global cursor visibility, confinement, resolution or source styles.
    PostQuitMessage(0);
}

void suspendPresentation() {
    active=false;cursorDirty=false;paused=true;
    cursorGuard.hide(false);
    if(mouseHook) {UnhookWindowsHookEx(mouseHook);mouseHook=nullptr;}
    // Keep stretched presentation below the user's foreground app. Never
    // activate either the presentation or the source while Alt+Tabbed away.
    if(overlay) {
        if(hideOnBlur) {ShowWindow(overlay,SW_HIDE);return;}
        SetWindowPos(overlay,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        backgroundForeground=GetForegroundWindow();
        if(backgroundForeground && backgroundForeground!=source && backgroundForeground!=overlay)
            SetWindowPos(overlay,backgroundForeground,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
}

bool mapCursor() {
    POINT p{mapPixel(cursorX,outputW,originalClient.right),
            mapPixel(cursorY,outputH,originalClient.bottom)};
    p.x+=clientOrigin.x; p.y+=clientOrigin.y;
    const int vx=GetSystemMetrics(SM_XVIRTUALSCREEN),vy=GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw=GetSystemMetrics(SM_CXVIRTUALSCREEN),vh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT i{}; i.type=INPUT_MOUSE;
    i.mi.dx=mapPixel(p.x-vx,vw,65536); i.mi.dy=mapPixel(p.y-vy,vh,65536);
    i.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    i.mi.dwExtraInfo=InputTag;
    return SendInput(1,&i,sizeof(i))==1;
}

LRESULT CALLBACK mouseProc(int code,WPARAM w,LPARAM l) {
    if(code>=0 && active && remapEnabled) {
        auto* m=reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        if(GetForegroundWindow()!=source) {
            active=false; PostMessageW(overlay,WM_APP+1,0,0);
        } else if(m->dwExtraInfo!=InputTag && w==WM_MOUSEMOVE) {
            // Physical deltas are consumed via WM_INPUT, not via warped screen coordinates.
            return 1;
        }
    }
    return CallNextHookEx(mouseHook,code,w,l);
}

LRESULT CALLBACK overlayProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_NCHITTEST) return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(msg==WM_HOTKEY && w==2) {
        remapEnabled=false;cursorDirty=false;
        cursorGuard.hide(false);
        if(mouseHook) {UnhookWindowsHookEx(mouseHook);mouseHook=nullptr;}
        if(diagnostics) {diagnostics<<"INPUT_DISABLED_BY_HOTKEY\n";diagnostics.flush();}
        return 0;
    }
    if(msg==WM_HOTKEY || msg==WM_CLOSE) {stop(L"Encerrado pelo usuario.");return 0;}
    if(msg==WM_APP+1) {suspendPresentation();return 0;}
    if(msg==WM_INPUT && active) {
        UINT size=0;
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER))!=0)
            return DefWindowProcW(hwnd,msg,w,l);
        std::vector<BYTE> bytes(size);
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,bytes.data(),&size,sizeof(RAWINPUTHEADER))==size) {
            auto* input=reinterpret_cast<RAWINPUT*>(bytes.data());
            if(input->header.dwType==RIM_TYPEMOUSE && input->header.hDevice &&
               input->data.mouse.ulExtraInformation!=InputTag &&
               !(input->data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)) {
                ++rawEvents;
                cursorX=std::clamp(cursorX+input->data.mouse.lLastX,0.0,double(outputW-1));
                cursorY=std::clamp(cursorY+input->data.mouse.lLastY,0.0,double(outputH-1));
                minCursorX=std::min(minCursorX,cursorX);maxCursorX=std::max(maxCursorX,cursorX);
                minCursorY=std::min(minCursorY,cursorY);maxCursorY=std::max(maxCursorY,cursorY);
                cursorDirty=true;
            }
        }
    }
    return DefWindowProcW(hwnd,msg,w,l);
}

struct Renderer {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain1> swap;
    com_ptr<ID2D1Factory1> factory;
    com_ptr<ID2D1Device> d2device;
    com_ptr<ID2D1DeviceContext> draw;
    com_ptr<ID2D1Bitmap1> target;
    com_ptr<ID2D1SolidColorBrush> brush;
    com_ptr<ID2D1SolidColorBrush> pointerWhite,pointerBlack;
    com_ptr<ID2D1PathGeometry> pointerShape;
    com_ptr<ID2D1Bitmap1> nativePointer;
    HCURSOR lastPointerHandle{};
    CursorPixels nativePixels;
    ULONGLONG pointerRetry{};
    IDirect3DDevice runtimeDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    com_ptr<ID3D11Texture2D> image;
    com_ptr<ID2D1Bitmap1> bitmap;
    winrt::Windows::Graphics::SizeInt32 size{};
    std::chrono::steady_clock::time_point nextPresent{};
    ULONGLONG lastFrame=GetTickCount64();
    ~Renderer() {
        try { if(session) session.Close(); } catch(...) {}
        try { if(pool) pool.Close(); } catch(...) {}
    }
    void init() {
        check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
            device.put(),nullptr,context.put()));
        auto dxgi=device.as<IDXGIDevice>();
        check_hresult(dxgi.as<IDXGIDevice1>()->SetMaximumFrameLatency(1));
        com_ptr<::IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        runtimeDevice=inspectable.as<IDirect3DDevice>();
        com_ptr<IDXGIAdapter> adapter; check_hresult(dxgi->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> dxFactory; check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),dxFactory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width=outputW;desc.Height=outputH;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // Blt-model presentation keeps the layered HWND click-through path simple.
        desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;
        desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        check_hresult(dxFactory->CreateSwapChainForHwnd(device.get(),overlay,&desc,nullptr,nullptr,swap.put()));
        check_hresult(dxFactory->MakeWindowAssociation(overlay,DXGI_MWA_NO_ALT_ENTER));
        D2D1_FACTORY_OPTIONS options{};
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,factory.put_void()));
        check_hresult(factory->CreateDevice(dxgi.get(),d2device.put()));
        check_hresult(d2device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,draw.put()));
        com_ptr<IDXGISurface> surface;
        check_hresult(swap->GetBuffer(0,__uuidof(IDXGISurface),surface.put_void()));
        auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_IGNORE),96,96);
        check_hresult(draw->CreateBitmapFromDxgiSurface(surface.get(),&props,target.put()));
        draw->SetTarget(target.get());
        check_hresult(draw->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Lime),brush.put()));
        check_hresult(draw->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),pointerWhite.put()));
        check_hresult(draw->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),pointerBlack.put()));
        check_hresult(factory->CreatePathGeometry(pointerShape.put()));
        com_ptr<ID2D1GeometrySink> sink;check_hresult(pointerShape->Open(sink.put()));
        sink->BeginFigure(D2D1::Point2F(0,0),D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F points[]={{0,23},{6,17},{11,28},{15,26},{10,15},{19,15}};
        sink->AddLines(points,6);sink->EndFigure(D2D1_FIGURE_END_CLOSED);check_hresult(sink->Close());
        startCapture();
    }
    void pauseCapture(bool keepImage=false) {
        if(session) {session.Close();session=nullptr;}
        if(pool) {pool.Close();pool=nullptr;}
        item=nullptr;
        if(!keepImage) {bitmap=nullptr;image=nullptr;}
    }
    void startCapture() {
        diagnostics<<"CAPTURE_SESSION_START\n";diagnostics.flush();
        auto interop=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(source,guid_of<GraphicsCaptureItem>(),put_abi(item)));
        size=item.Size();
        pool=Direct3D11CaptureFramePool::CreateFreeThreaded(runtimeDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
        session=pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        if(borderConsent && winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",L"IsBorderRequired"))
            session.IsBorderRequired(false);
        if(winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",L"MinUpdateInterval"))
            session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{0});
        session.StartCapture();
        lastFrame=GetTickCount64();
    }
    void updatePointer() {
        CURSORINFO cursorInfo{};cursorInfo.cbSize=sizeof(cursorInfo);
        if(!GetCursorInfo(&cursorInfo) || !cursorInfo.hCursor) return;
        if(cursorInfo.hCursor==lastPointerHandle && GetTickCount64()<pointerRetry) return;
        lastPointerHandle=cursorInfo.hCursor;pointerRetry=GetTickCount64()+1000;
        CursorPixels fresh;
        if(!rasterCursor(cursorInfo.hCursor,fresh)) {
            nativePointer=nullptr;
            diagnostics<<"CURSOR_NATIVE_UNAVAILABLE fallback=arrow\n";
            return;
        }
        // Games may change color/state without changing the cursor handle.
        // Poll its pixels at up to 60 Hz; upload only when the image changes.
        pointerRetry=GetTickCount64()+16;
        if(nativePointer && fresh.width==nativePixels.width && fresh.height==nativePixels.height &&
           fresh.hotspotX==nativePixels.hotspotX && fresh.hotspotY==nativePixels.hotspotY &&
           fresh.bgra==nativePixels.bgra) return;
        auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        com_ptr<ID2D1Bitmap1> freshBitmap;
        check_hresult(draw->CreateBitmap(D2D1::SizeU(UINT32(fresh.width),UINT32(fresh.height)),
            fresh.bgra.data(),UINT32(fresh.width*4),&props,freshBitmap.put()));
        nativePointer=std::move(freshBitmap);nativePixels=std::move(fresh);
        diagnostics<<"CURSOR_NATIVE_LOADED size="<<nativePixels.width<<"x"<<nativePixels.height
            <<" hotspot="<<nativePixels.hotspotX<<","<<nativePixels.hotspotY<<"\n";
    }
    void tick() {
        const auto now=std::chrono::steady_clock::now();
        if(now<nextPresent) return;
        const double rate=active ? (requestedFps>0 ? requestedFps : monitorHz) : 30.0;
        const auto period=std::chrono::nanoseconds(static_cast<long long>(1000000000.0/rate));
        if(nextPresent.time_since_epoch().count()==0 || now-nextPresent>period) nextPresent=now;
        nextPresent+=period;
        auto frame=pool ? pool.TryGetNextFrame() : Direct3D11CaptureFrame{nullptr};
        // Drain the small queue before rendering; never intentionally show a
        // frame that is older than another frame already available.
        if(pool) for(int drain=0;drain<2;++drain) {
            auto newer=pool.TryGetNextFrame();
            if(!newer) break;
            if(frame) frame.Close();frame=newer;
        }
        if(frame) {
            ++capturedFrames;
            auto newSize=frame.ContentSize();
            if(newSize.Width!=size.Width || newSize.Height!=size.Height) {
                // Restoration can leave tiny/stale frames in the queue. Keep
                // the last valid image and wait for a frame matching the client;
                // actual source resizing is checked in the main loop.
                frame.Close();
                if(active && GetTickCount64()-lastFrame>3000)
                    stop(L"Captura nao voltou ao tamanho esperado em 3 segundos.");
                return;
            }
            lastFrame=GetTickCount64();
            auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            com_ptr<ID3D11Texture2D> texture;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
            if(!image) {
                D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.MiscFlags=0;
                check_hresult(device->CreateTexture2D(&d,nullptr,image.put()));
                auto s=image.as<IDXGISurface>();
                auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(d.Format,D2D1_ALPHA_MODE_IGNORE),96,96);
                check_hresult(draw->CreateBitmapFromDxgiSurface(s.get(),&props,bitmap.put()));
            }
            context->CopyResource(image.get(),texture.get());
            context->Flush();
            frame.Close();
        }
        if(active && GetTickCount64()-lastFrame>3000) {
            stop(L"Captura sem frames por 3 segundos; encerrado."); return;
        }
        if(!bitmap) return;
        if(active && remapEnabled) updatePointer();
        // WGC frame covers the DWM extended bounds; crop decorations to client area.
        float left=float(clientOrigin.x-captureBounds.left),top=float(clientOrigin.y-captureBounds.top);
        auto crop=D2D1::RectF(left,top,left+originalClient.right,top+originalClient.bottom);
        if(crop.left<0 || crop.top<0 || crop.right>size.Width || crop.bottom>size.Height)
            throw hresult_error(E_FAIL,L"Recorte cliente fora da captura.");
        draw->BeginDraw();draw->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        draw->DrawBitmap(bitmap.get(),D2D1::RectF(0,0,float(outputW),float(outputH)),1,
                         D2D1_INTERPOLATION_MODE_LINEAR,&crop);
        if(active && remapEnabled) {
            if(nativePointer) {
                float sx=float(outputW)/originalClient.right,sy=float(outputH)/originalClient.bottom;
                float x=float(cursorX)-nativePixels.hotspotX*sx,y=float(cursorY)-nativePixels.hotspotY*sy;
                draw->DrawBitmap(nativePointer.get(),D2D1::RectF(x,y,x+nativePixels.width*sx,y+nativePixels.height*sy),
                    1,D2D1_INTERPOLATION_MODE_LINEAR);
            } else {
                draw->SetTransform(D2D1::Matrix3x2F::Translation(float(cursorX),float(cursorY)));
                draw->FillGeometry(pointerShape.get(),pointerWhite.get());
                draw->DrawGeometry(pointerShape.get(),pointerBlack.get(),1.5f);
                draw->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }
        check_hresult(draw->EndDraw());
        HRESULT presented=swap->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
        if(presented==DXGI_ERROR_WAS_STILL_DRAWING) ++presentBusy;
        else if(presented==S_OK) ++presentedFrames;
        if(presented!=DXGI_ERROR_WAS_STILL_DRAWING) check_hresult(presented);
    }
};

POINT testClick{-1,-1};
LRESULT CALLBACK testProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==WM_LBUTTONDOWN || m==WM_RBUTTONDOWN) {
        testClick={int(short(LOWORD(l))),int(short(HIWORD(l)))};InvalidateRect(h,nullptr,FALSE);return 0;
    }
    if(m==WM_PAINT) {
        PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);
        FillRect(dc,&r,reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SetBkMode(dc,TRANSPARENT);
        for(int x=0;x<r.right;x+=120) {MoveToEx(dc,x,0,nullptr);LineTo(dc,x,r.bottom);}
        for(int y=0;y<r.bottom;y+=100) {MoveToEx(dc,0,y,nullptr);LineTo(dc,r.right,y);}
        std::wstring text=L"CALIBRACAO 1680x1050 | Clique esquerdo/direito: "+
            std::to_wstring(testClick.x)+L", "+std::to_wstring(testClick.y);
        TextOutW(dc,20,20,text.c_str(),int(text.size()));
        Ellipse(dc,testClick.x-10,testClick.y-10,testClick.x+10,testClick.y+10);
        EndPaint(h,&ps);return 0;
    }
    if(m==WM_DESTROY) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(h,m,w,l);
}

int wmain(int argc,wchar_t** argv) {
    if(argc==4 && std::wstring(argv[1])==L"--cursor-guard")
        return runCursorGuard(static_cast<DWORD>(std::stoul(argv[2])),
            reinterpret_cast<HWND>(static_cast<ULONG_PTR>(std::stoull(argv[3]))));
    if(argc==2 && std::wstring(argv[1])==L"--restore-cursor") {
        if(!MagInitialize()) return 1;
        BOOL restored=MagShowSystemCursor(TRUE);MagUninitialize();return restored?0:1;
    }
    if(argc==2 && std::wstring(argv[1])==L"--cursor-guard-self-test") {
        // Null target prevents cursor hiding; parent exits forcibly, helper
        // must independently restore and exit within two seconds.
        CursorGuard guard;
        if(!guard.start(nullptr)) return 1;
        guard.hide(true);
        if(!guard.alive()) return 2;
        TerminateProcess(GetCurrentProcess(),72);
        return 3;
    }
    try {
        for(int a=1;a<argc;++a) {
            const std::wstring option=argv[a];
            if(option==L"--view-only") remapEnabled=false;
            else if(option==L"--hide-on-blur") hideOnBlur=true;
            else if(option==L"--window" && a+1<argc)
                requestedWindow=reinterpret_cast<HWND>(static_cast<ULONG_PTR>(std::stoull(argv[++a])));
            else if(option==L"--monitor-device" && a+1<argc) requestedMonitor=argv[++a];
            else if(option==L"--emergency-key" && a+1<argc) emergencyKey=UINT(std::stoul(argv[++a]));
            else if(option==L"--emergency-modifiers" && a+1<argc) emergencyModifiers=UINT(std::stoul(argv[++a]));
            else if(option==L"--fps" && a+1<argc) {
                requestedFps=std::stod(argv[++a]);
                if(requestedFps<0 || requestedFps>480) return 1;
            } else if(option==L"--input" && a+1<argc) {
                std::wstring mode=argv[++a];
                if(mode!=L"absolute" && mode!=L"relative" && mode!=L"visual") return 1;
                remapEnabled=mode==L"absolute";
            }
        }
    } catch(...) {return 1;}
    if(emergencyKey<1 || emergencyKey>254 || !(emergencyModifiers&(MOD_CONTROL|MOD_ALT|MOD_SHIFT)) || (emergencyModifiers&~UINT(MOD_CONTROL|MOD_ALT|MOD_SHIFT)) || (emergencyKey==VK_F11 && emergencyModifiers==(MOD_CONTROL|MOD_ALT))) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    init_apartment(apartment_type::multi_threaded);
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpszClassName=L"StretchPrototype";
    wc.lpfnWndProc=overlayProc;RegisterClassW(&wc);
    if(argc>1 && std::wstring(argv[1])==L"--test") {
        wc.lpszClassName=L"StretchCalibration";wc.lpfnWndProc=testProc;
        wc.hCursor=LoadCursorW(nullptr,IDC_CROSS);RegisterClassW(&wc);
        RECT r{0,0,1680,1050};AdjustWindowRectEx(&r,WS_OVERLAPPEDWINDOW,FALSE,0);
        CreateWindowExW(0,wc.lpszClassName,L"LoLStretch - Calibracao",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            20,20,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
        MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return 0;
    }
    emergencyReady=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!emergencyReady) return 1;
    emergencyThread=CreateThread(nullptr,0,emergencyProc,nullptr,0,nullptr);
    if(!emergencyThread || WaitForSingleObject(emergencyReady,2000)!=WAIT_OBJECT_0 || emergencyError.load()) {
        std::wcerr<<L"Saida de emergencia indisponivel (Win32 "<<emergencyError.load()
            <<L"). App nao sera ativado.\n";
        emergencyRunning=false;
        if(emergencyThread) {WaitForSingleObject(emergencyThread,1000);CloseHandle(emergencyThread);}
        CloseHandle(emergencyReady);return 1;
    }
    if(argc>1 && std::wstring(argv[1])==L"--hotkey-self-test") {
        PostThreadMessageW(GetThreadId(emergencyThread),WM_HOTKEY,101,0);
        Sleep(2000);return 2;
    }
    if(argc>1 && std::wstring(argv[1])==L"--emergency-self-test") {
        mainHeartbeat=GetTickCount64();watchdogArmed=true;
        // Deliberately stall only this process. Independent watchdog must kill it.
        Sleep(INFINITE);return 2;
    }
    const bool preciseTimer=timeBeginPeriod(1)==TIMERR_NOERROR;
    try {
        diagnostics.open("diagnostico.txt",std::ios::trunc);
        diagnostics<<"Onslaught 1.0.0; remap="<<remapEnabled<<"; overlay_alpha=254\n";
        if(!GraphicsCaptureSession::IsSupported()) throw hresult_error(E_FAIL,L"WGC indisponivel.");
        if(requestedWindow) {
            if(!IsWindow(requestedWindow)) throw hresult_error(E_FAIL,L"A janela selecionada fechou. Atualize a lista.");
            source=requestedWindow;
        } else {
        std::vector<HWND> windows;
        EnumWindows([](HWND h,LPARAM p)->BOOL {
            if(IsWindowVisible(h) && GetWindowTextLengthW(h)>0 && h!=GetConsoleWindow())
                reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
            return TRUE;
        },reinterpret_cast<LPARAM>(&windows));
        for(size_t n=0;n<windows.size();++n) {
            wchar_t title[512]{};GetWindowTextW(windows[n],title,512);
            RECT client{};GetClientRect(windows[n],&client);
            DWORD pid=0;GetWindowThreadProcessId(windows[n],&pid);
            std::wcout<<n+1<<L": "<<title<<L" | cliente "
                <<client.right<<L"x"<<client.bottom<<L" | PID "<<pid;
            if(IsIconic(windows[n])) std::wcout<<L" | minimizada (tamanho a confirmar apos restaurar)";
            else if(client.right==1680 && client.bottom==1050) std::wcout<<L" | tamanho esperado";
            std::wcout<<L"\n";
        }
        std::wcout<<L"Numero da janela: ";
        size_t selected=0;std::wcin>>selected;
        if(!selected || selected>windows.size()) return 1;
        source=windows[selected-1];
        }
        requestBorderless();
        std::wcout<<L"Restaurando e ativando a janela escolhida...\n";
        if(IsIconic(source)) ShowWindowAsync(source,SW_RESTORE);
        SetForegroundWindow(source);
        // Restore and activation are asynchronous across processes. No input
        // hook is installed until the target is restored and foreground.
        const ULONGLONG activationDeadline=GetTickCount64()+5000;
        bool requestedAgain=false;
        while(IsWindow(source) && (IsIconic(source)||GetForegroundWindow()!=source)) {
            if(GetTickCount64()>=activationDeadline) break;
            if(!IsIconic(source) && !requestedAgain) {
                SetForegroundWindow(source); requestedAgain=true;
                std::wcout<<L"Se o Windows bloquear o foco, clique no jogo agora (5 segundos).\n";
            }
            MSG pending{};
            while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&pending);DispatchMessageW(&pending);
            }
            MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        if(!IsWindow(source)||IsIconic(source)||GetForegroundWindow()!=source)
            throw hresult_error(E_FAIL,L"A janela nao restaurou/recebeu foco em 5 segundos. Tente novamente.");
        // Let the target process complete layout after restoration before
        // recording client dimensions and capture bounds.
        const ULONGLONG layoutDeadline=GetTickCount64()+300;
        while(GetTickCount64()<layoutDeadline) {
            MSG pending{};
            while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&pending);DispatchMessageW(&pending);
            }
            MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        GetClientRect(source,&originalClient);
        wchar_t selectedTitle[512]{};GetWindowTextW(source,selectedTitle,512);
        std::wcout<<L"Selecionada: "<<selectedTitle<<L" | cliente restaurado "
            <<originalClient.right<<L"x"<<originalClient.bottom<<L"\n";
        if(originalClient.right<=0 || originalClient.bottom<=0 || originalClient.right>16384 || originalClient.bottom>16384)
            throw hresult_error(E_FAIL,hstring(L"Area cliente encontrada: "+
                std::to_wstring(originalClient.right)+L"x"+std::to_wstring(originalClient.bottom)+
                L". A janela precisa ter uma area cliente valida e estar restaurada."));
        clientOrigin={0,0};ClientToScreen(source,&clientOrigin);
        check_hresult(DwmGetWindowAttribute(source,DWMWA_EXTENDED_FRAME_BOUNDS,&captureBounds,sizeof(captureBounds)));
        MONITORINFO mi{sizeof(mi)};
        HMONITOR selectedMonitor=MonitorFromWindow(source,MONITOR_DEFAULTTONEAREST);
        if(!requestedMonitor.empty()) {
            struct FindMonitor {const std::wstring* name;HMONITOR found;};
            FindMonitor search{&requestedMonitor,nullptr};
            EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
                auto* search=reinterpret_cast<FindMonitor*>(data);
                MONITORINFOEXW info{};info.cbSize=sizeof(info);
                if(GetMonitorInfoW(monitor,&info) && *search->name==info.szDevice) search->found=monitor;
                return TRUE;
            },reinterpret_cast<LPARAM>(&search));
            if(!search.found) throw hresult_error(E_FAIL,L"Monitor selecionado nao esta conectado.");
            selectedMonitor=search.found;
        }
        if(!GetMonitorInfoW(selectedMonitor,&mi)) throw_last_error();
        monitorRect=mi.rcMonitor;outputW=monitorRect.right-monitorRect.left;outputH=monitorRect.bottom-monitorRect.top;
        cursorX=outputW/2.0;cursorY=outputH/2.0;
        minCursorX=maxCursorX=cursorX;minCursorY=maxCursorY=cursorY;
        if(outputW<=0 || outputH<=0) throw hresult_error(E_FAIL,L"Monitor sem dimensoes validas.");
        MONITORINFOEXW displayInfo{};displayInfo.cbSize=sizeof(displayInfo);
        DEVMODEW mode{};mode.dmSize=sizeof(mode);
        if(GetMonitorInfoW(selectedMonitor,&displayInfo) &&
            EnumDisplaySettingsW(displayInfo.szDevice,ENUM_CURRENT_SETTINGS,&mode) && mode.dmDisplayFrequency>1) {
            monitorHz=double(mode.dmDisplayFrequency);
            std::wcout<<L"Monitor: "<<mode.dmPelsWidth<<L"x"<<mode.dmPelsHeight<<L" @ "<<monitorHz<<L" Hz\n";
            diagnostics<<"MONITOR_HZ="<<monitorHz<<"\n";
        }
        watchedSource=source;mainHeartbeat=GetTickCount64();watchdogArmed=true;
        overlay=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,
            wc.lpszClassName,L"Onslaught - Presentation",WS_POPUP,monitorRect.left,monitorRect.top,
            outputW,outputH,nullptr,nullptr,instance,nullptr);
        if(!overlay) throw_last_error();
        // Experimental occlusion mitigation: keep a tiny contribution from the
        // underlying window instead of covering it with a fully opaque HWND.
        if(!SetLayeredWindowAttributes(overlay,0,254,LWA_ALPHA)) throw_last_error();
        if(!RegisterHotKey(overlay,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F11)) throw_last_error();
        RAWINPUTDEVICE raw{0x01,0x02,RIDEV_INPUTSINK,overlay};
        if(!RegisterRawInputDevices(&raw,1,sizeof(raw))) throw_last_error();
        Renderer renderer;renderer.init();
        std::wcout<<L"Saida independente:";
        if(emergencyF12.load()) std::wcout<<L" Ctrl+Alt+F12";
        if(emergencyF10.load()) std::wcout<<L" Custom emergency key="<<emergencyKey<<L" modifiers="<<emergencyModifiers;
        std::wcout<<L". Ctrl+Alt+F11 desliga remapeamento. Watchdog: 5 segundos.\n";
        SetForegroundWindow(source);
        if(GetForegroundWindow()!=source) throw hresult_error(E_FAIL,L"Nao foi possivel dar foco a janela.");
        if(remapEnabled && !cursorGuard.start(source))
            throw hresult_error(E_FAIL,L"Protecao de restauracao do cursor indisponivel. Ativacao cancelada.");
        if(remapEnabled) {
            mouseHook=SetWindowsHookExW(WH_MOUSE_LL,mouseProc,instance,0);
            if(!mouseHook) throw_last_error();
        }
        ShowWindow(overlay,SW_SHOWNOACTIVATE);
        active=true;
        cursorGuard.hide(remapEnabled);
        if(remapEnabled && !mapCursor()) throw hresult_error(E_FAIL,L"SendInput bloqueado.");
        ULONGLONG lastDiagnostic=0;
        unsigned long long lastCaptured=0,lastPresented=0;
        MSG msg{};
        while(!stopped) {
            mainHeartbeat=GetTickCount64();
            if(remapEnabled && !cursorGuard.alive()) {
                stop(L"Protetor do cursor encerrou; app interrompido.");break;
            }
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                if(msg.message==WM_QUIT) {stopped=true;break;}
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
            if(stopped) break;
            if(!IsWindow(source)) {stop(L"Janela fechada.");break;}
            if(IsIconic(source)||GetForegroundWindow()!=source) {
                if(!paused) {
                    suspendPresentation();
                    diagnostics<<"BACKGROUND_INPUT_DISABLED\n";diagnostics.flush();
                }
                HWND foreground=GetForegroundWindow();
                if(foreground!=backgroundForeground) {
                    backgroundForeground=foreground;
                    if(foreground && foreground!=overlay && foreground!=source)
                        SetWindowPos(overlay,foreground,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                }
                // Keep the same WGC session even through minimization. Closing
                // and restarting it can flash the Windows capture indicator.
                // Continue rendering behind the foreground app. If the source
                // minimizes, retain its last frame instead of showing 1680x1050.
                if(!hideOnBlur) renderer.tick();
                MsgWaitForMultipleObjectsEx(0,nullptr,30,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                continue;
            }
            RECT now{};POINT origin{};GetClientRect(source,&now);ClientToScreen(source,&origin);
            if(!EqualRect(&now,&originalClient)) {
                stop(L"Resolucao cliente mudou; reinicie o app.");break;
            }
            if(paused) {
                clientOrigin=origin;
                check_hresult(DwmGetWindowAttribute(source,DWMWA_EXTENDED_FRAME_BOUNDS,&captureBounds,sizeof(captureBounds)));
                // Refresh cropping while reusing session, frame pool, GPU image
                // and swap chain. Alt+Tab must not restart WGC or clear its image.
                renderer.lastFrame=GetTickCount64();
                if(GetForegroundWindow()!=source || IsIconic(source)) continue;
                if(remapEnabled) {
                    mouseHook=SetWindowsHookExW(WH_MOUSE_LL,mouseProc,instance,0);
                    if(!mouseHook) throw_last_error();
                }
                SetWindowPos(overlay,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
                paused=false;active=true;cursorDirty=remapEnabled;
                cursorGuard.hide(remapEnabled);
                diagnostics<<"RESUMED_FOCUS_RETURNED\n";diagnostics.flush();
            } else if(origin.x!=clientOrigin.x||origin.y!=clientOrigin.y) {
                stop(L"Janela movida enquanto ativa; reinicie o app.");break;
            }
            // Coalesce high-polling mice: one synthetic move per iteration,
            // never call SendInput from inside the low-level hook callback.
            if(remapEnabled && cursorDirty) {
                cursorDirty=false;
                if(!mapCursor()) {stop(L"SendInput bloqueado.");break;}
            }
            renderer.tick();
            if(GetTickCount64()-lastDiagnostic>=1000) {
                ULONGLONG stamp=GetTickCount64();
                double elapsed=lastDiagnostic ? double(stamp-lastDiagnostic)/1000.0 : 1.0;
                lastDiagnostic=stamp;
                POINT physical{};RECT clip{};GetCursorPos(&physical);GetClipCursor(&clip);
                diagnostics<<"tick="<<lastDiagnostic<<" frames="<<capturedFrames
                    <<" raw="<<rawEvents<<" virtual="<<cursorX<<","<<cursorY
                    <<" ranges="<<minCursorX<<","<<maxCursorX<<"/"<<minCursorY<<","<<maxCursorY
                    <<" physical="<<physical.x<<","<<physical.y<<" clip="
                    <<clip.left<<","<<clip.top<<","<<clip.right<<","<<clip.bottom
                    <<" remap="<<remapEnabled<<" paused="<<paused<<"\n";
                diagnostics<<"RATES capture_fps="<<(capturedFrames-lastCaptured)/elapsed
                    <<" present_fps="<<(presentedFrames-lastPresented)/elapsed
                    <<" present_busy_total="<<presentBusy<<" monitor_hz="<<monitorHz<<"\n";
                lastCaptured=capturedFrames;lastPresented=presentedFrames;
                diagnostics.flush();
            }
            MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
    } catch(hresult_error const& e) {
        diagnostics<<"ERROR="<<unsigned(e.code())<<" "<<to_string(e.message())<<"\n";diagnostics.flush();
        std::wcerr<<L"Erro 0x"<<std::hex<<unsigned(e.code())<<L": "<<e.message().c_str()<<L"\n";
        stop(L"Erro; recursos liberados.");
    } catch(std::exception const& e) {engineExitCode=1;diagnostics<<"ERROR="<<e.what()<<"\n";std::cerr<<e.what()<<"\n";stop(L"Erro inesperado.");}
    active=false;if(mouseHook) UnhookWindowsHookEx(mouseHook);
    cursorGuard.hide(false);
    if(overlay) {UnregisterHotKey(overlay,2);DestroyWindow(overlay);}
    watchdogArmed=false;emergencyRunning=false;
    WaitForSingleObject(emergencyThread,1000);CloseHandle(emergencyThread);CloseHandle(emergencyReady);
    diagnostics<<"STOP frames="<<capturedFrames<<" raw="<<rawEvents<<"\n";
    std::wcout<<stopReason<<L"\n";
    if(preciseTimer) timeEndPeriod(1);
    return engineExitCode;
}
