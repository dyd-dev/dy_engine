#include "../ModelLoader.h"
#include "../ModelSkinning.h"
#include <dyf/Extends/Model/ModelScene.h>

#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>

namespace
{
using Scene = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;

std::string String(ufbx_string value)
{
    return value.data ? std::string(value.data, value.length) : std::string{};
}

void PrintError(const ufbx_error& error)
{
    char message[2048]{};
    ufbx_format_error(message, sizeof(message), &error);
    std::cerr << message << '\n';
}

void PrintMap(const char* name, const ufbx_material_map& map)
{
    std::cout << "  map " << name << " authored=" << map.has_value
        << " components=" << unsigned(map.value_components) << " value="
        << map.value_vec4.x << ',' << map.value_vec4.y << ','
        << map.value_vec4.z << ',' << map.value_vec4.w;
    if(map.texture)
    {
        std::cout << " texture=" << std::quoted(String(map.texture->relative_filename))
            << " enabled=" << map.texture_enabled
            << " uv_set=" << std::quoted(String(map.texture->uv_set))
            << " uv_transform=" << map.texture->has_uv_transform;
    }
    std::cout << '\n';
}

int InspectFbx(const std::string& path, bool geometry, bool meshes = false)
{
    ufbx_load_opts options{};
    options.ignore_geometry = !geometry;
    options.ignore_animation = true;
    options.ignore_embedded = true;
    options.load_external_files = false;
    options.retain_dom = !geometry;
    ufbx_error error{};
    Scene scene(ufbx_load_file(path.c_str(), &options, &error), ufbx_free_scene);
    if(!scene) { PrintError(error); return 1; }
    std::cout << std::setprecision(12) << "source=" << std::quoted(path)
        << " fbx_version=" << scene->metadata.version
        << " creator=" << std::quoted(String(scene->metadata.creator))
        << " unit_meters=" << scene->settings.unit_meters
        << " axes=" << scene->settings.axes.right << ',' << scene->settings.axes.up << ',' << scene->settings.axes.front
        << " nodes=" << scene->nodes.count << " meshes=" << scene->meshes.count
        << " materials=" << scene->materials.count << " cameras=" << scene->cameras.count
        << " lights=" << scene->lights.count << " geometry_loaded=" << geometry << '\n';
    for(const auto* node : scene->nodes)
    {
        if(node->light)
        {
            const auto& light=*node->light;
            const auto direction=ufbx_transform_direction(&node->node_to_world,light.local_direction);
            const auto position=node->node_to_world.cols[3];
            std::cout << "light " << std::quoted(String(node->name)) << " type=" << light.type
                << " color=" << light.color.x << ',' << light.color.y << ',' << light.color.z
                << " intensity=" << light.intensity << " decay=" << light.decay
                << " cone_deg=" << light.inner_angle << ',' << light.outer_angle
                << " position=" << position.x << ',' << position.y << ',' << position.z
                << " direction=" << direction.x << ',' << direction.y << ',' << direction.z << '\n';
        }
        if(node->camera)
        {
            const auto& camera=*node->camera;
            std::cout << "camera " << std::quoted(String(node->name)) << " projection=" << camera.projection_mode
                << " fov_xy_deg=" << camera.field_of_view_deg.x << ',' << camera.field_of_view_deg.y
                << " near_far=" << camera.near_plane << ',' << camera.far_plane
                << " resolution=" << camera.resolution.x << ',' << camera.resolution.y << " matrix=";
            for(const auto& column : node->node_to_world.cols)
                std::cout << column.x << ',' << column.y << ',' << column.z << ';';
            std::cout << '\n';
        }
        if(!geometry && node->attrib_type==UFBX_ELEMENT_MESH)
        {
            const auto* property=ufbx_find_prop(&node->props,"Culling");
            const auto* raw=node->element.dom_node ? ufbx_dom_find(node->element.dom_node,"Culling") : nullptr;
            std::cout << "culling node=" << std::quoted(String(node->name))
                << " property=" << (property?std::quoted(String(property->value_str)):std::quoted(std::string{}))
                << " raw=";
            if(raw) for(const auto& value:raw->values) std::cout << std::quoted(String(value.value_str)) << ':' << value.value_int << ';';
            std::cout << '\n';
        }
    }
    if(meshes) for(const auto* mesh : scene->meshes)
    {
        std::set<std::vector<uint32_t>> polygons;
        std::set<std::array<uint32_t,3>> triangles;
        uint64_t duplicatePolygons=0,duplicatePolygonTriangles=0,degeneratePolygons=0;
        uint64_t duplicateTriangles=0,degenerateTriangles=0,zeroAreaTriangles=0;
        std::vector<uint32_t> triangulated(mesh->max_face_triangles*3);
        for(const auto& face:mesh->faces)
        {
            std::vector<uint32_t> polygon;
            for(size_t corner=0;corner<face.num_indices;++corner)
                polygon.push_back(mesh->vertex_indices[face.index_begin+corner]);
            std::vector<uint32_t> key=polygon;
            for(unsigned reverse=0;reverse<2;++reverse)
            {
                for(size_t shift=0;shift<polygon.size();++shift)
                {
                    if(polygon<key) key=polygon;
                    std::rotate(polygon.begin(),polygon.begin()+1,polygon.end());
                }
                std::reverse(polygon.begin(),polygon.end());
            }
            if(!polygons.insert(key).second)
            {
                ++duplicatePolygons;
                duplicatePolygonTriangles+=face.num_indices>=3?face.num_indices-2:0;
            }
            std::sort(polygon.begin(),polygon.end());
            if(std::unique(polygon.begin(),polygon.end())-polygon.begin()<3) ++degeneratePolygons;
            if(face.num_indices<3) continue;
            const auto count=ufbx_triangulate_face(triangulated.data(),triangulated.size(),mesh,face);
            for(size_t triangle=0;triangle<count;++triangle)
            {
                std::array<uint32_t,3> indices{};
                for(size_t corner=0;corner<3;++corner) indices[corner]=mesh->vertex_indices[triangulated[triangle*3+corner]];
                std::sort(indices.begin(),indices.end());
                if(!triangles.insert(indices).second) ++duplicateTriangles;
                if(indices[0]==indices[1] || indices[1]==indices[2]) ++degenerateTriangles;
                const auto a=mesh->vertices[indices[0]], b=mesh->vertices[indices[1]], c=mesh->vertices[indices[2]];
                const double x=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
                const double y=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
                const double z=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
                if(x*x+y*y+z*z==0) ++zeroAreaTriangles;
            }
        }
        std::cout << "mesh id=" << mesh->typed_id << " name=" << std::quoted(String(mesh->name))
            << " vertices=" << mesh->num_vertices << " indices=" << mesh->num_indices
            << " polygons=" << mesh->faces.count << " triangles=" << mesh->num_triangles
            << " instances=" << mesh->instances.count << " duplicate_polygons=" << duplicatePolygons
            << " duplicate_polygon_triangles=" << duplicatePolygonTriangles << " degenerate_polygons=" << degeneratePolygons
            << " duplicate_triangles=" << duplicateTriangles << " degenerate_index_triangles=" << degenerateTriangles
            << " zero_area_triangles=" << zeroAreaTriangles << '\n';
    }
    for(const auto* material : scene->materials)
    {
        std::cout << "material " << std::quoted(String(material->name)) << " shader=" << material->shader_type
            << " double_sided=" << material->features.double_sided.enabled
            << " transmission_feature=" << material->features.transmission.enabled
            << " opacity_feature=" << material->features.opacity.enabled << '\n';
        PrintMap("pbr.base_factor", material->pbr.base_factor);
        PrintMap("pbr.base_color", material->pbr.base_color);
        PrintMap("pbr.opacity", material->pbr.opacity);
        PrintMap("pbr.transmission", material->pbr.transmission_factor);
        PrintMap("fbx.transparency_factor", material->fbx.transparency_factor);
        PrintMap("fbx.transparency_color", material->fbx.transparency_color);
        PrintMap("fbx.diffuse_color", material->fbx.diffuse_color);
        PrintMap("fbx.specular_color", material->fbx.specular_color);
        PrintMap("pbr.roughness", material->pbr.roughness);
        PrintMap("pbr.metalness", material->pbr.metalness);
        PrintMap("pbr.normal", material->pbr.normal_map);
        PrintMap("fbx.normal", material->fbx.normal_map);
        PrintMap("fbx.bump", material->fbx.bump);
        for(const auto& property : material->props.props)
        {
            std::string name = String(property.name), lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if(lower.find("alpha") == std::string::npos && lower.find("opac") == std::string::npos &&
                lower.find("transp") == std::string::npos && lower.find("cutoff") == std::string::npos &&
                lower.find("diffuse") == std::string::npos && lower.find("sided") == std::string::npos) continue;
            std::cout << "  property " << std::quoted(name) << " type=" << property.type
                << " flags=" << property.flags << " value=" << property.value_vec4.x << ','
                << property.value_vec4.y << ',' << property.value_vec4.z << ',' << property.value_vec4.w
                << " text=" << std::quoted(String(property.value_str)) << '\n';
        }
        for(const auto& binding : material->textures)
            std::cout << "  connection property=" << std::quoted(String(binding.material_prop))
                << " shader_property=" << std::quoted(String(binding.shader_prop))
                << " file=" << std::quoted(String(binding.texture->relative_filename)) << '\n';
    }
    if(geometry)
    {
        uint64_t vertices=0, triangles=0, colored=0, tangents=0;
        std::array<double,3> minimum{}, maximum{};
        minimum.fill(std::numeric_limits<double>::infinity());
        maximum.fill(-std::numeric_limits<double>::infinity());
        for(const auto* node : scene->nodes)
        {
            if(!node->mesh) continue;
            const auto& mesh = *node->mesh;
            vertices += mesh.num_vertices; triangles += mesh.num_triangles;
            colored += mesh.vertex_color.exists; tangents += mesh.vertex_tangent.exists;
            for(const auto& position : mesh.vertices)
            {
                const auto point = ufbx_transform_position(&node->geometry_to_world, position);
                for(size_t axis=0; axis<3; ++axis)
                {
                    minimum[axis] = std::min(minimum[axis], double(point.v[axis]));
                    maximum[axis] = std::max(maximum[axis], double(point.v[axis]));
                }
            }
        }
        std::cout << "geometry instances_vertices=" << vertices << " instances_triangles=" << triangles
            << " colored_instances=" << colored << " tangent_instances=" << tangents
            << " world_min=" << minimum[0] << ',' << minimum[1] << ',' << minimum[2]
            << " world_max=" << maximum[0] << ',' << maximum[1] << ',' << maximum[2] << '\n';
    }
    return 0;
}

int InspectImage(const std::string& path, dyf::ColorSpace colorSpace)
{
    dyf::Image image;
    if(!dyf::LoadImage(path, image)) return 1;
    image.SetColorSpace(colorSpace);
    std::array<uint64_t,256> alpha{};
    std::array<uint8_t,3> minimum{255,255,255}, maximum{};
    std::array<double,3> sum{}, coveredSum{};
    uint64_t covered=0;
    const auto& pixels=image.GetPixels();
    for(size_t offset=0; offset<pixels.size(); offset+=4)
    {
        ++alpha[pixels[offset+3]];
        if(pixels[offset+3]>=128) ++covered;
        for(size_t channel=0; channel<3; ++channel)
        {
            const auto value=pixels[offset+channel];
            minimum[channel]=std::min(minimum[channel],value); maximum[channel]=std::max(maximum[channel],value);
            sum[channel]+=value;
            if(pixels[offset+3]>=128) coveredSum[channel]+=value;
        }
    }
    const uint64_t count=pixels.size()/4, middle=count-alpha[0]-alpha[255];
    std::cout << std::setprecision(12) << "image=" << std::quoted(path) << " size=" << image.GetWidth() << 'x' << image.GetHeight()
        << " role=" << (colorSpace==dyf::ColorSpace::Srgb?"srgb":"linear")
        << " pixels=" << count << " alpha_zero=" << alpha[0] << " alpha_full=" << alpha[255]
        << " alpha_middle=" << middle << " coverage_128=" << covered << '\n';
    for(size_t channel=0; channel<3; ++channel)
        std::cout << "  channel=" << channel << " raw_min=" << unsigned(minimum[channel]) << " raw_max=" << unsigned(maximum[channel])
            << " raw_mean=" << sum[channel]/double(count) << " covered_raw_mean=" << (covered?coveredSum[channel]/double(covered):0) << '\n';
    std::cout << "  alpha_histogram";
    for(size_t value=0; value<alpha.size(); ++value) if(alpha[value]) std::cout << ' ' << value << ':' << alpha[value];
    std::cout << '\n';
    return 0;
}

int InspectFbxUVs(const std::string& path)
{
    ufbx_load_opts options{};
    options.ignore_animation=true;
    options.ignore_embedded=true;
    options.load_external_files=false;
    ufbx_error error{};
    Scene scene(ufbx_load_file(path.c_str(),&options,&error),ufbx_free_scene);
    if(!scene) { PrintError(error); return 1; }
    uint64_t checked=0,mismatches=0;
    std::set<uint32_t> reportedMeshes;
    for(const auto* node:scene->nodes)
    {
        const auto* mesh=node->mesh;
        if(!mesh) continue;
        std::set<uint32_t> slots;
        for(size_t face=0;face<mesh->faces.count;++face)
            if(mesh->faces[face].num_indices>=3)
                slots.insert(face<mesh->face_material.count?mesh->face_material[face]:0u);
        for(uint32_t slot:slots)
        {
            const auto* material=slot<node->materials.count?node->materials[slot]
                :slot<mesh->materials.count?mesh->materials[slot]:nullptr;
            if(!material) continue;
            std::set<const ufbx_texture*> textures;
            for(const auto& binding:material->textures)
            {
                const auto* texture=binding.texture;
                if(!textures.insert(texture).second) continue;
                ++checked;
                const auto primary=mesh->uv_sets.count?String(mesh->uv_sets[0].name):std::string{};
                if(mesh->vertex_uv.exists && (!texture->uv_set.length || String(texture->uv_set)==primary)) continue;
                ++mismatches;
                if(reportedMeshes.insert(mesh->typed_id).second)
                {
                    std::cout << "uv_mesh id=" << mesh->typed_id << " first_exists=" << mesh->vertex_uv.exists << " sets=";
                    for(const auto& uv:mesh->uv_sets)
                        std::cout << '[' << uv.index << ':' << std::quoted(String(uv.name)) << ":exists=" << uv.vertex_uv.exists
                            << ":values=" << uv.vertex_uv.values.count << ":indices=" << uv.vertex_uv.indices.count << ']';
                    std::cout << '\n';
                }
                std::cout << "uv_mismatch node=" << std::quoted(String(node->name)) << " node_id=" << node->typed_id
                    << " mesh_id=" << mesh->typed_id << " slot=" << slot << " material=" << std::quoted(String(material->name))
                    << " property=" << std::quoted(String(binding.material_prop))
                    << " texture=" << std::quoted(String(texture->relative_filename))
                    << " requested=" << std::quoted(String(texture->uv_set)) << " primary=" << std::quoted(primary) << '\n';
            }
        }
    }
    std::cout << "uv_summary nodes=" << scene->nodes.count << " meshes=" << scene->meshes.count
        << " checked_texture_uses=" << checked << " mismatches=" << mismatches << " mismatch_meshes=" << reportedMeshes.size() << '\n';
    return 0;
}

bool Require(bool condition, const char* message)
{
    if(!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool Near(float a,float b) { return std::abs(a-b)<1e-5f; }

const dyf::ModelMaterialInfo* Material(const dyf::ModelData& model,std::string_view name)
{
    const auto found=std::find_if(model.materials.begin(),model.materials.end(),
        [&](const auto& material) { return material.name==name; });
    return found==model.materials.end()?nullptr:&*found;
}

bool ImporterTests()
{
    const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures";
    auto load=[&](const char* file,dyf::ModelData& model,const dyf::ModelLoadOptions& options=dyf::ModelLoadOptions{}) {
        return Require(dyf::LoadModel((fixtures/file).string(),model,options),file);
    };
    bool passed=true;
    dyf::ModelData coverage;
    if(load("MaterialCoverage.gltf",coverage))
    {
        const std::array modes={dyf::MaterialAlphaMode::Opaque,dyf::MaterialAlphaMode::Mask,dyf::MaterialAlphaMode::Blend};
        for(size_t i=0;i<modes.size();++i)
            passed &= Require(coverage.materials.size()>i && coverage.materials[i].material.alphaMode==modes[i]
                && !coverage.materials[i].material.doubleSided && Near(coverage.materials[i].material.transmissionFactor,1),
                "glTF optical transmission preserves authored coverage and sidedness");
        passed &= Require(coverage.materials.size()>1 && Near(coverage.materials[1].material.alphaCutoff,.3f),
            "glTF MASK retains its authored alpha cutoff");
        dyf::ModelLoadOptions overrideOptions;
        dyf::ModelMaterialOverride materialOverride;
        materialOverride.name="OPAQUE"; materialOverride.transmissionFactor=.5f;
        overrideOptions.materialOverrides.push_back(materialOverride);
        dyf::ModelData overridden;
        if(load("MaterialCoverage.gltf",overridden,overrideOptions))
        {
            const auto* material=Material(overridden,"OPAQUE");
            passed &= Require(material && material->material.alphaMode==dyf::MaterialAlphaMode::Opaque
                && !material->material.doubleSided && Near(material->material.transmissionFactor,.5f),
                "a transmission-only override preserves coverage and sidedness");
        }
        else passed=false;
        overrideOptions.materialOverrides[0].alphaMode=dyf::MaterialAlphaMode::Blend;
        overrideOptions.materialOverrides[0].doubleSided=true;
        if(load("MaterialCoverage.gltf",overridden,overrideOptions))
        {
            const auto* material=Material(overridden,"OPAQUE");
            passed &= Require(material && material->material.alphaMode==dyf::MaterialAlphaMode::Blend && material->material.doubleSided,
                "explicit coverage and sidedness overrides are honored");
        }
        else passed=false;
    }
    else passed=false;
    dyf::ModelData cameras;
    passed &= load("Cameras.gltf",cameras);
    dyf::ModelData colored,flipped;
    dyf::ModelLoadOptions flip; flip.flipV=true;
    if(load("VertexColor.gltf",colored) && load("VertexColor.gltf",flipped,flip)
        && Require(colored.meshes.size()==2 && flipped.meshes.size()==2,"both normalized vertex-color forms are imported"))
    {
        const auto& rgba=colored.meshes[0].mesh.vertices[0].color;
        const auto& rgb=colored.meshes[1].mesh.vertices[0].color;
        passed &= Require(Near(rgba.x,128/255.f) && Near(rgba.y,64/255.f) && Near(rgba.z,1) && Near(rgba.w,32/255.f),
            "normalized byte RGBA is retained as linear vertex color");
        passed &= Require(Near(rgb.x,32768/65535.f) && Near(rgb.y,16384/65535.f) && Near(rgb.z,1) && Near(rgb.w,1),
            "normalized short RGB acquires alpha one without sRGB conversion");
        for(size_t mesh=0;mesh<2;++mesh) for(size_t vertex=0;vertex<3;++vertex)
        {
            const auto& a=colored.meshes[mesh].mesh.vertices[vertex];
            const auto& b=flipped.meshes[mesh].mesh.vertices[vertex];
            passed &= Require(Near(a.uv.y,1-b.uv.y) && a.tangent.w==-b.tangent.w && Near(a.color.x,b.color.x),
                "the optional V flip reverses authored tangent handedness and preserves vertex color");
        }
    }
    else passed=false;
    dyf::ModelData flat;
    if(load("MissingNormals.gltf",flat) && Require(flat.meshes.size()==2,"both missing-NORMAL variants are imported"))
    {
        const std::array<unsigned,6> joints={0,1,0,0,1,1};
        for(const auto& mesh:flat.meshes)
        {
            if(!Require(mesh.mesh.vertices.size()==6 && mesh.mesh.indices.size()==6 && mesh.skinInfluences.size()==6
                && mesh.morphTargets.size()==1,"flat-normal corner expansion keeps geometry, skin and morph counts aligned"))
            { passed=false; continue; }
            for(size_t index=0;index<6;++index)
            {
                const auto& vertex=mesh.mesh.vertices[index];
                passed &= Require(mesh.mesh.indices[index]==index && mesh.skinInfluences[index].jointIndices[0]==joints[index],
                    "expanded triangle corners retain their original joint influences and winding");
                passed &= Require(index<3?vertex.normal.z>.9999f:vertex.normal.y>.9999f,
                    "missing NORMAL generates a flat frame on each side of a shared edge");
                passed &= Require(vertex.tangent.x>.9999f,"TANGENT is ignored when NORMAL is absent");
            }
            const auto& target=mesh.morphTargets[0];
            passed &= Require(target.positionDeltas.size()==6 && target.normalDeltas.size()==6 && target.tangentDeltas.size()==6
                && Near(target.positionDeltas[1].z,.5f) && Near(target.positionDeltas[5].z,.5f)
                && std::abs(target.normalDeltas[0].x)>.1f,"morph positions and regenerated flat frames follow expanded corners");
            passed &= Require(Near(mesh.mesh.vertices[0].color.x,1) && Near(mesh.mesh.vertices[3].color.x,1)
                && Near(mesh.mesh.vertices[4].color.x,128/255.f) && Near(mesh.mesh.vertices[0].color.w,128/255.f)
                && Near(mesh.mesh.vertices[3].color.w,128/255.f) && Near(mesh.mesh.vertices[4].color.w,64/255.f),
                "corner expansion preserves source vertex colors and fractional alpha");
        }
        uint64_t decoded=0;
        dyf::ModelData budgetModel;
        passed &= Require(dyf::LoadModel((fixtures/"MissingNormals.gltf").string(),budgetModel,{},decoded),
            "the generated flat geometry exposes its decoded-byte accounting");
        dyf::ModelLoadOptions limited;
        limited.maxDecodedBytes=decoded?decoded-1:0;
        passed &= Require(decoded>0 && !dyf::LoadModel((fixtures/"MissingNormals.gltf").string(),budgetModel,limited),
            "the decoded-byte limit includes expanded corners and generated morph frames");
    }
    else passed=false;
    dyf::ModelLoadOptions fbxOptions;
    fbxOptions.fbxBaseColorTextureHasOpacity=true; fbxOptions.fbxSpecularTextureIsOrm=true;
    dyf::ModelData fbx;
    if(load("SourceProperties.fbx",fbx,fbxOptions))
    {
        for(const char* name:{"Scalar","Legacy"})
        {
            const auto* material=Material(fbx,name);
            passed &= Require(material && Near(material->material.baseColor.w,.25f)
                && Near(material->material.transmissionFactor,0) && material->material.alphaMode==dyf::MaterialAlphaMode::Blend,
                "legacy scalar transparency affects coverage without becoming optical transmission");
        }
        const auto* opaque=Material(fbx,"Opaque");
        const auto* cutout=Material(fbx,"Cutout");
        const auto cutoutCount=std::count_if(fbx.materials.begin(),fbx.materials.end(),
            [](const auto& material) { return material.name=="Cutout"; });
        const auto opaqueCount=std::count_if(fbx.materials.begin(),fbx.materials.end(),
            [](const auto& material) { return material.name=="Opaque"; });
        passed &= Require(cutout && cutout->material.doubleSided && cutoutCount==1 && opaqueCount==2,
            "off-only FBX materials reuse the source entry while mixed off/on materials retain one variant");
        passed &= Require(opaque && Near(opaque->material.baseColor.w,1) && Near(opaque->material.transmissionFactor,0),
            "black legacy transparency color preserves fully opaque coverage");
        passed &= Require(cutout && cutout->material.alphaMode==dyf::MaterialAlphaMode::Blend
            && cutout->hasTexture[dyf::ToIndex(dyf::MaterialTextureKind::Occlusion)],
            "automatic FBX alpha recognizes intermediate coverage and explicit ORM can carry AO");
        bool foundOff=false,foundOn=false;
        for(const auto& mesh:fbx.meshes)
        {
            if(mesh.nodeIndex>=fbx.nodes.size() || mesh.materialIndex>=fbx.materials.size()) continue;
            const auto& node=fbx.nodes[mesh.nodeIndex];
            if(node.name=="Opaque") foundOff=fbx.materials[mesh.materialIndex].material.doubleSided;
            if(node.name=="OpaqueCulled") foundOn=!fbx.materials[mesh.materialIndex].material.doubleSided;
        }
        passed &= Require(foundOff && foundOn,"authored node CullingOff changes only its own material instance and shared geometry variant");
        if(Require(!fbx.meshes.empty() && fbx.meshes[0].mesh.vertices.size()>=3,"FBX triangle geometry is present"))
        {
            const auto& vertex=fbx.meshes[0].mesh.vertices[0];
            passed &= Require(Near(vertex.color.x,.25f) && Near(vertex.color.y,.5f) && Near(vertex.color.w,.125f),
                "authored FBX vertex RGBA is preserved as linear color");
            dyf::ModelData fbxFlipped;
            auto flippedOptions=fbxOptions; flippedOptions.flipV=true;
            if(load("SourceProperties.fbx",fbxFlipped,flippedOptions))
            {
                const auto& other=fbxFlipped.meshes[0].mesh.vertices[0];
                passed &= Require(Near(vertex.uv.y,1-other.uv.y) && vertex.tangent.w==-other.tangent.w,
                    "FBX default V conversion and additional flip agree with tangent handedness");
            }
            else passed=false;
        }
        else passed=false;
        fbxOptions.fbxBaseColorAlphaMode=dyf::MaterialAlphaMode::Mask;
        fbxOptions.fbxSpecularTextureHasOcclusion=false;
        dyf::ModelData masked;
        if(load("SourceProperties.fbx",masked,fbxOptions))
        {
            cutout=Material(masked,"Cutout");
            passed &= Require(cutout && cutout->material.alphaMode==dyf::MaterialAlphaMode::Mask
                && cutout->hasTexture[dyf::ToIndex(dyf::MaterialTextureKind::MetallicRoughness)]
                && !cutout->hasTexture[dyf::ToIndex(dyf::MaterialTextureKind::Occlusion)],
                "explicit cutout/packed-texture convention keeps intermediate alpha in MASK without inventing AO");
        }
        else passed=false;
    }
    else passed=false;
    // Each asset isolates one unsupported authored semantic. Strict rejection and
    // explicit warned approximation both go through the public importer.
    for(const char* file:{"SamplerNearest.gltf","SamplerNonMip.gltf","SamplerClamp.gltf","SamplerMirror.gltf",
        "UVSet1.gltf","UVTransform.gltf","RequiredUVTransform.gltf","TransmissionTexture.gltf","ThicknessTexture.gltf",
        "OpaqueIor.gltf","MissingUV.gltf","UnsupportedMaterial.gltf","FbxUVSet.fbx","FbxUVTransform.fbx","FbxWrap.fbx","FbxBump.fbx","FbxClockwise.fbx"})
    {
        dyf::ModelData model;
        passed &= Require(!dyf::LoadModel((fixtures/file).string(),model) && model.meshes.empty(),file);
        dyf::ModelLoadOptions fallback; fallback.allowMaterialFallbacks=true;
        passed &= Require(dyf::LoadModel((fixtures/file).string(),model,fallback),"explicit material fallback permits a warned approximation");
    }
    dyf::ModelData supportedSampler;
    passed &= load("SamplerSupported.gltf",supportedSampler);
    dyf::ModelData repairedIor;
    dyf::ModelLoadOptions repaired;
    dyf::ModelMaterialOverride glass;
    glass.name="Contract"; glass.transmissionFactor=.75f; glass.indexOfRefraction=1.3f;
    repaired.materialOverrides.push_back(glass);
    passed &= Require(dyf::LoadModel((fixtures/"OpaqueIor.gltf").string(),repairedIor,repaired),
        "effective IOR is checked after an explicit transmission override selects supported glass shading");
    dyf::ModelData explicitNormal;
    dyf::ModelLoadOptions bumpNormal; bumpNormal.fbxBumpTextureIsNormalMap=true;
    passed &= Require(dyf::LoadModel((fixtures/"FbxBump.fbx").string(),explicitNormal,bumpNormal),
        "an explicit FBX bump-as-normal source convention is supported without allowing unrelated fallbacks");
    dyf::ModelLoadOptions uvRepair;
    dyf::ModelMaterialOverride uvOverride;
    uvOverride.name="Cutout"; uvOverride.fbxFallbackTextureUvSet="UVMap";
    uvRepair.materialOverrides.push_back(uvOverride);
    dyf::ModelData repairedUvs,referenceUvs;
    if(load("FbxUVSet.fbx",repairedUvs,uvRepair) && load("SourceProperties.fbx",referenceUvs))
    {
        auto cutoutMesh=[](const dyf::ModelData& model) {
            return std::find_if(model.meshes.begin(),model.meshes.end(),[&](const auto& mesh) {
                return mesh.materialIndex<model.materials.size() && model.materials[mesh.materialIndex].name=="Cutout";
            });
        };
        const auto a=cutoutMesh(repairedUvs),b=cutoutMesh(referenceUvs);
        if(Require(a!=repairedUvs.meshes.end() && b!=referenceUvs.meshes.end() &&
            a->mesh.vertices.size()==3 && b->mesh.vertices.size()==3,"UV repair retains the source material's triangle"))
        {
            for(size_t index=0;index<3;++index)
            {
                const auto& repaired=a->mesh.vertices[index];
                const auto& reference=b->mesh.vertices[index];
                passed &= Require(Near(repaired.uv.x,reference.uv.x) && Near(repaired.uv.y,reference.uv.y) &&
                    Near(repaired.tangent.x,reference.tangent.x) && Near(repaired.tangent.w,reference.tangent.w),
                    "an explicit first-UV repair preserves UV coordinates and the authored tangent frame");
            }
        }
        else passed=false;
        const auto validSource=std::find_if(repairedUvs.meshes.begin(),repairedUvs.meshes.end(),[&](const auto& mesh) {
            return mesh.nodeIndex<repairedUvs.nodes.size() && repairedUvs.nodes[mesh.nodeIndex].name=="CutoutValidUV";
        });
        passed &= Require(validSource!=repairedUvs.meshes.end() && validSource->mesh.vertices.size()==3 &&
            Near(validSource->mesh.vertices[1].uv.x,1) && Near(validSource->mesh.vertices[1].uv.y,1),
            "one material can repair a dangling UV name on one mesh while preserving that valid source name on another");
    }
    else passed=false;
    ufbx_error uvFixtureError{};
    Scene uvFixture(ufbx_load_file((fixtures/"SourceProperties.fbx").string().c_str(),nullptr,&uvFixtureError),ufbx_free_scene);
    passed &= Require(uvFixture && uvFixture->meshes.count==1 && uvFixture->meshes[0]->uv_sets.count==2 &&
        String(uvFixture->meshes[0]->uv_sets[1].name)=="LightMapUV",
        "the nonfirst-UV rejection fixture contains a distinct existing second UV set");
    dyf::ModelScene cachedScene;
    dyf::ModelAssetID cachedAsset=dyf::ModelAssetID::Invalid;
    passed &= Require(dyf::LoadModelAsset(cachedScene,(fixtures/"FbxUVSet.fbx").string(),uvRepair,&cachedAsset),
        "a valid explicit UV repair can populate the public CPU model-asset cache");
    for(const char* target:{"Unavailable","LightMapUV"})
    {
        uvRepair.materialOverrides[0].fbxFallbackTextureUvSet=target;
        for(bool allowFallback:{false,true})
        {
            uvRepair.allowMaterialFallbacks=allowFallback;
            dyf::ModelData rejected;
            passed &= Require(!dyf::LoadModel((fixtures/"FbxUVSet.fbx").string(),rejected,uvRepair) && rejected.meshes.empty(),
                "an explicit nonexistent or nonfirst FBX UV repair is rejected even when approximations are allowed");
            dyf::ModelAssetID rejectedAsset=dyf::ModelAssetID::Invalid;
            passed &= Require(!dyf::LoadModelAsset(cachedScene,(fixtures/"FbxUVSet.fbx").string(),uvRepair,&rejectedAsset),
                "the UV override participates in asset-cache identity and cannot reuse a differently repaired asset");
            dyf::ModelData validSource;
            passed &= Require(dyf::LoadModel((fixtures/"SourceProperties.fbx").string(),validSource,uvRepair),
                "an unused missing-UV fallback cannot replace or reject a valid source UV0 reference");
        }
    }
    uvRepair.materialOverrides[0].fbxFallbackTextureUvSet="UVMap";
    for(bool allowFallback:{false,true})
    {
        uvRepair.allowMaterialFallbacks=allowFallback;
        dyf::ModelData nonfirst;
        passed &= Require(!dyf::LoadModel((fixtures/"FbxNonfirstUV.fbx").string(),nonfirst,uvRepair) && nonfirst.meshes.empty(),
            "an explicit missing-name repair cannot redirect a real source second UV stream to UV0");
    }
    return passed;
}

bool SkinTransformTests()
{
    bool passed=true;
    auto same=[](const dyf::Math::float3& a,const dyf::Math::float3& b) {
        return std::memcmp(&a.x,&b.x,sizeof(float))==0 &&
            std::memcmp(&a.y,&b.y,sizeof(float))==0 && std::memcmp(&a.z,&b.z,sizeof(float))==0;
    };
    std::vector<dyf::SkinInfluence> patterns(35);
    for(uint32_t index=0;index<32;++index)
    {
        auto& influence=patterns[index];
        influence.jointIndices={index%4,(index+1)%4,(index+2)%4,(index+3)%4};
        influence.weights={float(index+1),float(33-index),float(index%3),.5f};
        influence.dqBlendWeight=float(index%3)*.5f;
    }
    patterns[32].jointIndices.fill(UINT32_MAX); // Zero weights must not dereference these joints.
    patterns[33].weights={1e-8f,0,0,0};
    patterns[34]=patterns[32];
    patterns[34]._padding[0]=-0.0f; // Group complete bytes, without approximate equality.
    std::vector<dyf::SkinInfluence> influences;
    for(uint32_t vertex=0;vertex<512;++vertex) influences.push_back(patterns[(vertex*11)%patterns.size()]);
    std::vector<uint32_t> representatives,vertexGroups;
    passed &= Require(dyf::Detail::BuildSkinTransformGroups(influences,representatives,vertexGroups) &&
        representatives.size()==patterns.size() && vertexGroups.size()==influences.size(),
        "skin transform groups preserve complete binary influence identity, including signed-zero bytes");
    if(!passed) return false;
    for(size_t vertex=0;vertex<influences.size();++vertex)
        passed &= Require(vertexGroups[vertex]<representatives.size() &&
            std::memcmp(&influences[vertex],&influences[representatives[vertexGroups[vertex]]],sizeof(dyf::SkinInfluence))==0,
            "every grouped vertex maps to its own exact influence representative");
    std::mt19937 random(0x534b494e);
    auto next=[&] { return float(random()%20001)/1000.f-10.f; };
    for(uint32_t frame=0;frame<8;++frame)
    {
        std::vector<dyf::SkinJointMatrices> palette(4);
        for(uint32_t index=0;index<palette.size();++index)
        {
            const auto translation=dyf::Math::float3(next(),next(),next());
            const auto rotation=dyf::Math::QuatFromAxisAngle({1,2,3},next());
            const auto scale=dyf::Math::float3(index%2?-1.5f:1.25f,.75f,2.f);
            auto& joint=palette[index];
            joint.positionMatrix=dyf::Math::Translation(translation)*dyf::Math::QuatToMatrix(rotation)*dyf::Math::Scaling(scale);
            passed &= Require(dyf::Math::InverseTranspose(joint.positionMatrix,joint.normalMatrix) &&
                dyf::Math::MakeDualQuaternionTRS(translation,rotation,scale,joint.dualQuaternion),
                "the skin equivalence palette has finite nonuniform and reflected transforms");
            if(index%2)
            {
                auto& real=joint.dualQuaternion.real;
                auto& dual=joint.dualQuaternion.dual;
                real={-real.x,-real.y,-real.z,-real.w};
                dual={-dual.x,-dual.y,-dual.z,-dual.w};
            }
        }
        std::vector<dyf::Detail::SkinTransform> transforms(representatives.size());
        for(size_t group=0;group<representatives.size();++group)
            passed &= Require(dyf::Detail::BuildSkinTransform(influences[representatives[group]],palette,transforms[group]),
                "each current palette can build all LBS, DQ, mixed and zero-weight group transforms");
        dyf::Math::Bounds3 cachedBounds,referenceBounds;
        for(size_t vertex=0;vertex<influences.size();++vertex)
        {
            dyf::Vertex source;
            source.position={next(),next(),next()}; // Different positions can also be per-entity morph output.
            source.normal={0,0,1}; source.tangent={1,0,0,1};
            dyf::Math::float3 reference;
            dyf::Vertex full;
            const auto cached=dyf::Detail::ApplySkinTransform(transforms[vertexGroups[vertex]],source.position);
            passed &= Require(dyf::Detail::SkinPosition(source,influences[vertex],palette,reference) &&
                dyf::SkinVertex(source,influences[vertex],palette,full) && same(cached,reference) && same(cached,full.position),
                "grouped bounds positions are bit-identical to per-vertex skinning across changing palettes and morph positions");
            cachedBounds.Include(cached); referenceBounds.Include(reference);
        }
        passed &= Require(same(cachedBounds.min,referenceBounds.min) && same(cachedBounds.max,referenceBounds.max),
            "grouped skinning retains exact extrema used by shadow fitting, sorting and LOD");
    }
    dyf::Detail::SkinTransform zero;
    passed &= Require(dyf::Detail::BuildSkinTransform(patterns[32],{},zero) && zero.copySource &&
        same(dyf::Detail::ApplySkinTransform(zero,{-0.f,0.f,-0.f}),{-0.f,0.f,-0.f}),
        "zero total weight copies signed-zero positions without an identity matrix multiply");
    std::vector<dyf::SkinJointMatrices> simple(2);
    simple[0].positionMatrix=dyf::Math::Translation({2,4,6});
    simple[1].positionMatrix=dyf::Math::Translation({6,8,10});
    dyf::SkinInfluence weighted;
    weighted.jointIndices={0,1,0,0}; weighted.weights={2,6,0,0};
    dyf::Detail::SkinTransform blend;
    passed &= Require(dyf::Detail::BuildSkinTransform(weighted,simple,blend) &&
        same(dyf::Detail::ApplySkinTransform(blend,{1,2,3}),{6,9,12}),
        "unnormalized positive LBS weights retain the independently known normalized translation");
    for(uint32_t invalid=0;invalid<7;++invalid)
    {
        auto influence=weighted;
        if(invalid==0) influence.weights.x=-1;
        if(invalid==1) influence.weights.y=std::numeric_limits<float>::quiet_NaN();
        if(invalid==2) influence.weights.z=std::numeric_limits<float>::infinity();
        if(invalid==3) influence.dqBlendWeight=-.1f;
        if(invalid==4) influence.dqBlendWeight=1.1f;
        if(invalid==5) influence.dqBlendWeight=std::numeric_limits<float>::quiet_NaN();
        if(invalid==6) influence.jointIndices[0]=2;
        passed &= Require(!dyf::Detail::BuildSkinTransform(influence,simple,blend),
            "group transform construction preserves invalid weight, joint and DQ rejection");
    }
    influences[0].weights.x+=.125f;
    passed &= Require(dyf::Detail::BuildSkinTransformGroups(influences,representatives,vertexGroups) &&
        representatives.size()==patterns.size()+1,
        "rebuilding groups observes changed influence bytes rather than stale mesh identity");
    return passed;
}

int Tests()
{
    bool passed=ImporterTests(), truncated=false;
    passed &= SkinTransformTests();
    const auto influence=dyf::MakeSkinInfluence({{0,1},{1,2},{2,3},{3,4},{4,5}},truncated);
    passed &= Require(truncated && influence.jointIndices[0]==4 && influence.jointIndices[3]==1,
        "skin influences retain the strongest four joints");
    const float sum=influence.weights.x+influence.weights.y+influence.weights.z+influence.weights.w;
    passed &= Require(std::abs(sum-1)<1e-6f,"retained skin weights are normalized");
    dyf::MeshData triangle;
    triangle.vertices.resize(3); triangle.indices={0,1,2};
    triangle.vertices[0].position={0,0,0};triangle.vertices[1].position={1,0,0};triangle.vertices[2].position={0,1,0};
    triangle.vertices[0].uv={0,0};triangle.vertices[1].uv={1,0};triangle.vertices[2].uv={0,1};
    passed &= Require(dyf::CalculateTangents(triangle,true),"missing normals and tangents can be generated on a triangle");
    for(const auto& vertex:triangle.vertices)
        passed &= Require(vertex.normal.z>.9999f && vertex.tangent.x>.9999f && vertex.tangent.w==1,
            "generated tangent frame agrees with the triangle UV orientation");
    for(auto& vertex:triangle.vertices) vertex.uv.y=1-vertex.uv.y;
    passed &= Require(dyf::CalculateTangents(triangle),"mirrored V coordinates retain a valid tangent frame");
    for(const auto& vertex:triangle.vertices)
        passed &= Require(vertex.tangent.w==-1,"mirroring V reverses tangent handedness");
    if(passed) std::cout << "Model conformance tests passed\n";
    return passed?0:1;
}
}

int main(int argc,char** argv)
{
    if(argc==1) return Tests();
    if(argc==3 && std::string_view(argv[1])=="--inspect-fbx") return InspectFbx(argv[2],false);
    if(argc==3 && std::string_view(argv[1])=="--inspect-fbx-geometry") return InspectFbx(argv[2],true);
    if(argc==3 && std::string_view(argv[1])=="--inspect-fbx-meshes") return InspectFbx(argv[2],true,true);
    if(argc==3 && std::string_view(argv[1])=="--inspect-fbx-uvs") return InspectFbxUVs(argv[2]);
    if((argc==3 || argc==4) && std::string_view(argv[1])=="--inspect-image")
    {
        const std::string_view role=argc==4?argv[3]:"srgb";
        if(role!="srgb" && role!="linear") return 2;
        return InspectImage(argv[2],role=="srgb"?dyf::ColorSpace::Srgb:dyf::ColorSpace::Linear);
    }
    std::cerr << "Usage: ModelConformanceTests [--inspect-fbx FILE | --inspect-fbx-geometry FILE | --inspect-fbx-meshes FILE | --inspect-fbx-uvs FILE | --inspect-image FILE [srgb|linear]]\n";
    return 2;
}
