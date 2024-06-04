#pragma once
#include "Matrix.h"
const int WINDOW_WIDTH = 800;
const int WINDOW_HEIGHT= 600;



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

struct AAPLMeshChunk
{
    AAPLBoundingBox3 boundingBox;
    vec4 normalDistribution;
    vec4 cluterMean;

    AAPLSphere boundingSphere;

    unsigned int materialIndex;
    unsigned int indexBegin;
    unsigned int indexCount;
};