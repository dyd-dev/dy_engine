#include "dyf/Image.h"

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

// 파일과 모델 내장 이미지의 디코딩은 이 CPU 이미지 로더에서 담당한다.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// bcdec 0.985, vendored unchanged under MIT; see ThirdParty/bcdec/ORIGIN.txt.
#define BCDECDEF static inline
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../ThirdParty/bcdec/bcdec.h"
#undef BCDEC_IMPLEMENTATION
#undef BCDEC_BC4BC5_PRECISE
#undef BCDECDEF

namespace dyf
{
namespace
{
    using DecodedPixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;

    enum class DdsFormat : uint8_t {BC1,BC2,BC3,BC4,BC5,BC7,RGBA8,BGRA8,RGBX8,BGRX8};
    struct DdsInfo
    {
        uint32_t width=0,height=0,mipCount=1,blockBytes=0;
        size_t offset=128,topBytes=0,rowPitch=0;
        DdsFormat format=DdsFormat::BC1;
        bool normalMap=false,swapRedGreen=false,swapRedAlpha=false;
    };

    uint32_t ReadU32(const uint8_t* bytes)
    {
        return uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24);
    }
    constexpr uint32_t FourCC(char a,char b,char c,char d)
    {
        return uint32_t(uint8_t(a))|(uint32_t(uint8_t(b))<<8)|(uint32_t(uint8_t(c))<<16)|(uint32_t(uint8_t(d))<<24);
    }
    bool IsDds(const uint8_t* bytes,size_t size)
    {
        return bytes && size>=4 && ReadU32(bytes)==FourCC('D','D','S',' ');
    }
    bool DdsFailure(const char* reason)
    {
        std::fprintf(stderr,"dyf: DDS image: %s\n",reason);return false;
    }

    // Parse little-endian fields individually: input bytes need not be aligned.
    // Validate the complete declared mip chain, while decoding only mip zero;
    // TextureCache builds the renderer's normal RGBA mip chain from that image.
    bool ParseDds(const uint8_t* bytes,size_t headerBytes,uint64_t fileBytes,DdsInfo& info)
    {
        if(!IsDds(bytes,headerBytes) || headerBytes<128 || ReadU32(bytes+4)!=124 || ReadU32(bytes+76)!=32)
            return DdsFailure("invalid or truncated header.");
        info.width=ReadU32(bytes+16);info.height=ReadU32(bytes+12);
        if(!info.width || !info.height || info.width>INT32_MAX || info.height>INT32_MAX)
            return DdsFailure("invalid image dimensions.");
        if(ReadU32(bytes+24)>1 || (ReadU32(bytes+112)&0x0020fe00u)!=0)
            return DdsFailure("only a single 2D image is supported; volume/cubemap DDS is unsupported.");
        const uint32_t flags=ReadU32(bytes+8),pixelFlags=ReadU32(bytes+80),code=ReadU32(bytes+84);
        info.normalMap=(pixelFlags&0x80000000u)!=0;
        if(pixelFlags&4u)
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
                // ATI2n reverses BC5 component order; NVIDIA A2XY reverses it
                // again. Bistro's NVTT DDS files contain both and cancel out.
                info.swapRedGreen=(code==FourCC('A','T','I','2'))!=(ReadU32(bytes+88)==FourCC('A','2','X','Y'));
                break;
            case FourCC('D','X','1','0'):
            {
                if(headerBytes<148) return DdsFailure("truncated DX10 header.");
                if(ReadU32(bytes+132)!=3 || ReadU32(bytes+140)!=1 || (ReadU32(bytes+136)&4u)!=0)
                    return DdsFailure("DX10 arrays, cubemaps and non-2D textures are unsupported.");
                if((ReadU32(bytes+144)&7u)==2u)
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
            info.swapRedAlpha=code==FourCC('D','X','T','5') && ReadU32(bytes+88)==FourCC('A','2','D','5');
        }
        else
        {
            if((pixelFlags&0x40u)==0 || ReadU32(bytes+88)!=32 || ReadU32(bytes+96)!=0x0000ff00u)
                return DdsFailure("unsupported uncompressed pixel format; only RGBA/BGRA 32-bit layouts are supported.");
            const uint32_t red=ReadU32(bytes+92),blue=ReadU32(bytes+100),alpha=ReadU32(bytes+104);
            if(alpha!=0 && alpha!=0xff000000u) return DdsFailure("unsupported alpha channel mask.");
            if(red==0x000000ffu && blue==0x00ff0000u) info.format=alpha ? DdsFormat::RGBA8 : DdsFormat::RGBX8;
            else if(red==0x00ff0000u && blue==0x000000ffu) info.format=alpha ? DdsFormat::BGRA8 : DdsFormat::BGRX8;
            else return DdsFailure("unsupported RGB channel masks.");
        }
        info.mipCount=std::max(1u,ReadU32(bytes+28));
        uint32_t maxMips=1;
        for(uint32_t size=std::max(info.width,info.height);size>1;size>>=1) ++maxMips;
        if(info.mipCount>maxMips) return DdsFailure("mip count exceeds image dimensions.");
        uint64_t required=info.offset;
        uint32_t width=info.width,height=info.height;
        for(uint32_t level=0;level<info.mipCount;++level)
        {
            uint64_t row=info.blockBytes ? ((uint64_t(width)+3)/4)*info.blockBytes : uint64_t(width)*4;
            const uint64_t rows=info.blockBytes ? (uint64_t(height)+3)/4 : height;
            if(!info.blockBytes && level==0 && (flags&8u))
            {
                const uint32_t declaredPitch=ReadU32(bytes+20);
                if(declaredPitch<row) return DdsFailure("declared row pitch is smaller than a pixel row.");
                row=declaredPitch;
            }
            if(row>SIZE_MAX || rows>UINT64_MAX/row) return DdsFailure("image byte count overflows.");
            const uint64_t size=row*rows;
            if(size>SIZE_MAX || required>UINT64_MAX-size) return DdsFailure("mip byte count overflows.");
            if(level==0) {info.rowPitch=static_cast<size_t>(row);info.topBytes=static_cast<size_t>(size);}
            required+=size;width=std::max(1u,width/2);height=std::max(1u,height/2);
        }
        if(required>fileBytes) return DdsFailure("file is truncated within its declared mip chain.");
        return true;
    }

    bool ReadImagePrefix(const std::string& path,std::ifstream& file,std::array<uint8_t,148>& prefix,
        size_t& prefixSize,uint64_t& fileBytes)
    {
        file.open(path,std::ios::binary|std::ios::ate);
        if(!file) return false;
        const auto end=file.tellg();
        if(end<0) return false;
        fileBytes=static_cast<uint64_t>(end);
        prefixSize=static_cast<size_t>(std::min<uint64_t>(prefix.size(),fileBytes));
        file.seekg(0);
        return bool(file.read(reinterpret_cast<char*>(prefix.data()),static_cast<std::streamsize>(prefixSize)));
    }

    bool DecodeDdsPixels(const DdsInfo& info,const uint8_t* source,uint64_t limit,Image& result,bool* exceeded)
    {
        const uint64_t required=uint64_t(info.width)*info.height*4;
        if(required>limit || required>SIZE_MAX)
        {
            if(exceeded) *exceeded=true;
            return DdsFailure("decoded RGBA image exceeds the memory budget.");
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(required));
        if(!info.blockBytes)
        {
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
                    if(info.format==DdsFormat::BC4)
                    {target[0]=pixels[index];target[1]=target[2]=0;target[3]=255;}
                    else if(info.format==DdsFormat::BC5)
                    {
                        target[0]=pixels[index*2];target[1]=pixels[index*2+1];target[2]=0;target[3]=255;
                    }
                    else std::memcpy(target,pixels.data()+index*4,4);
                    if(info.swapRedGreen) std::swap(target[0],target[1]);
                    if(info.swapRedAlpha) std::swap(target[0],target[3]);
                    if(info.normalMap && (info.format==DdsFormat::BC5 || info.swapRedAlpha))
                    {
                        const float nx=target[0]*(2.0f/255)-1,ny=target[1]*(2.0f/255)-1;
                        const float nz=std::sqrt(std::max(0.0f,1-nx*nx-ny*ny));
                        target[2]=static_cast<uint8_t>(std::lround((nz*.5f+.5f)*255));
                    }
                }
            }
        }
        result=Image(info.width,info.height,std::move(rgba));return true;
    }

    bool DecodedByteSize(int width, int height, uint64_t limit, size_t& bytes)
    {
        if(width <= 0 || height <= 0) return false;
        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if(pixels > std::numeric_limits<uint64_t>::max() / 4u) return false;
        const uint64_t size = pixels * 4u;
        if(size > limit || size > std::numeric_limits<size_t>::max()) return false;
        bytes = static_cast<size_t>(size);
        return true;
    }
}

static bool DecodeImage(const std::string& path, Image& result)
{
    if(path.empty()) return false;
    std::ifstream file;
    std::array<uint8_t,148> prefix={};size_t prefixSize=0;uint64_t fileBytes=0;
    if(ReadImagePrefix(path,file,prefix,prefixSize,fileBytes) && IsDds(prefix.data(),prefixSize))
    {
        DdsInfo info;
        if(!ParseDds(prefix.data(),prefixSize,fileBytes,info)) return false;
        if(info.topBytes>static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
            return DdsFailure("encoded mip exceeds the stream size limit.");
        std::vector<uint8_t> encoded(info.topBytes);
        file.seekg(static_cast<std::streamoff>(info.offset));
        if(!file.read(reinterpret_cast<char*>(encoded.data()),static_cast<std::streamsize>(encoded.size())))
            return DdsFailure("failed to read the complete top mip.");
        Image image;
        if(!DecodeDdsPixels(info,encoded.data(),UINT64_MAX,image,nullptr)) return false;
        image.SetSourcePath(path);result=std::move(image);return true;
    }
    int width = 0, height = 0;
    DecodedPixels pixels(stbi_load(path.c_str(), &width, &height, nullptr, 4), stbi_image_free);
    size_t bytes = 0;
    if(!pixels || !DecodedByteSize(width, height, UINT64_MAX, bytes)) return false;

    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        std::vector<uint8_t>(pixels.get(), pixels.get() + bytes));
    image.SetSourcePath(path);
    result = std::move(image);
    return true;
}

static bool DecodeImage(const uint8_t* encodedBytes, size_t encodedSize, Image& result,
    uint64_t maxDecodedBytes, bool* outLimitExceeded)
{
    if(outLimitExceeded) *outLimitExceeded = false;
    if(!encodedBytes || !encodedSize) return false;
    if(IsDds(encodedBytes,encodedSize))
    {
        DdsInfo info;
        if(!ParseDds(encodedBytes,encodedSize,encodedSize,info)) return false;
        return DecodeDdsPixels(info,encodedBytes+info.offset,maxDecodedBytes,result,outLimitExceeded);
    }
    if(encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

    int width = 0, height = 0;
    const auto size = static_cast<int>(encodedSize);
    if(!stbi_info_from_memory(encodedBytes, size, &width, &height, nullptr)) return false;
    size_t bytes = 0;
    if(!DecodedByteSize(width, height, maxDecodedBytes, bytes))
    {
        if(outLimitExceeded && width > 0 && height > 0) *outLimitExceeded = true;
        return false;
    }
    DecodedPixels pixels(stbi_load_from_memory(encodedBytes, size, &width, &height, nullptr, 4), stbi_image_free);
    if(!pixels) return false;
    if(!DecodedByteSize(width, height, maxDecodedBytes, bytes))
    {
        if(outLimitExceeded && width > 0 && height > 0) *outLimitExceeded = true;
        return false;
    }

    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        std::vector<uint8_t>(pixels.get(), pixels.get() + bytes));
    result = std::move(image);
    return true;
}
bool LoadImage(const std::string& path, Image& result)
{
    try
    {
        if(DecodeImage(path,result)) return true;
        std::fprintf(stderr,"dyf: image load failed: %s\n",path.c_str());
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: image load: %s\n",error.what()); }
    return false;
}
bool GetImageDimensions(const std::string& path, uint32_t& width, uint32_t& height)
{
    width = height = 0;
    std::ifstream file;
    std::array<uint8_t,148> prefix={};size_t prefixSize=0;uint64_t fileBytes=0;
    if(ReadImagePrefix(path,file,prefix,prefixSize,fileBytes) && IsDds(prefix.data(),prefixSize))
    {
        DdsInfo info;
        if(!ParseDds(prefix.data(),prefixSize,fileBytes,info)) return false;
        width=info.width;height=info.height;return true;
    }
    int imageWidth = 0, imageHeight = 0;
    if(path.empty() || !stbi_info(path.c_str(), &imageWidth, &imageHeight, nullptr) ||
        imageWidth <= 0 || imageHeight <= 0)
    {
        std::fprintf(stderr, "dyf: image metadata load failed: %s\n", path.c_str());
        return false;
    }
    width = static_cast<uint32_t>(imageWidth);
    height = static_cast<uint32_t>(imageHeight);
    return true;
}
bool LoadImage(const uint8_t* bytes,size_t size,Image& result,uint64_t limit,bool* exceeded)
{
    try
    {
        if(DecodeImage(bytes,size,result,limit,exceeded)) return true;
        std::fprintf(stderr,"dyf: image decode failed or decoded byte limit exceeded.\n");
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: image decode: %s\n",error.what()); }
    return false;
}

}

namespace dyf
{
Image::Image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba8, ColorSpace colorSpace)
    : m_width(width), m_height(height),
      m_pixels(std::make_shared<const std::vector<uint8_t>>(std::move(rgba8))), m_colorSpace(colorSpace)
{
}

bool Image::IsValid() const
{
    return m_width && m_height && m_pixels && m_pixels->size() % 4 == 0 &&
        static_cast<uint64_t>(m_width) * m_height == m_pixels->size() / 4;
}

const std::vector<uint8_t>& Image::GetPixels() const
{
    static const std::vector<uint8_t> empty;
    return m_pixels ? *m_pixels : empty;
}

void Image::SetSourcePath(std::string path)
{
    m_sourcePath = std::move(path);
}

HdrImage::HdrImage(uint32_t width,uint32_t height,std::vector<float> rgba)
    :m_width(width),m_height(height),m_pixels(std::make_shared<const std::vector<float>>(std::move(rgba))) {}

bool HdrImage::IsValid() const
{
    return m_width && m_height && m_pixels && m_pixels->size()%4==0 &&
        uint64_t(m_width)*m_height==m_pixels->size()/4;
}

const std::vector<float>& HdrImage::GetPixels() const
{
    static const std::vector<float> empty;
    return m_pixels ? *m_pixels : empty;
}

bool LoadHdrImage(const std::string& path,HdrImage& image)
{
    try
    {
        int width=0,height=0;
        if(path.empty() || !stbi_is_hdr(path.c_str()) || !stbi_info(path.c_str(),&width,&height,nullptr) ||
            width<=0 || height<=0 || uint64_t(width)*height>SIZE_MAX/(4*sizeof(float)))
        {std::fprintf(stderr,"dyf: invalid HDR image: %s\n",path.c_str());return false;}
        std::unique_ptr<float,decltype(&stbi_image_free)> pixels(
            stbi_loadf(path.c_str(),&width,&height,nullptr,4),stbi_image_free);
        if(!pixels) {std::fprintf(stderr,"dyf: HDR load failed: %s\n",path.c_str());return false;}
        const size_t count=size_t(width)*height*4;
        for(size_t i=0;i<count;++i)
            if(!std::isfinite(pixels.get()[i]) || pixels.get()[i]<0)
            {std::fprintf(stderr,"dyf: HDR image contains invalid radiance: %s\n",path.c_str());return false;}
        image=HdrImage(static_cast<uint32_t>(width),static_cast<uint32_t>(height),
            std::vector<float>(pixels.get(),pixels.get()+count));
        return true;
    }
    catch(const std::exception& error) {std::fprintf(stderr,"dyf: HDR load: %s\n",error.what());return false;}
}
}

