#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace fujinet::platform::pico::detail {
template<class Random> void fill_entropy(unsigned char* out,std::size_t length,Random random) {
    while(length) {
        const std::uint64_t word=random(); const auto n=std::min(length,sizeof(word));
        std::memcpy(out,&word,n); out+=n; length-=n;
    }
}
}
