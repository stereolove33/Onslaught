#pragma once
#include <string>
#include <vector>
#include <cwchar>
namespace onslaught {
// Window handles and PIDs are transient. Match the executable and prefer the previous title.
template<class Candidate>
int rememberedTarget(const std::vector<Candidate>& candidates,const std::wstring& executable,const std::wstring& title) {
    if(executable.empty()) return -1;int first=-1;
    for(size_t n=0;n<candidates.size();++n) {
        if(_wcsicmp(candidates[n].executable.c_str(),executable.c_str())!=0) continue;
        if(candidates[n].title==title) return int(n);if(first<0) first=int(n);
    }
    return first;
}
}
