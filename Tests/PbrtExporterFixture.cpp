#include <algorithm>
#include <iostream>
#include <cstring>
#include <cstdlib>
// Include the real implementation to exercise its block decoder as well as
// the public export API. GpuScene is the CPU fixture selected by include path.
#include "../Src/PbrtExporter.cpp"

std::vector<uint8_t> EncodedMip(const uint8_t* data, uint32_t length) {
    // Actual LZFSE uncompressed-block stream, wrapped in the .bin header.
    // No mock decoder: the production LZFSE decoder consumes these bytes.
    std::vector<uint8_t> result(24+8+length+4,0);
    auto put=[&](size_t offset,const auto& value) {
        std::memcpy(result.data()+offset,&value,sizeof(value));
    };
    put(0,uint32_t(2049)); put(8,uint64_t(length)); put(16,uint64_t(length+12));
    put(24,uint32_t(0x2d787662)); put(28,length);
    std::memcpy(result.data()+32,data,length);
    put(32+length,uint32_t(0x24787662));
    return result;
}

int main(int argc,char** argv) {
    if (argc < 3) return 2;
    std::string mode=argv[1];
    const bool lazyBlob = mode == "lazy_mask";
    if (lazyBlob) mode = "mask";
    const std::filesystem::path dir=argv[2];
    std::filesystem::create_directories(dir);
    if (mode=="bc3" || mode=="bc1") {
        uint8_t block[16]={255,255,0,0,0,0,0,0,0,0,255,255,255,255,255,255};
        uint8_t rgba[64]{};
        if(mode=="bc3") DecodeBC3Block(block,rgba); else DecodeBC1Block(block+8,rgba);
        std::ofstream(dir/"pixels.bin",std::ios::binary).write(reinterpret_cast<char*>(rgba),64);
        return 0;
    }
    vec3 positions[3]={{0,0,0},{1,0,0},{0,1,0}},normals[3]={{0,0,1},{0,0,1},{0,0,1}};
    vec2 uv[3]={{0,0},{1,0},{0,1}};
    // Backing storage is deliberately larger than the declared index count:
    // invalid-range tests must fail validation, not rely on an accidental crash.
    uint32_t indices[6]={0,1,2,0,1,2};
    uint16_t indices16[3]={0,1,2};
    AAPLMaterial materials[2]; AAPLSubMesh submeshes[2];
    submeshes[1].materialIndex=1;
    AAPLMeshData mesh; mesh._vertexData=positions; mesh._normalData=normals;
    mesh._uvData=uv; mesh._indexData=indices;
    GpuScene scene; Camera cam;
    scene.applMesh=&mesh; scene.cpuMaterials=materials; scene.m_SubMeshes=submeshes; scene.maincamera=&cam;
    if(mode.rfind("proxy_",0)==0) {
        std::vector<vec3> pv={{0,0,0},{2,0,0},{0,2,0},{0,0,2},
                              {10,0,0},{12,0,0},{10,2,0},{10,0,2}};
        std::vector<uint32_t> pi={0,2,1,0,1,3,0,3,2,1,2,3,
                                  4,6,5,4,5,7,4,7,6,5,6,7};
        materials[0].emissiveColor={1,1,1,0};
        materials[0].hasEmissiveTexture=true; materials[0].emissiveTextureHash=77;
        mesh._vertexCount=pv.size(); mesh._indexCount=pi.size(); submeshes[0].indexCount=pi.size();
        scene._pointLights.push_back({{{0.25f,0.25f,0.25f,100},{1,1,1,0}}});
        if(mode.rfind("proxy_street_",0)==0) {
            // Open metal shade; the light is outside its flat AABB. The near
            // face spans the clearance even though its vertices are outside.
            float x=mode=="proxy_street_far" ? 0.5f : 0.1f;
            pv={{x,-1,-1},{x,1,-1},{x,0,1},{10,0,0},{10,1,0},{10,0,1}};
            pi={0,1,2,3,4,5};
            mesh._vertexCount=pv.size(); mesh._indexCount=pi.size(); submeshes[0].indexCount=pi.size();
            materials[0].emissiveColor={}; materials[0].hasEmissiveTexture=false;
            scene._pointLights[0].data.posSqrRadius={0,0,0,100};
        }
        if(mode=="proxy_small_opening") {
            // Octahedral bulb with a small open neck (no top cap), as in Bistro.
            pv={{1,0,0},{0,1,0},{-1,0,0},{0,-1,0},{0,0,-1},
                {0.1f,0,0.9f},{0,0.1f,0.9f},{-0.1f,0,0.9f},{0,-0.1f,0.9f}};
            pi.clear();
            for(uint32_t j=0;j<4;++j) {
                uint32_t n=(j+1)%4;
                pi.insert(pi.end(),{j,4,n,j,n,5+n,j,5+n,5+j});
            }
            mesh._vertexCount=pv.size(); mesh._indexCount=pi.size(); submeshes[0].indexCount=pi.size();
            scene._pointLights[0].data.posSqrRadius={0,0,0,100};
        }
        if(mode=="proxy_outside") scene._pointLights[0].data.posSqrRadius={1.5f,1.5f,1.5f,100};
        if(mode=="proxy_none") scene._pointLights.clear();
        if(mode=="proxy_opaque") { materials[0].emissiveColor={}; materials[0].hasEmissiveTexture=false; }
        if(mode=="proxy_all") scene._pointLights.push_back({{{10.25f,0.25f,0.25f,100},{1,1,1,0}}});
        if(mode=="proxy_open") { pi.erase(pi.begin()+9,pi.begin()+12); mesh._indexCount=pi.size(); submeshes[0].indexCount=pi.size(); }
        if(mode=="proxy_vertex_attachment") {
            pv.push_back({-2,0,0}); pv.push_back({-2,-2,0});
            pi.insert(pi.end(),{0,8,9});
            mesh._vertexCount=pv.size(); mesh._indexCount=pi.size(); submeshes[0].indexCount=pi.size();
        }
        if(mode=="proxy_seams") {
            std::vector<vec3> split;
            for(auto& index:pi) { split.push_back(pv[index]); index=static_cast<uint32_t>(split.size()-1); }
            pv=std::move(split); mesh._vertexCount=pv.size();
        }
        if(mode=="proxy_mixed") {
            // An opaque housing at exactly the same location must remain.
            mesh._meshCount=mesh._materialCount=2;
            submeshes[1].indexBegin=0; submeshes[1].indexCount=12;
        }
        if(mode=="proxy_split_materials") {
            pi.resize(12); mesh._indexCount=12;
            mesh._meshCount=mesh._materialCount=2;
            submeshes[0].indexCount=6;
            submeshes[1].indexBegin=6; submeshes[1].indexCount=6;
            materials[1]=materials[0];
        }
        std::vector<uint16_t> shortIndices(pi.begin(),pi.end());
        mesh._vertexData=pv.data(); mesh._normalData=nullptr; mesh._uvData=nullptr;
        mesh._indexData=pi.data();
        if(mode=="proxy_16") { mesh._indexType=0; mesh._indexData=shortIndices.data(); }
        AAPLTextureData lightTexture;
        lightTexture._path="bulb.dds"; lightTexture._pathHash=77; lightTexture._pixelFormat=135;
        if(mode.rfind("proxy_street_",0)==0 && mode!="proxy_street_unrelated")
            lightTexture._path="Textures/Streetlight_Metal_BaseColor.dds";
        if(mode=="proxy_street_emissive") {
            lightTexture._path="Textures/Emissive_StreetLight_BaseColor.dds";
            materials[0].hasEmissiveTexture=true;
        }
        uint8_t whiteBlock[16]={255,255,0,0,0,0,0,0,255,255,0,0,0,0,0,0};
        auto lightPayload=EncodedMip(whiteBlock,16);
        lightTexture._pixelDataLength=lightPayload.size(); lightTexture._mipOffsets={0};
        lightTexture._mipLengths={static_cast<unsigned long>(lightPayload.size())};
        mesh._textureData=lightPayload.data(); mesh._textureDataLength=lightPayload.size();
        scene.streamingEntryMap[77]=0; scene.streamingEntries.push_back({&lightTexture});
        materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=77;
        materials[0].hasNormalMap=true; materials[0].normalMapHash=77;
        materials[0].hasMetallicRoughnessTexture=true; materials[0].metallicRoughnessHash=77;
        if(mode=="proxy_mixed") { materials[1].hasBaseColorTexture=true; materials[1].baseColorTextureHash=77; }
        bool ok=PbrtExporter::Export(scene,dir/"scene.pbrt");
        std::ofstream(dir/"result.txt") << (ok?"success":"failure");
        return 0;
    }
    if(mode=="point_positions") {
        // Asymmetric signed coordinates catch axis swaps, sign flips and
        // accidental reuse of the camera position or the preceding light.
        cam.eye={17,-23,41};
        scene._pointLights.push_back({{{-19.25f,-5.5f,18.75f,100},{1,0,0,0}}});
        scene._pointLights.push_back({{{7.125f,3.25f,-11.5f,4},{0,1,0,0}}});
    }
    AAPLTextureData tex; tex._path="fixture.dds"; tex._pathHash=1; tex._pixelFormat=135;
    // BC3: pixel 0 transparent, pixels 1..15 opaque; all RGB white.
    uint8_t block[32]={255,0,1,0,0,0,0,0,255,255,0,0,0,0,0,0,
                       255,255,0,0,0,0,0,0,0,248,0,248,0,0,0,0};
    mesh._textureData=block; tex._mipOffsets={0}; tex._mipLengths={16}; tex._pixelDataLength=16;
    scene.streamingEntryMap[1]=0; scene.streamingEntries.push_back({&tex});
    if(mode=="missing_color") { materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=99; }
    if(mode=="normal" || mode=="cropped_normal") {
        tex._pixelFormat=142; block[0]=block[1]=128; block[2]=0; block[8]=block[9]=128;
        materials[0].hasNormalMap=true; materials[0].normalMapHash=1;
        if(mode=="cropped_normal") { tex._width=3; tex._height=2; }
    }
    if(mode=="rough_texture") {
        // RGB565 green endpoints=32/63. sRGB decode then square gives ~0.04898.
        tex._pixelFormat=131; block[0]=0; block[1]=4; block[2]=0; block[3]=4;
        block[4]=block[5]=block[6]=block[7]=0; tex._mipLengths={8};
        materials[0].hasMetallicRoughnessTexture=true; materials[0].metallicRoughnessHash=1;
    }
    if(mode=="mask" || mode=="coverage" || mode=="shared_opacity") {
        materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=1;
        materials[0].hasDiffuseMask=(mode=="mask");
        if(mode=="coverage") materials[0].opacity=0.5f;
        if(mode=="shared_opacity") {
            materials[0].opacity=0.2f; materials[1]=materials[0]; materials[1].opacity=0.8f;
            mesh._materialCount=mesh._meshCount=2;
        }
    }
    if(mode=="missing_mask") { materials[0].hasDiffuseMask=true; materials[0].opacity=0.05f; materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=99; }
    if(mode=="emissive") materials[0].emissiveColor={1,0.5f,0.25f,0};
    if(mode=="emissive_texture") { materials[0].hasEmissiveTexture=true; materials[0].emissiveTextureHash=1; }
    if(mode=="missing_emissive") { materials[0].hasEmissiveTexture=true; materials[0].emissiveTextureHash=99; materials[0].emissiveColor={1,0,0,0}; }
    if(mode=="ply_failure") std::filesystem::create_directory(dir/"mesh_0.ply");
    if(mode=="no_geometry") mesh._vertexData=nullptr;
    if(mode=="scene_failure") std::filesystem::create_directory(dir/"scene.pbrt");
    if(mode=="null_mesh") scene.applMesh=nullptr;
    if(mode=="missing_materials") scene.cpuMaterials=nullptr;
    if(mode=="invalid_index_range") submeshes[0].indexBegin=3;
    if(mode=="incomplete_triangle") { mesh._indexCount=4; submeshes[0].indexCount=4; }
    if(mode=="invalid_index_type") mesh._indexType=8;
    if(mode=="invalid_vertex_index") indices[2]=3;
    if(mode=="empty_mesh") submeshes[0].indexCount=0;
    if(mode=="indices16") { mesh._indexType=0; mesh._indexData=indices16; }
    if(mode=="indices16_bytes") { mesh._indexType=2; mesh._indexData=indices16; }
    if(mode=="indices32_bytes") mesh._indexType=4;
    if(mode=="emissive_isolation") {
        materials[0].emissiveColor={1,0,0,0}; mesh._materialCount=mesh._meshCount=2;
    }
    if(mode=="close_opacities") {
        materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=1;
        materials[0].hasDiffuseMask=true; materials[0].opacity=0.0999999f;
        materials[1]=materials[0]; materials[1].opacity=0.1000001f;
        mesh._materialCount=mesh._meshCount=2;
    }
    AAPLTextureData otherTex;
    if(mode=="same_basename") {
        tex._path="first/shared.dds";
        otherTex._path="second/shared.dds"; otherTex._pathHash=2; otherTex._pixelFormat=135;
        otherTex._pixelDataOffset=16; otherTex._pixelDataLength=16;
        otherTex._mipOffsets={0}; otherTex._mipLengths={16};
        scene.streamingEntryMap[2]=1; scene.streamingEntries.push_back({&otherTex});
        materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=1;
        materials[1]=materials[0]; materials[1].baseColorTextureHash=2;
        mesh._materialCount=mesh._meshCount=2;
    }
    if(mode=="unsupported_format" || mode=="zero_texture_size" || mode=="invalid_stream_entry") {
        materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=1;
        materials[0].hasNormalMap=true; materials[0].normalMapHash=1;
        if(mode=="unsupported_format") { tex._pixelFormat=999; tex._mipLengths={8}; }
        if(mode=="zero_texture_size") tex._width=0;
        if(mode=="invalid_stream_entry") scene.streamingEntryMap[1]=50;
    }
    auto payload=EncodedMip(block,static_cast<uint32_t>(tex._mipLengths[0]));
    tex._mipLengths[0]=static_cast<unsigned long>(payload.size());
    tex._pixelDataLength=payload.size();
    if(mode=="same_basename") {
        otherTex._pixelDataOffset=payload.size();
        auto otherPayload=EncodedMip(block+16,16);
        otherTex._mipLengths[0]=static_cast<unsigned long>(otherPayload.size());
        otherTex._pixelDataLength=otherPayload.size();
        payload.insert(payload.end(),otherPayload.begin(),otherPayload.end());
    }
    if(mode=="truncated_mip" || mode=="bad_lzfse" || mode=="wrong_decoded_size" ||
       mode=="mip_out_of_range" || mode=="texture_out_of_range" ||
       mode=="bad_compression_mode" || mode=="bad_compressed_length" ||
       mode=="decoded_overflow" || mode=="coarser_mip") {
        materials[0].hasBaseColorTexture=true; materials[0].baseColorTextureHash=1;
        materials[0].hasNormalMap=true; materials[0].normalMapHash=1;
        if(mode=="truncated_mip") { payload.resize(8); tex._mipLengths[0]=8; tex._pixelDataLength=8; }
        if(mode=="bad_lzfse") std::memset(payload.data()+24,0,4);
        if(mode=="wrong_decoded_size") { uint64_t wrong=8; std::memcpy(payload.data()+8,&wrong,8); }
        if(mode=="mip_out_of_range") tex._mipOffsets[0]=1;
        if(mode=="texture_out_of_range") tex._pixelDataOffset=1;
        if(mode=="bad_compression_mode") std::memset(payload.data(),0,4);
        if(mode=="bad_compressed_length") { uint64_t wrong=1; std::memcpy(payload.data()+16,&wrong,8); }
        if(mode=="decoded_overflow") {
            payload=EncodedMip(block,32);
            uint64_t claimed=16; std::memcpy(payload.data()+8,&claimed,8);
            tex._mipLengths[0]=static_cast<unsigned long>(payload.size());
            tex._pixelDataLength=payload.size();
        }
        if(mode=="coarser_mip") {
            auto coarse=payload;
            std::memset(payload.data()+24,0,4);
            tex._mipmapLevelCount=2;
            tex._mipOffsets.push_back(static_cast<unsigned long>(payload.size()));
            tex._mipLengths.push_back(static_cast<unsigned long>(coarse.size()));
            payload.insert(payload.end(),coarse.begin(),coarse.end());
            tex._pixelDataLength=payload.size();
        }
    }
    mesh._textureData=payload.data(); mesh._textureDataLength=payload.size();
    if (lazyBlob) {
        mesh._textureData=nullptr;
        mesh._textureBlobPath=dir/"texture-blob.bin";
        mesh._textureBlobBase=13;
        std::ofstream blob(mesh._textureBlobPath,std::ios::binary);
        const char prefix[13] = {};
        blob.write(prefix,13);
        blob.write(reinterpret_cast<const char*>(payload.data()),payload.size());
    }
    bool ok=PbrtExporter::Export(scene,dir/"scene.pbrt");
    std::ofstream(dir/"result.txt") << (ok?"success":"failure");
    return 0;
}
