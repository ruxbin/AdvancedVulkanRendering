//shared by forward and deferred lighting pass



// Standard Smith geometric shadowing function.
static float G1V(float NdV, float k)
{
    return 1.0f / (NdV * (1.0f - k) + k);
}

// Standard GGX normal distribution function.
static float GGX_NDF(float NdH, float alpha)
{
    float alpha2 = alpha * alpha;

    float denom = NdH * NdH * (alpha2 - 1.0f) + 1.0f;

    denom = max(denom, 1e-3);

    return alpha2 / (M_PI_F * denom * denom);
}

float3 Fresnel_Schlick(half3 F0, float LdH)
{
    return (float3) (F0 + (1.0f - F0) * pow(1.0f - LdH, 5));
}

static half3 evaluateBRDF(AAPLPixelSurfaceData surface,
                           half3 viewDir,
                           half3 lightDirection)
{
    float3 H = normalize((float3) viewDir + (float3) lightDirection);

    float NdL = saturate(dot(surface.normal, lightDirection));
    float LdH = saturate(dot((float3) lightDirection, H));
    float NdH = saturate(dot((float3) surface.normal, H));
    float NdV = saturate(dot(surface.normal, viewDir));

    float alpha = surface.roughness * surface.roughness;
    float k = alpha / 2.0f;

    float3 diffuse = (float3) surface.albedo / M_PI_F;
    float3 F = Fresnel_Schlick(surface.F0, LdH);
    float G = G1V(NdL, k) * G1V(NdV, k);

    float3 specular = F * GGX_NDF(NdH, alpha) * G / 4.0f;

    return (half3) (diffuse * (1 - F) + specular) * NdL;
}

half3 lightingShader(AAPLPixelSurfaceData surfaceData,
                             
                             float depth,
                             float4 worldPosition,
                             AAPLFrameConstants frameData,
                             CameraParamsBuffer cameraParams
                            )
{
    //tocamera should use the second one!!
    //float3 tocamera = cameraParams.invViewMatrix[3].xyz - worldPosition.xyz;
    float3 tocamera2 = float3(cameraParams.invViewMatrix._m03, cameraParams.invViewMatrix._m13, cameraParams.invViewMatrix._m23) - worldPosition.xyz;
    half3 viewDir = (half3) normalize(tocamera2); //TODO:m03,m13,m23
    half3 lightDirection = (half3) frameData.sunDirection;
    half3 light = (half3) (frameData.sunColor * M_PI_F);
    
    
    half3 result = evaluateBRDF(surfaceData, viewDir, lightDirection) * light;
    
    result += surfaceData.emissive * frameData.emissiveScale;

    return result;
}