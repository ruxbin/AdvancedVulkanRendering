

// Helper struct to store view space bounds.
struct AAPLAxisBounds
{
    float3 upper;
    float3 lower;
};

// Calculates the min/max bound for an axis in view space based on the specified radius.
static AAPLAxisBounds getBoundsForAxis(float3 axis, float3 center, float radius, float nearZ)
{
    // Intersects or behind near plane.
    bool needsClipping = (center.z - radius) < nearZ;

    // Convert to AZ space - A = axis perpendicular to Z = depth
    const float2 az = float2(dot(axis, center), center.z);
    float tSquared = dot(az, az) - (radius * radius);
    float cosTheta = 0, sinTheta = 0;

    if (tSquared > 0)
    {
        float t = sqrt(tSquared);
        float cLength = length(az);
        cosTheta = t / cLength;
        sinTheta = radius / cLength;
    }

    float sqrtPart = 0;
    if (needsClipping)
    {
        float dz = nearZ - az.y;
        // The HLSL rotation below produces the negative-axis tangent first.
        // Clip that endpoint to the negative side of the near-plane circle;
        // using +sqrt here swaps the endpoints (or truncates one side).
        sqrtPart = -sqrt(max(0.0f, (radius * radius) - (dz * dz)));
    }

    float2 bounds_az[2];
    for (int i = 0; i < 2; ++i)
    {
        if (tSquared > 0)
        {
            float2x2 rotateTheta = float2x2(
                float2(cosTheta, -sinTheta),
                float2(sinTheta, cosTheta));
            bounds_az[i] = cosTheta * (mul(rotateTheta, az));
        }

        if (needsClipping && (tSquared <= 0 || bounds_az[i].y < nearZ))
        {
            bounds_az[i].x = az.x + sqrtPart;
            bounds_az[i].y = nearZ;
        }
        sinTheta *= -1; // negate theta for B
        sqrtPart *= -1; // negate sqrtPart for B
    }

AAPLAxisBounds b;
    b.upper   = bounds_az[0].x * axis;
    b.upper.z = bounds_az[0].y;
    b.lower   = bounds_az[1].x * axis;
    b.lower.z = bounds_az[1].y;
    return  b;
}

// Helper struct to store results.
struct AAPLBox2D
{
    float2 x;
    float2 y;
    
    //hlsl不支持constructor
    //AAPLBox2D(float minX, float minY, float maxX, float maxY)
    //{
    //    x = float2(minX,maxX);
    //    y = float2(minY,maxY);
    //}
    float2 min()
    {
        return float2(x.x, y.x);
    }
    float2 max()
    {
        return float2(x.y, y.y);
    }
};


// Helper function to project bounds points to clip space.
float3 project(float3 f, float4x4 projMatrix)
{
    float4 p = mul(projMatrix, float4(f, 1));
  
    return p.xyz / p.w;
}

// Calculates the XY bounding box for a bounding sphere projected into screen space.
AAPLBox2D getBoundingBox(float3 center,
                                float radius,
                                float nearZ,
                                float4x4 projMatrix)
{
    AAPLBox2D res;
    res.x = res.y = float2(0, 0);
    if (center.z + radius <= nearZ)
        return res;

    AAPLAxisBounds x = getBoundsForAxis(float3(1, 0, 0), center, radius, nearZ);
    AAPLAxisBounds y = getBoundsForAxis(float3(0, 1, 0), center, radius, nearZ);

    // Projection may flip either axis. Sort after projection rather than
    // assuming a particular camera handedness from the endpoint names.
    float x0 = project(x.upper, projMatrix).x;
    float x1 = project(x.lower, projMatrix).x;
    float y0 = project(y.upper, projMatrix).y;
    float y1 = project(y.lower, projMatrix).y;
    res.x = float2(min(x0, x1), max(x0, x1));
    res.y = float2(min(y0, y1), max(y0, y1));
    return res;
}
