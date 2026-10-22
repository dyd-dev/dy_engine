#include "dyf/Image.h"
#include "dyf/Canvas.h"
#include "../Platform/Profiler/CollectorInternal.h"
#include "dyf/Font.h"
#include "dyf/Utf8.h"
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace dyf
{
// 크기를 저장하고 전체 영역으로 초기화하며, 관측 도구가 이 Canvas를 추적할 수 있도록 수명 ID를 등록한다.
Canvas::Canvas(uint32_t width, uint32_t height)
{
    liveId=Platform::RegisterLiveObject(this,{Platform::LiveObjectKind::Canvas,0,0,0,0,"Canvas"});
    if(!width || !height) std::fprintf(stderr, "dyf: Canvas dimensions must be positive.\n"); // 크기가 0이면 진단하되 객체 생성은 계속한다.
    this->width = width; this->height = height;
    viewport = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    ResetClipRect();
}
// 관측 객체를 종료한다. 정점 배열과 Draw가 공유한 이미지 참조는 멤버 소멸로 정리된다.
Canvas::~Canvas() {Platform::RetireLiveObject(liveId);}
// 이동 대입으로 기록 내용과 관측 ID를 가져오며, 이동된 원본 Canvas는 유효하지 않은 상태가 된다.
Canvas::Canvas(Canvas&& other) noexcept { *this=std::move(other); }
// 기존 관측 ID를 종료하고 원본 ID를 새 주소에 연결한다. 원본의 크기와 ID는 0이 된다.
Canvas& Canvas::operator=(Canvas&& other) noexcept
{
    if(this!=&other) // 자기 자신으로의 이동이 아니면 이전 자원을 놓고 원본 상태를 가져온다.
    {
        Platform::RetireLiveObject(liveId);liveId=std::exchange(other.liveId,0);Platform::RebindLiveObject(liveId,this);
        width=std::exchange(other.width,0);height=std::exchange(other.height,0);
        clearColor=other.clearColor;viewport=other.viewport;clip=other.clip;
        vertices=std::move(other.vertices);draws=std::move(other.draws);
    }
    return *this;
}
// 새 배경색을 보관하고 기록을 비운다. viewport와 clip도 Canvas 전체 크기로 되돌린다.
void Canvas::Clear(Math::float4 color)
{
    clearColor = color;
    vertices.clear();
    draws.clear();
    viewport = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    ResetClipRect();
}
// 폭과 높이의 존재만 확인한다. GPU 장치나 기록된 정점의 유효성을 검사하지는 않는다.
bool Canvas::IsValid() const { return width && height; }
// 이후 Draw에 쓸 viewport를 검증·저장하고 clip을 그 영역 전체로 초기화한다.
bool Canvas::SetViewport(Rectangle viewport)
{
    if(!std::isfinite(viewport.x + viewport.y + viewport.width + viewport.height) || viewport.width <= 0 || viewport.height <= 0) // 좌표·크기 합이 비유한 값이거나 크기가 0 이하이면 이전 viewport를 유지하고 실패한다.
        { std::fprintf(stderr, "dyf: invalid canvas viewport.\n"); return false; }
    this->viewport = viewport; ResetClipRect();
    return true;
}
// clip은 viewport 내부 좌표로 보관한다. 화면 경계와의 교집합은 GPU 명령 기록 때 구한다.
void Canvas::SetClipRect(Rectangle clip) { this->clip = clip; }
// 잘라내기 범위를 현재 viewport 전체로 되돌린다.
void Canvas::ResetClipRect() { clip = {0, 0, viewport.width, viewport.height}; }
// 색을 가진 정점 세 개와 Draw 하나를 추가한다. 이미지가 없으면 렌더러의 흰색 텍스처를 쓴다.
void Canvas::Triangle(Math::float2 a, Math::float2 b, Math::float2 c, Math::float4 color)
{
    Draw draw{static_cast<uint32_t>(vertices.size()), 3, {}, viewport, clip};
    for(auto point : {a, b, c}) vertices.push_back({point.x, point.y, 0, 0, color.x, color.y, color.z, color.w});
    draws.push_back(std::move(draw));
}
// 사각형을 삼각형 두 개로 풀고, 목적 위치·UV·색·이미지를 같은 Draw에 연결한다.
void Canvas::Quad(Rectangle r, Rectangle uv, Math::float4 color, const dyf::Image& image)
{
    if(r.width <= 0 || r.height <= 0) return; // 면적이 없으면 아무 정점도 추가하지 않는다.
    Draw draw{static_cast<uint32_t>(vertices.size()), 6, image, viewport, clip};
    for(auto p : {Math::float2{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}})
        vertices.push_back({r.x + p.x * r.width, r.y + p.y * r.height, uv.x + p.x * uv.width, uv.y + p.y * uv.height,
            color.x, color.y, color.z, color.w});
    draws.push_back(std::move(draw));
}
// 점의 중심을 기준으로 작은 사각형을 기록한다.
void Canvas::Point(Math::float2 p, Math::float4 color, float size) { FillRect({p.x - size / 2, p.y - size / 2, size, size}, color); }
// 두 끝점의 차이를 선 길이로 나누고 90도 돌린 뒤 두께의 절반을 곱해, 선 중심에서 양옆으로 벌릴 벡터를 구한다.
// 두 끝점에 이 벡터를 더하고 빼서 네 꼭짓점을 만들고 삼각형 두 개로 기록한다.
void Canvas::Line(Math::float2 a, Math::float2 b, Math::float4 color, float width)
{
    if(width <= 0) return; // 선 두께가 양수가 아니면 기록하지 않는다.
    const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
    if(length <= 0) { Point(a, color, width); return; } // 양 끝점이 같으면 같은 두께의 점으로 대신 기록한다.
    const float nx = -dy / length * width / 2, ny = dx / length * width / 2;
    const Math::float2 p{a.x + nx, a.y + ny}, q{b.x + nx, b.y + ny}, r{a.x - nx, a.y - ny}, s{b.x - nx, b.y - ny};
    Triangle(p, q, r, color); Triangle(r, q, s, color);
}
// 사각형의 네 변을 각각 두께 있는 Line으로 기록한다.
void Canvas::Rect(Rectangle r, Math::float4 color, float width)
{
    Line({r.x,r.y}, {r.x+r.width,r.y}, color,width); Line({r.x+r.width,r.y}, {r.x+r.width,r.y+r.height},color,width);
    Line({r.x+r.width,r.y+r.height}, {r.x,r.y+r.height},color,width); Line({r.x,r.y+r.height}, {r.x,r.y},color,width);
}
// 이미지 없는 Quad로 단색 채움 사각형을 기록한다.
void Canvas::FillRect(Rectangle r, Math::float4 color) { Quad(r, {}, color); }
// 둘레를 64개 구간으로 나누고 인접 점 사이에 Line을 기록한다.
void Canvas::Circle(Math::float2 c, float radius, Math::float4 color, float width)
{
    if(radius <= 0) return; // 반지름이 양수가 아니면 원 둘레를 기록하지 않는다.
    for(uint32_t i=0; i<64; ++i) { const float a=i*6.283185307f/64,b=(i+1)*6.283185307f/64;
        Line({c.x+radius*std::cos(a),c.y+radius*std::sin(a)}, {c.x+radius*std::cos(b),c.y+radius*std::sin(b)},color,width); }
}
// 중심과 둘레의 인접 점을 잇는 64개 삼각형으로 원 내부를 채운다.
void Canvas::FillCircle(Math::float2 c, float radius, Math::float4 color)
{
    if(radius <= 0) return; // 반지름이 양수가 아니면 채움 삼각형을 기록하지 않는다.
    for(uint32_t i=0; i<64; ++i) { const float a=i*6.283185307f/64,b=(i+1)*6.283185307f/64;
        Triangle(c,{c.x+radius*std::cos(a),c.y+radius*std::sin(a)},{c.x+radius*std::cos(b),c.y+radius*std::sin(b)},color); }
}
// 유효한 RGBA8 이미지를 전체 UV 범위로 기록한다. tint는 정점 색으로 전달된다.
void Canvas::Image(const dyf::Image& image, Rectangle destination, Math::float4 tint)
{
    if(!image.IsValid()) {std::fprintf(stderr,"dyf: Canvas image has invalid RGBA8 pixels.\n");return;} // 픽셀 개수와 크기가 맞지 않으면 오류를 출력하고 끝낸다.
    Quad(destination,{0,0,1,1},tint,image);
}
// 문자열을 두 번 읽는다. 먼저 필요한 글리프를 모두 만들고, 완성된 atlas 사본을 각 Quad가 공유한다.
// 두 번째 순회는 줄바꿈·커닝·기준선 오프셋을 적용한다. false는 실패를 알리며 이미 추가한 Draw를 되돌리지는 않는다.
bool Canvas::Text(Font& font, std::string_view text, Math::float2 position, Math::float4 color)
{
    try
    {
    for(size_t at=0; at<text.size();) { const auto codepoint=DecodeUtf8(text,at); if(codepoint!='\n') { FontGlyph glyph; if(!font.GetGlyph(codepoint,glyph)) return false; } } // 줄바꿈을 제외한 모든 글리프를 먼저 준비한다. 하나라도 실패하면 atlas를 공유하기 전에 반환한다.
    // 글리프를 모두 준비한 뒤 atlas를 공유해야 UV와 픽셀이 같은 상태를 가리킨다.
    // Image 사본은 불변 픽셀을 공유하므로 이후 Font atlas가 갱신되어도 이 Draw의 픽셀과 UV가 유지된다.
    const auto atlas=font.GetAtlas();
    float x=position.x, y=position.y+font.GetAscent(); uint32_t previous=0;
    for(size_t at=0;at<text.size();)
    {
        const uint32_t codepoint=DecodeUtf8(text,at);
        if(codepoint=='\n') { x=position.x; y+=font.GetLineHeight(); previous=0; continue; } // 줄바꿈이면 시작 X와 다음 기준선으로 이동하고 이전 문자 커닝 연결을 끊는다.
        if(previous) x+=font.GetKerning(previous,codepoint); // 같은 줄에 앞 문자가 있으면 두 문자 사이 간격을 보정한다.
        FontGlyph g;
        if(!font.GetGlyph(codepoint,g)) return false; // 글리프를 얻지 못하면 텍스트 기록을 실패로 반환한다.
        Quad({x+g.offsetX,y+g.offsetY,static_cast<float>(g.width),static_cast<float>(g.height)},
            {g.x/static_cast<float>(atlas.GetWidth()),g.y/static_cast<float>(atlas.GetHeight()),g.width/static_cast<float>(atlas.GetWidth()),g.height/static_cast<float>(atlas.GetHeight())},color,atlas);
        x+=g.advance; previous=codepoint;
    }
    return true;
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: text: %s\n",error.what()); return false; }
}
}
