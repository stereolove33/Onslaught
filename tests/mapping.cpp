#include "mapping.h"
#include <iostream>
int main() {
    int failures = 0;
    auto check = [&](bool ok) { if (!ok) ++failures; };
    check(mapPixel(0,1920,1680)==0);
    check(mapPixel(1919,1920,1680)==1679);
    check(mapPixel(1079,1080,1050)==1049);
    check(mapPixel(-20,1920,1680)==0);
    check(mapPixel(2000,1920,1680)==1679);
    check(mapPixel(50,1,1680)==0);
    for(int x=0;x<1920;++x) {
        int src=mapPixel(x,1920,1680);
        int roundtrip=mapPixel(src,1680,1920);
        check(std::abs(roundtrip-x)<=1);
        if(x) check(src>=mapPixel(x-1,1920,1680));
    }
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
