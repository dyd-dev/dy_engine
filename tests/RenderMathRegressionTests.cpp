#include "../src/dyf/SceneAlgorithms.h"
#include "../src/dyf/ShaderLayout.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>

namespace M=dyf::Math;
namespace
{
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
bool Near(float a,float b,float tolerance=3e-4f)
{return std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=tolerance*std::max(1.f,std::abs(b));}
bool Same(const M::float4x4& a,const M::float4x4& b)
{for(unsigned i=0;i<16;++i)if(!Near(a.m[i],b.m[i]))return false;return true;}
M::float4 Multiply(const M::float4x4& matrix,M::float4 value)
{
    return {matrix.m[0]*value.x+matrix.m[4]*value.y+matrix.m[8]*value.z+matrix.m[12]*value.w,
        matrix.m[1]*value.x+matrix.m[5]*value.y+matrix.m[9]*value.z+matrix.m[13]*value.w,
        matrix.m[2]*value.x+matrix.m[6]*value.y+matrix.m[10]*value.z+matrix.m[14]*value.w,
        matrix.m[3]*value.x+matrix.m[7]*value.y+matrix.m[11]*value.z+matrix.m[15]*value.w};
}
void FiniteTrs()
{
    std::mt19937 random(20261029);
    std::uniform_real_distribution<float> unit(-1,1),scaleValue(.25f,4);
    for(unsigned i=0;i<512;++i)
    {
        const M::float3 t={unit(random)*20,unit(random)*20,unit(random)*20};
        const auto q=M::QuatFromAxisAngle({unit(random),unit(random),unit(random)},unit(random)*3);
        const M::float3 s={scaleValue(random)*(i&1?-1:1),scaleValue(random)*(i&2?-1:1),scaleValue(random)*(i&4?-1:1)};
        const auto matrix=M::Translation(t)*M::QuatToMatrix(q)*M::Scaling(s);
        M::float3 translation,scaling;M::quat rotation;
        Check(M::DecomposeTRS(matrix,translation,rotation,scaling),"signed TRS decomposition");
        Check(Same(M::Translation(translation)*M::QuatToMatrix(rotation)*M::Scaling(scaling),matrix),"signed TRS roundtrip");
        M::DualQuaternionTRS dual;
        Check(M::MakeDualQuaternionTRS(t,q,s,dual) && Same(M::DualQuaternionToMatrix(dual),matrix),"DQ roundtrip");
    }
    for(float sign:{-1.f,1.f})
    {
        const auto matrix=M::QuatToMatrix(M::QuatFromAxisAngle({1,2,3},.7f))*M::Scaling({sign*1e20f,2e20f,3e20f});
        M::float3 translation,scale;M::quat rotation;
        Check(M::DecomposeTRS(matrix,translation,rotation,scale),"large representable TRS rejected");
        Check(Same(M::QuatToMatrix(rotation)*M::Scaling(scale),matrix),"large signed TRS roundtrip");
    }
    auto overflow=M::float4x4::Identity();
    overflow.m[0]=overflow.m[1]=overflow.m[2]=std::numeric_limits<float>::max();
    M::float3 translation={7,8,9},scale={4,5,6};M::quat rotation={1,2,3,4};
    Check(!M::DecomposeTRS(overflow,translation,rotation,scale),"unrepresentable scale accepted");
    Check(translation.x==7 && scale.x==4 && rotation.x==1,"failed TRS changed outputs");
    M::DualQuaternionTRS dual;
    Check(M::MakeDualQuaternionTRS({3e38f,3e38f,3e38f},{.5f,.5f,.5f,.5f},{1,1,1},dual),"representable large DQ rejected");
    Check(Near(dual.dual.x,7.5e37f) && Near(dual.dual.y,7.5e37f) && Near(dual.dual.z,7.5e37f)
        && Near(dual.dual.w,-2.25e38f),"large DQ overflowed");
    const auto decoded=M::DualQuaternionTranslation(dual);
    Check(Near(decoded.x,3e38f) && Near(decoded.y,3e38f) && Near(decoded.z,3e38f),"large DQ translation roundtrip");
}
void ProjectionContracts()
{
    dyf::Camera camera;camera.view=M::float4x4::Identity();camera.position={0,0,0};
    Check(camera.SetOrthographic(10,10,.1f,100) && dyf::SupportsLodProjection(camera),"orthographic LOD control");
    auto origin=dyf::CameraViewPosition(camera);
    Check(origin.w==0 && Near(origin.z,1),"orthographic homogeneous eye");
    const auto direction=M::Normalize(M::float3{origin.x-2.5f*origin.w,origin.y,origin.z+5*origin.w});
    Check(Near(direction.x,0) && Near(direction.z,1),"orthographic off-axis view direction");
    Check(camera.LookAt({3,4,5},{4,4,5},{0,0,1}),"rotated view fixture");
    origin=dyf::CameraViewPosition(camera);
    Check(origin.w==0 && Near(origin.x,-1) && Near(origin.z,0),"rotated orthographic view direction");
    Check(camera.SetPerspective(1),"perspective fixture");
    origin=dyf::CameraViewPosition(camera);
    Check(origin.w==1 && origin.x==3 && origin.y==4 && origin.z==5,"perspective eye position changed");
    Check(dyf::SupportsLodProjection(camera),"perspective LOD control");
    camera.view.m[15]=.001f;
    Check(!dyf::SupportsLodProjection(camera),"projective camera must retain LOD0");
    camera.view=M::Scaling({2,1,1});
    Check(!dyf::SupportsLodProjection(camera),"scaled view must retain LOD0");
    camera.view=M::float4x4::Identity();camera.projection.m[3]=.01f;
    Check(!dyf::SupportsLodProjection(camera),"unsupported projection must retain LOD0");
    auto world=M::float4x4::Identity();world.m[15]=.001f;
    Check(!dyf::IsAffineTransform(world),"non-normalized world must retain LOD0");
    for(unsigned projection=0;projection<2;++projection)
    {
        Check(projection?camera.SetPerspective(1,1.04719755f,.1f,10000):camera.SetOrthographic(10,10,.1f,10000),"history projection fixture");
        M::float4x4 inverse;
        Check(M::Inverse(camera.projection,inverse),"history inverse projection");
        for(float depth:{1.f,5.f,50.f,9999.f})
        {
            const auto clip=Multiply(camera.projection,{2.5f,1,-depth,1});
            const auto previousView=Multiply(inverse,clip);
            const float previousDepth=-previousView.z;
            Check(Near(previousDepth,depth),"motion/history linear depth contract");
            if(!projection && depth==5)Check(clip.w==1 && previousDepth>4.99f,"orthographic clip.w regression fixture");
        }
    }
}
}
int main()
{
    try
    {
        static_assert(sizeof(dyf::RendererLightingConstants)==2368 && offsetof(dyf::RendererLightingConstants,cameraPosition)==0);
        FiniteTrs();ProjectionContracts();
        std::puts("PASS render math: 512 signed TRS/DQ cases, finite limits, camera/Lod contracts, perspective/orthographic history depth");
        return 0;
    }
    catch(const std::exception& error) {std::fprintf(stderr,"FAIL render math: %s\n",error.what());return 1;}
}
