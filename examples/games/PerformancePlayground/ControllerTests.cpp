#include "Controller.h"
#include <cstdio>
#include <stdexcept>

using namespace playground;
void Check(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
int main() try
{
    Controller straight, diagonal, running;
    for(int i=0;i<60;++i) {
        straight.Move(0,1,false,1.f/60,{});
        diagonal.Move(1,1,false,1.f/60,{});
        running.Move(0,1,true,1.f/60,{});
    }
    Check(std::abs(Length(straight.position)-3.f)<.001f,"walk speed must be 3 m/s");
    Check(std::abs(Length(diagonal.position)-3.f)<.001f,"diagonal must not accelerate");
    Check(std::abs(Length(running.position)-6.f)<.001f,"run speed must be 6 m/s");
    const std::vector<Box> walls={{-4,2,-1,-1,2.2f,4},{1,2,-1,4,2.2f,4}};
    Controller door;
    for(int i=0;i<90;++i) door.Move(0,1,false,1.f/60,walls);
    Check(door.position.y>3,"door opening must remain walkable");
    Controller wall; wall.position={2,0};
    for(int i=0;i<120;++i) wall.Move(0,1,true,.05f,walls);
    Check(wall.position.y<1.69f,"thin wall must block running without tunnelling");
    wall.Move(-1,1,true,.05f,walls);
    Check(wall.position.x<2,"wall collision must allow sliding");
    Controller paused; paused.Move(0,1,true,30,{});
    Check(paused.position.y<=.301f,"long pause must not teleport");
    const auto before=paused.position;
    paused.Move(1,1,true,-1,{});
    Check(Length(paused.position-before)==0,"negative dt must not move");
    Controller bounds; bounds.position={47.7f,0}; bounds.Move(1,0,true,.05f,{});
    Check(bounds.position.x<=47.7f,"world bounds must contain player");
    const float camera=CameraFraction({0,0,1.5f},{0,4,2.5f},{{-1,2,0,1,2.2f,4}});
    Check(camera>.35f&&camera<.5f,"camera must stop before wall");
    Check(CameraFraction({0,0,1.5f},{0,4,2.5f},{})==1,"open camera path must stay full length");
    Check(CameraFraction({0,0,1.5f},{0,4,2.5f},{{-1,2,4,1,3,5}})==1,"camera height must clear overhead box");
    std::puts("Playground controller: speed, diagonal, sprint, doorway, wall/slide, dt, bounds, camera PASS");
    return 0;
}
catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
