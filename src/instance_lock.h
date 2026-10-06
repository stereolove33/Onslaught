#pragma once
#include <windows.h>
namespace onslaught {
class InstanceLock {
    HANDLE handle_{};
public:
    bool alreadyRunning=false;
    explicit InstanceLock(const wchar_t* name,bool detectLegacy=false) {
        handle_=CreateMutexW(nullptr,FALSE,name);
        alreadyRunning=handle_ && GetLastError()==ERROR_ALREADY_EXISTS;
        if(detectLegacy && FindWindowW(L"OnslaughtInterface",nullptr)) alreadyRunning=true;
    }
    ~InstanceLock() {if(handle_) CloseHandle(handle_);}
    InstanceLock(const InstanceLock&)=delete;
    InstanceLock& operator=(const InstanceLock&)=delete;
    bool valid() const {return handle_!=nullptr;}
};
}
