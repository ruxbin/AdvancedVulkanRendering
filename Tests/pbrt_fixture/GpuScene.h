#pragma once
// CPU fixture for the exporter only. No Vulkan initialization or GPU behavior
// is simulated. The application build separately checks the real GpuScene ABI.
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
struct vec2 { float x=0, y=0; vec2(float a=0,float b=0):x(a),y(b){} };
struct vec3 { float x=0,y=0,z=0; vec3(float a=0,float b=0,float c=0):x(a),y(b),z(c){} };
struct vec4 { float x=0,y=0,z=0,w=0; vec4(float a=0,float b=0,float c=0,float d=0):x(a),y(b),z(c),w(d){} };
struct AAPLTextureData {
    std::string _path;
    uint32_t _pathHash=0, _pixelFormat=0;
    unsigned long long _width=4,_height=4,_mipmapLevelCount=1;
    unsigned long long _pixelDataOffset=0,_pixelDataLength=0;
    std::vector<unsigned long> _mipOffsets, _mipLengths;
};
struct TextureStreamingEntry { const AAPLTextureData* desc=nullptr; };
struct AAPLMeshData {
    // Loader's MTLIndexType enum: UInt16=0, UInt32=1. Fixture modes also
    // exercise byte-width tags 2/4; the actual Bistro asset uses 4.
    unsigned long long _vertexCount=3,_indexCount=3,_indexType=1,_meshCount=1,_materialCount=1;
    unsigned long long _opaqueMeshCount=1,_alphaMaskedMeshCount=0,_transparentMeshCount=0;
    void *_vertexData=nullptr,*_normalData=nullptr,*_uvData=nullptr,*_indexData=nullptr,*_textureData=nullptr;
    uint64_t _textureDataLength=0;
    std::filesystem::path _textureBlobPath;
    uint64_t _textureBlobBase=0;
};
struct AAPLMaterial {
    vec4 baseColor{0.4f,0.5f,0.6f,1};
    unsigned int baseColorTextureHash=0; bool hasBaseColorTexture=false,hasDiffuseMask=false;
    vec4 metallicRoughness{0,0.5f,0,0};
    unsigned int metallicRoughnessHash=0; bool hasMetallicRoughnessTexture=false;
    unsigned int normalMapHash=0; bool hasNormalMap=false;
    vec4 emissiveColor{}; unsigned int emissiveTextureHash=0; bool hasEmissiveTexture=false;
    float opacity=1;
};
struct AAPLSubMesh { uint32_t materialIndex=0,indexBegin=0,indexCount=3; };
struct VkExtent2D { uint32_t width=64,height=64; };
struct FixtureDevice { VkExtent2D extent; const VkExtent2D& getSwapChainExtent() const { return extent; } };
struct Camera {
    vec3 eye{0,0,-3}; const vec3& GetOrigin() const { return eye; }
    vec3 GetCameraDir() const { return {0,0,1}; } float Fov() const { return 1.0f; }
};
struct PointLightData { vec4 posSqrRadius,color; };
struct FixturePointLight { PointLightData data; const PointLightData* getPointLightData() const { return &data; } };
struct SpotLightData { vec4 posAndHeight,dirAndOuterAngle,colorAndInnerAngle; };
struct FixtureSpotLight { SpotLightData* _spotLightData=nullptr; };
struct GpuScene {
    AAPLMeshData* applMesh=nullptr; AAPLMaterial* cpuMaterials=nullptr; AAPLSubMesh* m_SubMeshes=nullptr;
    Camera* maincamera=nullptr; FixtureDevice device;
    struct { vec3 sunDirection{0,1,0},sunColor{1,1,1}; float localLightIntensity=1,emissiveScale=2; } frameConstants;
    std::vector<FixturePointLight> _pointLights; std::vector<FixtureSpotLight> _spotLights;
    std::unordered_map<uint32_t,size_t> streamingEntryMap;
    std::vector<TextureStreamingEntry> streamingEntries;
};
