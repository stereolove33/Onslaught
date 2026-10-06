#include "instance_lock.h"
#include <string>
#include <iostream>
int main() {
    auto name=L"Local\\OnslaughtInstanceTest-"+std::to_wstring(GetCurrentProcessId());
    {
        onslaught::InstanceLock first(name.c_str());
        if(!first.valid() || first.alreadyRunning) return 1;
        onslaught::InstanceLock duplicate(name.c_str());
        if(!duplicate.valid() || !duplicate.alreadyRunning) return 2;
    }
    onslaught::InstanceLock restarted(name.c_str());
    if(!restarted.valid() || restarted.alreadyRunning) return 3;
    std::cout<<"Single-instance detection and restart passed\n";return 0;
}
