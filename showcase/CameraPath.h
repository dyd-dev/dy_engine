#pragma once

#include "dyf/Math/Math.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace showcase
{
struct CameraKeyframe
{
    double seconds=0;
    dyf::Math::float3 eye{},target{};
    float fov=50;
    dyf::Math::float3 up={0,0,1};
};

// 두 렌더러가 같은 시간·좌표·보간 규칙으로 비교 동선을 재생한다.
class CameraPath
{
public:
    void Load(const std::string& filename)
    {
        std::ifstream input(filename);
        if(!input) throw std::runtime_error("Cannot open camera path: "+filename);
        std::vector<CameraKeyframe> keys;
        std::string line;
        size_t lineNumber=0;
        while(std::getline(input,line))
        {
            ++lineNumber;
            const auto first=line.find_first_not_of(" \t\r");
            if(first==std::string::npos || line[first]=='#') continue;
            std::replace(line.begin(),line.end(),',',' ');
            std::istringstream row(line);
            CameraKeyframe key;
            std::string extra;
            if(!(row>>key.seconds>>key.eye.x>>key.eye.y>>key.eye.z>>key.target.x>>key.target.y>>key.target.z
                >>key.fov>>key.up.x>>key.up.y>>key.up.z) || (row>>extra))
                throw std::runtime_error("Invalid camera CSV row "+std::to_string(lineNumber));
            if(!std::isfinite(key.seconds) || key.seconds<0 ||
                (!keys.empty() && key.seconds<=keys.back().seconds) || key.fov<=0 || key.fov>=179)
                throw std::runtime_error("Invalid camera time or FOV at row "+std::to_string(lineNumber));
            for(float value:{key.eye.x,key.eye.y,key.eye.z,key.target.x,key.target.y,key.target.z,
                key.fov,key.up.x,key.up.y,key.up.z})
                if(!std::isfinite(value)) throw std::runtime_error("Non-finite camera value");
            const auto forward=key.target-key.eye;
            if(dyf::Math::Dot(forward,forward)<1e-10f ||
                dyf::Math::Length(dyf::Math::Cross(forward,key.up))<1e-6f)
                throw std::runtime_error("Degenerate camera orientation");
            keys.push_back(key);
        }
        if(input.bad() || keys.size()<2 || keys.front().seconds!=0)
            throw std::runtime_error("Camera path requires at least two rows, starting at zero");
        m_keys=std::move(keys);
    }
    double Duration() const {return m_keys.empty()?0:m_keys.back().seconds;}
    CameraKeyframe Sample(double seconds) const
    {
        if(m_keys.empty() || !std::isfinite(seconds)) throw std::runtime_error("Invalid camera sample");
        seconds=std::clamp(seconds,0.0,Duration());
        const auto next=std::upper_bound(m_keys.begin(),m_keys.end(),seconds,
            [](double time,const CameraKeyframe& key) {return time<key.seconds;});
        if(next==m_keys.end()) return m_keys.back();
        const auto& a=*(next-1);const auto& b=*next;
        const float t=float((seconds-a.seconds)/(b.seconds-a.seconds));
        return {seconds,a.eye+(b.eye-a.eye)*t,a.target+(b.target-a.target)*t,a.fov+(b.fov-a.fov)*t,
            dyf::Math::NormalizeOr(a.up+(b.up-a.up)*t,{0,0,1})};
    }
private:
    std::vector<CameraKeyframe> m_keys;
};
}
