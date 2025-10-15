#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

// VERSION: 2.0 - Fixed camera-relative coordinate system
/* Notes: 
- Skyrim uses left-handed coordinate system, because it uses Direct3D 
- https://learn.microsoft.com/en-us/windows/win32/direct3d9/viewports-and-clipping

*/

// https://www.nickdarnell.com/hierarchical-z-buffer-occlusion-culling/

Texture2D<float> HiZBuffer : register(t0);  // This is just the depth buffer
SamplerState HiZSampler : register(s0);

// Input buffer of geometry bounds to test
StructuredBuffer<float4> GeometryBounds : register(t1); // xyz=center, w=radius

// Output buffer of visibility results: x = fSphereDepth, y = fMaxSampledDepth
RWStructuredBuffer<float2> VisibilityResults : register(u0);

// Hi-Z specific parameters
cbuffer HiZParams : register(b0)
{
    // x = mipCount, y = conservativeBias, z = geometryCount, w = debugMode
    float4 HiZSettings;
    // x=overlayEnabled(0/1), y=maxObjectsToDraw, z=unused, w=unused
    float4 overlaySettings;
    // x contains packed bits for 8 toggles
    float4 overlayColorToggles;
    // Camera world position for proper distance calculations
    float3 CameraWorldPos;
    float pad0;
    row_major float4x4 cameraViewMat;
    row_major float4x4 cameraProjMat;
    row_major float4x4 cameraViewProjMat;
    float2 BufferDim;      // screenWidth, screenHeight
    float2 BufferDimInv;   // 1/screenWidth, 1/screenHeight
};

// Debug output buffer - structured for comprehensive debugging
struct DebugData {
    float4 centerWS_radius;       // xyz=centerWS, w=radius (16 bytes, offset 0)
    float4 centerRel_objDepth;    // xyz=centerWSCameraRelative, w=objDepth (16 bytes, offset 16)
    float sceneDepth;             // 4 bytes (offset 32)
    uint earlyOutReason;          // 4 bytes (offset 36)
    float2 padding;               // 8 bytes padding (offset 40) -> total 48 bytes
};

RWStructuredBuffer<DebugData> DebugOutput : register(u1);

RWTexture2D<unorm float4> DebugOverlay : register(u2);

void DrawPixel(int2 p, uint baseW, uint baseH, float4 color) {
    if ((p.x >= 0) && (p.x < (int)baseW) && (p.y >= 0) && (p.y < (int)baseH)) {
        DebugOverlay[p] = color;
    }
}

void DrawCross(int2 p, uint baseW, uint baseH, float4 color, int thickness) {
    // Variable size cross based on thickness
    int halfSize = max(1, thickness);
    [unroll] for (int i = -halfSize; i <= halfSize; ++i) {
        DrawPixel(int2(p.x + i, p.y), baseW, baseH, color);
        DrawPixel(int2(p.x, p.y + i), baseW, baseH, color);
    }
}

void DrawRectOutline(int2 minTex0, int2 maxTex0, uint baseW, uint baseH, float4 color, int thickness) {
    // Draw multiple offset lines for thickness
    int halfThickness = max(0, thickness / 2);
    
    for (int t = -halfThickness; t <= halfThickness; ++t) {
        // Top/bottom
        [loop] for (int x = minTex0.x; x <= maxTex0.x; ++x) {
            DrawPixel(int2(x, minTex0.y + t), baseW, baseH, color);
            DrawPixel(int2(x, maxTex0.y + t), baseW, baseH, color);
        }
        // Left/right
        [loop] for (int y = minTex0.y; y <= maxTex0.y; ++y) {
            DrawPixel(int2(minTex0.x + t, y), baseW, baseH, color);
            DrawPixel(int2(maxTex0.x + t, y), baseW, baseH, color);
        }
    }
}

void DrawBounds(float3 centerVS, float radius, int earlyOutReason) {
    // Check if this color is enabled
    uint toggleBits = uint(overlayColorToggles.x);
    bool shouldDraw = false;
    
    if (earlyOutReason == 1 && (toggleBits & 1)) shouldDraw = true;    // Behind camera
    if (earlyOutReason == 2 && (toggleBits & 2)) shouldDraw = true;    // Invalid radius
    if (earlyOutReason == 4 && (toggleBits & 4)) shouldDraw = true;    // Camera inside
    if (earlyOutReason == 5 && (toggleBits & 8)) shouldDraw = true;    // Invalid depth
    if (earlyOutReason == 6 && (toggleBits & 16)) shouldDraw = true;   // Nearest off-screen
    if (earlyOutReason == 0 && (toggleBits & 32)) shouldDraw = true;   // Visible
    if (earlyOutReason == -1 && (toggleBits & 64)) shouldDraw = true;  // Occluded
    
    if (!shouldDraw) return;  // Early exit if this color is filtered out
    
    // Compute base dimensions
    uint baseW, baseH, mipCount;
    HiZBuffer.GetDimensions(0, baseW, baseH, mipCount);

    // Center point UV
    float2 centerUV = FrameBuffer::ViewToUV(centerVS);
    int2 centerPix = int2(centerUV * float2(baseW, baseH));
    
    // Calculate distance-based thickness
    // Close objects (depth 0-100) = thick (3-5 pixels)
    // Medium objects (depth 100-500) = medium (2-3 pixels)
    // Far objects (depth 500+) = thin (1 pixel)
    float distance = length(centerVS);
    int thickness = 1;
    if (distance < 100.0) {
        thickness = 5;
    } else if (distance < 250.0) {
        thickness = 3;
    } else if (distance < 500.0) {
        thickness = 2;
    }

    // Color based on early-out reason or culling result
    float4 color;
    if (earlyOutReason == 1) {
        color = float4(1, 0, 1, 1);  // Magenta = Behind camera
    } else if (earlyOutReason == 2) {
        color = float4(0.5, 0.5, 0, 1);  // Dark Yellow = Invalid radius
    } else if (earlyOutReason == 4) {
        color = float4(1, 0.5, 0, 1);  // Orange = Camera inside sphere
    } else if (earlyOutReason == 5) {
        color = float4(1, 0, 0.5, 1);  // Pink = Invalid depth
    } else if (earlyOutReason == 6) {
        color = float4(1, 1, 0, 1);  // Yellow = Nearest point off-screen
    } else if (earlyOutReason == 0) {
        color = float4(0, 1, 0, 1);  // Green = Visible
    } else {
        color = float4(1, 0, 0, 1);  // Red = Occluded
    }

    // Draw center point cross with distance-based thickness
    DrawCross(centerPix, baseW, baseH, color, thickness);

    // Calculate approximate screen-space bounding box for the sphere
    // Project sphere bounds to get conservative rectangle
    float3 viewDir = centerVS * rsqrt(max(dot(centerVS, centerVS), 1e-12));
    float centerDist = length(centerVS);
    
    // Calculate approximate corner positions in view space
    float3 right = float3(1, 0, 0);
    float3 up = float3(0, 1, 0);
    
    // Estimate screen bounds by projecting offset points
    float2 minUV = float2(1, 1);
    float2 maxUV = float2(0, 0);
    
    // Sample 8 points around the sphere to estimate screen bounds
    for (int i = 0; i < 8; i++) {
        float angle = (i / 8.0) * 6.28318530718;  // 2*PI
        float3 offset = (cos(angle) * right + sin(angle) * up) * radius;
        float3 sampleVS = centerVS + offset;
        float2 sampleUV = FrameBuffer::ViewToUV(sampleVS);
        minUV = min(minUV, sampleUV);
        maxUV = max(maxUV, sampleUV);
    }
    
    // Draw outline of bounding rectangle
    int2 minTex0 = int2(
        clamp((int)floor(minUV.x * baseW), 0, (int)baseW - 1),
        clamp((int)floor(minUV.y * baseH), 0, (int)baseH - 1)
    );
    int2 maxTex0 = int2(
        clamp((int)floor(maxUV.x * baseW), 0, (int)baseW - 1),
        clamp((int)floor(maxUV.y * baseH), 0, (int)baseH - 1)
    );
    DrawRectOutline(minTex0, maxTex0, baseW, baseH, color, thickness);
}

// Determines the appropriate mip level for a given object based on its screen coverage
float GetMipLevel(float3 centerVS, float radius) {
    float depth = abs(centerVS.z);
    
    // Prevent division by zero
    if (depth < 0.01) return 0.0;
    
    // Get projection scale from first element of projection matrix
    // CameraProj[0][0][0] = horizontal FOV scale = 1/tan(fovX/2)
    float projScaleX = FrameBuffer::CameraProj[0][0][0];
    
    // Screen-space radius in NDC: (radius / depth) * projectionScale  
    float screenRadiusNDC = (radius / depth) * projScaleX;
    
    // Convert to pixels (NDC is [-1,1], so multiply by half width)
    uint hiZWidth, hiZHeight, hiZMipCount;
    HiZBuffer.GetDimensions(0, hiZWidth, hiZHeight, hiZMipCount);
    float screenRadiusPixels = abs(screenRadiusNDC) * hiZWidth * 0.5;
    
    // Diameter in pixels
    float screenSizePixels = screenRadiusPixels * 2.0;
    
    // Subtract 1-2 levels to use finer depth resolution
    float mipLevel = max(0.0, log2(max(1.0, screenSizePixels)) - 1.5);
    
    return clamp(mipLevel, 0.0, HiZSettings.x - 1.0);  // Also avoid highest mip
}

void WriteDebugOutput(int geometryIndex, float3 centerWS, float radius, float3 centerWSCameraRelative, float objDepth, float sceneDepth, uint earlyOutReason) {
    DebugData data;
    data.centerWS_radius = float4(centerWS, radius);
    data.centerRel_objDepth = float4(centerWSCameraRelative, objDepth);
    data.sceneDepth = sceneDepth;
    data.earlyOutReason = earlyOutReason;
    data.padding = float2(0, 0);
    DebugOutput[geometryIndex] = data;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint geometryIndex = dispatchThreadID.x;
    uint geometryCount = (uint)HiZSettings.z;
    if (geometryIndex >= geometryCount)
        return;

    // Set default return value to unculled
    VisibilityResults[geometryIndex] = float2(0, 1);

    float4 Bounds = GeometryBounds[geometryIndex];
    float3 centerWS = Bounds.xyz;
    float radius = Bounds.w;

    // Skyrim uses camera-relative coordinates - subtract camera position before view transform
    float3 centerWSCameraRelative = centerWS - CameraWorldPos;
    float3 centerVS = mul(FrameBuffer::CameraView[0], float4(centerWSCameraRelative, 1)).xyz;

    // Early validation: Check for invalid bounds
    int earlyOutReason = 0;  // 0=none, 1=behind_camera, 2=too_far, 3=invalid_radius, 4=invalid_depth

    // Check for invalid radius
    if (radius <= 0.0) {
        earlyOutReason = 2;  // Invalid radius
        if (overlaySettings.x != 0 && geometryIndex < (uint)overlaySettings.y) {
            DrawBounds(centerVS, radius, earlyOutReason);
        }
        VisibilityResults[geometryIndex] = float2(-2, 0);
        if (HiZSettings.w == 1) {
            WriteDebugOutput(geometryIndex, centerWS, radius, centerWSCameraRelative, 0.0, 0.0, earlyOutReason);
        }
        return;
    }

    // Compute sphere's nearest point
    float centerDistSq = dot(centerVS, centerVS);
    float3 viewDir = centerVS * rsqrt(max(centerDistSq, 1e-12));  // Normalize direction to center
    
    float centerDist = sqrt(centerDistSq);
    if (centerDist < radius) {
        earlyOutReason = 4;  // Camera inside sphere
        if (overlaySettings.x != 0 && geometryIndex < (uint)overlaySettings.y) {
            DrawBounds(centerVS, radius, earlyOutReason);
        }
        VisibilityResults[geometryIndex] = float2(-4, 0); // Not culled
        if (HiZSettings.w == 1) {
            WriteDebugOutput(geometryIndex, centerWS, radius, centerWSCameraRelative, 0.0, 0.0, earlyOutReason);
        }
        return;
    }

    // Choose appropriate mip based on screen coverage of object bounds
    float mipLevel = GetMipLevel(centerVS, radius);
    float conservativeBias = HiZSettings.y * (1.0 + mipLevel * 0.15);

    bool isVisible = false;
    bool anyPointOnScreen = false;
    float minHiZDepth = 1.0;
    float nearestPointCenterDepth = 0.0;

    // 1. Test nearest point
    float nearestDist = centerDist - radius;
    float3 nearestPointVS = viewDir * nearestDist;
    float4 npClip = mul(FrameBuffer::CameraProj[0], float4(nearestPointVS, 1));
    nearestPointCenterDepth = npClip.z / npClip.w;

    if (nearestPointCenterDepth >= 0.0 && nearestPointCenterDepth <= 1.0) {
        float2 nearestPointUV = FrameBuffer::ViewToUV(nearestPointVS);
        if (!FrameBuffer::IsOutsideFrame(nearestPointUV)) {
            anyPointOnScreen = true;
            float hiZDepth = HiZBuffer.SampleLevel(HiZSampler, nearestPointUV, mipLevel).r;
            minHiZDepth = min(minHiZDepth, hiZDepth);
            if (nearestPointCenterDepth <= (hiZDepth + conservativeBias)) {
                isVisible = true;
            }
        }
    }

    // 2. Test cardinal points
    if (!isVisible) {
        static const float3 cardinals[] = {
            float3(1, 0, 0), float3(-1, 0, 0),
            float3(0, 1, 0), float3(0, -1, 0),
            float3(0, 0, 1), float3(0, 0, -1)
        };

        [unroll]
        for (int i = 0; i < 6; ++i) {
            float3 pointVS = centerVS + cardinals[i] * radius;
            if (pointVS.z < 0) continue;

            float4 pointClip = mul(FrameBuffer::CameraProj[0], float4(pointVS, 1));
            float pointDepth = pointClip.z / pointClip.w;
            
            if (pointDepth >= 0.0 && pointDepth <= 1.0) {
                float2 pointUV = FrameBuffer::ViewToUV(pointVS);
                if (!FrameBuffer::IsOutsideFrame(pointUV)) {
                    anyPointOnScreen = true;
                    float hiZDepth = HiZBuffer.SampleLevel(HiZSampler, pointUV, mipLevel).r;
                    minHiZDepth = min(minHiZDepth, hiZDepth);
                    if (pointDepth <= (hiZDepth + conservativeBias)) {
                        isVisible = true;
                        break;
                    }
                }
            }
        }
    }

    // 3. Test cardinal points offset by radius towards cam
    if (!isVisible) {
        static const float3 cardinals[] = {
            float3(1, 0, 0), float3(-1, 0, 0),
            float3(0, 1, 0), float3(0, -1, 0),
            float3(0, 0, 1), float3(0, 0, -1)
        };

        [unroll]
        for (int i = 0; i < 6; ++i) {
            float3 pointVS = centerVS + cardinals[i] * radius - viewDir * radius;
            if (pointVS.z < 0) continue;

            float4 pointClip = mul(FrameBuffer::CameraProj[0], float4(pointVS, 1));
            float pointDepth = pointClip.z / pointClip.w;

            if (pointDepth >= 0.0 && pointDepth <= 1.0) {
                float2 pointUV = FrameBuffer::ViewToUV(pointVS);
                if (!FrameBuffer::IsOutsideFrame(pointUV)) {
                    anyPointOnScreen = true;
                    float hiZDepth = HiZBuffer.SampleLevel(HiZSampler, pointUV, mipLevel).r;
                    minHiZDepth = min(minHiZDepth, hiZDepth);
                    if (pointDepth <= (hiZDepth + conservativeBias)) {
                        isVisible = true;
                        break;
                    }
                }
            }
        }
    }

    // 4. Test bounding box corners
    if (!isVisible) {
        static const float3 corners[] = {
            float3(1, 1, 1), float3(1, 1, -1),
            float3(1, -1, 1), float3(1, -1, -1),
            float3(-1, 1, 1), float3(-1, 1, -1),
            float3(-1, -1, 1), float3(-1, -1, -1)
        };
        static const float cornerScale = 0.577350269; // 1/sqrt(3)

        [unroll]
        for (int i = 0; i < 8; ++i) {
            float3 pointVS = centerVS + corners[i] * radius * cornerScale;
            if (pointVS.z < 0) continue;

            float4 pointClip = mul(FrameBuffer::CameraProj[0], float4(pointVS, 1));
            float pointDepth = pointClip.z / pointClip.w;

            if (pointDepth >= 0.0 && pointDepth <= 1.0) {
                float2 pointUV = FrameBuffer::ViewToUV(pointVS);
                if (!FrameBuffer::IsOutsideFrame(pointUV)) {
                    anyPointOnScreen = true;
                    float hiZDepth = HiZBuffer.SampleLevel(HiZSampler, pointUV, mipLevel).r;
                    minHiZDepth = min(minHiZDepth, hiZDepth);
                    if (pointDepth <= (hiZDepth + conservativeBias)) {
                        isVisible = true;
                        break;
                    }
                }
            }
        }
    }

    // 5. Final culling decision
    bool isOccluded = anyPointOnScreen && !isVisible;
    
    float sphereDepth = nearestPointCenterDepth;
    float hiZDepth = minHiZDepth;

    if (isOccluded) {
        VisibilityResults[geometryIndex] = float2(sphereDepth, hiZDepth);
        earlyOutReason = -1;
    } else {
        // Not occluded, or no points on screen
        VisibilityResults[geometryIndex] = float2(0, 1);
        earlyOutReason = 0;
    }

    // Draw bounds if overlay enabled
    if (overlaySettings.x != 0 && geometryIndex < (uint)overlaySettings.y) {
        DrawBounds(centerVS, radius, earlyOutReason);
    }

    // Write debug output if debug mode is enabled
    if (HiZSettings.w == 1) {
        WriteDebugOutput(geometryIndex, centerWS, radius, centerWSCameraRelative, sphereDepth, hiZDepth, earlyOutReason);
    }
    return;
}
