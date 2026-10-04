#include <dyf/Extends/Model/Model.h>
#include <dyf/Platform/Log.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Bytes = std::vector<uint8_t>;

    void Require(bool condition, const std::string& message)
    {
        if(!condition) throw std::runtime_error(message);
    }

    void U32(Bytes& bytes, uint32_t value)
    {
        for(unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    }

    void F32(Bytes& bytes, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        U32(bytes, bits);
    }

    std::string Join(const std::vector<std::string>& values)
    {
        std::string result = "[";
        for(const auto& value : values)
        {
            if(result.size() > 1) result += ',';
            result += value;
        }
        return result + ']';
    }

    void Replace(std::string& text, const std::string& before, const std::string& after)
    {
        const auto position = text.find(before);
        Require(position != std::string::npos, "fixture replacement did not match");
        text.replace(position, before.size(), after);
    }

    std::string Base64(const Bytes& bytes)
    {
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        for(size_t offset = 0; offset < bytes.size(); offset += 3)
        {
            const uint32_t value = (uint32_t(bytes[offset]) << 16)
                | (offset + 1 < bytes.size() ? uint32_t(bytes[offset + 1]) << 8 : 0u)
                | (offset + 2 < bytes.size() ? uint32_t(bytes[offset + 2]) : 0u);
            result += alphabet[(value >> 18) & 63];
            result += alphabet[(value >> 12) & 63];
            result += offset + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
            result += offset + 2 < bytes.size() ? alphabet[value & 63] : '=';
        }
        return result;
    }

    struct Fixture
    {
        Bytes bytes;
        std::string nodes = R"([{"mesh":0}])";
        std::string scenes = R"([{"nodes":[0]}])";
        std::string defaultScene = "0";
        std::string meshes = R"([{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])";
        std::vector<std::string> views = {
            R"({"buffer":0,"byteLength":36})",
            R"({"buffer":0,"byteOffset":36,"byteLength":6})"
        };
        std::vector<std::string> accessors = {
            R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})",
            R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"})"
        };
        std::string extras;
        std::string uri = "fixture.bin";
        size_t logicalLength = 42;
        bool glb = false;

        Fixture()
        {
            for(float value : {0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f}) F32(bytes, value);
            bytes.insert(bytes.end(), {0,0,1,0,2,0});
        }

        std::string Json() const
        {
            return R"({"asset":{"version":"2.0"},"scene":)" + defaultScene
                + R"(,"scenes":)" + scenes + R"(,"nodes":)" + nodes
                + R"(,"meshes":)" + meshes + R"(,"buffers":[{)"
                + (glb ? "" : R"("uri":")" + uri + R"(",)")
                + R"("byteLength":)" + std::to_string(logicalLength)
                + R"(}],"bufferViews":)" + Join(views)
                + R"(,"accessors":)" + Join(accessors) + extras + '}';
        }
    };

    void Write(const std::filesystem::path& path, const Bytes& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        Require(bool(file), "cannot create fixture");
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Require(bool(file), "cannot write fixture");
    }

    std::filesystem::path WriteFixture(const std::filesystem::path& directory, const Fixture& fixture)
    {
        std::filesystem::create_directories(directory);
        const std::string json = fixture.Json();
        const auto path = directory / (fixture.glb ? "fixture.glb" : "fixture.gltf");
        if(fixture.glb)
        {
            std::string paddedJson = json;
            while(paddedJson.size() % 4) paddedJson += ' ';
            Bytes paddedBuffer = fixture.bytes;
            while(paddedBuffer.size() % 4) paddedBuffer.push_back(0);
            Bytes glb;
            U32(glb, 0x46546c67u);
            U32(glb, 2u);
            U32(glb, static_cast<uint32_t>(28 + paddedJson.size() + paddedBuffer.size()));
            U32(glb, static_cast<uint32_t>(paddedJson.size()));
            U32(glb, 0x4e4f534au);
            glb.insert(glb.end(), paddedJson.begin(), paddedJson.end());
            U32(glb, static_cast<uint32_t>(paddedBuffer.size()));
            U32(glb, 0x004e4942u);
            glb.insert(glb.end(), paddedBuffer.begin(), paddedBuffer.end());
            Write(path, glb);
        }
        else
        {
            Write(directory / "fixture.bin", fixture.bytes);
            Write(path, Bytes(json.begin(), json.end()));
        }
        return path;
    }

    struct Case
    {
        std::string name;
        Fixture fixture;
        bool success = false;
        dyf::ModelLoadOptions options;
        std::function<void(const dyf::ModelData&)> check;
    };

    Fixture SparseTriangle(unsigned indexBytes = 1)
    {
        Fixture fixture;
        fixture.bytes.clear();
        for(uint8_t index : {0, 1, 2})
        {
            fixture.bytes.push_back(index);
            for(unsigned padding = 1; padding < indexBytes; ++padding) fixture.bytes.push_back(0);
        }
        while(fixture.bytes.size() % 4) fixture.bytes.push_back(0);
        const auto valueOffset = fixture.bytes.size();
        for(float value : {0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f}) F32(fixture.bytes, value);
        const auto faceOffset = fixture.bytes.size();
        fixture.bytes.insert(fixture.bytes.end(), {0,0,1,0,2,0});
        fixture.logicalLength = fixture.bytes.size();
        fixture.views = {
            R"({"buffer":0,"byteLength":)" + std::to_string(3 * indexBytes) + '}',
            R"({"buffer":0,"byteOffset":)" + std::to_string(valueOffset) + R"(,"byteLength":36})",
            R"({"buffer":0,"byteOffset":)" + std::to_string(faceOffset) + R"(,"byteLength":6})"
        };
        fixture.accessors[0] = R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0],"sparse":{"count":3,"indices":{"bufferView":0,"componentType":)"
            + std::to_string(indexBytes == 1 ? 5121 : indexBytes == 2 ? 5123 : 5125) + R"(},"values":{"bufferView":1}}})";
        fixture.accessors[1] = R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"})";
        return fixture;
    }

    Fixture SkinnedTriangle()
    {
        Fixture fixture;
        fixture.bytes.resize(56, 0);
        for(unsigned vertex = 0; vertex < 3; ++vertex)
            for(float weight : {1.f, 0.f, 0.f, 0.f}) F32(fixture.bytes, weight);
        for(unsigned component = 0; component < 16; ++component)
            F32(fixture.bytes, component % 5 == 0 ? 1.f : 0.f);
        fixture.logicalLength = 168;
        fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":12})");
        fixture.views.push_back(R"({"buffer":0,"byteOffset":56,"byteLength":48})");
        fixture.views.push_back(R"({"buffer":0,"byteOffset":104,"byteLength":64})");
        fixture.accessors.push_back(R"({"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"})");
        fixture.accessors.push_back(R"({"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"})");
        fixture.accessors.push_back(R"({"bufferView":4,"componentType":5126,"count":1,"type":"MAT4"})");
        Replace(fixture.meshes, R"("POSITION":0)", R"("POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3)");
        fixture.nodes = R"([{"mesh":0,"skin":0}])";
        fixture.extras = R"(,"skins":[{"joints":[0],"inverseBindMatrices":4}])";
        return fixture;
    }

    Fixture AnimatedTriangle()
    {
        Fixture fixture;
        fixture.bytes.resize(44, 0);
        F32(fixture.bytes, 0.f);
        F32(fixture.bytes, 1.f);
        for(float value : {0.f,0.f,0.f,2.f,0.f,0.f}) F32(fixture.bytes, value);
        fixture.logicalLength = 76;
        fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":8})");
        fixture.views.push_back(R"({"buffer":0,"byteOffset":52,"byteLength":24})");
        fixture.accessors.push_back(R"({"bufferView":2,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]})");
        fixture.accessors.push_back(R"({"bufferView":3,"componentType":5126,"count":2,"type":"VEC3"})");
        fixture.extras = R"(,"animations":[{"samplers":[{"input":2,"output":3}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}])";
        return fixture;
    }

    void CheckTriangle(const dyf::ModelData& model)
    {
        Require(model.meshes.size() == 1, "triangle mesh count changed");
        const auto& mesh = model.meshes[0].mesh;
        Require(mesh.vertices.size() == 3 && mesh.indices == std::vector<uint32_t>({0,1,2}), "triangle topology changed");
        Require(mesh.vertices[1].position.x == 1.f && mesh.vertices[2].position.y == 1.f, "triangle positions changed");
    }

    std::vector<Case> Cases()
    {
        std::vector<Case> cases;
        auto add = [&](std::string name, Fixture fixture, bool success = false) -> Case& {
            cases.push_back({std::move(name), std::move(fixture), success, {}, {}});
            if(success) cases.back().check = CheckTriangle;
            return cases.back();
        };
        add("valid-local", Fixture(), true);
        Fixture fixture;
        fixture.uri = "data:application/octet-stream;base64," + Base64(fixture.bytes);
        add("valid-data-uri", fixture, true);
        fixture = {};
        fixture.glb = true;
        add("valid-glb-padding", fixture, true);
        add("valid-sparse", SparseTriangle(), true);
        add("valid-sparse-ushort-indices", SparseTriangle(2), true);
        add("valid-sparse-uint-indices", SparseTriangle(4), true);
        fixture = {};
        fixture.bytes.resize(44, 0);
        fixture.bytes.push_back(1);
        fixture.bytes.resize(48, 0);
        for(float value : {2.f, 0.f, 0.f}) F32(fixture.bytes, value);
        fixture.logicalLength = 60;
        fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":1})");
        fixture.views.push_back(R"({"buffer":0,"byteOffset":48,"byteLength":12})");
        fixture.accessors[0].pop_back();
        fixture.accessors[0] += R"(,"sparse":{"count":1,"indices":{"bufferView":2,"componentType":5121},"values":{"bufferView":3}}})";
        add("valid-sparse-over-base", fixture, true).check = [](const auto& model) {
            const auto& vertices = model.meshes[0].mesh.vertices;
            Require(vertices.size() == 3 && vertices[1].position.x == 2.f && vertices[2].position.y == 1.f,
                "sparse override changed the base or missed the replacement");
        };
        fixture = {};
        fixture.views[0] = R"({"buffer":0,"byteLength":44,"byteStride":16})";
        fixture.views[1] = R"({"buffer":0,"byteOffset":44,"byteLength":6})";
        fixture.bytes.clear();
        for(float value : {0.f,0.f,0.f,99.f,1.f,0.f,0.f,99.f,0.f,1.f,0.f}) F32(fixture.bytes, value);
        fixture.bytes.insert(fixture.bytes.end(), {0,0,1,0,2,0});
        fixture.logicalLength = 50;
        add("valid-stride-last-element", fixture, true);
        fixture = {};
        fixture.bytes.resize(44, 0);
        fixture.bytes.insert(fixture.bytes.end(), {0,0,0,0,255,0,0,0,0,255});
        fixture.logicalLength = 54;
        fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":10,"byteStride":4})");
        fixture.accessors.push_back(R"({"bufferView":2,"componentType":5121,"normalized":true,"count":3,"type":"VEC2"})");
        Replace(fixture.meshes, R"("POSITION":0)", R"("POSITION":0,"TEXCOORD_0":2)");
        add("valid-normalized", fixture, true).check = [](const auto& model) {
            CheckTriangle(model);
            Require(model.meshes[0].mesh.vertices[1].uv.x == 1.f && model.meshes[0].mesh.vertices[2].uv.y == 1.f, "normalized UV changed");
        };
        for(const auto& matrix : std::vector<std::pair<std::string, std::pair<unsigned,unsigned>>>{
            {"MAT2", {5121, 6}}, {"MAT3", {5121, 11}}, {"MAT3", {5123, 22}}})
        {
            fixture = {};
            fixture.bytes.resize(44 + matrix.second.second, 0);
            fixture.logicalLength = fixture.bytes.size();
            fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":)" + std::to_string(matrix.second.second) + '}');
            fixture.accessors.push_back(R"({"bufferView":2,"componentType":)" + std::to_string(matrix.second.first)
                + R"(,"count":1,"type":")" + matrix.first + R"("})");
            add("valid-matrix-padding-" + matrix.first + '-' + std::to_string(matrix.second.first), fixture, true);
            Replace(fixture.views.back(), R"("byteLength":)" + std::to_string(matrix.second.second),
                R"("byteLength":)" + std::to_string(matrix.second.second - 1));
            add("matrix-truncated-" + matrix.first + '-' + std::to_string(matrix.second.first), fixture);
            const unsigned stride = matrix.first == "MAT2" ? 8u : matrix.second.first == 5121 ? 12u : 24u;
            fixture = {};
            fixture.bytes.resize(44 + stride + matrix.second.second, 0);
            fixture.logicalLength = fixture.bytes.size();
            fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":)" + std::to_string(stride + matrix.second.second) + '}');
            fixture.accessors.push_back(R"({"bufferView":2,"componentType":)" + std::to_string(matrix.second.first)
                + R"(,"count":2,"type":")" + matrix.first + R"("})");
            add("valid-matrix-stride-" + matrix.first + '-' + std::to_string(matrix.second.first), fixture, true);
        }
        fixture = {};
        fixture.bytes.resize(70, 0);
        fixture.logicalLength = 70;
        fixture.views.push_back(R"({"buffer":0,"byteOffset":44,"byteLength":1})");
        fixture.views.push_back(R"({"buffer":0,"byteOffset":48,"byteLength":22})");
        fixture.accessors.push_back(R"({"componentType":5123,"count":1,"type":"MAT3","sparse":{"count":1,"indices":{"bufferView":2,"componentType":5121},"values":{"bufferView":3}}})");
        add("valid-sparse-matrix-padding", fixture, true);
        Replace(fixture.views.back(), R"("byteLength":22)", R"("byteLength":21)");
        add("sparse-matrix-truncated", fixture);
        fixture = {};
        Replace(fixture.accessors[0], R"("bufferView":0,)", "");
        add("valid-zero-initialized", fixture, true).check = [](const auto& model) {
            Require(model.meshes[0].mesh.vertices.size() == 3, "zero accessor count changed");
            for(const auto& vertex : model.meshes[0].mesh.vertices)
                Require(vertex.position.x == 0.f && vertex.position.y == 0.f && vertex.position.z == 0.f, "missing buffer view is not initialized to zero");
        };
        add("valid-animation", AnimatedTriangle(), true).check = [](const auto& model) {
            CheckTriangle(model);
            Require(model.animations.size() == 1 && model.animations[0].tracks.size() == 1, "animation was lost");
            Require(model.animations[0].tracks[0].translations.size() == 2, "animation keys were lost");
        };
        add("valid-skin-matrix", SkinnedTriangle(), true).check = [](const auto& model) {
            CheckTriangle(model);
            Require(model.skins.size() == 1 && model.skins[0].inverseBindMatrices.size() == 1, "inverse-bind matrix was lost");
            const auto& matrix = model.skins[0].inverseBindMatrices[0];
            for(unsigned component = 0; component < 16; ++component)
                Require(matrix.m[component] == (component % 5 == 0 ? 1.f : 0.f), "inverse-bind matrix changed");
            Require(model.meshes[0].skinInfluences.size() == 3 && model.meshes[0].skinInfluences[0].weights.x == 1.f,
                "skin influence changed");
        };
        fixture = SkinnedTriangle();
        Replace(fixture.views[4], R"("byteLength":64)", R"("byteLength":60)");
        add("skin-matrix-truncated", fixture);
        fixture = SkinnedTriangle();
        Replace(fixture.accessors[4], R"("type":"MAT4")", R"("type":"SCALAR")");
        add("skin-matrix-accessor-type", fixture);
        fixture = AnimatedTriangle();
        Replace(fixture.accessors[3], R"("type":"VEC3")", R"("type":"SCALAR")");
        add("animation-output-accessor-type", fixture);
        fixture = {};
        Replace(fixture.accessors[1], R"("type":"SCALAR")", R"("type":"VEC2")");
        add("indices-accessor-type", fixture);
        fixture = {};
        Replace(fixture.meshes, R"("indices":1)", R"("indices":1,"targets":[{"POSITION":1}])");
        add("morph-accessor-type", fixture);
        fixture = {};
        fixture.nodes = R"([{"children":[1]},{"mesh":0}])";
        add("depth-at-limit", fixture, true).options.maxNodeDepth = 2;
        add("depth-over-limit", fixture).options.maxNodeDepth = 1;
        add("depth-zero", Fixture()).options.maxNodeDepth = 0;
        fixture = {};
        std::vector<std::string> chain;
        for(unsigned node = 0; node < 4999; ++node)
            chain.push_back(R"({"children":[)" + std::to_string(node + 1) + "]}");
        chain.push_back(R"({"mesh":0})");
        fixture.nodes = Join(chain);
        add("valid-deep-iterative-hierarchy", fixture, true).options.maxNodeDepth = 5000;
        add("deep-hierarchy-over-limit", fixture).options.maxNodeDepth = 4999;
        fixture = {};
        fixture.nodes = R"([{"mesh":0,"translation":[10,0,0]},{"mesh":0,"translation":[20,0,0]}])";
        fixture.scenes = R"([{"nodes":[1,0]},{"nodes":[0]}])";
        add("valid-root-order", fixture, true).check = [](const auto& model) {
            Require(model.meshes.size() == 2 && model.meshes[0].mesh.vertices[0].position.x == 20.f
                && model.meshes[1].mesh.vertices[0].position.x == 10.f, "root traversal order or transform changed");
        };
        fixture.nodes = R"([{"children":[2,1],"translation":[3,0,0],"scale":[2,3,1]},{"mesh":0,"translation":[10,0,0]},{"mesh":0,"translation":[20,0,0]}])";
        fixture.scenes = R"([{"nodes":[0]}])";
        add("valid-child-order", fixture, true).check = [](const auto& model) {
            Require(model.meshes.size() == 2 && model.meshes[0].mesh.vertices[0].position.x == 43.f
                && model.meshes[1].mesh.vertices[0].position.x == 23.f, "child order or matrix multiplication changed");
        };
        fixture = {};
        fixture.scenes = R"([{"nodes":[0,0]}])";
        add("duplicate-root", fixture);
        fixture.scenes = R"([{"nodes":[0]},{"nodes":[0,0]}])";
        add("unselected-scene-duplicate-root", fixture);
        fixture = {};
        fixture.nodes = R"([{"mesh":0,"children":[0]}])";
        add("self-cycle", fixture);
        fixture.nodes = R"([{"children":[1]},{"mesh":0,"children":[0]}])";
        add("two-node-cycle", fixture);
        fixture.nodes = R"([{"mesh":0},{"children":[2]},{"children":[1]}])";
        add("detached-cycle", fixture);
        fixture.nodes = R"([{"children":[1]},{"mesh":0}])";
        fixture.scenes = R"([{"nodes":[1]}])";
        add("parented-root", fixture);
        fixture.scenes = R"([{"nodes":[0,1]}])";
        add("overlapping-roots", fixture);
        fixture = {};
        fixture.nodes = R"([{"mesh":0,"children":[1,1]},{}])";
        add("duplicate-child", fixture);
        fixture.nodes = R"([{"mesh":0,"children":[1,2]},{"children":[2]},{}])";
        add("multiple-parents", fixture);
        fixture = {};
        fixture.nodes = R"([{"mesh":0,"children":[1]}])";
        add("invalid-child", fixture);
        fixture = {};
        fixture.defaultScene = "1";
        add("invalid-default-scene", fixture);
        fixture = {};
        fixture.scenes = R"([{"nodes":[1]}])";
        add("invalid-scene-root", fixture);
        for(const auto& edit : std::vector<std::pair<std::string,std::pair<std::string,std::string>>>{
            {"animation-sampler-reference", {R"("sampler":0)", R"("sampler":1)"}},
            {"animation-input-reference", {R"("input":2)", R"("input":4)"}},
            {"animation-output-reference", {R"("output":3)", R"("output":4)"}},
            {"animation-node-reference", {R"("node":0)", R"("node":1)"}}})
        {
            fixture = AnimatedTriangle();
            Replace(fixture.extras, edit.second.first, edit.second.second);
            add(edit.first, fixture);
        }
        for(const std::string kind : {"indices", "values"})
        {
            fixture = SparseTriangle();
            Replace(fixture.accessors[0], '"' + kind + R"(":{"bufferView":)" + (kind == "indices" ? "0" : "1"),
                '"' + kind + R"(":{"bufferView":3)");
            add("sparse-" + kind + "-reference", fixture);
        }
        fixture = {};
        fixture.extras = R"(,"images":[{"bufferView":2,"mimeType":"image/png"}])";
        add("image-view-reference", fixture);
        fixture.extras.clear();
        Replace(fixture.views[0], R"("buffer":0)", R"("buffer":1)");
        add("view-buffer-reference", fixture);
        fixture = {};
        Replace(fixture.accessors[0], R"("bufferView":0)", R"("bufferView":2)");
        add("accessor-view-reference", fixture);
        fixture = {};
        Replace(fixture.meshes, R"("POSITION":0)", R"("POSITION":2)");
        add("attribute-accessor-reference", fixture);
        fixture = {};
        Replace(fixture.meshes, R"("indices":1)", R"("indices":2)");
        add("indices-accessor-reference", fixture);
        fixture = {};
        Replace(fixture.meshes, R"("indices":1)", R"("indices":1,"targets":[{"POSITION":2}])");
        add("morph-accessor-reference", fixture);
        fixture = {};
        fixture.extras = R"(,"skins":[{"joints":[0],"inverseBindMatrices":2}])";
        add("inverse-bind-accessor-reference", fixture);
        fixture.extras = R"(,"skins":[{"joints":[1]}])";
        add("skin-joint-reference", fixture);
        fixture = {};
        fixture.uri = "https://example.invalid/unloaded.bin";
        add("unloaded-buffer-source", fixture);
        fixture = {};
        fixture.logicalLength = 48;
        add("loaded-buffer-shorter-than-declared", fixture);
        fixture = {};
        fixture.logicalLength = 41;
        add("view-beyond-logical-buffer", fixture);
        fixture = {};
        fixture.views[0] = R"({"buffer":0,"byteOffset":40,"byteLength":36})";
        add("view-beyond-loaded-buffer", fixture);
        fixture.views[0] = R"({"buffer":0,"byteOffset":18446744073709551612,"byteLength":36})";
        add("view-offset-overflow", fixture);
        fixture.views[0] = R"({"buffer":0,"byteLength":18446744073709551615})";
        add("view-length-overflow", fixture);
        fixture = {};
        Replace(fixture.accessors[0], R"("count":3)", R"("count":6)");
        add("accessor-count-exceeds-view", fixture);
        fixture = {};
        Replace(fixture.accessors[0], R"("count":3)", R"("count":18446744073709551615)");
        add("accessor-count-overflow", fixture);
        fixture = {};
        Replace(fixture.accessors[0], R"("bufferView":0)", R"("bufferView":0,"byteOffset":4)");
        add("accessor-offset-exceeds-view", fixture);
        fixture = {};
        Replace(fixture.accessors[0], R"("bufferView":0)", R"("bufferView":0,"byteOffset":18446744073709551612)");
        add("accessor-offset-overflow", fixture);
        fixture = {};
        Replace(fixture.views[0], R"("byteLength":36)", R"("byteLength":36,"byteStride":16)");
        add("accessor-stride-exceeds-view", fixture);
        fixture = {};
        Replace(fixture.views[0], R"("byteLength":36)", R"("byteLength":36,"byteStride":4)");
        add("accessor-stride-smaller-than-element", fixture);
        fixture = {};
        Replace(fixture.views[0], R"("byteLength":36)", R"("byteLength":36,"byteStride":18446744073709551612)");
        add("accessor-stride-overflow", fixture);
        for(const std::string kind : {"indices", "values"})
        {
            fixture = SparseTriangle();
            Replace(fixture.views[kind == "indices" ? 0 : 1], kind == "indices" ? R"("byteLength":3)" : R"("byteLength":36)",
                kind == "indices" ? R"("byteLength":2)" : R"("byteLength":35)");
            add("sparse-" + kind + "-truncated", fixture);
            fixture = SparseTriangle();
            Replace(fixture.accessors[0], '"' + kind + R"(":{"bufferView":)" + (kind == "indices" ? "0" : "1"),
                '"' + kind + R"(":{"byteOffset":18446744073709551612,"bufferView":)" + (kind == "indices" ? "0" : "1"));
            add("sparse-" + kind + "-offset-overflow", fixture);
        }
        fixture = SparseTriangle();
        Replace(fixture.accessors[0], R"("sparse":{"count":3)", R"("sparse":{"count":4)");
        add("sparse-count-exceeds-accessor", fixture);
        fixture = SparseTriangle();
        Replace(fixture.accessors[0], R"("sparse":{"count":3)", R"("sparse":{"count":0)");
        add("sparse-zero-count", fixture);
        fixture = SparseTriangle();
        fixture.bytes[2] = 3;
        add("sparse-index-out-of-range", fixture);
        fixture = SparseTriangle();
        fixture.bytes[2] = 1;
        add("sparse-index-duplicate", fixture);
        fixture = SparseTriangle();
        fixture.bytes[0] = 1;
        fixture.bytes[1] = 0;
        add("sparse-index-unsorted", fixture);
        fixture = SparseTriangle();
        Replace(fixture.accessors[0], R"("componentType":5121)", R"("componentType":5126)");
        add("sparse-invalid-index-component", fixture);
        fixture = AnimatedTriangle();
        Replace(fixture.views[3], R"("byteLength":24)", R"("byteLength":20)");
        add("animation-output-truncated", fixture);
        fixture = {};
        fixture.extras = R"(,"images":[{"bufferView":0,"mimeType":"image/png"}])";
        fixture.views[0] = R"({"buffer":0,"byteOffset":40,"byteLength":36})";
        add("embedded-image-bounds", fixture);
        return cases;
    }

    bool Empty(const dyf::ModelData& model)
    {
        return model.meshes.empty() && model.materials.empty() && model.textures.empty()
            && model.nodes.empty() && model.skins.empty() && model.animations.empty();
    }

    void Run(const Case& test, const std::filesystem::path& directory)
    {
        const auto path = WriteFixture(directory / test.name, test.fixture);
        dyf::ModelData model;
        model.meshes.emplace_back(); model.materials.emplace_back(); model.textures.emplace_back();
        model.nodes.emplace_back(); model.skins.emplace_back(); model.animations.emplace_back();
        std::string diagnostic;
        dyf::Platform::Log::SetCallback([](const auto& record, void* context) {
            *static_cast<std::string*>(context) += record.message + '\n';
        }, &diagnostic);
        const bool success = dyf::LoadModel(path.string(), model, test.options);
        dyf::Platform::Log::SetCallback(nullptr);
        Require(success == test.success, test.success ? "valid fixture rejected: " + diagnostic : "malformed fixture accepted");
        if(success)
        {
            Require(!model.meshes.empty(), "valid fixture produced no geometry");
            if(test.check) test.check(model);
        }
        else
        {
            Require(Empty(model), "rejected fixture left stale output");
            Require(diagnostic.find(path.string() + ": ") != std::string::npos, "rejected fixture has no diagnostic");
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
        const auto cases = Cases();
        std::string selected;
        std::filesystem::path directory = "gltf-validation-fixtures";
        for(int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if(argument == "--list")
            {
                for(const auto& test : cases) std::cout << test.name << '\n';
                return 0;
            }
            Require(index + 1 < argc, "missing argument value");
            if(argument == "--case") selected = argv[++index];
            else if(argument == "--fixture-dir") directory = argv[++index];
            else throw std::runtime_error("unknown argument: " + argument);
        }
        dyf::Platform::Log::SetDefaultOutputEnabled(false);
        size_t count = 0;
        for(const auto& test : cases)
        {
            if(!selected.empty() && selected != test.name) continue;
            std::cout << "RUN " << test.name << std::endl;
            Run(test, std::filesystem::absolute(directory));
            std::cout << "PASS " << test.name << std::endl;
            ++count;
        }
        Require(count != 0, "unknown test case: " + selected);
        std::cout << "glTF validation: " << count << " cases passed\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        dyf::Platform::Log::SetCallback(nullptr);
        std::cerr << "glTF validation failure: " << error.what() << '\n';
        return 1;
    }
}
