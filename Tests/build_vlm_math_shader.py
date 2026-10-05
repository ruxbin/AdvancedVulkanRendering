"""Extract the actual production BRDF math, then add an independent quadrature test.
No copy of the production sampling/PDF formula is maintained in this test.
"""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "shaders/rt_path_common.hlsl").read_text(encoding="utf-8")
math = source.split("// --- PCG random number generator ---", 1)[1].split("// --- Geometry helpers ---", 1)[0]
output = pathlib.Path(sys.argv[2])
shader = output.with_suffix(".hlsl")
shader.write_text("""#define VLM_PHYSICAL_BSDF 1
#define PI 3.14159265358979f
#define TWO_PI 6.28318530717959f
#define INV_PI 0.31830988618379f
""" + math + """
[[vk::binding(0,0)]] RWStructuredBuffer<float4> results;
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    float4 sums = 0;
    uint rng = id.x * 2654435761u + 17u;
    const float3 N = float3(0,0,1), V = float3(0,0,1);
    for (uint i=0; i<4096; ++i) {
        float3 wi;
        sums.x += sampleBRDF(N,V,float3(.5,.5,.5),float3(.04,.04,.04),.65,rng,wi).x;
        sums.z += sampleBRDF(N,V,float3(.5,.5,.5),float3(.5,.5,.5),1.0,rng,wi).x;
        // Deterministic equal-area hemisphere quadrature, independent of sampler.
        uint s = id.x * 4096 + i;
        float z = (float(s)+.5)/(64.0*4096.0);
        float phi = TWO_PI * frac(float(s)*.61803398875);
        float r = sqrt(max(0.0,1-z*z));
        float3 L = float3(r*cos(phi),r*sin(phi),z);
        sums.y += evalBRDF(N,V,L,float3(.5,.5,.5),float3(.04,.04,.04),.65).x * TWO_PI;
        sums.w += evalBRDF(N,V,L,float3(.5,.5,.5),float3(.5,.5,.5),1.0).x * TWO_PI;
    }
    results[id.x]=sums/4096.0;
}
""", encoding="utf-8")
subprocess.run([sys.argv[1], "-spirv", "-T", "cs_6_0", "-E", "main", str(shader), "-Fo", str(output)], check=True)
