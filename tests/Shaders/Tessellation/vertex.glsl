#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 0) out vec2 controlPosition;

void main()
{
    controlPosition = inPosition;
}
