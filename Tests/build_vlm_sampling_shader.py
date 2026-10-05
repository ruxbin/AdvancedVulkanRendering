"""Compile the production VLM sampler against analytic spatial fixtures."""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
shaders = root / "shaders"
evaluate = (shaders / "ibl_common.hlsl").read_text(encoding="utf-8").split("float3 IBLSpecular", 1)[0]
sample = (shaders / "vlm_common.hlsl").read_text(encoding="utf-8").split("float3 VlmIndirect", 1)[0]
output = pathlib.Path(sys.argv[2])
source = output.with_suffix(".hlsl")
source.write_text(evaluate + sample + (shaders / "vlm_flags.hlsl").read_text(encoding="utf-8") + r'''
[[vk::binding(0,0)]] RWStructuredBuffer<float4> results;
[[vk::binding(1,0)]] StructuredBuffer<float4> data;
[[vk::binding(2,0)]] StructuredBuffer<float4> emptyData;
[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {
    VlmParams P;
    P.boundsMin=0; P.bandWidth=0; P.step=1; P.normalBias=0;
    P.invStep=1; P.normalBiasMax=0; P.cells=2; P.probeTotal=27;
    float3 q=float3(.5,.5,.5);
    float expectedWeight=1, expectedE=0;
    uint test=id.x%11;
    if(test==1) q=2;
    if(test==2) q=float3(1+(id.x%2 ? 1e-5 : -1e-5),1,1);
    if(test==3) { q=float3(-.1,1,1); expectedWeight=0; }
    if(test==4) q=1; // all eight corners invalid in a separate fixture
    if(test==5) q=0; // exact invalid corner: no sky-filled hole
    if(test==6) { q=float3(.25,.5,.5); P.bandWidth=1; expectedWeight=.25; }
    if(test==7) { P.normalBias=.25; P.normalBiasMax=.125; q.z+=.125; }
    if(test==8) { P.probeTotal=0; expectedWeight=0; }
    if(test==9) { q=float3(1.75,1.5,1.5); P.bandWidth=1; expectedWeight=.25; }
    float w0=all(q<=1) ? (1-q.x)*(1-q.y)*(1-q.z) : 0;
    // Independent reference: affine field minus the missing origin contribution.
    if(test!=3 && test!=4 && test!=5 && test!=8)
        expectedE=(1+q.x+2*q.y+4*q.z-w0)/(1-w0);
    float3 lookup=q;
    if(test==7) lookup.z-=.125; // production sampler performs the bias itself
    VlmSampleResult actual;
    if(test==4) actual=SampleVlm(P,emptyData,lookup,float3(0,0,1),float3(0,0,1));
    else actual=SampleVlm(P,data,lookup,float3(0,0,1),float3(0,0,1));
    if(test==10) {
        float owns=VlmEnvironmentSun(2,1) && !VlmEnvironmentSun(1,1) &&
                   !VlmEnvironmentSun(0,1) && !VlmEnvironmentSun(2,0) ? 1 : 0;
        results[id.x]=float4(owns,1,0,0);
    } else results[id.x]=float4(actual.E.x+actual.E.y+actual.E.z,expectedE*6,actual.weight,expectedWeight);
}
''', encoding="utf-8")
subprocess.run([sys.argv[1], "-spirv", "-T", "cs_6_0", "-E", "main", str(source), "-Fo", str(output)], check=True)
