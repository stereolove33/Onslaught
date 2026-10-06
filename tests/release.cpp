#include "release.h"
#include "target_selection.h"
#include <iostream>
int main() {
    using namespace onslaught;
    if(std::wstring(OfficialRepository)!=L"stereolove33/Onslaught") return 6;
    if(normalizeRepository(L" https://github.com/owner/Onslaught.git ")!=L"owner/Onslaught") return 1;
    for(auto invalid:{L"https://evil.example/owner/repo",L"owner/repo/extra",L"owner/..",L"owner/repo?token=secret",L""}) if(!normalizeRepository(invalid).empty()) return 2;
    if(compareVersion(L"v1.1.0",Version)!=1 || compareVersion(L"1.0.0",Version)!=0 || compareVersion(L"0.9.0",Version)!=-1 || compareVersion(L"v1.1.0-beta",Version)!=-2) return 3;
    if(releaseTag(R"({"tag_name":"v1.1.0","body":"hello"})")!=L"v1.1.0" || !releaseTag(R"({"tag_name":"bad"})").empty()) return 4;
    struct Candidate {std::wstring executable,title;};
    std::vector<Candidate> candidates{{L"C:\\Apps\\game.exe",L"Launcher"},{L"C:\\Apps\\other.exe",L"Game"},{L"c:\\apps\\GAME.exe",L"Game"}};
    if(rememberedTarget(candidates,L"C:\\Apps\\game.exe",L"Game")!=2 || rememberedTarget(candidates,L"C:\\Apps\\game.exe",L"Changed title")!=0 || rememberedTarget(candidates,L"missing.exe",L"Game")!=-1) return 5;
    std::cout<<"Repository validation, release parsing and semantic versions passed\n";return 0;
}

