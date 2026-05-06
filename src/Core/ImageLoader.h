#pragma once
#include "Core/Image.h"
#include <string>

namespace dy::Core
{
    // PNG, JPG, BMP, TGA 등 일반 이미지 파일을 로드해서 Image로 반환
    // 실패 시 IsValid() == false인 Image 반환
    Image LoadImageFromFile(const std::string& filepath);
}
