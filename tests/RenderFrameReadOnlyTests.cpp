#include "dyf/RenderExtension.h"
#include "dyf/Scene.h"
#include "dyf/Camera.h"
#include "dyf/RHI.h"
#include "Backends/Null/NullDevice.h"
#include "dyf/DepthPrepassValidation.h"
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
using namespace dyf;
struct Device final:Backends::NullDevice {
    Device(){Check(Initialize(nullptr,{})==0,"Device creation");}
    bool CreateSwapchainNative(const RHI::SwapchainDesc& input) override {
        auto desc=input;desc.initialWidth=desc.initialHeight=16;return NullDevice::CreateSwapchainNative(desc);
    }
};
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<ReadOnlyRenderFrame&>().geometry)>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<ReadOnlyRenderFrame&>().camera)>>);
static_assert(!std::is_assignable_v<decltype((std::declval<ReadOnlyRenderFrame&>().geometry[0].baseColor.x)),float>);

struct ReadOnlyModule final:ReadOnlyRenderExtension {
    const void* identity=nullptr;bool fail=false;unsigned recorded=0;
    bool Prepare(ReadOnlyRenderFrame& frame,RHI::ICommandList&) override {
        identity=frame.GetIdentity();
        Check(frame.geometry.size()==1,"Immutable geometry view changed its range");
        frame.opaqueDepthComplete=true;
        return !fail;
    }
    bool AddPasses(RenderStage,ReadOnlyRenderFrame& frame,RHI::RenderGraph& graph) override {
        Check(identity==frame.GetIdentity(),"Module phases lost their stable frame identity");
        const float value=frame.geometry.front().baseColor.x;
        graph.AddPass("immutable module").SetExecute([this,value](RHI::ICommandList*) {
            Check(value==.25f,"Pass lost its copied input");++recorded;
        });
        frame.outputWritten=true;
        return true;
    }
};
struct LegacyModule final:RenderExtension {
    bool Prepare(RenderFrame& frame,RHI::ICommandList&) override {
        frame.geometry.front().baseColor.x=.75f;frame.camera.view.m[12]=3;return true;
    }
};
struct SceneAliasModule final:ReadOnlyRenderExtension {
    EntityHandle entity;
    bool Prepare(ReadOnlyRenderFrame&,RHI::ICommandList&) override {return entity.SetPosition({2,0,0});}
};
struct CameraAliasModule final:ReadOnlyRenderExtension {
    Camera* retained=nullptr;
    bool PrepareCamera(Camera& camera,ReadOnlyRenderFrame&) override {retained=&camera;camera.projection.m[8]+=.01f;return true;}
    bool Prepare(ReadOnlyRenderFrame&,RHI::ICommandList&) override {retained->position.x+=1;return true;}
};
}
int main() try {
    Device device;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1));Check(bool(renderer),"Renderer creation");
    dyf::Scene scene;dyf::Camera camera;dyf::RenderFrame frame{*renderer,scene,camera};
    dyf::MaterialDesc material;
    frame.geometry.resize(1);frame.geometry.front().baseColor.x=.25f;frame.geometry.front().material=&material;
    dyf::RHI::ResourceScope resources(device);auto* commands=resources.Keep(device.AcquireCommandList());
    Check(commands!=nullptr,"Command creation");
    ReadOnlyModule module;dyf::RenderExtension& extension=module;
    Check(extension.HasReadOnlyGeometryInputs(),"Typed module must select immutable input validation");
    Check(extension.Prepare(frame,*commands) && frame.opaqueDepthComplete,"Immutable module lost writable outputs");
    dyf::RHI::RenderGraph graph;
    Check(extension.AddPasses(dyf::RenderStage::BeforeOpaque,frame,graph) && frame.outputWritten,"Immutable pass output");
    Check(graph.Compile() && graph.Execute(commands) && module.recorded==1,"Immutable callback execution");
    module.fail=true;frame.opaqueDepthComplete=false;
    Check(!extension.Prepare(frame,*commands) && frame.opaqueDepthComplete,"Failure must preserve legacy output semantics");
    LegacyModule legacy;Check(!legacy.HasReadOnlyGeometryInputs(),"Legacy mutable callbacks must retain validation");
    dyf::SurfaceGeometrySnapshot proof;proof.Capture(frame.geometry);
    Check(legacy.Prepare(frame,*commands) && !proof.Matches(frame.geometry) && camera.view.m[12]==3,"Legacy edits must remain visible");
    dyf::MeshData mesh;mesh.vertices.resize(3);mesh.indices={0,1,2};
    SceneAliasModule alias;alias.entity=scene.Add(mesh);
    Check(!static_cast<RenderExtension&>(alias).Prepare(frame,*commands),"Readonly callbacks must reject supported Scene edits through retained handles");
    CameraAliasModule cameraAlias;RenderExtension& cameraExtension=cameraAlias;
    Check(cameraExtension.PrepareCamera(frame),"Camera preparation must still allow jitter");
    Check(!cameraExtension.Prepare(frame,*commands),"Retained camera aliases cannot mutate frozen inputs");
    std::puts("PASS immutable frame inputs, writable outputs, phase identity, callback lifetime and legacy mutations");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL immutable frame: %s\n",e.what());return 1;}
