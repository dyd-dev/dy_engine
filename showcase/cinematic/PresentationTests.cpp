#include "Presentation.h"
#include "CinematicTask.h"
#include <cstdio>
#include <stdexcept>

namespace
{
void Check(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}
showcase::CinematicTask RetainedScene(int& calls, int& alive)
{
    struct Lifetime { int& alive; Lifetime(int& value):alive(value){++alive;} ~Lifetime(){--alive;} } lifetime(alive);
    ++calls;
    co_yield showcase::PresentationStep::Ready;
    ++calls;
    co_yield showcase::PresentationStep::Restart;
    co_return 7;
}
}

int main()
{
    try
    {
        showcase::PresentationTimeline timeline;
        struct Boundary { double seconds; unsigned shot,phase; bool finished; };
        for(const auto& test : {Boundary{0,0,0,false}, {1.999,0,0,false}, {2,0,1,false},
            {3.999,0,1,false}, {4,0,2,false}, {5.999,0,2,false}, {6,1,0,false},
            {26,4,1,false}, {28,4,2,false}, {30,4,2,true}, {90,4,2,true}})
        {
            timeline.Seek(test.seconds);
            const auto frame=timeline.Frame();
            Check(frame.shot==test.shot && frame.phase==test.phase && frame.finished==test.finished,
                "A presentation boundary selected the wrong shot, ON/OFF phase or completion state");
            Check(frame.Enabled()==(test.phase==1), "Only seconds 2 through 4 must be ON");
        }
        timeline.Seek(1);
        timeline.Advance(10,false);
        Check(timeline.Seconds()==1,"Paused presentation advanced");
        timeline.Advance(1,true);
        Check(timeline.Seconds()==2 && timeline.Frame().Enabled(),"Playback did not enter ON at two seconds");
        timeline.Seek(-5);
        Check(timeline.Seconds()==0,"Seeking before the beginning was not clamped");
        timeline.Advance(40,true);
        Check(timeline.Seconds()==2 && timeline.Frame().Enabled(),"A stalled frame skipped visible demonstration phases");
        timeline.Seek(90);
        Check(timeline.Seconds()==31 && !timeline.Frame().Enabled(),"The last OFF frame must remain for a one-second scene transition");
        timeline.Seek(0);
        Check(!timeline.Frame().finished && timeline.Frame().shot==0,"Restart did not reset completion");
        timeline.Seek(30);
        Check(!timeline.TransitionReady(),"Scene advanced without the final one-second hold");
        timeline.Advance(.999,true);
        Check(!timeline.TransitionReady(),"Scene advanced before one second elapsed");
        timeline.Advance(.001,true);
        Check(timeline.TransitionReady(),"Scene did not advance after the hold");
        timeline.Seek(2);
        timeline.WaitForFrame();
        timeline.Advance(3,true);
        Check(timeline.Seconds()==2 && timeline.Frame().Enabled(),"Acquire retries consumed an ON phase before any frame was submitted");
        Check(timeline.FrameSubmitted(),"First submitted frame did not release the timeline");
        timeline.Advance(1.99,true);
        Check(timeline.Frame().Enabled(),"A newly submitted ON frame was shown for less than two seconds");
        timeline.Advance(.02,true);
        Check(!timeline.Frame().Enabled(),"ON phase failed to end after two displayed seconds");
        int calls=0,alive=0;
        {
            auto scene=RetainedScene(calls,alive);
            Check(calls==0,"An inactive scene started loading prematurely");
            Check(scene.Resume()==showcase::PresentationStep::Ready && calls==1 && alive==1,
                "Preloading did not retain scene resources");
            Check(scene.Resume()==showcase::PresentationStep::Restart && calls==2 && alive==1,
                "Resuming a prepared scene reloaded or destroyed it");
            scene.Resume();
            Check(scene.Done() && scene.Result()==7 && alive==0,"Scene return or resource cleanup was lost");
        }
        {
            auto scene=RetainedScene(calls,alive);
            scene.Resume();
        }
        Check(alive==0,"Cancelling a suspended scene leaked its resources");
        for(unsigned scene=0;scene<3;++scene)
            Check(showcase::NextPresentationScene(scene,showcase::PresentationStep::Restart)==0,
                "R must restart from Bistro regardless of the active asset");
        Check(showcase::NextPresentationScene(0,showcase::PresentationStep::Next)==1 &&
            showcase::NextPresentationScene(1,showcase::PresentationStep::Next)==2 &&
            showcase::NextPresentationScene(2,showcase::PresentationStep::Next)==2,
            "Scene order must be Bistro, Interior, Emerald and then hold");
        using F=showcase::PresentationFeature;
        dyf::RendererConfig baseline,config;
        baseline.exposure=.12f;
        baseline.lighting.shadows=true;
        baseline.advanced.fog.enabled=false;
        baseline.advanced.diffuseGI=false;
        showcase::ConfigurePresentation(baseline);
        Check(!baseline.advanced.ambientOcclusion && !baseline.advanced.diffuseGI && !baseline.advanced.reflections &&
            !baseline.enableOcclusionCulling && baseline.lighting.shadows,
            "Presentation still pays for excluded screen-space effects or lost its demonstrated shadows");
        showcase::ApplyPresentationFeature(config,baseline,F::Shadows,false);
        Check(!config.lighting.shadows,"Shadow OFF did not reach renderer configuration");
        showcase::ApplyPresentationFeature(config,baseline,F::Fog,true);
        Check(config.lighting.shadows && config.advanced.fog.enabled && config.exposure==.12f,
            "Next shot failed to restore previous effect or changed exposure");
        showcase::ApplyPresentationFeature(config,baseline,F::Surface,true);
        Check(!config.advanced.diffuseGI && config.advanced.surfaceDebugView==dyf::SurfaceDebugView::Normal,
            "Surface shot failed to restore scene-specific GI baseline");
        showcase::ApplyPresentationFeature(config,baseline,F::Lod,true);
        Check(config.enableLod && config.debugLodColor && config.advanced.surfaceDebugView==dyf::SurfaceDebugView::None,
            "LOD view lost its colors or retained surface debug output");
        showcase::ApplyPresentationFeature(config,baseline,F::Lod,false);
        Check(!config.enableLod && config.debugLodColor,"LOD OFF must select original geometry while keeping its diagnostic colors");
        showcase::ApplyPresentationFeature(config,baseline,F::GpuPasses,true);
        Check(config.enableLod && !config.debugLodColor,"Profiler shot retained diagnostic LOD rendering");
        showcase::ApplyPresentationFeature(config,baseline,F::Bloom,true);
        Check(config.advanced.bloom && config.advanced.bloomStrength>baseline.advanced.bloomStrength,
            "Presentation bloom was not emphasized");
        showcase::ApplyPresentationFeature(config,baseline,F::Wireframe,true);
        Check(config.debugWireframe && config.advanced.bloomStrength==baseline.advanced.bloomStrength,
            "The next shot retained presentation bloom strength");
        showcase::ApplyPresentationFeature(config,baseline,F::Depth,false);
        Check(!config.debugWireframe && config.advanced.surfaceDebugView==dyf::SurfaceDebugView::None,
            "Debug OFF did not restore shaded rendering");
        Check(showcase::GetPresentationShot(showcase::PresentationScene::Exterior,3,false).feature==F::Surface,
            "Unsupported wireframe must use an available diagnostic view");
        std::puts("PASS presentation boundaries, pause, seek, completion and effect isolation");
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"Presentation test failed: %s\n",error.what());
        return 1;
    }
}
