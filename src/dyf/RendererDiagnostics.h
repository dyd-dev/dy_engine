#pragma once

#include <cstdio>

namespace dyf
{
// 예상한 실패는 기존 오류 메시지와 반환값으로 전달한다.
inline bool RendererFailure(const char* message)
{
    std::fprintf(stderr,"dyf: %s\n",message);
    return false;
}
}
