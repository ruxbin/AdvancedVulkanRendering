#pragma once
#include "Matrix.h"
const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT= 1080;



//appl definitions
struct AAPLBoundingBox3
{
    alignas(16) vec3 min;
    alignas(16) vec3 max;
};


struct AAPLSphere
{
    vec4 data;//xyz center, w radius
};

struct alignas(16) AAPLMeshChunk
{
    AAPLBoundingBox3 boundingBox;
    vec4 normalDistribution;
    vec4 cluterMean;

    AAPLSphere boundingSphere;

    unsigned int materialIndex;
    unsigned int indexBegin;
    unsigned int indexCount;
};