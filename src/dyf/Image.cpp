#include "dyf/Image.h"
#include "ImageDds.h"

#include <limits>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <utility>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <atomic>
#include <mutex>

// 파일과 모델 내장 이미지의 디코딩은 이 CPU 이미지 로더에서 담당한다.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// CMake로 받은 bcdec 0.985를 사용한다. 원저작권과 MIT 라이선스는 헤더에 있다.
#define BCDECDEF static inline
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#undef BCDEC_IMPLEMENTATION
#undef BCDEC_BC4BC5_PRECISE
#undef BCDECDEF

namespace dyf
{
namespace Detail
{
struct ImagePixels
{
    explicit ImagePixels(std::vector<uint8_t> values):pixels(std::move(values)),byteCount(pixels.size()) {}
    std::vector<uint8_t> pixels;
    const size_t byteCount;
    std::mutex mutex;
    std::atomic<bool> decoded{true};
};
}
namespace
{
    bool RestorableDds(const Detail::ImageDdsData* dds,uint32_t width,uint32_t height)
    {
        if(!dds || dds->mips.empty())return false;
        const uint32_t blockBytes=dds->format==RHI::Format::BC1_UNORM?8u:
            dds->format==RHI::Format::BC3_UNORM || dds->format==RHI::Format::BC5_UNORM?16u:0u;
        return blockBytes && uint64_t((uint64_t(width)+3)/4)*((uint64_t(height)+3)/4)*blockBytes==dds->mips[0].size();
    }
    // stb 임시 픽셀의 소유자다. 조기 반환이나 예외에도 stbi_image_free로 해제한다.
    using DecodedPixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;

    // 여기서 RGBA8로 풀 수 있는 DDS 형식만 열거한다. HDR BC6H·배열·큐브맵은 이 경로의 입력이 아니다.
    enum class DdsFormat : uint8_t {BC1,BC2,BC3,BC4,BC5,BC7,RGBA8,BGRA8,RGBX8,BGRX8};
    // 검증한 해상도·mip 수·최상위 mip 위치와 채널 복원 규칙을 디코더에 넘긴다.
    struct DdsInfo
    {
        uint32_t width=0,height=0,mipCount=1,blockBytes=0;
        size_t offset=128,topBytes=0,rowPitch=0,mipBytes=0;
        DdsFormat format=DdsFormat::BC1;
        bool normalMap=false,swapRedGreen=false,swapRedAlpha=false;
    };

    // 여러 바이트로 된 숫자에서 낮은 자리의 바이트부터 저장하는 순서(little-endian)에 맞춰 네 바이트를 정수로 조립한다.
    // 입력 주소를 uint32_t 포인터로 바꾸지 않고 각 바이트를 읽어 이동하므로 4바이트 경계에 정렬되지 않은 입력도 읽을 수 있다.
    uint32_t ReadU32(const uint8_t* bytes)
    {
        return uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24);
    }
    // DDS의 네 글자 식별자를 파일에 기록된 바이트 순서와 같은 정수로 만든다.
    constexpr uint32_t FourCC(char a,char b,char c,char d)
    {
        return uint32_t(uint8_t(a))|(uint32_t(uint8_t(b))<<8)|(uint32_t(uint8_t(c))<<16)|(uint32_t(uint8_t(d))<<24);
    }
    // 입력이 최소 네 바이트인지 먼저 확인한 뒤 첫 네 바이트가 DDS 식별자인 D, D, S, 공백인지 검사한다.
    bool IsDds(const uint8_t* bytes,size_t size)
    {
        return bytes && size>=4 && ReadU32(bytes)==FourCC('D','D','S',' ');
    }
    // DDS 실패 이유를 stderr에 남기고 호출 흐름에 false를 전달한다.
    bool DdsFailure(const char* reason)
    {
        std::fprintf(stderr,"dyf: DDS image: %s\n",reason);return false;
    }

    // 정렬되지 않은 입력도 읽도록 little-endian 필드를 바이트 단위로 해석하고, DDS 헤더·픽셀 형식·선언된 전체 mip 길이를 검증한다.
    // 디코딩할 mip은 해상도 제한에 따라 나중에 선택한다. 원본 전체 mip 길이는 먼저 검증한다.
    bool ParseDds(const uint8_t* bytes,size_t headerBytes,uint64_t fileBytes,DdsInfo& info)
    {
        if(!IsDds(bytes,headerBytes) || headerBytes<128 || ReadU32(bytes+4)!=124 || ReadU32(bytes+76)!=32) // 매직·표준 헤더 크기·픽셀 형식 구조 크기 중 하나라도 틀리면 읽기를 중단한다.
            return DdsFailure("invalid or truncated header.");
        info.width=ReadU32(bytes+16);info.height=ReadU32(bytes+12);
        if(!info.width || !info.height || info.width>INT32_MAX || info.height>INT32_MAX) // 0 크기나 signed 32비트 범위를 넘는 이미지는 거부한다.
            return DdsFailure("invalid image dimensions.");
        if(ReadU32(bytes+24)>1 || (ReadU32(bytes+112)&0x0020fe00u)!=0) // 볼륨 또는 큐브맵 플래그가 있으면 단일 2D 경로로 읽지 않는다.
            return DdsFailure("only a single 2D image is supported; volume/cubemap DDS is unsupported.");
        // 헤더의 픽셀 형식 플래그가 압축 FourCC인지, 32비트 채널 마스크인지 결정한다.
        const uint32_t flags=ReadU32(bytes+8),pixelFlags=ReadU32(bytes+80),code=ReadU32(bytes+84);
        info.normalMap=(pixelFlags&0x80000000u)!=0;
        if(pixelFlags&4u) // FourCC가 있으면 압축 형식 또는 DX10 확장 헤더로 해석한다.
        {
            switch(code)
            {
            case FourCC('D','X','T','1'):info.format=DdsFormat::BC1;info.blockBytes=8;break;
            case FourCC('D','X','T','3'):info.format=DdsFormat::BC2;info.blockBytes=16;break;
            case FourCC('D','X','T','5'):info.format=DdsFormat::BC3;info.blockBytes=16;break;
            case FourCC('A','T','I','1'):
            case FourCC('B','C','4','U'):info.format=DdsFormat::BC4;info.blockBytes=8;break;
            case FourCC('A','T','I','2'):
            case FourCC('B','C','5','U'):
                info.format=DdsFormat::BC5;info.blockBytes=16;
                // ATI2 표시는 BC5의 두 성분 순서를 뒤집고 A2XY 표시도 같은 순서를 한 번 뒤집는다.
                // 두 표시 중 하나만 있으면 R/G를 교환한다. Bistro의 NVTT DDS처럼 둘 다 있으면 두 번 뒤집힌 결과가 원래 순서이므로 교환하지 않는다.
                info.swapRedGreen=(code==FourCC('A','T','I','2'))!=(ReadU32(bytes+88)==FourCC('A','2','X','Y'));
                break;
            // DX10 확장에서는 차원·배열 길이·알파 방식과 DXGI 형식을 추가 검사한다.
            case FourCC('D','X','1','0'):
            {
                if(headerBytes<148) return DdsFailure("truncated DX10 header."); // DX10 추가 헤더를 포함한 148바이트를 확보하지 못하면 잘린 파일로 거부한다.
                if(ReadU32(bytes+132)!=3 || ReadU32(bytes+140)!=1 || (ReadU32(bytes+136)&4u)!=0) // 2D 한 장이 아니거나 큐브맵이면 지원 범위 밖이다.
                    return DdsFailure("DX10 arrays, cubemaps and non-2D textures are unsupported.");
                if((ReadU32(bytes+144)&7u)==2u) // 미리 곱한 알파는 이 디코더에서 변환하지 않고 거부한다.
                    return DdsFailure("premultiplied-alpha DDS is unsupported.");
                info.offset=148;
                switch(ReadU32(bytes+128))
                {
                case 71:case 72:info.format=DdsFormat::BC1;info.blockBytes=8;break;
                case 74:case 75:info.format=DdsFormat::BC2;info.blockBytes=16;break;
                case 77:case 78:info.format=DdsFormat::BC3;info.blockBytes=16;break;
                case 80:info.format=DdsFormat::BC4;info.blockBytes=8;break;
                case 83:info.format=DdsFormat::BC5;info.blockBytes=16;break;
                case 98:case 99:info.format=DdsFormat::BC7;info.blockBytes=16;break;
                case 28:case 29:info.format=DdsFormat::RGBA8;break;
                case 87:case 91:info.format=DdsFormat::BGRA8;break;
                case 88:case 93:info.format=DdsFormat::BGRX8;break;
                default:return DdsFailure("unsupported DXGI format (HDR BC6H, signed and typeless formats are not RGBA8 inputs).");
                }
                break;
            }
            default:return DdsFailure("unsupported FourCC compression format.");
            }
            // NVTT의 A2D5 표시는 알파에 저장된 빨강 성분을 되돌리는 데 사용한다.
            info.swapRedAlpha=code==FourCC('D','X','T','5') && ReadU32(bytes+88)==FourCC('A','2','D','5');
        }
        else
        {
            if((pixelFlags&0x40u)==0 || ReadU32(bytes+88)!=32 || ReadU32(bytes+96)!=0x0000ff00u) // 비압축 입력은 32비트 RGB와 지원하는 녹색 마스크여야 한다.
                return DdsFailure("unsupported uncompressed pixel format; only RGBA/BGRA 32-bit layouts are supported.");
            const uint32_t red=ReadU32(bytes+92),blue=ReadU32(bytes+100),alpha=ReadU32(bytes+104);
            if(alpha!=0 && alpha!=0xff000000u) return DdsFailure("unsupported alpha channel mask."); // 알파가 없거나 최상위 8비트인 배치만 허용한다.
            if(red==0x000000ffu && blue==0x00ff0000u) info.format=alpha ? DdsFormat::RGBA8 : DdsFormat::RGBX8; // 하위 바이트가 빨강이면 RGBA 또는 RGBX로 해석한다.
            else if(red==0x00ff0000u && blue==0x000000ffu) info.format=alpha ? DdsFormat::BGRA8 : DdsFormat::BGRX8; // 하위 바이트가 파랑이면 BGRA 또는 BGRX로 해석한다.
            else return DdsFailure("unsupported RGB channel masks.");
        }
        // 해상도에서 가능한 최대 mip 수를 구한 뒤 각 레벨의 행 바이트 수와 전체 범위를 계산한다.
        info.mipCount=std::max(1u,ReadU32(bytes+28));
        uint32_t maxMips=1;
        for(uint32_t size=std::max(info.width,info.height);size>1;size>>=1) ++maxMips;
        if(info.mipCount>maxMips) return DdsFailure("mip count exceeds image dimensions."); // 해상도로 만들 수 있는 mip 수보다 많이 선언하면 거부한다.
        uint64_t required=info.offset;
        uint32_t width=info.width,height=info.height;
        for(uint32_t level=0;level<info.mipCount;++level)
        {
            uint64_t row=info.blockBytes ? ((uint64_t(width)+3)/4)*info.blockBytes : uint64_t(width)*4;
            const uint64_t rows=info.blockBytes ? (uint64_t(height)+3)/4 : height;
            if(!info.blockBytes && level==0 && (flags&8u)) // 비압축 최상위 mip에 행 간격이 명시되었으면 그 패딩을 반영한다.
            {
                const uint32_t declaredPitch=ReadU32(bytes+20);
                if(declaredPitch<row) return DdsFailure("declared row pitch is smaller than a pixel row."); // 한 행의 픽셀조차 담지 못하는 행 간격은 잘못된 헤더다.
                row=declaredPitch;
            }
            if(row>SIZE_MAX || rows>UINT64_MAX/row) return DdsFailure("image byte count overflows."); // 행 크기나 행 수 곱셈이 표현 범위를 넘으면 거부한다.
            const uint64_t size=row*rows;
            if(size>SIZE_MAX || required>UINT64_MAX-size) return DdsFailure("mip byte count overflows."); // mip 크기 또는 전체 크기 덧셈이 넘치면 거부한다.
            if(level==0) {info.rowPitch=static_cast<size_t>(row);info.topBytes=static_cast<size_t>(size);} // 실제 디코딩할 최상위 mip의 위치 계산에 필요한 크기를 저장한다.
            required+=size;width=std::max(1u,width/2);height=std::max(1u,height/2);
        }
        if(required>fileBytes) return DdsFailure("file is truncated within its declared mip chain."); // 선언된 mip 체인 끝이 파일을 벗어나면 잘린 파일로 판단한다.
        if(required-info.offset>SIZE_MAX) return DdsFailure("complete mip chain exceeds the addressable size.");
        info.mipBytes=static_cast<size_t>(required-info.offset);
        return true;
    }

    // 파일 전체 크기와 최대 148바이트 헤더만 먼저 읽는다. DDS 여부와 전체 mip 길이를 큰 픽셀 할당 전에 확인한다.
    bool ReadImagePrefix(const std::string& path,std::ifstream& file,std::array<uint8_t,148>& prefix,
        size_t& prefixSize,uint64_t& fileBytes)
    {
        file.open(path,std::ios::binary|std::ios::ate);
        if(!file) return false; // 파일을 열 수 없으면 헤더 조회를 실패시킨다.
        const auto end=file.tellg();
        if(end<0) return false; // 파일 크기를 얻지 못하면 범위를 신뢰하지 않는다.
        fileBytes=static_cast<uint64_t>(end);
        prefixSize=static_cast<size_t>(std::min<uint64_t>(prefix.size(),fileBytes));
        file.seekg(0);
        return bool(file.read(reinterpret_cast<char*>(prefix.data()),static_cast<std::streamsize>(prefixSize)));
    }

    RHI::Format PreservedDdsFormat(const DdsInfo& info)
    {
        // Normal-map reconstruction and legacy swizzles change decoded pixels.
        // Keep those inputs on the existing RGBA path.
        uint32_t fullMips=1;
        for(uint32_t size=std::max(info.width,info.height);size>1;size>>=1) ++fullMips;
        if(info.mipCount!=fullMips) return RHI::Format::Unknown;
        if(info.format==DdsFormat::BC5 && info.normalMap && !info.swapRedGreen && !info.swapRedAlpha)
            return RHI::Format::BC5_UNORM;
        if(info.normalMap || info.swapRedGreen || info.swapRedAlpha) return RHI::Format::Unknown;
        if(info.format==DdsFormat::BC1) return RHI::Format::BC1_UNORM;
        if(info.format==DdsFormat::BC3) return RHI::Format::BC3_UNORM;
        return RHI::Format::Unknown;
    }

    void PreserveDdsBlocks(const DdsInfo& info,const uint8_t* source,Image& image)
    {
        const auto format=PreservedDdsFormat(info);
        if(format==RHI::Format::Unknown) return;
        auto data=std::make_shared<Detail::ImageDdsData>();
        data->format=format;
        data->normalMap=info.normalMap;
        data->mips.reserve(info.mipCount);
        uint32_t width=info.width,height=info.height;
        // ParseDds has checked every level and the complete source byte range.
        for(uint32_t mip=0;mip<info.mipCount;++mip)
        {
            const size_t bytes=size_t((uint64_t(width)+3)/4)*((uint64_t(height)+3)/4)*info.blockBytes;
            data->mips.emplace_back(source,source+bytes);
            source+=bytes;
            width=std::max(1u,width/2);height=std::max(1u,height/2);
        }
        Detail::ImageDdsAccess::Set(image,std::move(data));
    }

    // 검증된 최상위 mip을 RGBA8로 푼다. 출력 용량을 먼저 검사하고 완성된 Image만 result에 대입한다.
    bool DecodeDdsPixels(const DdsInfo& info,const uint8_t* source,uint64_t limit,Image& result,bool* exceeded)
    {
        const uint64_t required=uint64_t(info.width)*info.height*4;
        if(required>limit || required>SIZE_MAX) // 디코딩 결과가 호출자 예산이나 주소 범위를 넘으면 할당하지 않는다.
        {
            if(exceeded) *exceeded=true; // 용량 초과 결과를 받을 포인터가 있으면 true로 기록한다.
            return DdsFailure("decoded RGBA image exceeds the memory budget.");
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(required));
        if(!info.blockBytes) // 비압축 DDS는 행 패딩·채널 배치·알파를 처리하고, 압축 DDS는 블록 디코더로 푼다.
        {
            // 비압축 DDS는 행 패딩을 건너뛰고 채널 순서를 바꾼다. 알파 없는 X 형식은 불투명으로 채운다.
            const bool bgra=info.format==DdsFormat::BGRA8 || info.format==DdsFormat::BGRX8;
            const bool alpha=info.format==DdsFormat::RGBA8 || info.format==DdsFormat::BGRA8;
            for(uint32_t y=0;y<info.height;++y) for(uint32_t x=0;x<info.width;++x)
            {
                const auto* pixel=source+size_t(y)*info.rowPitch+size_t(x)*4;
                auto* target=rgba.data()+(size_t(y)*info.width+x)*4;
                target[0]=pixel[bgra ? 2 : 0];target[1]=pixel[1];target[2]=pixel[bgra ? 0 : 2];target[3]=alpha ? pixel[3] : 255;
            }
        }
        else
        {
            // 압축 DDS는 4×4 블록을 풀고 가장자리의 실제 이미지 영역만 복사한다.
            const uint32_t columns=(info.width+3u)/4,rows=(info.height+3u)/4;
            for(uint32_t by=0;by<rows;++by) for(uint32_t bx=0;bx<columns;++bx)
            {
                const auto* block=source+(size_t(by)*columns+bx)*info.blockBytes;
                alignas(uint32_t) std::array<uint8_t,64> pixels={};
                switch(info.format)
                {
                case DdsFormat::BC1:bcdec_bc1(block,pixels.data(),16);break;
                case DdsFormat::BC2:bcdec_bc2(block,pixels.data(),16);break;
                case DdsFormat::BC3:bcdec_bc3(block,pixels.data(),16);break;
                case DdsFormat::BC4:bcdec_bc4(block,pixels.data(),4,0);break;
                case DdsFormat::BC5:bcdec_bc5(block,pixels.data(),8,0);break;
                case DdsFormat::BC7:bcdec_bc7(block,pixels.data(),16);break;
                default:return DdsFailure("invalid compressed format.");
                }
                const uint32_t blockWidth=std::min(4u,info.width-bx*4),blockHeight=std::min(4u,info.height-by*4);
                for(uint32_t y=0;y<blockHeight;++y) for(uint32_t x=0;x<blockWidth;++x)
                {
                    const uint32_t index=y*4+x;
                    auto* target=rgba.data()+((size_t(by)*4+y)*info.width+bx*4+x)*4;
                    if(info.format==DdsFormat::BC4) // 한 채널은 빨강에 놓고 나머지 색은 0, 알파는 255로 채운다.
                    {target[0]=pixels[index];target[1]=target[2]=0;target[3]=255;}
                    else if(info.format==DdsFormat::BC5) // 두 채널을 빨강·녹색으로 복사한다.
                    {
                        target[0]=pixels[index*2];target[1]=pixels[index*2+1];target[2]=0;target[3]=255;
                    }
                    else std::memcpy(target,pixels.data()+index*4,4);
                    if(info.swapRedGreen) std::swap(target[0],target[1]); // DDS가 반대로 저장한 두 성분을 교환한다.
                    if(info.swapRedAlpha) std::swap(target[0],target[3]); // 알파에 저장했던 빨강 성분을 복원한다.
                    // normalMap은 표면에 수직인 방향(법선)을 저장한 맵이라는 표시다.
                    if(info.normalMap && (info.format==DdsFormat::BC5 || info.swapRedAlpha)) // 법선맵으로 표시된 BC5 또는 빨강·알파 교환 형식에서만 세 번째 축을 복원한다.
                    {
                        // 저장된 두 채널을 0~255에서 -1~1의 x·y로 바꾸고, 법선 길이가 1이라는 조건에서 z=sqrt(1-x²-y²)의 양수 쪽을 복원한다.
                        // 1-x²-y²가 음수이면 제곱근을 계산할 수 없으므로 0으로 제한하고, 얻은 z를 다시 0~255 채널값으로 저장한다.
                        const float nx=target[0]*(2.0f/255)-1,ny=target[1]*(2.0f/255)-1;
                        const float nz=std::sqrt(std::max(0.0f,1-nx*nx-ny*ny));
                        target[2]=static_cast<uint8_t>(std::lround((nz*.5f+.5f)*255));
                    }
                }
            }
        }
        result=Image(info.width,info.height,std::move(rgba));return true;
    }

    // RGBA8에 필요한 용량을 64비트로 계산하고 호출자 한도와 size_t 범위를 확인한다.
    bool DecodedByteSize(int width, int height, uint64_t limit, size_t& bytes)
    {
        if(width <= 0 || height <= 0) return false; // 유효한 해상도가 아니면 용량 계산을 실패시킨다.
        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if(pixels > std::numeric_limits<uint64_t>::max() / 4u) return false; // RGBA 네 바이트 곱셈 전에 오버플로를 막는다.
        const uint64_t size = pixels * 4u;
        if(size > limit || size > std::numeric_limits<size_t>::max()) return false; // 호출자 예산과 플랫폼 배열 크기 한도 모두 안에 있어야 한다.
        bytes = static_cast<size_t>(size);
        return true;
    }

    void LimitImageDimensions(uint32_t& width,uint32_t& height,uint32_t maximum)
    {
        const uint32_t longest=std::max(width,height);
        if(!maximum || longest<=maximum)return;
        width=std::max(1u,static_cast<uint32_t>(uint64_t(width)*maximum/longest));
        height=std::max(1u,static_cast<uint32_t>(uint64_t(height)*maximum/longest));
    }

    void SelectDdsMip(DdsInfo& info,uint32_t maximum)
    {
        while(maximum && std::max(info.width,info.height)>maximum && info.mipCount>1)
        {
            info.offset+=info.topBytes;
            info.mipBytes-=info.topBytes;
            --info.mipCount;
            info.width=std::max(1u,info.width/2);
            info.height=std::max(1u,info.height/2);
            info.rowPitch=info.blockBytes ? size_t((uint64_t(info.width)+3)/4)*info.blockBytes : size_t(info.width)*4;
            info.topBytes=info.rowPitch*(info.blockBytes ? (uint64_t(info.height)+3)/4 : info.height);
        }
    }

    Image CopyBoundedPixels(const uint8_t* source,uint32_t width,uint32_t height,uint32_t maximum)
    {
        uint32_t outputWidth=width,outputHeight=height;
        LimitImageDimensions(outputWidth,outputHeight,maximum);
        if(outputWidth==width && outputHeight==height)
            return Image(width,height,std::vector<uint8_t>(source,source+size_t(width)*height*4));
        std::vector<uint8_t> pixels(size_t(outputWidth)*outputHeight*4);
        for(uint32_t y=0;y<outputHeight;++y)
            for(uint32_t x=0;x<outputWidth;++x)
            {
                const auto sx=static_cast<uint32_t>(uint64_t(x)*width/outputWidth);
                const auto sy=static_cast<uint32_t>(uint64_t(y)*height/outputHeight);
                std::memcpy(pixels.data()+(size_t(y)*outputWidth+x)*4,
                    source+(size_t(sy)*width+sx)*4,4);
            }
        return Image(outputWidth,outputHeight,std::move(pixels));
    }
}

// 파일 헤더가 DDS이면 전용 경로로 mip 0을 읽고, 나머지는 stb로 RGBA8을 만든다. 성공한 이미지에 출처 경로를 붙인다.
static bool DecodeImage(const std::string& path, Image& result)
{
    if(path.empty()) return false; // 경로가 비어 있으면 파일 읽기를 시작하지 않는다.
    std::ifstream file;
    std::array<uint8_t,148> prefix={};size_t prefixSize=0;uint64_t fileBytes=0;
    if(ReadImagePrefix(path,file,prefix,prefixSize,fileBytes) && IsDds(prefix.data(),prefixSize)) // DDS 헤더가 확인되면 전용 디코더로 분기한다.
    {
        DdsInfo info;
        if(!ParseDds(prefix.data(),prefixSize,fileBytes,info)) return false; // DDS 헤더와 선언된 전체 mip 길이 검증에 실패하면 디코딩하지 않는다.
        if(info.topBytes>static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) // 스트림 API가 한 번에 표현할 수 없는 읽기 크기는 거부한다.
            return DdsFailure("encoded mip exceeds the stream size limit.");
        const uint64_t readBytes=PreservedDdsFormat(info)!=RHI::Format::Unknown ? info.mipBytes : info.topBytes;
        if(readBytes>SIZE_MAX || readBytes>uint64_t(std::numeric_limits<std::streamsize>::max()))
            return DdsFailure("encoded mip chain exceeds the stream size limit.");
        std::vector<uint8_t> encoded(static_cast<size_t>(readBytes));
        file.seekg(static_cast<std::streamoff>(info.offset));
        if(!file.read(reinterpret_cast<char*>(encoded.data()),static_cast<std::streamsize>(encoded.size()))) // 최상위 mip 전체를 읽지 못하면 잘린 입력으로 실패한다.
            return DdsFailure("failed to read the complete top mip.");
        Image image;
        if(!DecodeDdsPixels(info,encoded.data(),UINT64_MAX,image,nullptr)) return false; // DDS 픽셀 변환이 실패하면 기존 result를 유지한다.
        PreserveDdsBlocks(info,encoded.data(),image);
        image.SetSourcePath(path);result=std::move(image);return true;
    }
    int width = 0, height = 0;
    DecodedPixels pixels(stbi_load(path.c_str(), &width, &height, nullptr, 4), stbi_image_free);
    size_t bytes = 0;
    if(!pixels || !DecodedByteSize(width, height, UINT64_MAX, bytes)) return false; // stb 디코딩 결과나 RGBA 용량이 잘못되면 실패한다.

    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        std::vector<uint8_t>(pixels.get(), pixels.get() + bytes));
    image.SetSourcePath(path);
    result = std::move(image);
    return true;
}

// 메모리 입력은 디코딩 전후의 크기를 검사한다. 용량 문제는 outLimitExceeded로 별도 알리고 result는 성공할 때 교체한다.
static bool DecodeImage(const uint8_t* encodedBytes, size_t encodedSize, Image& result,
    uint64_t maxDecodedBytes, uint32_t maxDimension, bool* outLimitExceeded)
{
    if(outLimitExceeded) *outLimitExceeded = false; // 이번 호출의 용량 초과 상태를 먼저 초기화한다.
    if(!encodedBytes || !encodedSize) return false; // 메모리 주소나 데이터가 없으면 디코딩하지 않는다.
    if(IsDds(encodedBytes,encodedSize)) // DDS는 stb의 길이 제한 검사 전에 전용 경로로 처리한다.
    {
        DdsInfo info;
        if(!ParseDds(encodedBytes,encodedSize,encodedSize,info)) return false; // 메모리 DDS의 헤더와 실제 길이가 맞지 않으면 거부한다.
        SelectDdsMip(info,maxDimension);
        Image image;
        if(!DecodeDdsPixels(info,encodedBytes+info.offset,maxDecodedBytes,image,outLimitExceeded)) return false;
        if(maxDimension && std::max(info.width,info.height)>maxDimension)
            image=CopyBoundedPixels(image.GetPixels().data(),info.width,info.height,maxDimension);
        else PreserveDdsBlocks(info,encodedBytes+info.offset,image);
        result=std::move(image);
        return true;
    }
    if(encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) return false; // stb 길이 인자의 int 범위를 넘는 메모리 입력은 거부한다.

    int width = 0, height = 0;
    const auto size = static_cast<int>(encodedSize);
    if(!stbi_info_from_memory(encodedBytes, size, &width, &height, nullptr)) return false; // 픽셀 할당 전에 헤더의 해상도를 얻지 못하면 실패한다.
    size_t bytes = 0;
    if(!DecodedByteSize(width, height, maxDecodedBytes, bytes)) // 헤더에서 예상한 출력 용량을 할당 전에 제한한다.
    {
        if(outLimitExceeded && width > 0 && height > 0) *outLimitExceeded = true; // 양수 해상도의 용량 계산 실패이면 호출자가 요청한 초과 표식을 켠다.
        return false;
    }
    DecodedPixels pixels(stbi_load_from_memory(encodedBytes, size, &width, &height, nullptr, 4), stbi_image_free);
    if(!pixels) return false; // stb가 픽셀을 만들지 못했으면 결과를 교체하지 않는다.
    if(!DecodedByteSize(width, height, maxDecodedBytes, bytes)) // 실제 디코더 반환 크기를 다시 확인한다.
    {
        if(outLimitExceeded && width > 0 && height > 0) *outLimitExceeded = true; // 실제 디코딩 크기의 용량 검증 실패도 초과 표식으로 알린다.
        return false;
    }

    Image image=CopyBoundedPixels(pixels.get(),static_cast<uint32_t>(width),static_cast<uint32_t>(height),maxDimension);
    result = std::move(image);
    return true;
}
// 파일 디코딩 실패와 예외를 진단한 뒤 false로 돌려준다.
bool LoadImage(const std::string& path, Image& result)
{
    try
    {
        if(DecodeImage(path,result)) return true; // 파일 디코딩이 성공하면 오류 출력 없이 성공을 반환한다.
        std::fprintf(stderr,"dyf: image load failed: %s\n",path.c_str());
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: image load: %s\n",error.what()); }
    return false;
}
// 픽셀을 만들지 않고 크기를 조회한다. DDS는 전체 파일 길이까지 검사하며 실패 시 출력 크기는 0이다.
bool GetImageDimensions(const std::string& path, uint32_t& width, uint32_t& height)
{
    return GetImageDimensions(path,width,height,0);
}
bool GetImageDimensions(const std::string& path, uint32_t& width, uint32_t& height,uint32_t maxDimension)
{
    width = height = 0;
    std::ifstream file;
    std::array<uint8_t,148> prefix={};size_t prefixSize=0;uint64_t fileBytes=0;
    if(ReadImagePrefix(path,file,prefix,prefixSize,fileBytes) && IsDds(prefix.data(),prefixSize)) // DDS이면 전용 헤더 검증에서 해상도를 얻는다.
    {
        DdsInfo info;
        if(!ParseDds(prefix.data(),prefixSize,fileBytes,info)) return false; // DDS 메타데이터 검증에 실패하면 크기 조회도 실패한다.
        SelectDdsMip(info,maxDimension);
        width=info.width;height=info.height;
        LimitImageDimensions(width,height,maxDimension);
        return true;
    }
    int imageWidth = 0, imageHeight = 0;
    if(path.empty() || !stbi_info(path.c_str(), &imageWidth, &imageHeight, nullptr) || // 빈 경로·헤더 조회 실패·양수가 아닌 크기이면 크기 조회를 실패시킨다.
        imageWidth <= 0 || imageHeight <= 0)
    {
        std::fprintf(stderr, "dyf: image metadata load failed: %s\n", path.c_str());
        return false;
    }
    width = static_cast<uint32_t>(imageWidth);
    height = static_cast<uint32_t>(imageHeight);
    LimitImageDimensions(width,height,maxDimension);
    return true;
}
// 메모리 디코더의 예외를 잡아 false로 변환한다. limit은 압축 해제한 RGBA 픽셀 용량의 상한이다.
bool LoadImage(const uint8_t* bytes,size_t size,Image& result,uint64_t limit,bool* exceeded)
{
    return LoadImage(bytes,size,result,limit,0,exceeded);
}
bool LoadImage(const uint8_t* bytes,size_t size,Image& result,uint64_t limit,uint32_t maxDimension,bool* exceeded)
{
    try
    {
        if(DecodeImage(bytes,size,result,limit,maxDimension,exceeded)) return true; // 메모리 디코딩이 성공하면 완성된 result를 그대로 반환한다.
        std::fprintf(stderr,"dyf: image decode failed or decoded byte limit exceeded.\n");
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: image decode: %s\n",error.what()); }
    return false;
}

}

namespace dyf
{
// 입력 벡터를 불변 공유 저장소로 옮긴다. 생성 시 크기 불일치를 거부하지 않으므로 IsValid로 확인한다.
Image::Image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba8, ColorSpace colorSpace)
    : m_width(width), m_height(height),
      m_pixels(std::make_shared<Detail::ImagePixels>(std::move(rgba8))), m_colorSpace(colorSpace)
{
}

// 양수 해상도와 RGBA 네 성분 단위 및 정확한 픽셀 개수를 검사한다.
bool Image::IsValid() const
{
    return m_width && m_height && m_pixels && m_pixels->byteCount % 4 == 0 &&
        static_cast<uint64_t>(m_width) * m_height == m_pixels->byteCount / 4;
}

// 저장소가 없으면 공통 빈 배열을 반환해 읽는 쪽에 항상 유효한 참조를 준다.
const std::vector<uint8_t>& Image::GetPixels() const
{
    static const std::vector<uint8_t> empty;
    if(!m_pixels)return empty;
    if(!m_pixels->decoded.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(m_pixels->mutex);
        if(!m_pixels->decoded.load(std::memory_order_relaxed))
        {
            if(!RestorableDds(m_dds.get(),m_width,m_height))throw std::runtime_error("Invalid preserved DDS cache.");
            DdsInfo info;info.width=m_width;info.height=m_height;
            info.format=m_dds->format==RHI::Format::BC1_UNORM?DdsFormat::BC1:
                m_dds->format==RHI::Format::BC3_UNORM?DdsFormat::BC3:DdsFormat::BC5;
            info.normalMap=m_dds->normalMap;
            info.blockBytes=info.format==DdsFormat::BC1?8:16;
            Image decoded;
            if(!DecodeDdsPixels(info,m_dds->mips[0].data(),m_pixels->byteCount,decoded,nullptr))
                throw std::runtime_error("Preserved DDS reconstruction failed.");
            m_pixels->pixels=std::move(decoded.m_pixels->pixels);
            m_pixels->decoded.store(true,std::memory_order_release);
        }
    }
    return m_pixels->pixels;
}

uint64_t Image::ReleaseDecodedPixels() const
{
    if(!IsValid() || !RestorableDds(m_dds.get(),m_width,m_height))return 0;
    std::lock_guard<std::mutex> lock(m_pixels->mutex);
    const uint64_t bytes=m_pixels->pixels.capacity();
    std::vector<uint8_t>().swap(m_pixels->pixels);
    m_pixels->decoded.store(false,std::memory_order_release);
    return bytes;
}

// 이미지의 출처 문자열만 교체한다. 픽셀이나 GPU 캐시를 수정하지 않는다.
void Image::SetSourcePath(std::string path)
{
    m_sourcePath = std::move(path);
}

// float RGBA 배열을 불변 공유 저장소로 옮겨 HDR 복사본끼리 픽셀을 공유한다.
HdrImage::HdrImage(uint32_t width,uint32_t height,std::vector<float> rgba)
    :m_width(width),m_height(height),m_pixels(std::make_shared<const std::vector<float>>(std::move(rgba))) {}

// 해상도와 float 성분 개수를 검사한다. 값의 유한성 검사는 파일 로더에서 따로 한다.
bool HdrImage::IsValid() const
{
    return m_width && m_height && m_pixels && m_pixels->size()%4==0 &&
        uint64_t(m_width)*m_height==m_pixels->size()/4;
}

// HDR 픽셀이 없으면 공통 빈 배열을 반환한다.
const std::vector<float>& HdrImage::GetPixels() const
{
    static const std::vector<float> empty;
    return m_pixels ? *m_pixels : empty;
}

// HDR 파일 여부·해상도·할당 범위를 확인하고 float로 디코딩한다. 모든 성분이 유한하고 음수가 아닐 때만 결과를 교체한다.
bool LoadHdrImage(const std::string& path,HdrImage& image)
{
    return LoadHdrImage(path,image,0);
}
bool LoadHdrImage(const std::string& path,HdrImage& image,uint32_t maxDimension)
{
    try
    {
        int width=0,height=0;
        if(path.empty() || !stbi_is_hdr(path.c_str()) || !stbi_info(path.c_str(),&width,&height,nullptr) || // HDR 여부·헤더 크기·float RGBA 배열 용량 조건을 검사한다.
            width<=0 || height<=0 || uint64_t(width)*height>SIZE_MAX/(4*sizeof(float)))
        {std::fprintf(stderr,"dyf: invalid HDR image: %s\n",path.c_str());return false;}
        std::unique_ptr<float,decltype(&stbi_image_free)> pixels(
            stbi_loadf(path.c_str(),&width,&height,nullptr,4),stbi_image_free);
        if(!pixels) {std::fprintf(stderr,"dyf: HDR load failed: %s\n",path.c_str());return false;} // HDR 디코더가 픽셀을 반환하지 못하면 진단하고 실패한다.
        if(width<=0 || height<=0 || uint64_t(width)*height>SIZE_MAX/(4*sizeof(float)))return false;
        const size_t count=size_t(width)*height*4;
        // 특정 방향에서 들어오거나 나가는 빛의 세기를 나타내는 값(방사휘도)은 RGB에 저장된다.
        // 알파도 함께 검사하며, 검사에 실패하면 기존 image를 교체하지 않는다.
        for(size_t i=0;i<count;++i)
            if(!std::isfinite(pixels.get()[i]) || pixels.get()[i]<0) // NaN·무한대·음수 성분이 하나라도 있으면 실패한다.
            {std::fprintf(stderr,"dyf: HDR image contains invalid radiance: %s\n",path.c_str());return false;}
        uint32_t outputWidth=static_cast<uint32_t>(width),outputHeight=static_cast<uint32_t>(height);
        LimitImageDimensions(outputWidth,outputHeight,maxDimension);
        if(outputWidth==static_cast<uint32_t>(width) && outputHeight==static_cast<uint32_t>(height))
            image=HdrImage(outputWidth,outputHeight,std::vector<float>(pixels.get(),pixels.get()+count));
        else
        {
            // Copy only retained pixels; the decoder already owns the temporary
            // full float image. Never build a second full-resolution vector.
            std::vector<float> retained(size_t(outputWidth)*outputHeight*4);
            for(uint32_t y=0;y<outputHeight;++y)
                for(uint32_t x=0;x<outputWidth;++x)
                {
                    const auto sx=static_cast<uint32_t>(uint64_t(x)*width/outputWidth);
                    const auto sy=static_cast<uint32_t>(uint64_t(y)*height/outputHeight);
                    std::memcpy(retained.data()+(size_t(y)*outputWidth+x)*4,
                        pixels.get()+(size_t(sy)*width+sx)*4,4*sizeof(float));
                }
            image=HdrImage(outputWidth,outputHeight,std::move(retained));
        }
        return true;
    }
    catch(const std::exception& error) {std::fprintf(stderr,"dyf: HDR load: %s\n",error.what());return false;}
}
}

