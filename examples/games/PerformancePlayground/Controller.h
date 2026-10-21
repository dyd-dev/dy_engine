#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace playground {
struct Point {
    float x=0,y=0;
    Point operator-(Point p) const {return {x-p.x,y-p.y};}
};
inline float Length(Point p) {return std::sqrt(p.x*p.x+p.y*p.y);}
struct Point3 {float x,y,z;};
struct Box {float x0,y0,z0,x1,y1,z1;};
inline constexpr float PlayerRadius=.32f;

inline bool CanStand(Point p,const std::vector<Box>& solids)
{
    for(const auto& b:solids) {
        if(b.z0>=1.75f||b.z1<=.12f) continue;
        const float dx=p.x-std::clamp(p.x,b.x0,b.x1),dy=p.y-std::clamp(p.y,b.y0,b.y1);
        if(dx*dx+dy*dy<PlayerRadius*PlayerRadius) return false;
    }
    return true;
}

inline float CameraFraction(Point3 from,Point3 to,const std::vector<Box>& solids)
{
    float nearest=1;
    const float origin[]={from.x,from.y,from.z}, delta[]={to.x-from.x,to.y-from.y,to.z-from.z};
    for(const auto& box:solids) {
        const float low[]={box.x0-.12f,box.y0-.12f,box.z0-.12f};
        const float high[]={box.x1+.12f,box.y1+.12f,box.z1+.12f};
        float enter=0,leave=1;
        for(int i=0;i<3;++i) {
            if(std::abs(delta[i])<1e-6f) {
                if(origin[i]<low[i]||origin[i]>high[i]) {leave=-1;break;}
            } else {
                float a=(low[i]-origin[i])/delta[i],b=(high[i]-origin[i])/delta[i];
                if(a>b) std::swap(a,b);
                enter=std::max(enter,a);leave=std::min(leave,b);
            }
        }
        if(enter<=leave&&leave>=0) nearest=std::min(nearest,std::max(.04f,enter-.03f));
    }
    return nearest;
}

struct Controller {
    Point position;
    float yaw=0,pitch=.24f,phase=0,distance=0,facing=0,speed=0;
    bool sprinting=false;
    void Move(float right,float forward,bool run,float seconds,const std::vector<Box>& solids)
    {
        speed=0;sprinting=false;
        if(!std::isfinite(seconds)||seconds<=0||!std::isfinite(right)||!std::isfinite(forward)) return;
        const float dt=std::min(seconds,.05f),input=Length({right,forward});
        if(input<.001f) return;
        right/=std::max(1.f,input);forward/=std::max(1.f,input);
        const float velocity=run?6.f:3.f;
        const float dx=(std::cos(yaw)*right+std::sin(yaw)*forward)*velocity*dt;
        const float dy=(-std::sin(yaw)*right+std::cos(yaw)*forward)*velocity*dt;
        const auto start=position;
        const int steps=std::max(1,int(std::ceil(Length({dx,dy})/.08f)));
        for(int step=0;step<steps;++step) {
            Point next={std::clamp(position.x+dx/steps,-48.f+PlayerRadius,48.f-PlayerRadius),position.y};
            if(CanStand(next,solids)) position=next;
            next={position.x,std::clamp(position.y+dy/steps,-38.f+PlayerRadius,58.f-PlayerRadius)};
            if(CanStand(next,solids)) position=next;
        }
        const float travel=Length(position-start);
        speed=travel/dt;sprinting=run&&speed>.1f;
        if(travel>.0001f) {
            facing=std::atan2(dx,dy);distance+=travel;
            phase+=travel*(run?4.8f:6.f);
        }
    }
};
}
