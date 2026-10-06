#pragma once
#include <windows.h>
#include <magnification.h>
#include <string>

// A separate process owns system-cursor visibility. It never calls D3D/WGC.
inline std::wstring cursorEventName(DWORD pid) {
    return L"Local\\OnslaughtCursor-"+std::to_wstring(pid);
}
inline int runCursorGuard(DWORD parentPid, HWND target) {
    HANDLE parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid);
    HANDLE desired=OpenEventW(SYNCHRONIZE,FALSE,cursorEventName(parentPid).c_str());
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(cursorEventName(parentPid)+L"-ready").c_str());
    if(!parent || !desired || !ready || !MagInitialize()) {
        if(parent) CloseHandle(parent);if(desired) CloseHandle(desired);if(ready) CloseHandle(ready);
        return 1;
    }
    if(!MagShowSystemCursor(TRUE)) {MagUninitialize();CloseHandle(parent);CloseHandle(desired);CloseHandle(ready);return 2;}
    SetEvent(ready);
    bool hidden=false;
    for(;;) {
        DWORD exitCode=STILL_ACTIVE;
        if(WaitForSingleObject(parent,20)!=WAIT_TIMEOUT ||
           !GetExitCodeProcess(parent,&exitCode) || exitCode!=STILL_ACTIVE) break;
        bool wantHide=target && IsWindow(target) && !IsIconic(target) &&
            GetForegroundWindow()==target && WaitForSingleObject(desired,0)==WAIT_OBJECT_0;
        if(wantHide!=hidden) {
            if(!MagShowSystemCursor(wantHide ? FALSE : TRUE)) break;
            hidden=wantHide;
        }
    }
    MagShowSystemCursor(TRUE);
    MagUninitialize();CloseHandle(parent);CloseHandle(desired);CloseHandle(ready);
    return 0;
}
struct CursorGuard {
    HANDLE desired{},ready{},process{};
    bool start(HWND target) {
        const auto name=cursorEventName(GetCurrentProcessId());
        desired=CreateEventW(nullptr,TRUE,FALSE,name.c_str());
        ready=CreateEventW(nullptr,TRUE,FALSE,(name+L"-ready").c_str());
        if(!desired || !ready) return false;
        wchar_t executable[32768]{};
        if(!GetModuleFileNameW(nullptr,executable,32768)) return false;
        std::wstring command=L"\""+std::wstring(executable)+L"\" --cursor-guard "+
            std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(target));
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION info{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                           nullptr,nullptr,&startup,&info)) return false;
        process=info.hProcess;CloseHandle(info.hThread);
        return WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;
    }
    void hide(bool yes) {
        if(desired) {if(yes) SetEvent(desired);else ResetEvent(desired);}
    }
    bool alive() const {return process && WaitForSingleObject(process,0)==WAIT_TIMEOUT;}
    ~CursorGuard() {
        hide(false);
        // Do not terminate the helper: it must observe parent exit and restore
        // the cursor even if parent shutdown is forced or graphics is stuck.
        if(desired) CloseHandle(desired);if(ready) CloseHandle(ready);if(process) CloseHandle(process);
    }
};
