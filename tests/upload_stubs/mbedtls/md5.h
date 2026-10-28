#pragma once
#include <cstddef>
#include <cstdint>
inline int mbedtls_md5(const uint8_t *,size_t,uint8_t *out) {for(unsigned i=0;i<16;++i) out[i]=0;return 0;}
