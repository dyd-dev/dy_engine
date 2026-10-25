#pragma once

#include "dyf/Scene.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace showcase::arena
{
    struct ArenaObstacle
    {
        dyf::Math::float3 center;
        float radius;
    };

    struct DungeonEnvironment
    {
        std::vector<ArenaObstacle> obstacles;
    };

    namespace dungeon_detail
    {
        namespace M = dyf::Math;
        constexpr float Pi = 3.14159265358979323846f;

        inline M::float4x4 Transform(M::float3 position, M::float3 size, float angle = 0)
        {
            return M::Translation(position) * M::RotationZ(angle) * M::Scaling(size);
        }

        inline void Triangle(dyf::MeshData& mesh, M::float3 a, M::float3 b, M::float3 c,
            M::float3 outward)
        {
            if(M::Dot(M::Cross(b - a, c - a), outward) < 0) std::swap(b, c);
            const auto normal = M::NormalizeOr(M::Cross(b - a, c - a), outward);
            const auto tangent = M::NormalizeOr(b - a, M::float3(1, 0, 0));
            const auto first = static_cast<uint32_t>(mesh.vertices.size());
            for(const auto& p : {a, b, c})
            {
                dyf::Vertex v;
                v.position = p;
                v.normal = normal;
                v.uv = {p.x, p.y};
                v.tangent = {tangent.x, tangent.y, tangent.z, 1};
                mesh.vertices.push_back(v);
            }
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2});
        }

        inline void Quad(dyf::MeshData& mesh, M::float3 a, M::float3 b, M::float3 c,
            M::float3 d, M::float3 outward)
        {
            Triangle(mesh, a, b, c, outward);
            Triangle(mesh, a, c, d, outward);
        }

        // Real bevels catch grazing light on the stonework without texture assets.
        inline dyf::MeshData BeveledBlock()
        {
            dyf::MeshData mesh;
            constexpr float a = .5f, b = .445f;
            for(float s : {-1.0f, 1.0f})
            {
                Quad(mesh, {s*a,-b,-b}, {s*a,b,-b}, {s*a,b,b}, {s*a,-b,b}, {s,0,0});
                Quad(mesh, {-b,s*a,-b}, {b,s*a,-b}, {b,s*a,b}, {-b,s*a,b}, {0,s,0});
                Quad(mesh, {-b,-b,s*a}, {b,-b,s*a}, {b,b,s*a}, {-b,b,s*a}, {0,0,s});
            }
            for(float sx : {-1.0f, 1.0f})
                for(float sy : {-1.0f, 1.0f})
                {
                    Quad(mesh, {sx*a,sy*b,-b}, {sx*b,sy*a,-b}, {sx*b,sy*a,b}, {sx*a,sy*b,b}, {sx,sy,0});
                    Quad(mesh, {sx*a,-b,sy*b}, {sx*b,-b,sy*a}, {sx*b,b,sy*a}, {sx*a,b,sy*b}, {sx,0,sy});
                    Quad(mesh, {-b,sx*a,sy*b}, {-b,sx*b,sy*a}, {b,sx*b,sy*a}, {b,sx*a,sy*b}, {0,sx,sy});
                    for(float sz : {-1.0f, 1.0f})
                        Triangle(mesh, {sx*a,sy*b,sz*b}, {sx*b,sy*a,sz*b}, {sx*b,sy*b,sz*a}, {sx,sy,sz});
                }
            return mesh;
        }

        inline dyf::MeshData Cylinder(uint32_t sides = 24)
        {
            dyf::MeshData mesh;
            for(uint32_t i = 0; i < sides; ++i)
            {
                const float a = 2 * Pi * float(i) / float(sides);
                const float b = 2 * Pi * float(i + 1) / float(sides);
                const M::float3 p{std::cos(a), std::sin(a), -.5f};
                const M::float3 q{std::cos(b), std::sin(b), -.5f};
                const M::float3 r{q.x, q.y, .5f}, s{p.x, p.y, .5f};
                Quad(mesh, p, q, r, s, {p.x+q.x,p.y+q.y,0});
                Triangle(mesh, {0,0,.5f}, s, r, {0,0,1});
                Triangle(mesh, {0,0,-.5f}, q, p, {0,0,-1});
            }
            return mesh;
        }

        inline dyf::MeshData Ring(float inner = .82f, uint32_t sides = 48, float arc = 2 * Pi)
        {
            dyf::MeshData mesh;
            for(uint32_t i = 0; i < sides; ++i)
            {
                const float a = arc * float(i) / float(sides);
                const float b = arc * float(i + 1) / float(sides);
                const M::float3 p{std::cos(a),std::sin(a),-.5f};
                const M::float3 q{std::cos(b),std::sin(b),-.5f};
                const M::float3 r{q.x,q.y,.5f}, s{p.x,p.y,.5f};
                const M::float3 ip{p.x*inner,p.y*inner,-.5f};
                const M::float3 iq{q.x*inner,q.y*inner,-.5f};
                const M::float3 ir{iq.x,iq.y,.5f}, is{ip.x,ip.y,.5f};
                const M::float3 out{p.x+q.x,p.y+q.y,0};
                Quad(mesh,p,q,r,s,out);
                Quad(mesh,ip,is,ir,iq,out*-1);
                Quad(mesh,is,s,r,ir,{0,0,1});
                Quad(mesh,ip,iq,q,p,{0,0,-1});
                if(i == 0) Quad(mesh,ip,p,s,is,{std::sin(a),-std::cos(a),0});
                if(i + 1 == sides) Quad(mesh,iq,ir,r,q,{-std::sin(b),std::cos(b),0});
            }
            return mesh;
        }

        inline dyf::MeshData Crystal()
        {
            dyf::MeshData mesh;
            constexpr uint32_t sides = 6;
            for(uint32_t i = 0; i < sides; ++i)
            {
                const float a = 2 * Pi * float(i) / float(sides);
                const float b = 2 * Pi * float(i + 1) / float(sides);
                const M::float3 p{.5f*std::cos(a),.5f*std::sin(a),-.12f};
                const M::float3 q{.5f*std::cos(b),.5f*std::sin(b),-.12f};
                Triangle(mesh,p,q,{0,0,.7f},{p.x+q.x,p.y+q.y,1});
                Triangle(mesh,q,p,{0,0,-.5f},{p.x+q.x,p.y+q.y,-1});
            }
            return mesh;
        }

        inline dyf::MeshData Rock()
        {
            dyf::MeshData mesh;
            constexpr uint32_t sides = 7;
            for(uint32_t i = 0; i < sides; ++i)
            {
                const float a = 2 * Pi * float(i) / float(sides);
                const float b = 2 * Pi * float(i + 1) / float(sides);
                const float r0 = (i & 1) ? .44f : .57f;
                const float r1 = ((i + 1) & 1) ? .44f : .57f;
                const M::float3 p{r0*std::cos(a),r0*std::sin(a),-.5f};
                const M::float3 q{r1*std::cos(b),r1*std::sin(b),-.5f};
                const M::float3 r{q.x*.7f-.08f,q.y*.7f,.24f+float(i%3)*.1f};
                const M::float3 s{p.x*.7f-.08f,p.y*.7f,.24f+float((i+6)%3)*.1f};
                const M::float3 out{p.x+q.x,p.y+q.y,0};
                Quad(mesh,p,q,r,s,out);
                Triangle(mesh,s,r,{-.09f,.06f,.62f},{0,0,1});
                Triangle(mesh,q,p,{0,0,-.5f},{0,0,-1});
            }
            return mesh;
        }

        enum class Shape : size_t { Cube, Stone, Cylinder, Ring, ArchStone, Crystal, Rock, Count };
        enum class Surface : size_t
        {
            Foundation, Floor, FloorLight, FloorDark, Wall, Edge, Trim, Iron,
            TealStone, TealGlow, AmberGlow, Portal, Rock, Count
        };

        struct Placement
        {
            M::float4x4 transform;
        };

        inline dyf::MaterialDesc Material(M::float3 color, float roughness = .85f, float metal = 0,
            M::float3 emission = {0,0,0})
        {
            dyf::MaterialDesc material;
            material.baseColor = {color.x,color.y,color.z,1};
            material.roughnessFactor = roughness;
            material.metallicFactor = metal;
            material.emissiveColor = emission;
            return material;
        }

        class Masonry
        {
        public:
            void Place(Shape shape, Surface surface, M::float3 p, M::float3 size, float angle = 0)
            {
                Place(shape, surface, Transform(p,size,angle));
            }

            void Place(Shape shape, Surface surface, const M::float4x4& transform)
            {
                placements[static_cast<size_t>(shape)][static_cast<size_t>(surface)].push_back({transform});
            }

            void Flush(dyf::Scene& scene)
            {
                const std::array<dyf::MeshData,static_cast<size_t>(Shape::Count)> meshes = {
                    dyf::CreateCubeMesh(), BeveledBlock(), Cylinder(), Ring(.975f),
                    Ring(.76f,1,Pi/12), Crystal(), Rock()
                };
                const std::array<dyf::MaterialDesc,static_cast<size_t>(Surface::Count)> materials = {
                    Material({.065f,.082f,.105f}), Material({.23f,.28f,.30f}),
                    Material({.28f,.32f,.33f}), Material({.16f,.205f,.225f}),
                    Material({.19f,.235f,.255f}), Material({.34f,.38f,.38f}),
                    Material({.42f,.285f,.12f},.35f,.72f), Material({.07f,.09f,.105f},.45f,.8f),
                    Material({.045f,.22f,.225f},.38f,.28f),
                    Material({.03f,.56f,.51f},.35f,.2f,{.05f,3.8f,3.1f}),
                    Material({1,.32f,.045f},.4f,.1f,{7.5f,2.1f,.12f}),
                    Material({.015f,.095f,.11f},.5f,0,{.01f,.38f,.46f}),
                    Material({.11f,.145f,.17f})
                };
                for(size_t shape = 0; shape < meshes.size(); ++shape)
                    for(size_t surface = 0; surface < materials.size(); ++surface)
                    {
                        dyf::EntityHandle source;
                        for(const auto& placement : placements[shape][surface])
                        {
                            auto entity = source ? scene.AddInstance(source,placement.transform) :
                                scene.Add(meshes[shape],materials[surface],placement.transform);
                            if(!entity) throw std::runtime_error("Cannot create the dungeon environment.");
                            if(surface == static_cast<size_t>(Surface::TealGlow) ||
                                surface == static_cast<size_t>(Surface::AmberGlow) ||
                                surface == static_cast<size_t>(Surface::Portal))
                                entity.SetLighting({false,true});
                            if(!source) source = entity;
                        }
                    }
            }

        private:
            std::array<std::array<std::vector<Placement>,static_cast<size_t>(Surface::Count)>,
                static_cast<size_t>(Shape::Count)> placements;
        };
    }

    // All decoration is outside the walkable arena except five explicit obstacles.
    // The first entity of every mesh/material group is a visible scene member;
    // subsequent pieces use AddInstance and preserve its geometry and material.
    inline DungeonEnvironment BuildDungeon(dyf::Scene& scene)
    {
        using namespace dungeon_detail;
        using M::float3;
        DungeonEnvironment result;
        Masonry masonry;
        auto box = [&](Surface surface, float3 p, float3 size, float angle = 0) {
            masonry.Place(Shape::Stone,surface,p,size,angle);
        };
        auto round = [&](Surface surface, float3 p, float3 size, float angle = 0) {
            masonry.Place(Shape::Cylinder,surface,p,size,angle);
        };
        auto ring = [&](Surface surface, float3 p, float radius, float height = .025f, float angle = 0) {
            masonry.Place(Shape::Ring,surface,p,{radius,radius,height},angle);
        };

        // A stepped island foundation and individual, bevelled flagstones give
        // the courtyard depth even with the presentation camera pulled back.
        box(Surface::Foundation,{0,0,-1.05f},{37,37,1.35f});
        box(Surface::Wall,{0,0,-.47f},{35.4f,35.4f,.55f});
        for(int y = 0; y < 16; ++y)
            for(int x = 0; x < 16; ++x)
            {
                const uint32_t variation = uint32_t(x*31+y*47+x*y*7);
                const auto surface = variation%11 == 0 ? Surface::FloorDark :
                    variation%4 == 0 ? Surface::FloorLight : Surface::Floor;
                box(surface,{-15+2.0f*x,-15+2.0f*y,-.135f},{1.965f,1.965f,.33f});
            }
        for(int side : {-1,1})
        {
            box(Surface::Edge,{side*16.45f,0,.015f},{.85f,33.6f,.35f});
            box(Surface::Edge,{0,side*16.45f,.015f},{32.0f,.85f,.35f});
            box(Surface::Trim,{side*15.93f,0,.05f},{.065f,31.8f,.035f});
            box(Surface::Trim,{0,side*15.93f,.05f},{31.8f,.065f,.035f});
            // Broken edge drains and repeated brass seals establish scale.
            for(int i = -6; i <= 6; ++i)
            {
                box(Surface::Iron,{side*15.55f,i*2.3f,.055f},{.28f,1.2f,.025f});
                box(Surface::Trim,{i*2.3f,side*15.55f,.056f},{.28f,.28f,.04f},Pi*.25f);
            }
        }

        // Inlaid concentric ritual marks stay almost flush with the floor.
        // The raised altar alone participates in movement collision.
        round(Surface::FloorDark,{0,0,.047f},{5.4f,5.4f,.035f});
        ring(Surface::Trim,{0,0,.073f},5.27f);
        ring(Surface::TealGlow,{0,0,.079f},4.90f,.02f);
        ring(Surface::Trim,{0,0,.077f},3.42f,.025f);
        ring(Surface::TealGlow,{0,0,.083f},3.24f,.018f);
        for(int i = 0; i < 24; ++i)
        {
            const float a = float(i)*Pi/12;
            const float c = std::cos(a), s = std::sin(a);
            box(i%3 == 0 ? Surface::TealGlow : Surface::Trim,
                {4.48f*c,4.48f*s,.079f},{.12f,i%3 == 0 ? .52f : .25f,.025f},a-Pi*.5f);
            if(i%3 == 0)
            {
                box(Surface::TealGlow,{3.82f*c,3.82f*s,.082f},{.35f,.055f,.025f},a);
                box(Surface::TealGlow,{3.98f*c-.13f*s,3.98f*s+.13f*c,.082f},{.055f,.29f,.025f},a);
            }
        }
        round(Surface::Edge,{0,0,.18f},{1.78f,1.78f,.29f},Pi/8);
        round(Surface::Wall,{0,0,.40f},{1.53f,1.53f,.23f},Pi/8);
        round(Surface::Trim,{0,0,.55f},{1.57f,1.57f,.085f},Pi/8);
        round(Surface::TealStone,{0,0,.64f},{1.35f,1.35f,.14f},Pi/8);
        ring(Surface::TealGlow,{0,0,.721f},1.19f,.025f);
        masonry.Place(Shape::Crystal,Surface::TealGlow,{0,0,1.6f},{.77f,.77f,1.75f},Pi/6);
        for(int i = 0; i < 6; ++i)
        {
            const float a = i*Pi/3;
            masonry.Place(Shape::Crystal,Surface::TealStone,{.95f*std::cos(a),.95f*std::sin(a),.96f},
                {.29f,.29f,.7f},a);
        }
        result.obstacles.push_back({{0,0,0},1.8f});

        // Four compact reliquary columns create deliberate combat lanes.
        for(int x : {-1,1})
            for(int y : {-1,1})
            {
                const float3 p{8.0f*x,8.0f*y,0};
                box(Surface::Edge,p+float3{0,0,.19f},{1.8f,1.8f,.34f},Pi/4);
                box(Surface::Trim,p+float3{0,0,.43f},{1.44f,1.44f,.14f},Pi/4);
                round(Surface::Wall,p+float3{0,0,1.75f},{.57f,.57f,2.48f},Pi/8);
                for(float z : {.67f,2.53f,2.90f})
                    round(Surface::Trim,p+float3{0,0,z},{.66f,.66f,.13f},Pi/8);
                box(Surface::Edge,p+float3{0,0,3.11f},{1.6f,1.6f,.33f},Pi/4);
                round(Surface::Iron,p+float3{0,0,3.41f},{.51f,.51f,.26f});
                round(Surface::AmberGlow,p+float3{0,0,3.58f},{.36f,.36f,.09f});
                masonry.Place(Shape::Crystal,Surface::AmberGlow,p+float3{0,0,3.85f},{.49f,.49f,.75f},Pi/6);
                for(int slat = 0; slat < 4; ++slat)
                {
                    const float a = Pi*.5f*slat;
                    box(Surface::Trim,p+float3{.37f*std::cos(a),.37f*std::sin(a),3.75f},{.075f,.075f,.55f},a);
                }
                result.obstacles.push_back({p,1.05f});
                dyf::PointLight fire;
                fire.position = p+float3{0,0,4.05f};
                fire.color = {1,.36f,.075f};
                fire.intensity = 22;
                fire.range = 10;
                fire.castShadow = false;
                (void)scene.Add(fire);
            }

        // Thick, articulated walls frame three sides. The south parapet is low
        // so it cannot hide the player from the default southern camera.
        for(int side = 0; side < 4; ++side)
        {
            const float a = side*Pi*.5f;
            auto along = [&](float x, float y, float z) {
                return float3{x*std::cos(a)-y*std::sin(a),x*std::sin(a)+y*std::cos(a),z};
            };
            const bool front = side == 2;
            const int courses = front ? 1 : 4;
            for(int row = 0; row < courses; ++row)
                for(int column = -8; column <= 8; ++column)
                {
                    const float x = column*1.94f+(row%2 ? .35f : 0);
                    if(std::abs(x) < 3.85f) continue;
                    box((column+row)%5 == 0 ? Surface::FloorDark : Surface::Wall,
                        along(x,17.15f,.49f+row*.91f),{1.90f,1.5f,.88f},a);
                }
            for(int sign : {-1,1})
            {
                box(Surface::Edge,along(sign*10.2f,17.12f,.11f),{13.0f,1.85f,.35f},a);
                box(Surface::Edge,along(sign*10.2f,17.12f,front ? 1.05f : 3.84f),
                    {13.0f,1.8f,.28f},a);
            }
            for(int i : {-7,-5,-3,3,5,7})
            {
                const float h = front ? 1.55f : 4.5f;
                box(Surface::FloorDark,along(i*2.17f,16.90f,h*.5f),{.72f,2.13f,h},a);
                box(Surface::Edge,along(i*2.17f,16.90f,h+.15f),{1.03f,2.36f,.3f},a);
                if(!front)
                    box(Surface::Trim,along(i*2.17f,15.78f,2.8f),{.12f,.075f,1.3f},a);
            }
            // A broad staircase and bronze threshold carry the view into each gate.
            for(int step = 0; step < 4; ++step)
                box(Surface::Edge,along(0,17.35f+step*.62f,-.08f-step*.19f),
                    {6.2f,.76f,.3f},a);
            box(Surface::Trim,along(0,16.10f,.07f),{6.5f,.2f,.07f},a);
            for(int sign : {-1,1})
            {
                box(Surface::Edge,along(sign*3.33f,17.08f,1.48f),{1.17f,1.9f,2.95f},a);
                box(Surface::Trim,along(sign*3.33f,16.10f,1.5f),{.16f,.08f,2.45f},a);
                box(Surface::FloorDark,along(sign*3.33f,17.08f,.22f),{1.48f,2.1f,.42f},a);
                box(Surface::Edge,along(sign*3.33f,17.08f,2.93f),{1.45f,2.1f,.28f},a);
            }
            if(!front)
            {
                for(int stone = 0; stone < 12; ++stone)
                {
                    const auto transform = M::Translation(along(0,17.08f,2.96f)) * M::RotationZ(a) *
                        M::RotationX(Pi*.5f) * M::RotationZ(stone*Pi/12+.012f) * M::Scaling({3.48f,3.48f,1.72f});
                    masonry.Place(Shape::ArchStone,stone%3 ? Surface::Edge : Surface::FloorLight,transform);
                }
                box(Surface::Trim,along(0,16.15f,6.26f),{.78f,.18f,.69f},a+Pi*.25f);
            }
            if(side == 0)
            {
                // The far gate terminates in a subdued, emissive magic seal.
                masonry.Place(Shape::Cylinder,Surface::Portal,
                    M::Translation(along(0,18.12f,2.85f))*M::RotationZ(a)*M::RotationX(Pi*.5f)*
                    M::Scaling({2.65f,3.2f,.08f}));
                for(int i = 0; i < 3; ++i)
                    masonry.Place(Shape::Ring,Surface::TealGlow,
                        M::Translation(along(0,18.0f,2.98f))*M::RotationZ(a)*M::RotationX(Pi*.5f)*
                        M::RotationZ(i*Pi/9)*M::Scaling({2.48f-i*.27f,2.48f-i*.27f,.028f}));
                for(int i = 0; i < 12; ++i)
                {
                    const float t = i*Pi/6;
                    const auto local = M::Translation({2.05f*std::cos(t),2.05f*std::sin(t),0})*
                        M::RotationZ(t)*M::Scaling({.22f,.09f,.04f});
                    masonry.Place(Shape::Cube,Surface::TealGlow,
                        M::Translation(along(0,17.93f,2.98f))*M::RotationZ(a)*M::RotationX(Pi*.5f)*local);
                }
            }
        }

        // Monumental corner towers, broken crowns and buttresses create a
        // recognisable dungeon silhouette beyond the combat floor.
        for(int x : {-1,1})
            for(int y : {-1,1})
            {
                const float3 p{17.5f*x,17.5f*y,0};
                const float h = y > 0 ? 6.8f : 3.1f;
                box(Surface::FloorDark,p+float3{0,0,h*.5f},{3.1f,3.1f,h});
                box(Surface::Edge,p+float3{0,0,.27f},{3.7f,3.7f,.55f});
                box(Surface::Edge,p+float3{0,0,h+.15f},{3.5f,3.5f,.35f});
                for(int corner = 0; corner < 4; ++corner)
                {
                    const float a = Pi*.25f+corner*Pi*.5f;
                    box(Surface::Wall,p+float3{1.52f*std::cos(a),1.52f*std::sin(a),h+.65f},
                        {.8f,.8f,.86f});
                }
                box(Surface::TealStone,p+float3{0,-1.59f,h*.62f},{.62f,.1f,1.6f});
                box(Surface::TealGlow,p+float3{0,-1.65f,h*.62f},{.08f,.04f,1.13f});
            }
        for(int i = 0; i < 21; ++i)
        {
            const float a = .06f+float(i)*Pi/20;
            const float r = 25.0f+float(i%4)*2.8f;
            const float h = 2.7f+float((i*7)%6);
            masonry.Place(Shape::Rock,Surface::Rock,{r*std::cos(a),r*std::sin(a)+3,h*.22f-1.1f},
                {4.8f+float(i%3),3.5f,h},a+.31f);
            if(i%3 == 0)
            {
                box(Surface::Foundation,{r*std::cos(a),r*std::sin(a)+4,h*.48f-1},
                    {1.5f,1.9f,h*1.5f},a+.12f);
                box(Surface::Wall,{r*std::cos(a),r*std::sin(a)+4,h*1.23f-1},
                    {2.6f,2.6f,.4f},a+.12f);
            }
        }
        // Small masonry rubble remains beyond the collision boundary.
        for(int i = 0; i < 44; ++i)
        {
            const float t = float(i)*2.39996323f;
            const float side = (i & 1) ? -1.0f : 1.0f;
            const float x = side*(17.8f+float(i%4)*.63f);
            const float y = -15.0f+float((i*11)%31);
            box(i%3 ? Surface::Wall : Surface::FloorDark,{x,y,-.16f+float(i%3)*.12f},
                {.5f+float(i%3)*.28f,.53f,.48f},t);
        }

        masonry.Flush(scene);

        dyf::DirectionalLight moon;
        // DirectionalLight stores the surface-to-light vector, as does the
        // direct-light shader and the directional shadow camera.
        moon.direction = {.35f,.52f,1};
        moon.color = {.54f,.74f,1};
        moon.intensity = 2.3f;
        moon.shadowStrength = .66f;
        moon.angularRadius = .018f;
        (void)scene.Add(moon);
        dyf::DirectionalLight fill;
        fill.direction = {-.5f,-.7f,.45f};
        fill.color = {.80f,.54f,.31f};
        fill.intensity = .48f;
        fill.castShadow = false;
        (void)scene.Add(fill);
        dyf::PointLight altar;
        altar.position = {0,0,2.8f};
        altar.color = {.075f,1,.84f};
        altar.intensity = 15;
        altar.range = 9;
        altar.castShadow = false;
        (void)scene.Add(altar);
        dyf::PointLight portal;
        portal.position = {0,15.2f,3.5f};
        portal.color = {.07f,.8f,1};
        portal.intensity = 24;
        portal.range = 12;
        portal.castShadow = false;
        (void)scene.Add(portal);
        return result;
    }
}
