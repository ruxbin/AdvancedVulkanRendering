"""Use the real direction/PDF code against a known finite-area bright sky texel."""
import pathlib
import subprocess
import sys

root=pathlib.Path(__file__).resolve().parents[1]
shaders=root/"shaders"
rt=(shaders/"rt_path_common.hlsl").read_text(encoding="utf-8")
rng=rt.split("// --- PCG random number generator ---",1)[1].split("// ---",1)[0]
bake=(shaders/"vlm_bake.hlsl").read_text(encoding="utf-8")
basis=bake[bake.index("void evalSh9Basis("):bake.index("float3 probeWorldPos(")]
output=pathlib.Path(sys.argv[2])
source=output.with_suffix(".hlsl")
source.write_text(rng+(shaders/"vlm_env_sampling.hlsl").read_text(encoding="utf-8")+basis+r'''
[[vk::binding(0,0)]] RWStructuredBuffer<float4> results;
[[vk::binding(1,0)]] StructuredBuffer<float> cdf;
[[vk::binding(2,0)]] StructuredBuffer<float4> referenceSh;
[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {
    uint rng=id.x*2654435761u+17;
    float c0=0,c1=0,c6=0,c8=0;
    const uint count=65536;
    for(uint i=0;i<count;++i) {
        float z=1-2*rngF(rng),phi=6.28318530717959*rngF(rng);
        float r=sqrt(max(0.0,1-z*z));
        float3 dir=float3(r*cos(phi),z,r*sin(phi));
        if(rngF(rng)<.5) dir=VlmSampleEnv(64,32,cdf,float3(rngF(rng),rngF(rng),rngF(rng)));
        float pdf=.5/12.56637061435917+.5*VlmEnvPdf(dir,64,32,cdf);
        uint x=min(uint(frac(atan2(-dir.x,dir.z)/6.28318530717959+.5)*64),63);
        uint y=min(uint(acos(clamp(dir.y,-1.0,1.0))/3.14159265358979*32),31);
        float radiance=x==10 && y==8 ? 2048 : 1;
        float basis[9]; evalSh9Basis(dir,basis);
        c0+=radiance*basis[0]/pdf;
        c1+=radiance*basis[1]/pdf;
        c6+=radiance*basis[6]/pdf;
        c8+=radiance*basis[8]/pdf;
    }
    c0*=3.14159265358979*.282095/count;
    c1*=2.0943951023931953*.488603/count;
    c6*=.7853981633974483*.315392/count;
    c8*=.7853981633974483*.546274/count;
    float up=c0+c1-c6-c8;
    results[id.x]=float4(c0,referenceSh[0].x,up,referenceSh[0].x+referenceSh[1].x-referenceSh[6].x-referenceSh[8].x);
}
''',encoding="utf-8")
subprocess.run([sys.argv[1],"-spirv","-T","cs_6_0","-E","main",str(source),"-Fo",str(output)],check=True)
