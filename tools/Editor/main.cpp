#include "EditorScene.h"
#include "dyf/Camera.h"
#include "dyf/ImGui.h"
#include "dyf/Platform/Window.h"
#include "dyf/Renderer.h"
#include "dyf/RHI.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <string>

using namespace dyf;
namespace fs=std::filesystem;
namespace M=dyf::Math;
using Clock=std::chrono::steady_clock;

struct Viewport
{
    RHI::IDevice& device;
    Gui& gui;
    RHI::TextureHandle texture=nullptr;
    uint64_t id=0;
    RHI::ResourceState state=RHI::ResourceState::Undefined;
    ~Viewport(){if(id)gui.UnregisterTexture(id);if(texture)device.DestroyTexture(texture);}
    bool Resize(uint32_t width,uint32_t height)
    {
        if(texture && texture->GetDesc().width==width && texture->GetDesc().height==height)return true;
        RHI::TextureDesc desc;desc.width=width;desc.height=height;desc.format=RHI::Format::B8G8R8A8_UNORM_SRGB;
        desc.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
        auto replacement=device.CreateTexture(desc);if(!replacement)return false;
        const auto replacementId=gui.RegisterTexture(replacement);
        if(!replacementId){device.DestroyTexture(replacement);return false;}
        // Submitted RHI commands retain the previous resource set and texture until completion.
        if(id)gui.UnregisterTexture(id);if(texture)device.DestroyTexture(texture);
        texture=replacement;id=replacementId;state=RHI::ResourceState::Undefined;return true;
    }
};

struct OrbitCamera
{
    M::float3 target={0,0,.5f};
    float yaw=-1.05f,pitch=.4f,distance=12;
    Camera camera;
    void Update(float aspect)
    {
        const M::float3 offset={std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),std::sin(pitch)};
        camera.LookAt(target+offset*distance,target);camera.SetPerspective(aspect, .87f,.05f,500);
    }
    void Focus(const editor::Object& object)
    {
        target={object.position[0],object.position[1],object.position[2]};
        distance=std::clamp(std::max({object.scale[0],object.scale[1],object.scale[2]})*3.f,2.f,40.f);
    }
    M::float3 Ray(ImVec2 point,ImVec2 origin,ImVec2 size) const
    {
        const auto forward=M::Normalize(target-camera.position),right=M::Normalize(M::Cross(forward,{0,0,1})),up=M::Cross(right,forward);
        const float x=2*(point.x-origin.x)/size.x-1,y=1-2*(point.y-origin.y)/size.y,tangent=std::tan(.87f*.5f);
        return M::Normalize(forward+right*(x*tangent*size.x/size.y)+up*(y*tangent));
    }
};

static void Style()
{
    ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();
    style.WindowRounding=0;style.ChildRounding=0;style.FrameRounding=2;style.GrabRounding=2;style.TabRounding=0;
    style.WindowPadding={8,7};style.FramePadding={7,4};style.ItemSpacing={7,6};style.WindowBorderSize=1;style.TabBorderSize=0;
    auto* c=style.Colors;
    c[ImGuiCol_WindowBg]={.15f,.15f,.16f,1};c[ImGuiCol_ChildBg]={.13f,.13f,.14f,1};c[ImGuiCol_PopupBg]={.17f,.17f,.18f,1};
    c[ImGuiCol_Border]={.08f,.08f,.09f,1};c[ImGuiCol_FrameBg]={.11f,.11f,.12f,1};c[ImGuiCol_FrameBgHovered]={.23f,.24f,.27f,1};
    c[ImGuiCol_TitleBg]=c[ImGuiCol_TitleBgActive]={.12f,.12f,.13f,1};c[ImGuiCol_MenuBarBg]={.12f,.12f,.13f,1};
    c[ImGuiCol_Tab]={.12f,.12f,.13f,1};c[ImGuiCol_TabSelected]={.21f,.23f,.26f,1};c[ImGuiCol_TabHovered]={.24f,.34f,.46f,1};
    c[ImGuiCol_TabSelectedOverline]={.23f,.55f,.86f,1};c[ImGuiCol_Header]={.2f,.32f,.46f,1};c[ImGuiCol_HeaderHovered]={.25f,.38f,.53f,1};
    c[ImGuiCol_Button]={.24f,.24f,.26f,1};c[ImGuiCol_ButtonHovered]={.3f,.37f,.46f,1};c[ImGuiCol_CheckMark]={.3f,.63f,.95f,1};
    c[ImGuiCol_DockingEmptyBg]={.08f,.08f,.09f,1};
}

enum class Action { None,New,Open,Quit };
struct Editor
{
    editor::Document document;
    OrbitCamera camera;
    fs::path project,folder;
    std::vector<std::string> messages;
    std::vector<fs::directory_entry> files;
    std::optional<std::vector<editor::Object>> editBefore;
    bool quit=false,failed=false,resetLayout=false,openFileDialog=false,saveDialog=false,confirmDiscard=false;
    bool showGrid=true;
    Action pending=Action::None;
    fs::path pendingPath;
    char filePath[1024]={},fileFilter[128]={};
    std::string fileError;
    ImVec2 imageOrigin{},imageSize={1,1};

    explicit Editor(fs::path root):project(std::move(root)),folder(project/"Assets")
    {
        fs::create_directories(project/"Assets"/"Scenes");Refresh();
        Log("Editor ready. Select an object in the Scene or Hierarchy.");
        Log("RMB orbit | MMB pan | wheel zoom | F frame selected | Ctrl+S save");
    }
    void Log(const std::string& text){messages.push_back(text);if(messages.size()>500)messages.erase(messages.begin());}
    void Refresh()
    {
        files.clear();std::error_code ec;
        for(fs::directory_iterator it(folder,ec),end;!ec && it!=end;it.increment(ec))files.push_back(*it);
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){std::error_code ec1,ec2;const bool ad=a.is_directory(ec1),bd=b.is_directory(ec2);return ad!=bd?ad:a.path().filename()<b.path().filename();});
        if(ec)Log("Cannot read directory: "+ec.message());
    }
    void FinishEdit()
    {
        if(editBefore){if(document.Commit(*editBefore))Log("Object properties changed.");editBefore.reset();}
    }
    void Execute(Action action,const fs::path& path={})
    {
        FinishEdit();
        if(action==Action::New){document.New();Log("New scene.");}
        else if(action==Action::Open){std::string error;if(document.Load(path,error))Log("Loaded "+path.filename().u8string());else Log("Load failed: "+error);}
        else if(action==Action::Quit)quit=true;
    }
    void Request(Action action,const fs::path& path={})
    {
        FinishEdit();pending=action;pendingPath=path;
        if(document.Dirty())confirmDiscard=true;
        else{Execute(action,path);pending=Action::None;}
    }
    bool Save(const fs::path& path)
    {
        FinishEdit();std::string error;
        if(!document.Save(path,error)){Log("Save failed: "+error);fileError=error;return false;}
        Log("Saved "+path.filename().u8string());Refresh();return true;
    }
    void ShowSave()
    {
        saveDialog=true;fileError.clear();const auto path=document.path.empty()?project/"Assets"/"Scenes"/"Untitled.dyscene":document.path;
        std::snprintf(filePath,sizeof(filePath),"%s",path.u8string().c_str());
    }
    void SaveCurrent(){if(document.path.empty())ShowSave();else Save(document.path);}
    void Add(editor::Primitive p){FinishEdit();document.Add(p);Log(std::string("Created ")+editor::PrimitiveName(p));}
    void Menus()
    {
        if(ImGui::BeginMainMenuBar())
        {
            if(ImGui::BeginMenu("File"))
            {
                if(ImGui::MenuItem("New scene","Ctrl+N",false,!document.playing))Request(Action::New);
                if(ImGui::MenuItem("Open scene...","Ctrl+O",false,!document.playing)){openFileDialog=true;fileError.clear();std::snprintf(filePath,sizeof(filePath),"%s",(project/"Assets"/"Scenes").u8string().c_str());}
                if(ImGui::MenuItem("Save","Ctrl+S"))SaveCurrent();
                if(ImGui::MenuItem("Save as..."))ShowSave();
                ImGui::Separator();if(ImGui::MenuItem("Exit"))Request(Action::Quit);ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("Edit"))
            {
                if(ImGui::MenuItem("Undo","Ctrl+Z",false,document.CanUndo()&&!document.playing)){FinishEdit();document.Undo();Log("Undo.");}
                if(ImGui::MenuItem("Redo","Ctrl+Y",false,document.CanRedo()&&!document.playing)){document.Redo();Log("Redo.");}
                if(ImGui::MenuItem("Duplicate","Ctrl+D",false,document.selected>=0&&!document.playing)){FinishEdit();document.Duplicate();Log("Duplicated object.");}
                if(ImGui::MenuItem("Delete","Delete",false,document.selected>=0&&!document.playing)){FinishEdit();document.Delete();Log("Deleted object.");}ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("GameObject",!document.playing)){for(auto p:{editor::Primitive::Cube,editor::Primitive::Sphere,editor::Primitive::Floor})if(ImGui::MenuItem(editor::PrimitiveName(p)))Add(p);ImGui::EndMenu();}
            if(ImGui::BeginMenu("Window")){if(ImGui::MenuItem("Reset layout"))resetLayout=true;ImGui::EndMenu();}
            ImGui::EndMainMenuBar();
        }
        const auto& io=ImGui::GetIO();
        if(!io.WantTextInput && !ImGui::IsAnyItemActive())
        {
            if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))SaveCurrent();
            if(!document.playing)
            {
                if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))Request(Action::New);
                if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)){openFileDialog=true;std::snprintf(filePath,sizeof(filePath),"%s",(project/"Assets"/"Scenes").u8string().c_str());}
                if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && document.Undo())Log("Undo.");
                if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y) && document.Redo())Log("Redo.");
                if(document.selected>=0 && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)){document.Duplicate();Log("Duplicated object.");}
                if(document.selected>=0 && ImGui::IsKeyPressed(ImGuiKey_Delete)){document.Delete();Log("Deleted object.");}
            }
        }
    }
    void Dock()
    {
        const auto* view=ImGui::GetMainViewport();const auto pos=view->WorkPos;const auto size=view->WorkSize;
        ImGui::SetNextWindowPos(pos);ImGui::SetNextWindowSize({size.x,36});
        ImGui::Begin("##Toolbar",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("dy_engine");ImGui::SameLine();ImGui::TextDisabled("|  %s%s",document.path.empty()?"Untitled":document.path.filename().u8string().c_str(),document.Dirty()?" *":"");
        ImGui::SameLine(std::max(300.f,size.x*.45f));
        ImGui::BeginDisabled(document.playing);if(ImGui::Button("Play",{65,0})){FinishEdit();document.Play();Log("Preview started (temporary animation).");}ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(!document.playing);
        if(ImGui::Button(document.paused?"Resume":"Pause",{65,0})){document.paused=!document.paused;Log(document.paused?"Preview paused.":"Preview resumed.");}
        ImGui::SameLine();if(ImGui::Button("Stop",{65,0})){document.Stop();Log("Preview stopped; edit transforms restored.");}ImGui::EndDisabled();
        ImGui::SameLine();ImGui::TextDisabled("%s",document.playing?(document.paused?"PAUSED":"PREVIEW"):"EDIT");ImGui::End();
        ImGui::SetNextWindowPos({pos.x,pos.y+36});ImGui::SetNextWindowSize({size.x,size.y-36});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::Begin("##DockHost",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus|ImGuiWindowFlags_NoSavedSettings);
        const ImGuiID dock=ImGui::GetID("EditorDock");
        if(!ImGui::DockBuilderGetNode(dock)||resetLayout)
        {
            resetLayout=false;ImGui::DockBuilderRemoveNode(dock);ImGui::DockBuilderAddNode(dock,ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock,{size.x,size.y-36});
            ImGuiID top=dock,bottom=ImGui::DockBuilderSplitNode(top,ImGuiDir_Down,.27f,nullptr,&top);
            ImGuiID scene=top,right=ImGui::DockBuilderSplitNode(scene,ImGuiDir_Right,.23f,nullptr,&scene);
            ImGuiID inspector=right,hierarchy=ImGui::DockBuilderSplitNode(inspector,ImGuiDir_Up,.36f,nullptr,&inspector);
            ImGui::DockBuilderDockWindow("Scene",scene);ImGui::DockBuilderDockWindow("Hierarchy",hierarchy);ImGui::DockBuilderDockWindow("Inspector",inspector);
            ImGui::DockBuilderDockWindow("Project",bottom);ImGui::DockBuilderDockWindow("Console",bottom);ImGui::DockBuilderFinish(dock);
        }
        ImGui::DockSpace(dock,{0,0});ImGui::End();ImGui::PopStyleVar();
    }
    bool ProjectPoint(M::float3 point,ImVec2& out) const
    {
        const auto p=M::TransformPoint(camera.camera.view,point);if(p.z>=-.05f)return false;
        const float x=p.x*camera.camera.projection.m[0]/-p.z,y=p.y*camera.camera.projection.m[5]/-p.z;
        out={imageOrigin.x+(x+1)*.5f*imageSize.x,imageOrigin.y+(1-y)*.5f*imageSize.y};return true;
    }
    void ScenePanel(Viewport& viewport,int testWidth=0,int testHeight=0)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        if(ImGui::Begin("Scene"))
        {
            ImGui::SetCursorPosX(8);ImGui::TextDisabled("Perspective  |  Lit");ImGui::SameLine();
            if(ImGui::SmallButton("Frame selected")&&document.selected>=0)camera.Focus(document.objects[document.selected]);
            ImGui::SameLine();ImGui::Checkbox("Grid",&showGrid);
            imageSize=ImGui::GetContentRegionAvail();imageSize.x=std::max(1.f,imageSize.x);imageSize.y=std::max(1.f,imageSize.y);
            if(!viewport.Resize(testWidth?testWidth:uint32_t(imageSize.x),testHeight?testHeight:uint32_t(imageSize.y))){failed=true;ImGui::TextUnformatted("Viewport texture creation failed.");}
            else
            {
                imageOrigin=ImGui::GetCursorScreenPos();ImGui::Image(ImTextureRef(viewport.id),imageSize);
                const bool hovered=ImGui::IsItemHovered()&&!testWidth;auto& io=ImGui::GetIO();
                camera.Update(float(viewport.texture->GetDesc().width)/viewport.texture->GetDesc().height);
                if(hovered)
                {
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Right)){camera.yaw-=io.MouseDelta.x*.008f;camera.pitch=std::clamp(camera.pitch+io.MouseDelta.y*.008f,-1.45f,1.45f);}
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                    {
                        const auto forward=M::Normalize(camera.target-camera.camera.position),right=M::Normalize(M::Cross(forward,{0,0,1})),up=M::Cross(right,forward);
                        camera.target=camera.target+right*(-io.MouseDelta.x*camera.distance*.0015f)+up*(io.MouseDelta.y*camera.distance*.0015f);
                    }
                    camera.distance=std::clamp(camera.distance*std::exp(-io.MouseWheel*.12f),.3f,150.f);
                    if(ImGui::IsKeyPressed(ImGuiKey_F)&&document.selected>=0)camera.Focus(document.objects[document.selected]);
                    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)){FinishEdit();document.selected=document.Pick(camera.camera.position,camera.Ray(io.MousePos,imageOrigin,imageSize));}
                }
                camera.Update(float(viewport.texture->GetDesc().width)/viewport.texture->GetDesc().height);
                auto* draw=ImGui::GetWindowDrawList();draw->PushClipRect(imageOrigin,{imageOrigin.x+imageSize.x,imageOrigin.y+imageSize.y},true);
                // Grid is an editor overlay; all scene surfaces are rendered by dyf::Renderer.
                if(showGrid)
                {
                    for(int i=-6;i<=6;++i)
                    {
                        ImVec2 a,b;
                        if(ProjectPoint({float(i),-5,.005f},a)&&ProjectPoint({float(i),5,.005f},b))draw->AddLine(a,b,IM_COL32(160,165,175,32));
                        if(ProjectPoint({-6,float(i),.005f},a)&&ProjectPoint({6,float(i),.005f},b))draw->AddLine(a,b,IM_COL32(160,165,175,32));
                    }
                }
                if(document.selected>=0)
                {
                    const auto transform=document.scene->GetEntity(static_cast<EntityID>(document.selected)).GetTransform();
                    ImVec2 points[8];bool visible[8];
                    for(int i=0;i<8;++i)visible[i]=ProjectPoint(M::TransformPoint(transform,{i&1?.5f:-.5f,i&2?.5f:-.5f,i&4?.5f:-.5f}),points[i]);
                    for(int i=0;i<8;++i)for(int axis=1;axis<=4;axis*=2)if(!(i&axis)&&visible[i]&&visible[i|axis])draw->AddLine(points[i],points[i|axis],IM_COL32(239,165,55,255),1.5f);
                }
                draw->AddText({imageOrigin.x+12,imageOrigin.y+imageSize.y-25},IM_COL32(205,208,213,220),"RMB Orbit  /  MMB Pan  /  Wheel Zoom  /  F Focus");draw->PopClipRect();
            }
        }
        ImGui::End();ImGui::PopStyleVar();
    }
    void Hierarchy()
    {
        ImGui::Begin("Hierarchy");
        ImGui::BeginDisabled(document.playing);if(ImGui::Button("+ Create"))ImGui::OpenPopup("Create primitive");
        if(ImGui::BeginPopup("Create primitive")){for(auto p:{editor::Primitive::Cube,editor::Primitive::Sphere,editor::Primitive::Floor})if(ImGui::MenuItem(editor::PrimitiveName(p)))Add(p);ImGui::EndPopup();}ImGui::EndDisabled();
        ImGui::SameLine();ImGui::TextDisabled("%zu objects",document.objects.size());ImGui::Separator();
        for(size_t i=0;i<document.objects.size();++i)
        {
            ImGui::PushID(int(i));const auto& o=document.objects[i];
            if(ImGui::Selectable(o.name.c_str(),document.selected==int(i))){FinishEdit();document.selected=int(i);}
            if(ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(0))camera.Focus(o);
            ImGui::PopID();
        }
        if(document.objects.empty())ImGui::TextDisabled("Create a primitive to begin.");
        ImGui::End();
    }
    void Inspector()
    {
        ImGui::Begin("Inspector");
        if(document.selected<0 || document.selected>=int(document.objects.size())){ImGui::TextDisabled("Select a scene object.");ImGui::End();return;}
        ImGui::BeginDisabled(document.playing);
        auto& o=document.objects[document.selected];const auto before=document.objects;
        const auto track=[&](bool changed)
        {
            if((ImGui::IsItemActivated()||changed)&&!editBefore)editBefore=before;
            if(changed)document.Sync();
            if(ImGui::IsItemDeactivatedAfterEdit())FinishEdit();
        };
        char name[128];std::snprintf(name,sizeof(name),"%s",o.name.c_str());
        const bool named=ImGui::InputText("Name",name,sizeof(name));if(named)o.name=name;track(named);
        ImGui::TextDisabled("%s  |  Entity %d",editor::PrimitiveName(o.primitive),document.selected);ImGui::Separator();
        if(ImGui::CollapsingHeader("Transform",ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushItemWidth(-75);
            track(ImGui::DragFloat3("Position",o.position.data(),.025f,-10000,10000,"%.2f",ImGuiSliderFlags_AlwaysClamp));
            track(ImGui::DragFloat3("Rotation",o.rotation.data(),.5f,-360000,360000,"%.1f",ImGuiSliderFlags_AlwaysClamp));
            track(ImGui::DragFloat3("Scale",o.scale.data(),.015f,.01f,1000,"%.2f",ImGuiSliderFlags_AlwaysClamp));ImGui::PopItemWidth();
        }
        ImGui::Spacing();if(ImGui::CollapsingHeader("Material",ImGuiTreeNodeFlags_DefaultOpen))
        {
            track(ImGui::ColorEdit3("Base color",o.color.data(),ImGuiColorEditFlags_Float));
            track(ImGui::SliderFloat("Roughness",&o.roughness,.04f,1,"%.2f",ImGuiSliderFlags_AlwaysClamp));
            track(ImGui::SliderFloat("Metallic",&o.metallic,0,1,"%.2f",ImGuiSliderFlags_AlwaysClamp));
        }
        if(editBefore&&!ImGui::IsAnyItemActive())FinishEdit();
        ImGui::Spacing();ImGui::Separator();
        if(ImGui::Button("Duplicate")){FinishEdit();document.Duplicate();Log("Duplicated object.");}
        ImGui::SameLine();if(ImGui::Button("Delete")){FinishEdit();document.Delete();Log("Deleted object.");}
        ImGui::EndDisabled();if(document.playing)ImGui::TextWrapped("Stop preview to edit the scene.");ImGui::End();
    }
    void Project()
    {
        ImGui::Begin("Project");
        ImGui::BeginChild("Folders",{170,0},ImGuiChildFlags_Borders);
        ImGui::TextDisabled("PROJECT");
        if(ImGui::Selectable(project.filename().u8string().c_str(),folder==project)){folder=project;Refresh();}
        if(ImGui::Selectable("  Assets",folder==project/"Assets")){folder=project/"Assets";Refresh();}
        if(ImGui::Selectable("    Scenes",folder==project/"Assets"/"Scenes")){folder=project/"Assets"/"Scenes";Refresh();}
        ImGui::Spacing();ImGui::TextWrapped("Double-click a .dyscene file to open it.");ImGui::EndChild();ImGui::SameLine();
        ImGui::BeginChild("Assets",{0,0});
        if(ImGui::SmallButton("Refresh"))Refresh();ImGui::SameLine();
        ImGui::BeginDisabled(folder==project);if(ImGui::SmallButton("Up")){folder=folder.parent_path();Refresh();}ImGui::EndDisabled();
        ImGui::SameLine();ImGui::TextUnformatted(folder.lexically_relative(project).u8string().c_str());
        ImGui::SetNextItemWidth(220);ImGui::InputTextWithHint("##FileFilter","Filter files...",fileFilter,sizeof(fileFilter));
        ImGui::TextDisabled("PRIMITIVES");ImGui::BeginDisabled(document.playing);
        for(auto p:{editor::Primitive::Cube,editor::Primitive::Sphere,editor::Primitive::Floor})
        {
            if(p!=editor::Primitive::Cube)ImGui::SameLine();
            const std::string label=std::string("+ ")+editor::PrimitiveName(p)+"\nCreate in Scene";
            if(ImGui::Button(label.c_str(),{145,47}))Add(p);
        }
        ImGui::EndDisabled();ImGui::Separator();
        for(const auto& entry:files)
        {
            const auto name=entry.path().filename().u8string();if(*fileFilter&&name.find(fileFilter)==std::string::npos)continue;
            std::error_code ec;const bool directory=entry.is_directory(ec);const bool scene=entry.path().extension()==".dyscene";
            const auto label=(directory?"[Folder] ":scene?"[Scene]  ":"[File]   ")+name;
            if(ImGui::Selectable(label.c_str(),false,ImGuiSelectableFlags_AllowDoubleClick)&&ImGui::IsMouseDoubleClicked(0))
            {
                if(directory){folder=entry.path();Refresh();break;}
                if(scene&&!document.playing)Request(Action::Open,entry.path());
            }
        }
        if(files.empty())ImGui::TextDisabled("No files in this directory.");
        ImGui::EndChild();ImGui::End();
    }
    void Console()
    {
        ImGui::Begin("Console");if(ImGui::Button("Clear"))messages.clear();ImGui::SameLine();ImGui::TextDisabled("%zu messages",messages.size());ImGui::Separator();
        ImGui::BeginChild("Log");for(const auto& message:messages)ImGui::TextUnformatted(message.c_str());ImGui::EndChild();ImGui::End();
    }
    void Dialogs()
    {
        if(confirmDiscard){ImGui::OpenPopup("Unsaved scene");confirmDiscard=false;}
        if(ImGui::BeginPopupModal("Unsaved scene",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Save changes before leaving this scene?");
            if(ImGui::Button("Save",{85,0}))
            {
                if(document.path.empty()){ImGui::CloseCurrentPopup();ShowSave();}
                else if(Save(document.path)){Execute(pending,pendingPath);pending=Action::None;ImGui::CloseCurrentPopup();}
            }
            ImGui::SameLine();if(ImGui::Button("Discard",{85,0})){Execute(pending,pendingPath);pending=Action::None;ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Cancel",{85,0})){pending=Action::None;ImGui::CloseCurrentPopup();}
            if(!fileError.empty())ImGui::TextWrapped("%s",fileError.c_str());ImGui::EndPopup();
        }
        if(saveDialog){ImGui::OpenPopup("Save scene");saveDialog=false;}
        if(openFileDialog){ImGui::OpenPopup("Open scene");openFileDialog=false;}
        for(bool saving:{true,false})
        {
            if(ImGui::BeginPopupModal(saving?"Save scene":"Open scene",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Scene path (.dyscene)");ImGui::SetNextItemWidth(650);ImGui::InputText("##ScenePath",filePath,sizeof(filePath));
                if(!fileError.empty())ImGui::TextWrapped("%s",fileError.c_str());
                if(ImGui::Button(saving?"Save":"Open",{85,0}))
                {
                    fs::path chosen=fs::u8path(filePath);if(chosen.is_relative())chosen=project/chosen;
                    if(chosen.extension()!=".dyscene")fileError="Choose a .dyscene file.";
                    else if(saving){if(Save(chosen)){ImGui::CloseCurrentPopup();if(pending!=Action::None){Execute(pending,pendingPath);pending=Action::None;}}}
                    else{ImGui::CloseCurrentPopup();Request(Action::Open,chosen);}
                }
                ImGui::SameLine();if(ImGui::Button("Cancel",{85,0})){pending=Action::None;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
            }
        }
    }
};

static bool Capture(RHI::IDevice& device,RHI::TextureHandle texture,const fs::path& path)
{
    RHI::TextureReadback read;if(!device.ReadTexture(texture,read))return false;
    std::ofstream out(path,std::ios::binary);out<<"P6\n"<<read.width<<' '<<read.height<<"\n255\n";
    const bool bgra=read.format==RHI::Format::B8G8R8A8_UNORM || read.format==RHI::Format::B8G8R8A8_UNORM_SRGB;
    for(uint32_t y=0;y<read.height;++y)for(uint32_t x=0;x<read.width;++x){const auto* p=read.pixels.data()+size_t(y)*read.rowPitch+x*4;const char rgb[]={char(p[bgra?2:0]),char(p[1]),char(p[bgra?0:2])};out.write(rgb,3);}
    return out.good();
}
static bool HashPixels(const RHI::TextureReadback& read,int left,int top,int right,int bottom,uint64_t& hash)
{
    left=std::clamp(left,0,int(read.width));right=std::clamp(right,0,int(read.width));
    top=std::clamp(top,0,int(read.height));bottom=std::clamp(bottom,0,int(read.height));
    if(left>=right || top>=bottom)return false;
    std::set<uint32_t> colors;hash=1469598103934665603ull;
    for(int y=top;y<bottom;++y)for(int x=left;x<right;++x)
    {
        const auto* p=read.pixels.data()+size_t(y)*read.rowPitch+x*4;
        colors.insert(uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16));
        for(int c=0;c<3;++c){hash^=p[c];hash*=1099511628211ull;}
    }
    return colors.size()>64;
}
static bool InspectPixels(RHI::IDevice& device,RHI::TextureHandle texture,uint64_t& hash)
{
    RHI::TextureReadback read;if(!device.ReadTexture(texture,read))return false;
    return HashPixels(read,0,0,int(read.width),int(read.height),hash);
}
static bool InspectComposite(RHI::IDevice& device,RHI::TextureHandle texture,const Editor& app,uint64_t& hash)
{
    RHI::TextureReadback read;if(!device.ReadTexture(texture,read))return false;
    const auto display=ImGui::GetIO().DisplaySize,origin=ImGui::GetMainViewport()->Pos;
    if(display.x<=0 || display.y<=0)return false;
    const float scaleX=read.width/display.x,scaleY=read.height/display.y;
    // Exclude panel chrome and the bottom camera-help text; inspect the composited Scene Image itself.
    return HashPixels(read,int((app.imageOrigin.x-origin.x+8)*scaleX),int((app.imageOrigin.y-origin.y+8)*scaleY),
        int((app.imageOrigin.x-origin.x+app.imageSize.x-8)*scaleX),int((app.imageOrigin.y-origin.y+app.imageSize.y-36)*scaleY),hash);
}

int main(int argc,char** argv)
{
    bool selfTest=false;int maxFrames=0;fs::path project=fs::current_path()/"EditorProject",capture;
    for(int i=1;i<argc;++i)
    {
        if(std::strcmp(argv[i],"--self-test")==0){selfTest=true;if(!maxFrames)maxFrames=32;}
        else if(std::strncmp(argv[i],"--frames=",9)==0){auto p=argv[i]+9;const auto result=std::from_chars(p,p+std::strlen(p),maxFrames);if(result.ec!=std::errc{}||*result.ptr||maxFrames<1)return 1;}
        else if(std::strncmp(argv[i],"--project=",10)==0)project=fs::u8path(argv[i]+10);
        else if(std::strncmp(argv[i],"--capture=",10)==0)capture=fs::u8path(argv[i]+10);
        else{std::fprintf(stderr,"Unknown option: %s\n",argv[i]);return 1;}
    }
    if(selfTest&&maxFrames<32)return 1;if(!capture.empty()&&!maxFrames)maxFrames=60;
    try
    {
        project=fs::absolute(project);Editor app(project);
        if(selfTest){std::string error;if(!editor::SelfTest(project/"SelfTest",error)){std::fprintf(stderr,"FAIL model: %s\n",error.c_str());return 1;}std::puts("PASS model edit, duplicate, delete, undo/redo, scene roundtrip, malformed preservation, failed save preservation, preview restoration, picking.");}
        Platform::Window window(1440,900,"dy_engine - 3D Scene Editor");if(!window.GetHandle())return 1;
        RendererConfig config;config.clearColor={.025f,.027f,.032f,1};config.enableProfilerHud=false;config.vsync=!selfTest;
        config.allowReadback=selfTest||!capture.empty();config.lighting.shadows=true;config.lighting.ambientIntensity=.12f;
        auto renderer=Renderer::Create(window.GetHandle(),config);if(!renderer)return 1;
        const auto ini=(project/"editor-layout.ini").u8string(); // The context retains this pointer until Gui destruction.
        auto& device=renderer->GetDevice();auto gui=Gui::Create(window,device);if(!gui)return 1;
        ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_DockingEnable|ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetIO().IniFilename=selfTest?nullptr:ini.c_str();
#ifdef _WIN32
        ImFontConfig font;font.OversampleH=font.OversampleV=1;
        ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/malgun.ttf",15,&font,ImGui::GetIO().Fonts->GetGlyphRangesKorean());
#endif
        Style();Viewport viewport{device,*gui};uint64_t baselineHash=0,editedHash=0,restoredHash=0,resizedHash=0,previewHash=0,stoppedHash=0;
        uint64_t compositeBaseline=0,compositeEdited=0,compositeRestored=0;
        bool resized=false;auto previous=Clock::now();int completed=0;
        for(int frame=0;!app.quit&&(!maxFrames||frame<maxFrames);++frame)
        {
            window.PollEvents();
            if(!window.IsRunning())
            {
                glfwSetWindowShouldClose(window.GetGlfwHandle(),GLFW_FALSE);app.Request(Action::Quit);if(app.quit)break;
                if(glfwGetWindowAttrib(window.GetGlfwHandle(),GLFW_ICONIFIED))glfwRestoreWindow(window.GetGlfwHandle());
            }
            int framebufferWidth=0,framebufferHeight=0;
            glfwGetFramebufferSize(window.GetGlfwHandle(),&framebufferWidth,&framebufferHeight);
            if(!framebufferWidth || !framebufferHeight || glfwGetWindowAttrib(window.GetGlfwHandle(),GLFW_ICONIFIED))
            {glfwWaitEventsTimeout(.05);continue;}
            if(selfTest)
            {
                if(frame==6){auto before=app.document.objects;for(auto& o:app.document.objects)if(o.primitive!=editor::Primitive::Floor)o.color={.05f,.12f,.95f,1};app.document.Commit(before);}
                if(frame==10)app.document.Undo();
                if(frame==14){glfwSetWindowSize(window.GetGlfwHandle(),1280,800);resized=true;}
                if(frame==18)app.document.Play();
                if(frame==24)app.document.Stop();
            }
            const auto now=Clock::now();app.document.Tick(selfTest?.1f:std::min(.1f,std::chrono::duration<float>(now-previous).count()));previous=now;
            if(!gui->BeginFrame()){if(device.IsLost())return 1;glfwWaitEventsTimeout(.01);continue;}
            app.Menus();app.Dock();app.ScenePanel(viewport,selfTest?(frame<14?800:640):0,selfTest?(frame<14?500:400):0);
            app.Hierarchy();app.Inspector();app.Project();app.Console();app.Dialogs();gui->EndFrame();
            if(app.failed)return 1;
            if(viewport.texture)
            {
                if(!renderer->RenderToTexture(*app.document.scene,app.camera.camera,viewport.texture,viewport.state))return 1;
                viewport.state=RHI::ResourceState::ShaderResource;
                if(selfTest)
                {
                    uint64_t* hash=frame==4?&baselineHash:frame==8?&editedHash:frame==12?&restoredHash:frame==16?&resizedHash:frame==22?&previewHash:frame==28?&stoppedHash:nullptr;
                    if(hash&&!InspectPixels(device,viewport.texture,*hash)){std::fprintf(stderr,"FAIL viewport readback frame %d\n",frame);return 1;}
                }
            }
            const auto acquireStart=Clock::now();
            bool acquired=false;
            while(!(acquired=device.BeginFrame()))
            {
                if(device.IsLost() || (maxFrames && Clock::now()-acquireStart>std::chrono::seconds(10)))return 1;
                glfwWaitEventsTimeout(.001);
                if(!maxFrames || !window.IsRunning() || glfwGetWindowAttrib(window.GetGlfwHandle(),GLFW_ICONIFIED))break;
            }
            if(!acquired)continue; // Retry through the event/UI loop so close always reaches the dirty-scene dialog.
            RHI::ResourceScope resources(device);auto* commands=resources.Keep(device.AcquireCommandList());auto* target=device.GetBackBuffer();if(!commands||!target)return 1;
            const RHI::ResourceBarrierDesc begin{nullptr,target,RHI::ResourceState::Present,RHI::ResourceState::RenderTarget,{}};commands->ResourceBarrier(&begin,1);
            RHI::ColorAttachment color;color.texture=target;color.loadOp=RHI::LoadOp::Clear;color.storeOp=RHI::StoreOp::Store;color.clearColor[3]=1;
            commands->BeginRendering({&color,1,nullptr});commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr,target,RHI::ResourceState::RenderTarget,RHI::ResourceState::Present,{}};commands->ResourceBarrier(&end,1);
            if(!gui->Record(*commands,target)||!commands->Close()||!device.Submit(&commands,1))return 1;
            if(selfTest)
            {
                uint64_t* hash=frame==4?&compositeBaseline:frame==8?&compositeEdited:frame==12?&compositeRestored:nullptr;
                if(hash&&!InspectComposite(device,target,app,*hash)){std::fprintf(stderr,"FAIL composited Scene Image readback frame %d\n",frame);return 1;}
            }
            if(!capture.empty()&&frame==maxFrames-1&&!Capture(device,target,capture))return 1;
            if(!device.Present())return 1;++completed;
            const auto title=std::string("dy_engine - ")+(app.document.path.empty()?"Untitled":app.document.path.filename().u8string())+(app.document.Dirty()?" *":"")+" - 3D Scene Editor";
            glfwSetWindowTitle(window.GetGlfwHandle(),title.c_str());
        }
        if(!device.WaitIdle())return 1;
        if(selfTest)
        {
            const bool sampled=compositeBaseline&&compositeEdited!=compositeBaseline&&compositeRestored==compositeBaseline;
            const bool passed=baselineHash&&editedHash!=baselineHash&&restoredHash==baselineHash&&resized&&resizedHash&&previewHash!=resizedHash&&stoppedHash==resizedHash&&sampled;
            std::ofstream report(project/"SelfTest"/"result.txt");
            report<<(passed?"PASS":"FAIL")<<" gpu viewport, material edit, undo, resize, preview, stop\n"<<"frames="<<completed<<" baseline="<<baselineHash<<" edited="<<editedHash<<" restored="<<restoredHash<<" resized="<<resizedHash<<" preview="<<previewHash<<" stopped="<<stoppedHash<<'\n';
            report<<"composite_baseline="<<compositeBaseline<<" composite_edited="<<compositeEdited<<" composite_restored="<<compositeRestored<<'\n';
            std::printf("%s GPU viewport sampled through ImGui; frames=%d material_change=%d undo_restore=%d resize=%d preview_change=%d stop_restore=%d composite_change_restore=%d\n",passed?"PASS":"FAIL",completed,editedHash!=baselineHash,restoredHash==baselineHash,resized,previewHash!=resizedHash,stoppedHash==resizedHash,sampled);
            if(!passed)return 1;
        }
    }
    catch(const std::exception& e){std::fprintf(stderr,"DyEditor: %s\n",e.what());return 1;}
    return 0;
}
