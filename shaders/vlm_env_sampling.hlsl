#ifndef VLM_ENV_SAMPLING_HLSL
#define VLM_ENV_SAMPLING_HLSL
// PDF in solid angle. CDF increments are the actual float probabilities uploaded
// by the CPU, so rounding/zero-width bins cannot disagree with the sampler.
float VlmEnvPdf(float3 direction,uint width,uint height,StructuredBuffer<float> cdf) {
    float phi=atan2(-direction.x,direction.z);
    float theta=acos(clamp(direction.y,-1.0,1.0));
    uint x=min(uint(frac(phi/6.28318530717959+.5)*width),width-1);
    uint y=min(uint(theta/3.14159265358979*height),height-1);
    uint index=y*width+x;
    float mass=cdf[index]-(index ? cdf[index-1] : 0);
    float area=12.56637061435917/width*sin(3.14159265358979*(y+.5)/height)*sin(3.14159265358979/(2.0*height));
    return mass/max(area,1e-20);
}
float3 VlmSampleEnv(uint width,uint height,StructuredBuffer<float> cdf,float3 xi) {
    xi=clamp(xi,0.0,0.99999994); // uint-to-float RNG conversion can round to exactly 1
    uint lo=0,hi=width*height-1;
    while(lo<hi) {
        uint mid=lo+(hi-lo)/2;
        if(cdf[mid]<=xi.x) lo=mid+1; else hi=mid;
    }
    uint x=lo%width,y=lo/width;
    float phi=6.28318530717959*((x+xi.y)/width-.5);
    float c0=cos(3.14159265358979*y/height),c1=cos(3.14159265358979*(y+1)/height);
    float z=lerp(c0,c1,xi.z);
    float r=sqrt(max(0.0,1-z*z));
    return float3(-r*sin(phi),z,r*cos(phi));
}
#endif
