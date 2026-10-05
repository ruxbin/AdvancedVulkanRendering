// Physical irradiance, shared by deferred and forward Vulkan receivers.
struct VlmParams {
    float3 boundsMin; float bandWidth;
    float3 step; float normalBias;
    float3 invStep; float normalBiasMax;
    uint3 cells; uint probeTotal;
};
struct VlmSampleResult { float3 E; float weight; };
VlmSampleResult SampleVlm(VlmParams P, StructuredBuffer<float4> data,
                          float3 position, float3 normal, float3 geometricNormal) {
    VlmSampleResult result; result.E=0; result.weight=0;
    if(P.probeTotal<8 || any(P.cells==0)) return result;
    float bias=min(P.normalBias*min(P.step.x,min(P.step.y,P.step.z)),P.normalBiasMax);
    float3 q=position+geometricNormal*bias;
    float3 hi=P.boundsMin+P.step*float3(P.cells);
    if(any(q<P.boundsMin) || any(q>hi)) return result;
    float3 edge=min(q-P.boundsMin,hi-q);
    result.weight=P.bandWidth>0 ? saturate(min(edge.x,min(edge.y,edge.z))/P.bandWidth) : 1;
    float3 local=(q-P.boundsMin)*P.invStep;
    int3 cell=clamp((int3)floor(local),int3(0,0,0),(int3)P.cells-1);
    float3 f=saturate(local-float3(cell));
    float total=0;
    [unroll] for(uint z=0;z<2;++z)
    [unroll] for(uint y=0;y<2;++y)
    [unroll] for(uint x=0;x<2;++x) {
        uint3 c=(uint3)cell+uint3(x,y,z), dims=P.cells+1;
        uint index=(c.z*dims.y+c.y)*dims.x+c.x;
        if(index>=P.probeTotal) continue;
        float validity=data[index*7+6].w;
        float w=(x?f.x:1-f.x)*(y?f.y:1-f.y)*(z?f.z:1-f.z)*validity;
        if(w<=0) continue;
        float flat[28];
        [unroll] for(uint k=0;k<7;++k) {
            float4 v=data[index*7+k];
            flat[k*4]=v.x; flat[k*4+1]=v.y; flat[k*4+2]=v.z; flat[k*4+3]=v.w;
        }
        float4 sh[9];
        [unroll] for(uint j=0;j<9;++j) sh[j]=float4(flat[j*3],flat[j*3+1],flat[j*3+2],0);
        result.E+=w*evaluateShCoefficients(normal,sh); total+=w;
    }
    // Interior holes retain coverage and return zero, never bright sky fallback.
    result.E=total>1e-6 ? max(result.E/total,0) : float3(0,0,0);
    return result;
}

float3 VlmIndirect(AAPLPixelSurfaceData surface, float3 position, float3 geometricNormal,
                   float3 viewDir, VlmParams params, StructuredBuffer<float4> data,
                   float4 skySh[9], TextureCube env, Texture2D<float2> dfg, SamplerState samplerState,
                   float vlmScale, float iblScale, float specularScale) {
    VlmSampleResult sample=SampleVlm(params,data,position,float3(surface.normal),geometricNormal);
    float3 E=lerp(max(evaluateShCoefficients(float3(surface.normal),skySh),0),sample.E,sample.weight);
    // Surface albedo already has (1-metallic) applied by getPixelSurfaceData.
    // Normal-incidence Fresnel is the receiver's diffuse energy approximation.
    float3 diffuse=float3(surface.albedo)*(1-saturate(float3(surface.F0)))*E*(vlmScale/3.14159265358979);
    return diffuse+IBLSpecular(surface,env,dfg,samplerState,viewDir,iblScale,specularScale);
}
