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
                             CameraParamsBufferFull cameraParams
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


// Smoothes the attenuation due to distance for a point or spot light.
inline float smoothDistanceAttenuation(float squaredDistance, float invSqrAttRadius)
{
    float factor = squaredDistance * invSqrAttRadius;
    float smoothFactor = saturate(1.0 - factor * factor);
    return smoothFactor * smoothFactor;
}

// Calculates the attenuation due to distance for a point or spot light.
inline float getDistanceAttenuation(float3 unormalizedLightVector, float invSqrAttRadius)
{
    float sqrDist = dot(unormalizedLightVector, unormalizedLightVector);
    float attenuation = 1.0 / max(sqrDist, 0.01 * 0.01);
    attenuation *= smoothDistanceAttenuation(sqrDist, invSqrAttRadius);

    return attenuation;
}

half3 lightingShaderPointSpot(AAPLPixelSurfaceData surfaceData,

                             float depth,
                             float4 worldPosition,
                             AAPLFrameConstants frameData,
                             CameraParamsBufferFull cameraParams,
			     float4 posSqrRadius,
			     float3 color
                            )
{
    //tocamera should use the second one!!
    //float3 tocamera = cameraParams.invViewMatrix[3].xyz - worldPosition.xyz;
    float3 tocamera2 = float3(cameraParams.invViewMatrix._m03, cameraParams.invViewMatrix._m13, cameraParams.invViewMatrix._m23) - worldPosition.xyz;
    half3 viewDir = (half3) normalize(tocamera2); //TODO:m03,m13,m23
    float3 lightDirection = posSqrRadius.xyz - worldPosition.xyz;
    if (dot(lightDirection, lightDirection) > posSqrRadius.w)
        return 0;
    float attenuation = getDistanceAttenuation(lightDirection, 1.0 / posSqrRadius.w);
    half3 light = (half3) (color * M_PI_F) * attenuation * frameData.localLightIntensity;


    half3 result = evaluateBRDF(surfaceData, viewDir, normalize(lightDirection)) * light;



    return result;
}

// Spot light shader. Mirrors Apple's applySpotLight (AAPLLightingCommon.h:144-196):
// distance cutoff -> distance attenuation -> cone cutoff -> smoothstep^2 angle
// falloff between cos(outer) and cos(inner) -> BRDF * color.
half3 applySpotLight(AAPLPixelSurfaceData surfaceData,
                     float4 worldPosition,
                     AAPLFrameConstants frameData,
                     CameraParamsBufferFull cameraParams,
                     AAPLSpotLightCullingData spot,
                     float shadow)
{
    float3 toLight = spot.posAndHeight.xyz - worldPosition.xyz;
    float  dist    = length(toLight);
    if (dist > spot.posAndHeight.w) return (half3)0;

    float3 L        = toLight / max(dist, 1e-4f);
    float  cosTheta = dot(-L, spot.dirAndOuterAngle.xyz);
    if (cosTheta < spot.dirAndOuterAngle.w) return (half3)0;

    float invSqrRadius = 1.0f / (spot.posAndHeight.w * spot.posAndHeight.w);
    float distAtt      = getDistanceAttenuation(toLight, invSqrRadius);

    float angleRange = max(spot.cosInnerAngle - spot.dirAndOuterAngle.w, 1e-4f);
    float t          = saturate((cosTheta - spot.dirAndOuterAngle.w) / angleRange);
    float angleAtt   = t * t;

    float3 toCamera = float3(cameraParams.invViewMatrix._m03,
                             cameraParams.invViewMatrix._m13,
                             cameraParams.invViewMatrix._m23) - worldPosition.xyz;
    half3  V        = (half3) normalize(toCamera);

    half3 lightCol = (half3)(spot.color.xyz * M_PI_F)
                   * (distAtt * angleAtt * shadow)
                   * frameData.localLightIntensity;
    return evaluateBRDF(surfaceData, V, (half3)L) * lightCol;
}


