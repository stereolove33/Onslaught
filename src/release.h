#pragma once
#include <string>
#include <array>
#include <regex>
namespace onslaught {
inline constexpr wchar_t OfficialRepository[]=L"stereolove33/Onslaught";
inline constexpr wchar_t Version[]=L"1.0.0";
inline std::wstring normalizeRepository(std::wstring value) {
    auto first=value.find_first_not_of(L" \t\r\n"),last=value.find_last_not_of(L" \t\r\n");
    if(first==std::wstring::npos) return {};value=value.substr(first,last-first+1);
    const std::wstring prefix=L"https://github.com/";
    if(value.rfind(prefix,0)==0) value.erase(0,prefix.size());
    if(!value.empty() && value.back()==L'/') value.pop_back();
    if(value.size()>4 && value.substr(value.size()-4)==L".git") value.resize(value.size()-4);
    if(!std::regex_match(value,std::wregex(L"[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]{1,100}"))) return {};
    auto repo=value.substr(value.find(L'/')+1);if(repo==L"."||repo==L"..") return {};return value;
}
inline bool parseVersion(std::wstring value,std::array<unsigned,3>& result) {
    if(!value.empty() && (value[0]==L'v'||value[0]==L'V')) value.erase(0,1);
    std::wsmatch match;
    if(!std::regex_match(value,match,std::wregex(L"([0-9]{1,6})\\.([0-9]{1,6})\\.([0-9]{1,6})"))) return false;
    for(size_t n=0;n<3;++n) result[n]=unsigned(std::stoul(match[n+1].str()));return true;
}
inline int compareVersion(const std::wstring& latest,const std::wstring& current) {
    std::array<unsigned,3>a{},b{};if(!parseVersion(latest,a)||!parseVersion(current,b)) return -2;
    return a>b?1:a<b?-1:0;
}
inline std::wstring releaseTag(const std::string& json) {
    std::smatch match;
    if(!std::regex_search(json,match,std::regex("\"tag_name\"\\s*:\\s*\"([vV]?[0-9]+\\.[0-9]+\\.[0-9]+)\""))) return {};
    auto value=match[1].str();return std::wstring(value.begin(),value.end());
}
}

