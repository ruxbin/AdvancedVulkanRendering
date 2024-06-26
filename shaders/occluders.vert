#version 450

layout(location = 0) in vec3 position;

layout (set=0,binding=0 )uniform camera
{
    mat4 projectionMatrix;
    mat4 viewMatrix;
} Camera;


//push constants block
layout( push_constant ) uniform constants
{
	mat4 objectToCameraMatrix;
} PushConstants;

void main() {
    mat4 finalMatrix = Camera.projectionMatrix * PushConstants.objectToCameraMatrix;
    vec4 pos1=vec4(position,1.0);
    gl_Position = finalMatrix * pos1;
}
