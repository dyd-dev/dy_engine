#pragma once

#include <cstdio>

namespace dyf
{
// 오류 메시지를 stderr에 남기고 false를 반환해 호출부로 실패를 전달하며, 자원 정리는 호출 범위가 맡는다.
inline bool RendererFailure(const char* message)
{
    std::fprintf(stderr,"dyf: %s\n",message);
    return false;
}
}
