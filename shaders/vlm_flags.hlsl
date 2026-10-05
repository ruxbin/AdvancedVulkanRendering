#ifndef VLM_FLAGS_HLSL
#define VLM_FLAGS_HLSL
// Frame constants encode disabled=0, active analytic=1, active environment sun=2.
bool VlmEnvironmentSun(float flags, float scale) {
    return flags > 1.5f && scale > 0.0f;
}
#endif
