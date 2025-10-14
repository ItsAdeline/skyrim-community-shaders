#include "HiZOcclusion.h"
#include <cmath>
#include "Globals.h"
#include "State.h"
#include "Util.h"
#include "ShaderCache.h"
#include "Utils/UI.h"
#include "Utils/Game.h"
#include <imgui.h>
#include <algorithm>
#include <unordered_set>
#include <DirectXMath.h>
#include <RE/N/NiBound.h>
#include "Features/Upscaling.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    HiZOcclusion::Settings,
    enableHiZViewer,
    hizViewerMip,
    hizViewerScale,
    enableHiZCulling,
    conservativeBias,
    showCullingStats,
    debugMode,
    enableBoundsViewer,
    boundsMaxObjects,
    showBehindCamera,
    showInvalidRadius,
    showCameraInside,
    showInvalidDepth,
    showNearestOffscreen,
    showVisible,
    showOccluded
)

namespace
{
    static std::atomic<uint32_t> g_hiZ_last_print_frame{ std::numeric_limits<uint32_t>::max() };
    constexpr uint32_t kMaxUserDataObjects = 1024;
    static std::atomic<uint32_t> g_hiZ_userdata_write_idx{0};
    static RE::NiAVObject* g_hiZ_userdata_objects[kMaxUserDataObjects] = {};

    std::atomic<uint32_t> g_hiZ_setupgeometry_stats_null_visible{ 0 };
    std::atomic<uint32_t> g_hiZ_setupgeometry_stats_invalid_bound{ 0 };
    std::atomic<uint32_t> g_hiZ_setupgeometry_stats_valid{ 0 };
}

bool HiZOcclusion::SetupBoundsOverlayResources(uint32_t width, uint32_t height) {
    auto device = globals::d3d::device;
    if (!device || width == 0 || height == 0) return false;

    // Release old
    ReleaseBoundsOverlayResources();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(device->CreateTexture2D(&td, nullptr, &boundsOverlayTex))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srd{};
    srd.Format = td.Format;
    srd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srd.Texture2D.MostDetailedMip = 0;
    srd.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(boundsOverlayTex, &srd, &boundsOverlaySRV))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = td.Format;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavd.Texture2D.MipSlice = 0;
    if (FAILED(device->CreateUnorderedAccessView(boundsOverlayTex, &uavd, &boundsOverlayUAV))) return false;

    boundsOverlayW = width;
    boundsOverlayH = height;
    return true;
}

void HiZOcclusion::ReleaseBoundsOverlayResources() {
    if (boundsOverlayUAV) { boundsOverlayUAV->Release(); boundsOverlayUAV = nullptr; }
    if (boundsOverlaySRV) { boundsOverlaySRV->Release(); boundsOverlaySRV = nullptr; }
    if (boundsOverlayTex) { boundsOverlayTex->Release(); boundsOverlayTex = nullptr; }
    boundsOverlayW = boundsOverlayH = 0;
}

void HiZOcclusion::ClearBoundsOverlay() {
    if (!boundsOverlayUAV) return;
    auto ctx = globals::d3d::context;
    static const float clearColor[4] = {0.f,0.f,0.f,0.f};
    ctx->ClearUnorderedAccessViewFloat(boundsOverlayUAV, clearColor);
}

void HiZOcclusion::DrawSettings()
{
    // logger::info("Drawing Hi-Z settings (frame={})", currentFrame);
    if (ImGui::TreeNodeEx("Hi-Z Viewer", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show viewer window", &settings.enableHiZViewer);
        if (auto _tt = Util::HoverTooltipWrapper()) {
            Util::DrawMultiLineTooltip({
                "Displays the selected Hi-Z mip level in a separate window.",
                "Useful to verify pyramid contents and downsampling correctness."
            });
        }

        // Clamp mip selection to available range when resources exist
        uint32_t maxMip = hiZMipCount > 0 ? (hiZMipCount - 1) : 0;
        if (settings.hizViewerMip > maxMip) settings.hizViewerMip = maxMip;

        ImGui::SliderInt("Mip", reinterpret_cast<int*>(&settings.hizViewerMip), 0, static_cast<int>(maxMip));
        if (auto _tt = Util::HoverTooltipWrapper()) {
            Util::DrawMultiLineTooltip({
                "Mip 0 is full resolution. Higher mips are progressively smaller.",
                "Range is based on the currently built Hi-Z mip count."
            });
        }
        ImGui::SliderFloat("Scale", &settings.hizViewerScale, 0.1f, 4.0f, "%.2fx");
        if (auto _tt = Util::HoverTooltipWrapper()) {
            Util::DrawMultiLineTooltip({
                "Visual scale applied to the displayed texture.",
                "Use to enlarge small mip levels."
            });
        }

        if (ImGui::TreeNodeEx("Bounds Overlay", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Bounds Overlay", &settings.enableBoundsViewer);
            if (auto _tt = Util::HoverTooltipWrapper()) {
                Util::DrawMultiLineTooltip({
                    "Shows colored outlines around tested geometry based on their culling status.",
                    "Enable individual colors below to filter what is displayed."
                });
            }
            
            ImGui::SliderInt("Max Objects", reinterpret_cast<int*>(&settings.boundsMaxObjects), 8, 2048);
            if (auto _tt = Util::HoverTooltipWrapper()) {
                Util::DrawMultiLineTooltip({
                    "Maximum number of objects to draw outlines for per frame.",
                    "Higher values may impact performance."
                });
            }
            
            if (settings.enableBoundsViewer) {
                ImGui::Separator();
                ImGui::Text("Color Filters:");
                
                ImGui::Checkbox("Green (Visible)", &settings.showVisible);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.visibleCount);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Objects that passed all tests and are visible");
                }
                
                ImGui::Checkbox("Red (Occluded)", &settings.showOccluded);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.occludedCount);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Objects occluded by Hi-Z depth test");
                }
                
                ImGui::Checkbox("Magenta (Behind Camera)", &settings.showBehindCamera);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.behindCamera);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Objects with center behind camera (negative Z in view space)");
                }
                
                ImGui::Checkbox("Yellow (Nearest Off-Screen)", &settings.showNearestOffscreen);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.nearestOffscreen);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Objects with nearest point outside screen bounds");
                }
                
                ImGui::Checkbox("Orange (Camera Inside)", &settings.showCameraInside);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.cameraInside);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Camera is inside the bounding sphere");
                }
                
                ImGui::Checkbox("Dark Yellow (Invalid Radius)", &settings.showInvalidRadius);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.invalidRadius);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Objects with invalid (zero or negative) radius");
                }
                
                ImGui::Checkbox("Pink (Invalid Depth)", &settings.showInvalidDepth);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ": %u", stats.invalidDepth);
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::SetTooltip("Objects with depth values outside valid range (0-1)");
                }
            }
            
            ImGui::TreePop();
        }

        // Status line
        ImGui::Separator();
        ImGui::Text("Frame: %u", globals::state ? globals::state->frameCount : 0);
		//ImGui::Text("Resources: %s", resourcesSetup ? "true" : "false");
		ImGui::Text("Status: %s", status.c_str());
        //ImGui::Text("Hi-Z: %s, mips=%u", hiZTexture ? "Ready" : "Not Built", hiZMipCount);
        //ImGui::Text("SRVs: %u, UAVs: %u", (uint32_t)hiZSRVsPerMip.size(), (uint32_t)hiZUAVs.size());
        ImGui::Text("Geometry from frame %u: %u", globals::state->frameCount - 1, stats.geometryListSize);
        //ImGui::Text("CPU Results list size: %u", (uint32_t)visibilityResultsCPU.size());
        //ImGui::Text("Map Results list size: %u", (uint32_t)visibilityResultsMap.size());
        ImGui::Text("Total tested: %u", stats.totalTested);
        ImGui::Text("Culled: %u", stats.culled);
        ImGui::Text("Visible: %u", stats.visible);
        // Display profiling durations in micro seconds
        ImGui::Text("GPU time: %.2f us", stats.gpuCullingTimeMs * 1000);
        ImGui::Text("Copy results time: %.2f us", stats.copyTimeMs * 1000);
        ImGui::Text("Map time: %.2f us", stats.mapTimeMs * 1000);
        ImGui::Text("Copy data time: %.2f us", stats.copyDataTimeMs * 1000);
        ImGui::Text("Unmap time: %.2f us", stats.unmapTimeMs * 1000);
        ImGui::Text("CPU Readback time: %.2f us", stats.readbackTimeMs * 1000);

        /*
        // Detailed resource validation in UI
        if (ImGui::TreeNode("Resource Validation")) {
            ImGui::Text("Texture pointer: %p", hiZTexture);
            ImGui::Text("Main SRV: %p", hiZSRV);
            
            for (uint32_t i = 0; i < std::min(hiZMipCount, (uint32_t)hiZSRVsPerMip.size()); ++i) {
                ImVec4 color = hiZSRVsPerMip[i] ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1);
                ImGui::TextColored(color, "Mip %u SRV: %p", i, hiZSRVsPerMip[i]);
            }
            
            ImGui::TreePop();
        }
        if (hiZTexture && (hiZSRVsPerMip.size() != hiZMipCount || hiZUAVs.size() != hiZMipCount)) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Warning: view count mismatch (mips=%u, SRVs=%llu, UAVs=%llu)",
                hiZMipCount,
                static_cast<unsigned long long>(hiZSRVsPerMip.size()),
                static_cast<unsigned long long>(hiZUAVs.size()));
        }
        */

        ImGui::TreePop();
    }
    
    // Hi-Z Culling Settings
    if (ImGui::TreeNodeEx("Hi-Z Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable Hi-Z Culling", &settings.enableHiZCulling)) {
            logger::info("Hi-Z culling toggled: {}", settings.enableHiZCulling);
        }
        ImGui::SliderFloat("Conservative Bias", &settings.conservativeBias, -1.0f, 1.0f, "%.3f");
        
        if (ImGui::Checkbox("Show Culling Stats", &settings.showCullingStats)) {
            logger::info("Culling stats display toggled: {}", settings.showCullingStats);
        }
        
        if (ImGui::Checkbox("Debug Mode", &settings.debugMode)) {
            logger::info("Debug mode toggled: {}", settings.debugMode);
        }
        
        ImGui::TreePop();
    }

    // Separate viewer window (persists while settings are open)
    if (settings.enableHiZViewer) {
        if (ImGui::Begin("Hi-Z Pyramid Viewer", &settings.enableHiZViewer)) {
            if (hiZTexture) {
                // Compute selected mip dimensions
                uint32_t mip = settings.hizViewerMip;
                uint32_t w = std::max(1u, hiZWidth  >> mip);
                uint32_t h = std::max(1u, hiZHeight >> mip);
                ImVec2 size = ImVec2(w * settings.hizViewerScale, h * settings.hizViewerScale);

                // Draw the texture
                ImGui::Text("Mip %u  (%ux%u)", mip, w, h);
                if (mip < hiZSRVsPerMip.size() && hiZSRVsPerMip[mip]) {
                    ImGui::Image(reinterpret_cast<ImTextureID>(hiZSRVsPerMip[mip]), size);
                    
                    // Debug info
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("SRV: %p", hiZSRVsPerMip[mip]);
                        ImGui::Text("Scale: %.2fx", settings.hizViewerScale);
                        ImGui::Text("Display size: %.0fx%.0f", size.x, size.y);
                        ImGui::EndTooltip();
                    }
                } else {
                    ImGui::Text("SRV not available (mip=%u, size=%zu)", mip, hiZSRVsPerMip.size());
                    if (mip < hiZSRVsPerMip.size()) {
                        ImGui::Text("SRV pointer for mip %u: %p", mip, hiZSRVsPerMip[mip]);
                    }
                    ImGui::Text("Texture: %p, Main SRV: %p", hiZTexture, hiZSRV);
                }
            }
            else {
                ImGui::Text("Hi-Z pyramid unavailable.");
            }
        }
        ImGui::End();
    }
}

void HiZOcclusion::DrawOverlay()
{
    // Draw bounds overlay full-screen when enabled
    // This is called every frame regardless of menu state
    if (settings.enableBoundsViewer && boundsOverlaySRV) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 p0 = vp->Pos;
        ImVec2 p1 = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    
        // Draw overlay image full-screen
        // Tint alpha controls opacity (e.g., 0x80 = 50%)
        ImU32 tint = IM_COL32(255, 255, 255, 192);
        ImGui::GetBackgroundDrawList()->AddImage(
            reinterpret_cast<ImTextureID>(boundsOverlaySRV),
            p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
    
        // Optional: red border around whole screen
        ImGui::GetBackgroundDrawList()->AddRect(p0, p1, IM_COL32(255, 0, 0, 255));
    }
}

bool HiZOcclusion::IsOverlayVisible() const
{
    // Overlay is visible when bounds viewer is enabled and resource exists
    return settings.enableBoundsViewer && boundsOverlaySRV != nullptr;
}

void HiZOcclusion::LoadSettings(json& o_json)
{
    settings = o_json;
}

void HiZOcclusion::SaveSettings(json& o_json)
{
    o_json = settings;
}

void HiZOcclusion::RestoreDefaultSettings()
{
    settings = {};
}

// Preserve Feature base-class contract
void HiZOcclusion::SetupResources()
{
    InitShaders();
}

void HiZOcclusion::InitShaders()
{
    logger::info("Initializing HiZ shaders (frame={})", globals::state ? globals::state->frameCount : 0);
    
    // Ensure we have a valid device before attempting shader compilation
    auto device = globals::d3d::device;
    if (!device) {
        status = "no D3D device for shader compilation";
        logger::error("{}", status);
        return;
    }
    
    // Compile Hi-Z build shaders with error handling
    if (!hiZBuildLevel0CS) {
        try {
            hiZBuildLevel0CS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZBuildLevel0CS.hlsl", {}, "cs_5_0");
            if (!hiZBuildLevel0CS) { 
                status = "failed to compile HiZBuildLevel0CS"; 
                logger::error("{}", status);
                return;
            }
            else { logger::info("compiled HiZBuildLevel0CS"); }
        } catch (const std::exception& e) {
            status = "exception during HiZBuildLevel0CS compilation";
            logger::error("{}: {}", status, e.what());
            return;
        }
    }
    
    if (!hiZDownsampleCS) {
        try {
            hiZDownsampleCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZDownsampleCS.hlsl", {}, "cs_5_0");
            if (!hiZDownsampleCS) { 
                status = "failed to compile HiZDownsampleCS"; 
                logger::error("{}", status);
                return;
            }
            else { logger::info("compiled HiZDownsampleCS"); }
        } catch (const std::exception& e) {
            status = "exception during HiZDownsampleCS compilation";
            logger::error("{}: {}", status, e.what());
            return;
        }
    }
    
    if (!hiZTestCS) {
        try {
            hiZTestCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\HiZOcclusion\\HiZTestCS.hlsl", {}, "cs_5_0");
            if (!hiZTestCS) { 
                status = "failed to compile HiZTestCS"; 
                logger::error("{}", status);
                return;
            }
            else { logger::info("compiled HiZTestCS"); }
        } catch (const std::exception& e) {
            status = "exception during HiZTestCS compilation";
            logger::error("{}: {}", status, e.what());
            return;
        }
    }
    
    resourcesSetup = true;
    status = "shaders_compiled";
    logger::info("HiZ shader compilation completed successfully");
    
    // Skip resource validation for a few frames after shader compilation
    // to avoid crashes during device state transitions
    skipValidationThisFrame = true;
}

void HiZOcclusion::ClearShaderCache()
{
    if (hiZBuildLevel0CS) { hiZBuildLevel0CS->Release(); hiZBuildLevel0CS = nullptr; }
    if (hiZDownsampleCS) { hiZDownsampleCS->Release(); hiZDownsampleCS = nullptr; }
    if (hiZTestCS) { hiZTestCS->Release(); hiZTestCS = nullptr; }
}

void HiZOcclusion::EarlyPrepass()
{
    if (settings.debugMode) {
        logger::debug("Frame {} EarlyPrepass - {} hidden geometries queued for re-test", 
                     globals::state->frameCount, unCullNextFrame.size());
    }
}

void HiZOcclusion::Prepass()
{
    logger::info("Frame {} - HiZOcclusion::Prepass", globals::state->frameCount);
    if (!settings.enableHiZCulling) {
        return;
    }
    status = "Resetting stats";
    
    // Reset culling stats for new frame - always reset to avoid accumulation
    stats.frameIndex = currentFrame;
    stats.totalTested = 0;
    stats.geometryListSize = (uint32_t)pendingGeometry.size();
    stats.culled = 0;
    stats.visible = 0;
    stats.resourceSetupDurationMS = 0.0f;
    stats.recreateDurationMS = 0.0f;
    stats.behindCamera = 0;
    stats.invalidRadius = 0;
    stats.cameraInside = 0;
    stats.invalidDepth = 0;
    stats.nearestOffscreen = 0;
    stats.visibleCount = 0;
    stats.occludedCount = 0;

    // Reset current-frame accumulation
    //visibilityResultsCPU.clear();
    //visibilityResultsMap.clear();
    batchDispatchedThisFrame = false;
    
    // Reset timing statistics for this frame
    stats.geometryProcessingTimeMs = 0.0f;
    //stats.gpuCullingTimeMs = 0.0f;
    //stats.readbackTimeMs = 0.0f;

    if (!resourcesSetup) {
        auto start = std::chrono::high_resolution_clock::now();
        InitShaders();
        auto end = std::chrono::high_resolution_clock::now();
        const double durationMs = std::chrono::duration<double, std::milli>(end - start).count();
        stats.resourceSetupDurationMS = static_cast<float>(durationMs);
    }
    
    // Early return if shader compilation failed
    if (!resourcesSetup) {
        status = "failed to compile Hi-Z shaders";
        return;
    }

    status = "Building Hi-Z pyramid";

    if (!InitHiZResources()) {
        logger::error("HiZOcclusion::EarlyPrepass - failed to initialize Hi-Z resources");
        status = "failed to initialize Hi-Z resources";
        return;
    }
    
    // Setup GPU culling resources if not already done
    if (settings.enableHiZCulling && (!geometryBoundsBuffer || !hiZTestParamsBuffer || !hiZSampler || !visibilityResultsBuffer)) {
        if (!SetupGPUCullingResources()) {
            logger::error("HiZOcclusion::EarlyPrepass - failed to setup GPU culling resources");
            status = "failed to setup GPU culling resources";
            return;
        }
    }

    // Reserve vector capacity
    if (pendingGeometry.capacity() < 16384) {
        pendingGeometry.reserve(16384);
        geometryBounds.reserve(16384);
    }

    if (!unCullNextFrame.empty()) {
        // Re-add previously hidden geometry for continuous testing
        for (auto* geo : unCullNextFrame) {
            if (geo && pendingGeometrySet.find(geo) == pendingGeometrySet.end()) {
                //geo->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
                pendingGeometry.push_back(geo);
                pendingGeometrySet.insert(geo);
            }
        }
    }

    if (readbackState.hasPendingRead || !pendingGeometry.empty()) {
        ExecuteVisibilityTests();
    }

    // Only process visibility tests if we have geometry from previous frame
    if (!pendingGeometry.empty()) {
        // Clean up resources that we are finished with
        pendingGeometry.clear();
        pendingGeometrySet.clear();
        geometryBounds.clear();
        geometryIndexMap.clear();
    } else {
        logger::debug("Frame {} - No pending geometry to process in Prepass", globals::state->frameCount);
    }

    UnbindD3DResources();

    status = "HiZ Tests executed";
};

bool HiZOcclusion::InitHiZResources()
{
    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;

    // Ensure resources are ready
    if (!renderer) {
		logger::error("Renderer not ready");
		status = "Renderer not ready";
        return false;
    }

    // Get previous frame depth dimensions
    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    if (!depth.depthSRV) {
		logger::error("no depth texture SRV");
		status = "no depth texture SRV";
        return false;
    }

    // Verify depth buffer source and log details
    //logger::info("Frame {} - HiZ using depth buffer: kPOST_ZPREPASS_COPY", globals::state->frameCount);
    
    // Check if other depth targets are available for comparison
    //auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
    /*logger::info("Available depth targets:");
    for (int i = 0; i < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL; ++i) {
        if (depthStencils[i].depthSRV) {
            logger::info("  Target {}: Available", i);
        }
    }
    */
    
    // Log depth buffer properties
    D3D11_TEXTURE2D_DESC depthTexDesc{};
    depth.texture->GetDesc(&depthTexDesc);
    logger::info("Depth buffer: {}x{}, Format: {}, MipLevels: {}", 
                depthTexDesc.Width, depthTexDesc.Height, 
                static_cast<int>(depthTexDesc.Format), depthTexDesc.MipLevels);

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
    depth.depthSRV->GetDesc(&depthSRVDesc);

    // Check if depth format is compatible
	if (depthSRVDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS && 
		depthSRVDesc.Format != DXGI_FORMAT_R32_FLOAT &&
		depthSRVDesc.Format != DXGI_FORMAT_R16_UNORM) {
		logger::warn("Unexpected depth format: {}", static_cast<int>(depthSRVDesc.Format));
	}

    D3D11_TEXTURE2D_DESC depthDesc{};
    depth.texture->GetDesc(&depthDesc);

	if (!depthDesc.Width || !depthDesc.Height) {
		logger::error("depth texture has invalid dimensions");
		status = "depth texture has invalid dimensions";
		return false;
	}

    uint32_t desiredW;
    uint32_t desiredH;

    if (globals::features::upscaling.loaded && globals::features::upscaling.IsUpscalingActive()) {
        uint32_t displayW = static_cast<uint32_t>(globals::state->screenSize.x);
        uint32_t displayH = static_cast<uint32_t>(globals::state->screenSize.y);
        desiredW = static_cast<uint32_t>(displayW * globals::features::upscaling.dynamicResolutionWidthRatio);
        desiredH = static_cast<uint32_t>(displayH * globals::features::upscaling.dynamicResolutionHeightRatio);
        // Ensure dimensions are at least 1
        desiredW = std::max(1u, desiredW);
        desiredH = std::max(1u, desiredH);
        logger::info("HiZOcclusion: Upscaling active. Using scaled resolution: {}x{}", desiredW, desiredH);
    } else {
        desiredW = depthDesc.Width;
        desiredH = depthDesc.Height;
        logger::info("HiZOcclusion: Upscaling inactive. Using depth buffer resolution: {}x{}", desiredW, desiredH);
    }

    // Build Hi-Z pyramid from previous frame depth buffer
    // Ensure Hi-Z texture exists and matches current depth dimensions
    auto device = globals::d3d::device;
    if (!device) {
        logger::error("no D3D device");
        status = "no D3D device";
        return false;
    }

    // Detailed diagnostics for resource recreation
    const bool textureNull = (hiZTexture == nullptr);
    const bool widthMismatch = (hiZWidth != desiredW);
    const bool heightMismatch = (hiZHeight != desiredH);
    const bool needRecreate = textureNull || widthMismatch || heightMismatch;
    
    if (needRecreate) {
        auto startRecreateTimer = std::chrono::high_resolution_clock::now();
        logger::info("Recreating Hi-Z resources: {}x{}", desiredW, desiredH);

        // Compute mip count for the new texture
        uint32_t w = desiredW;
        uint32_t h = desiredH;
        uint32_t newMipCount = 1;
        while (w > 1 || h > 1) { w = std::max(1u, w >> 1); h = std::max(1u, h >> 1); ++newMipCount; }
        // logger::info("new mip count: {}", newMipCount);

        // Create new resources into temporaries
        ID3D11Texture2D* newTexture = nullptr;
        ID3D11ShaderResourceView* newSRV = nullptr;
        std::vector<ID3D11ShaderResourceView*> newSRVsPerMip;
        std::vector<ID3D11UnorderedAccessView*> newUAVs;
        newSRVsPerMip.reserve(newMipCount);
        newUAVs.reserve(newMipCount);

        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = desiredW;
        tdesc.Height = desiredH;
        tdesc.MipLevels = newMipCount;
        tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_R32_FLOAT;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DEFAULT;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateTexture2D(&tdesc, nullptr, &newTexture);
        if (FAILED(hr) || !newTexture) {
            status = "failed to create Hi-Z texture";
            logger::error("{}", status);
            if (newTexture) newTexture->Release();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sdesc{};
        sdesc.Format = DXGI_FORMAT_R32_FLOAT;
        sdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sdesc.Texture2D.MostDetailedMip = 0;
        sdesc.Texture2D.MipLevels = newMipCount;
        HRESULT srvResult = device->CreateShaderResourceView(newTexture, &sdesc, &newSRV);
        if (FAILED(srvResult) || !newSRV) {
            status = "failed to create Hi-Z SRV";
            logger::error("{}", status);
            if (newSRV) newSRV->Release();
            if (newTexture) newTexture->Release();
            return false;
        }

        bool perMipOk = true;
        for (uint32_t i = 0; i < newMipCount; ++i) {
            ID3D11ShaderResourceView* srvMip = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sM{};
            sM.Format = DXGI_FORMAT_R32_FLOAT;
            sM.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sM.Texture2D.MostDetailedMip = i;
            sM.Texture2D.MipLevels = 1;
            HRESULT srvMipResult = device->CreateShaderResourceView(newTexture, &sM, &srvMip);
            if (FAILED(srvMipResult) || !srvMip) { perMipOk = false; }
            else { newSRVsPerMip.push_back(srvMip); }

            ID3D11UnorderedAccessView* uavMip = nullptr;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uM{};
            uM.Format = DXGI_FORMAT_R32_FLOAT;
            uM.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uM.Texture2D.MipSlice = i;
            HRESULT uavMipResult = device->CreateUnorderedAccessView(newTexture, &uM, &uavMip);
            if (FAILED(uavMipResult) || !uavMip) { perMipOk = false; }
            else { newUAVs.push_back(uavMip); }

            if (!perMipOk) break;
        }

        if (!perMipOk || newSRVsPerMip.size() != newMipCount || newUAVs.size() != newMipCount) {
            status = "failed to create per-mip views";
            logger::error("{}", status);
            for (auto* v : newSRVsPerMip) { if (v) v->Release(); }
            for (auto* u : newUAVs) { if (u) u->Release(); }
            if (newSRV) newSRV->Release();
            if (newTexture) newTexture->Release();
            return false;
        }

        // Success: release old and swap in new resources
        if (hiZSRV) { hiZSRV->Release(); hiZSRV = nullptr; }
        for (auto* v : hiZSRVsPerMip) { if (v) v->Release(); }
        hiZSRVsPerMip.clear();
        for (auto* u : hiZUAVs) { if (u) u->Release(); }
        hiZUAVs.clear();
        if (hiZTexture) { hiZTexture->Release(); hiZTexture = nullptr; }

        hiZTexture = newTexture;
        hiZSRV = newSRV;
        hiZSRVsPerMip = std::move(newSRVsPerMip);
        hiZUAVs = std::move(newUAVs);
        hiZWidth = desiredW;
        hiZHeight = desiredH;
        hiZMipCount = newMipCount;
        resourceCreationFrame = globals::state->frameCount;
        resourcesValid = true;
        logger::info("Hi-Z resources ready: {}x{}, mips={}.", hiZWidth, hiZHeight, hiZMipCount);
        // logger::info("Created {} SRVs and {} UAVs", hiZSRVsPerMip.size(), hiZUAVs.size());
        
        // Validate all SRVs are non-null with safe validation
        bool allSRVsValid = true;
        for (uint32_t i = 0; i < hiZMipCount; ++i) {
            if (!hiZSRVsPerMip[i]) {
                logger::error("SRV for mip {} is null!", i);
                allSRVsValid = false;
            } else {
                // Safe validation: only check if pointer is non-null
                // Avoid calling methods that could crash if resource is invalid
                try {
                    // Test reference count safely
                    ULONG refCount = hiZSRVsPerMip[i]->AddRef();
                    hiZSRVsPerMip[i]->Release();
                    
                    if (refCount <= 1) {
                        logger::warn("SRV for mip {} has low reference count: {}", i, refCount - 1);
                    }
                    
                    // Only call GetDesc if we're confident the SRV is valid
                    // Skip this validation during shader compilation to avoid crashes
                    if (refCount > 1) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                        // Use a simple validation approach without structured exception handling
                        // to avoid C++ unwinding conflicts
                        //HRESULT descResult = S_OK;
                        bool descValid = true;
                        
                        // Attempt to get description - if this crashes, it indicates a deeper issue
                        try {
                            hiZSRVsPerMip[i]->GetDesc(&srvDesc);
                            // logger::info("SRV mip {} format: {}, most detailed mip: {}", i, 
                            //            (int)srvDesc.Format, srvDesc.Texture2D.MostDetailedMip);
                        } catch (...) {
                            logger::error("SRV for mip {} failed GetDesc validation - resource may be invalid", i);
                            allSRVsValid = false;
                            descValid = false;
                        }
                    }
                } catch (...) {
                    logger::error("Exception during SRV validation for mip {}", i);
                    allSRVsValid = false;
                }
            }
        }
        
        // If validation failed, mark resources as needing recreation
        if (!allSRVsValid) {
            logger::warn("SRV validation failed - resources may need recreation on next frame");
            status = "validation_failed";
            resourcesValid = false;
            // Skip validation on next few frames to prevent repeated crashes
            skipValidationThisFrame = true;
        }
        auto endRecreateTimer = std::chrono::high_resolution_clock::now();
        const double recreateDuration = std::chrono::duration<double, std::milli>(endRecreateTimer - startRecreateTimer).count();
        stats.recreateDurationMS = static_cast<float>(recreateDuration);
    }

    // Build level 0 from depth with safety checks
    {
        // Verify all required resources are valid before proceeding
        if (!hiZBuildLevel0CS) {
            logger::error("hiZBuildLevel0CS is null - cannot build Hi-Z pyramid");
            status = "shader_null";
            return false;
        }
        
        if (hiZUAVs.empty() || !hiZUAVs[0]) {
            logger::error("hiZUAVs[0] is null - cannot build Hi-Z pyramid");
            status = "uav_null";
            return false;
        }
        
        // logger::info("Building level 0 from depth (frame={})", currentFrame);
        ID3D11ShaderResourceView* srvs[1] = { depth.depthSRV };
        context->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[1] = { hiZUAVs[0] };
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->CSSetShader(hiZBuildLevel0CS, nullptr, 0);

		const uint32_t tgX = 16, tgY = 16;
		uint32_t groupsX = (hiZWidth + tgX - 1) / tgX;
		uint32_t groupsY = (hiZHeight + tgY - 1) / tgY;
		context->Dispatch(groupsX, groupsY, 1);

		// Unbind (must pass arrays, not raw nullptr, when Count > 0)
		// logger::info("Unbinding");
		ID3D11UnorderedAccessView* nullUAVs_lvl0[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAVs_lvl0, nullptr);
		ID3D11ShaderResourceView* nullSRVs_lvl0[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRVs_lvl0);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// logger::info("Downsampling pyramid with max reduction (frame={})", currentFrame);
	// Downsample pyramid with max reduction
	uint32_t srcW = desiredW;
	uint32_t srcH = desiredH;
	for (uint32_t mip = 0; mip + 1 < hiZMipCount; ++mip) {
		uint32_t dstW = std::max(1u, srcW >> 1);
		uint32_t dstH = std::max(1u, srcH >> 1);

		// Safety checks for each mip level
		if (mip >= hiZSRVsPerMip.size() || !hiZSRVsPerMip[mip]) {
			logger::error("hiZSRVsPerMip[{}] is null - aborting pyramid build", mip);
			status = "srv_null_during_build";
			break;
		}
		
		if (mip + 1 >= hiZUAVs.size() || !hiZUAVs[mip + 1]) {
			logger::error("hiZUAVs[{}] is null - aborting pyramid build", mip + 1);
			status = "uav_null_during_build";
			break;
		}
		
		if (!hiZDownsampleCS) {
			logger::error("hiZDownsampleCS is null - aborting pyramid build");
			status = "downsample_shader_null";
			break;
		}

		ID3D11ShaderResourceView* srvIn[1] = { hiZSRVsPerMip[mip] };
		context->CSSetShaderResources(0, 1, srvIn);
		ID3D11UnorderedAccessView* uavOut[1] = { hiZUAVs[mip + 1] };
		context->CSSetUnorderedAccessViews(0, 1, uavOut, nullptr);
		context->CSSetShader(hiZDownsampleCS, nullptr, 0);

		const uint32_t tgX = 16, tgY = 16;
		uint32_t groupsX = (dstW + tgX - 1) / tgX;
		uint32_t groupsY = (dstH + tgY - 1) / tgY;
		// logger::info("Dispatching {}x{}", groupsX, groupsY);
		context->Dispatch(groupsX, groupsY, 1);

		// Unbind for next level (must pass arrays, not raw nullptr)
		// logger::info("Unbinding for next level (frame={})", currentFrame);
		ID3D11UnorderedAccessView* nullUAVs_ds[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAVs_ds, nullptr);
		ID3D11ShaderResourceView* nullSRVs_ds[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRVs_ds);
		context->CSSetShader(nullptr, nullptr, 0);

		srcW = dstW; srcH = dstH;
    } // End Hi-Z build timing

    return true;
}

bool HiZOcclusion::SetupGPUCullingResources()
{
    auto device = globals::d3d::device;
    if (!device) return false;
    
    // Create geometry bounds buffer (input)
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = maxGeometryCount * sizeof(DirectX::XMFLOAT4);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.StructureByteStride = sizeof(DirectX::XMFLOAT4);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    HRESULT hr = device->CreateBuffer(&bufferDesc, nullptr, &geometryBoundsBuffer);
    if (FAILED(hr)) {
        logger::error("Failed to create geometry bounds buffer");
        return false;
    }
    
    // Create SRV for geometry bounds
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = maxGeometryCount;
    
    HRESULT srvCreateResult = device->CreateShaderResourceView(geometryBoundsBuffer, &srvDesc, &geometryBoundsSRV);
    if (FAILED(srvCreateResult)) {
        logger::error("Failed to create geometry bounds SRV");
        return false;
    }
    
    // Create visibility results buffer (output, batch) as float2 per element (objectDepth, sceneDepth)
    bufferDesc.ByteWidth = maxGeometryCount * sizeof(OcclusionResult);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.CPUAccessFlags = 0;  // No CPU access for UAV buffers
    bufferDesc.StructureByteStride = sizeof(OcclusionResult);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    HRESULT visibilityBufferResult = device->CreateBuffer(&bufferDesc, nullptr, &visibilityResultsBuffer);
    if (FAILED(visibilityBufferResult)) {
        logger::error("Failed to create visibility results buffer");
        return false;
    }
    
    // Create UAV for visibility results (batch)
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = maxGeometryCount;
    
    HRESULT visibilityUAVResult = device->CreateUnorderedAccessView(visibilityResultsBuffer, &uavDesc, &visibilityResultsUAV);
    if (FAILED(visibilityUAVResult)) {
        logger::error("Failed to create visibility results UAV");
        return false;
    }
    
    // Create constant buffer for Hi-Z test parameters
    bufferDesc.ByteWidth = sizeof(HiZSettings);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.StructureByteStride = 0;
    bufferDesc.MiscFlags = 0;
    
    HRESULT paramsBufferResult = device->CreateBuffer(&bufferDesc, nullptr, &hiZTestParamsBuffer);
    if (FAILED(paramsBufferResult)) {
        logger::error("Failed to create Hi-Z test params buffer");
        return false;
    }
    
    // Create triple-buffered staging buffers for async readback
    D3D11_BUFFER_DESC readbackDesc = {};
    readbackDesc.ByteWidth = maxGeometryCount * sizeof(OcclusionResult);
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readbackDesc.StructureByteStride = 0;
    readbackDesc.MiscFlags = 0;

    for (int i = 0; i < AsyncReadbackState::BUFFER_COUNT; ++i) {
        HRESULT rbhr = device->CreateBuffer(&readbackDesc, nullptr, &readbackState.stagingBuffers[i]);
        if (FAILED(rbhr)) {
            logger::error("Failed to create visibility readback buffer {}", i);
            // Clean up any buffers we created
            for (int j = 0; j < i; ++j) {
                if (readbackState.stagingBuffers[j]) {
                    readbackState.stagingBuffers[j]->Release();
                    readbackState.stagingBuffers[j] = nullptr;
                }
            }
            return false;
        }
        readbackState.hasPendingRead[i] = false;
        readbackState.pendingFrameIndex[i] = 0;
    }
    readbackState.writeIndex = 0;
    readbackState.readIndex = 0;
    readbackState.numPendingReads = 0;
    logger::info("Created {} staging buffers for triple-buffered readback", AsyncReadbackState::BUFFER_COUNT);

    // Create sampler for Hi-Z sampling
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT sr = device->CreateSamplerState(&sampDesc, &hiZSampler);
    if (FAILED(sr)) {
        logger::error("Failed to create Hi-Z sampler");
        return false;
    }

    visibilityResultsCPU.resize(maxGeometryCount);
    logger::info("GPU culling resources created successfully");

    // Debug output buffers
    if (settings.debugMode || settings.enableBoundsViewer) {
        // Only create debug buffer when actually debugging
        if (!debugResultsBuffer) {
            CreateDebugBuffer();
        }
    } else {
        // Release debug buffer when not needed
        ReleaseDebugBuffer();
    }
    return true;
}

void HiZOcclusion::CreateDebugBuffer()
{
    auto device = globals::d3d::device;
    if (!device) return;
    
    const uint32_t debugElementCount = maxGeometryCount;
    D3D11_BUFFER_DESC dbgDesc = {};
    dbgDesc.ByteWidth = debugElementCount * sizeof(HiZOcclusion::DebugData);
    dbgDesc.Usage = D3D11_USAGE_DEFAULT;
    dbgDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    dbgDesc.CPUAccessFlags = 0;
    dbgDesc.StructureByteStride = sizeof(HiZOcclusion::DebugData);
    dbgDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    HRESULT dbr = device->CreateBuffer(&dbgDesc, nullptr, &debugResultsBuffer);
    if (FAILED(dbr)) {
        logger::warn("Failed to create debugResultsBuffer");
        return;
    }
    
    D3D11_UNORDERED_ACCESS_VIEW_DESC dbgUavDesc = {};
    dbgUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    dbgUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    dbgUavDesc.Buffer.NumElements = debugElementCount;
    device->CreateUnorderedAccessView(debugResultsBuffer, &dbgUavDesc, &debugResultsUAV);
    
    // Create double-buffered staging buffers for debug readback
    D3D11_BUFFER_DESC dbgReadback = {};
    dbgReadback.ByteWidth = dbgDesc.ByteWidth;
    dbgReadback.Usage = D3D11_USAGE_STAGING;
    dbgReadback.BindFlags = 0;
    dbgReadback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    
    device->CreateBuffer(&dbgReadback, nullptr, &debugReadbackBuffer);
    
    logger::info("Debug buffer created");
}

void HiZOcclusion::ReleaseDebugBuffer()
{
    if (debugResultsBuffer) {
        debugResultsBuffer->Release();
        debugResultsBuffer = nullptr;
    }
    if (debugResultsUAV) {
        debugResultsUAV->Release();
        debugResultsUAV = nullptr;
    }
    if (debugReadbackBuffer) {
        debugReadbackBuffer->Release();
        debugReadbackBuffer = nullptr;
    }
}

void HiZOcclusion::ExecuteVisibilityTests()
{
    logger::info("Frame {} - HiZOcclusion::ExecuteVisibilityTests()", globals::state->frameCount);
    auto context = globals::d3d::context;
    auto device = globals::d3d::device;
    if (!context || !device) {
        logger::warn("ExecuteVisibilityTests: D3D context or device not initialized");
        return;
    }

    // Clear previous frame results and prepare for N+1 processing
    visibilityResultsMap.clear();
    visibilityResultsCPU.clear();

    // Check if we have geometry from previous frame to process
    if (pendingGeometry.empty() && readbackState.numPendingReads == 0) {
        logger::debug("ExecuteVisibilityTests: No geometry to test and no pending results");
        return;
    }

    // Try to read results from any pending staging buffers (multi-buffered approach)
    {
        auto readStart = std::chrono::high_resolution_clock::now();
        
        // Try to read from all pending buffers (oldest first)
        uint32_t attemptsToRead = readbackState.numPendingReads;
        for (uint32_t attempt = 0; attempt < attemptsToRead && readbackState.numPendingReads > 0; ++attempt) {
            uint32_t bufferIdx = readbackState.readIndex;
            
            if (readbackState.hasPendingRead[bufferIdx]) {
                auto mapStart = std::chrono::high_resolution_clock::now();
                
                HRESULT hr = context->Map(readbackState.stagingBuffers[bufferIdx], 0, 
                    D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, 
                    &readbackState.mappedData[bufferIdx]);
                
                auto mapEnd = std::chrono::high_resolution_clock::now();
                stats.mapTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(mapEnd - mapStart).count());
                
                if (SUCCEEDED(hr)) {
                    auto copyStart = std::chrono::high_resolution_clock::now();
                    
                    // Successfully mapped - process results using the geometry snapshot from this buffer
                    ProcessVisibilityResults(bufferIdx);
                    
                    auto copyEnd = std::chrono::high_resolution_clock::now();
                    stats.copyDataTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(copyEnd - copyStart).count());
                    
                    auto unmapStart = std::chrono::high_resolution_clock::now();
                    context->Unmap(readbackState.stagingBuffers[bufferIdx], 0);
                    auto unmapEnd = std::chrono::high_resolution_clock::now();
                    stats.unmapTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(unmapEnd - unmapStart).count());
                    
                    // Mark buffer as free
                    readbackState.hasPendingRead[bufferIdx] = false;
                    readbackState.numPendingReads--;
                    
                    uint32_t latency = globals::state->frameCount - readbackState.pendingFrameIndex[bufferIdx];
                    if (settings.debugMode) {
                        logger::info("Successfully read results from buffer {} (frame {} -> {}, latency = {} frames)",
                                    bufferIdx, readbackState.pendingFrameIndex[bufferIdx], 
                                    globals::state->frameCount, latency);
                    }
                    
                    // Advance read index for next frame
                    readbackState.readIndex = (readbackState.readIndex + 1) % AsyncReadbackState::BUFFER_COUNT;
                    break;  // Successfully processed one buffer, don't read more this frame
                } else {
                    // GPU not done yet - try next buffer in ring
                    readbackState.readIndex = (readbackState.readIndex + 1) % AsyncReadbackState::BUFFER_COUNT;
                    if (settings.debugMode && attempt == 0) {
                        logger::debug("Buffer {} not ready (frame {}), will retry next frame",
                                     bufferIdx, readbackState.pendingFrameIndex[bufferIdx]);
                    }
                }
            } else {
                // This buffer has no pending read, advance
                readbackState.readIndex = (readbackState.readIndex + 1) % AsyncReadbackState::BUFFER_COUNT;
            }
        }
        
        auto readEnd = std::chrono::high_resolution_clock::now();
        stats.readbackTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(readEnd - readStart).count());
    }

    // Dispatch new test if we have geometry to process
    if (!pendingGeometry.empty()) {
        numGeometry = static_cast<uint32_t>(pendingGeometry.size());
        pendingGeometrySnapshot = pendingGeometry;

        // Create the worldBound array for the remaining geometry
        geometryBounds.clear();
        geometryBounds.resize(numGeometry);
        for (uint32_t i = 0; i < numGeometry; i++) {
            if (!pendingGeometry[i]) // remove nullptrs
                continue;
            auto& worldBound = pendingGeometry[i]->worldBound;

            geometryBounds[i] = DirectX::XMFLOAT4(worldBound.center.x, worldBound.center.y, worldBound.center.z, worldBound.radius);
        }

        logger::debug("ExecuteVisibilityTests: Processing {} geometry objects from frame {}", geometryBounds.size(), globals::state->frameCount - 1);

        // Execute HiZ Tests for this frame
        DispatchComputeShader();

        // Check if we have a free staging buffer
        if (readbackState.numPendingReads >= AsyncReadbackState::BUFFER_COUNT) {
            logger::warn("All {} staging buffers are full! GPU readback is severely delayed. Skipping oldest buffer.",
                        AsyncReadbackState::BUFFER_COUNT);
            // Force-free the oldest buffer (readIndex points to it)
            uint32_t oldestIdx = readbackState.readIndex;
            if (readbackState.hasPendingRead[oldestIdx]) {
                readbackState.hasPendingRead[oldestIdx] = false;
                readbackState.numPendingReads--;
                readbackState.readIndex = (readbackState.readIndex + 1) % AsyncReadbackState::BUFFER_COUNT;
            }
        }

        // Copy current frame results to next available staging buffer
        uint32_t writeIdx = readbackState.writeIndex;
        
        auto copyStart = std::chrono::high_resolution_clock::now();
        context->CopyResource(readbackState.stagingBuffers[writeIdx], visibilityResultsBuffer);
        auto copyEnd = std::chrono::high_resolution_clock::now();
        stats.copyTimeMs = static_cast<float>(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(copyEnd - copyStart).count());

        // Store geometry snapshot WITH this buffer so results match when read back
        readbackState.geometrySnapshots[writeIdx] = pendingGeometrySnapshot;
        readbackState.geometryCount[writeIdx] = numGeometry;
        
        // Mark buffer as pending
        readbackState.hasPendingRead[writeIdx] = true;
        readbackState.pendingFrameIndex[writeIdx] = globals::state->frameCount;
        readbackState.numPendingReads++;
        
        // Advance write index for next frame
        readbackState.writeIndex = (readbackState.writeIndex + 1) % AsyncReadbackState::BUFFER_COUNT;
        
        if (settings.debugMode) {
            logger::info("Dispatched HiZ test for frame {} to buffer {} ({} pending)",
                        globals::state->frameCount, writeIdx, readbackState.numPendingReads);
        }

        // Update statistics
        stats.frameIndex = globals::state->frameCount;
    }

    /*
    // Read back and log runtime diagnostics from shader when enabled
    if ((settings.debugMode || settings.enableBoundsViewer) && debugResultsBuffer && debugReadbackBuffer) {

        context->CopyResource(debugReadbackBuffer, debugResultsBuffer);

        if (debugReadbackBuffer) {
            D3D11_MAPPED_SUBRESOURCE dbgMap{};
            if (SUCCEEDED(context->Map(debugReadbackBuffer, 0, D3D11_MAP_READ, 0, &dbgMap))) {
                auto* debugData = reinterpret_cast<const HiZOcclusion::DebugData*>(dbgMap.pData);            // Print first up to 10 entries written by the shader
            
                // Print first up to 10 CULLED objects for debugging
                logger::info("=== HiZ Debug Data (First 10 culled objects) ===");

                // Map early-out reason codes to readable strings
                auto getReasonString = [](uint32_t reason) -> const char* {
                    switch (reason) {
                        case 1: return "Behind camera";
                        case 2: return "Invalid radius";
                        case 4: return "Camera inside sphere";
                        case 5: return "Invalid depth";
                        case 6: return "Nearest point off-screen";
                        case 0: return "Visible (passed all tests)";
                        case 0xFFFFFFFF: return "Occluded by Hi-Z";  // -1 as uint
                        default: return "Unknown";
                    }
                };

                uint32_t culledCount = 0;
                for (uint32_t i = 0; i < numGeometry && culledCount < 10; ++i) {
                    const auto& data = debugData[i];
                    
                    // Only log objects that were culled (earlyOutReason != 0)
                    // earlyOutReason == 0 means visible (passed all tests)
                    if (data.earlyOutReason != 0) {
                        logger::info("Geo {}: centerWS=({:.2f}, {:.2f}, {:.2f}), radius={:.2f}",
                            i, data.centerWS_radius.x, data.centerWS_radius.y, data.centerWS_radius.z, data.centerWS_radius.w);
                        logger::info("  CameraRel=({:.2f}, {:.2f}, {:.2f}), objDepth={:.4f}, sceneDepth={:.4f}",
                            data.centerRel_objDepth.x, data.centerRel_objDepth.y, 
                            data.centerRel_objDepth.z, data.centerRel_objDepth.w, data.sceneDepth);
                        logger::info("  earlyOutReason={} ({})", 
                            data.earlyOutReason, getReasonString(data.earlyOutReason));
                        culledCount++;
                    }
                }
            
                if (culledCount == 0) {
                    logger::info("  No culled objects found in this frame");
                } else {
                    logger::info("=== Logged {} culled objects ===", culledCount);
                }

                // Update statistics
                stats.behindCamera = 0;
                stats.invalidRadius = 0;
                stats.cameraInside = 0;
                stats.invalidDepth = 0;
                stats.nearestOffscreen = 0;
                stats.visibleCount = 0;
                stats.occludedCount = 0;
                
                for (uint32_t i = 0; i < numGeometry; ++i) {
                    const auto& data = debugData[i];
                    switch (data.earlyOutReason) {
                        case 1: stats.behindCamera++; break;
                        case 2: stats.invalidRadius++; break;
                        case 4: stats.cameraInside++; break;
                        case 5: stats.invalidDepth++; break;
                        case 6: stats.nearestOffscreen++; break;
                        case 0: stats.visibleCount++; break;
                        case 0xFFFFFFFF: stats.occludedCount++; break;  // -1 as uint
                        default: break;
                    }
                }

                context->Unmap(debugReadbackBuffer, 0);
            }
        }
    }
    */
}

void HiZOcclusion::UnbindD3DResources()
{
    auto context = globals::d3d::context;
    context->CSSetShaderResources(0, 2, nullSRVs); // t0 and t1
    context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CSSetSamplers(0, 1, nullSamplers);
    context->CSSetConstantBuffers(0, 1, nullCBs);
}

void HiZOcclusion::VerifyDepthBufferContents()
{
    if (!settings.debugMode) return;
    
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) return;
    
    auto context = globals::d3d::context;
    if (!context) return;
    
    // Get the depth buffer we're using for HiZ
    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    if (!depth.depthSRV || !depth.texture) return;
    
    // Create a staging texture to read back depth values
    D3D11_TEXTURE2D_DESC depthDesc{};
    depth.texture->GetDesc(&depthDesc);
    
    D3D11_TEXTURE2D_DESC stagingDesc = depthDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    
    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr) || !stagingTexture) {
        logger::warn("Failed to create staging texture for depth verification");
        return;
    }
    
    // Copy depth buffer to staging
    context->CopyResource(stagingTexture, depth.texture);
    
    // Map and sample a few key positions
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        // Sample center, corners, and a few random positions
        uint32_t centerX = depthDesc.Width / 2;
        uint32_t centerY = depthDesc.Height / 2;
        
        auto sampleDepth = [&](uint32_t x, uint32_t y) -> float {
            if (x >= depthDesc.Width || y >= depthDesc.Height) return -1.0f;
            
            uint8_t* row = static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            
            // Handle different depth formats
            if (depthDesc.Format == DXGI_FORMAT_R32_FLOAT) {
                return *reinterpret_cast<float*>(row + x * 4);
            } else if (depthDesc.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) {
                uint32_t packed = *reinterpret_cast<uint32_t*>(row + x * 4);
                return (packed & 0xFFFFFF) / float(0xFFFFFF);
            } else if (depthDesc.Format == 44) {  // DXGI_FORMAT_D24_UNORM_S8_UINT
                uint32_t packed = *reinterpret_cast<uint32_t*>(row + x * 4);
                return (packed & 0xFFFFFF) / float(0xFFFFFF);  // Extract 24-bit depth, ignore 8-bit stencil
            } else if (depthDesc.Format == DXGI_FORMAT_R16_UNORM) {
                uint16_t depth16 = *reinterpret_cast<uint16_t*>(row + x * 2);
                return depth16 / 65535.0f;
            }
            logger::warn("Unsupported depth format: {}", static_cast<int>(depthDesc.Format));
            return -1.0f;
        };
        
        float centerDepth = sampleDepth(centerX, centerY);
        float topLeftDepth = sampleDepth(depthDesc.Width / 4, depthDesc.Height / 4);
        float topRightDepth = sampleDepth(3 * depthDesc.Width / 4, depthDesc.Height / 4);
        float bottomLeftDepth = sampleDepth(depthDesc.Width / 4, 3 * depthDesc.Height / 4);
        float bottomRightDepth = sampleDepth(3 * depthDesc.Width / 4, 3 * depthDesc.Height / 4);
        
        logger::info("Frame {} - Depth Buffer Verification:", globals::state->frameCount);
        logger::info("  Center ({}, {}): {}", centerX, centerY, centerDepth);
        logger::info("  TopLeft: {}, TopRight: {}", topLeftDepth, topRightDepth);
        logger::info("  BottomLeft: {}, BottomRight: {}", bottomLeftDepth, bottomRightDepth);
        
        // Check for suspicious values
        if (centerDepth <= 0.0f || centerDepth >= 1.0f) {
            logger::warn("Suspicious center depth value: {}", centerDepth);
        }
        
        // Check if all depths are the same (might indicate stale/cleared buffer)
        if (centerDepth == topLeftDepth && centerDepth == topRightDepth && 
            centerDepth == bottomLeftDepth && centerDepth == bottomRightDepth) {
            logger::warn("All sampled depths are identical ({}), buffer might be cleared/stale", centerDepth);
        }
        
        context->Unmap(stagingTexture, 0);
    } else {
        logger::warn("Failed to map staging texture for depth verification");
    }
    
    stagingTexture->Release();
}

void HiZOcclusion::UpdatePerformanceMetrics()
{
    // Calculate current frame metrics
    if (stats.totalTested > 0) {
        stats.cullingEfficiency = (float(stats.culled) / float(stats.totalTested)) * 100.0f;
    } else {
        stats.cullingEfficiency = 0.0f;
    }
    
    // Calculate total overhead (sum of all timing components)
    stats.cullingOverheadMs = stats.hiZBuildTimeMs + stats.geometryProcessingTimeMs + 
                             stats.gpuCullingTimeMs + stats.readbackTimeMs;
    
    // Calculate geometry processing rate
    if (stats.cullingOverheadMs > 0.0f) {
        stats.avgGeometryPerMs = float(stats.totalTested) / stats.cullingOverheadMs;
    } else {
        stats.avgGeometryPerMs = 0.0f;
    }
    
    // Update running averages (maintain last 60 frames)
    stats.recentEfficiency.push_back(stats.cullingEfficiency);
    stats.recentOverhead.push_back(stats.cullingOverheadMs);
    stats.recentGeometryCount.push_back(stats.totalTested);
    
    // Trim to max history size
    if (stats.recentEfficiency.size() > stats.maxHistoryFrames) {
        stats.recentEfficiency.erase(stats.recentEfficiency.begin());
        stats.recentOverhead.erase(stats.recentOverhead.begin());
        stats.recentGeometryCount.erase(stats.recentGeometryCount.begin());
    }
    
    // Calculate running averages
    if (!stats.recentEfficiency.empty()) {
        float sumEfficiency = 0.0f;
        float sumOverhead = 0.0f;
        uint32_t sumGeometry = 0;
        
        for (size_t i = 0; i < stats.recentEfficiency.size(); ++i) {
            sumEfficiency += stats.recentEfficiency[i];
            sumOverhead += stats.recentOverhead[i];
            sumGeometry += stats.recentGeometryCount[i];
        }
        
        size_t frameCount = stats.recentEfficiency.size();
        stats.avgCullingEfficiency = sumEfficiency / float(frameCount);
        stats.avgOverheadMs = sumOverhead / float(frameCount);
        stats.avgGeometryCount = float(sumGeometry) / float(frameCount);
    }
    
    // Log performance summary every 60 frames when debug mode is enabled
    if (settings.debugMode && (currentFrame % 60 == 0) && !stats.recentEfficiency.empty()) {
        logger::info("HiZ Performance Summary (last {} frames):", stats.recentEfficiency.size());
        logger::info("  Avg Overhead: {:.3f}ms", stats.avgOverheadMs);
        logger::info("  Avg Geometry Count: {:.0f}", stats.avgGeometryCount);
        logger::info("  Avg Processing Rate: {:.0f} geo/ms", 
                    stats.avgGeometryCount > 0 ? stats.avgGeometryCount / std::max(stats.avgOverheadMs, 0.001f) : 0.0f);
        logger::info("  Points Tested Per Object: {}", stats.pointsTestedPerObject);
    }
}

void HiZOcclusion::DispatchComputeShader() {
    auto context = globals::d3d::context;
    if (!context) return;

    // Write all geometry bounds to GPU buffer
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = context->Map(geometryBoundsBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, geometryBounds.data(), numGeometry * sizeof(DirectX::XMFLOAT4));
            context->Unmap(geometryBoundsBuffer, 0);
        } else {
            logger::warn("ExecuteVisibilityTests: Failed to map geometry bounds buffer");
            return;
        }
    }
    
    // Update constant buffer with camera parameters
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HiZSettings params{};
        params.hiZParams = DirectX::XMFLOAT4(static_cast<float>(hiZMipCount), settings.conservativeBias, static_cast<float>(numGeometry), static_cast<float>(settings.debugMode));
        auto eyePos = Util::GetEyePosition(0);
        params.cameraWorldPos = DirectX::XMFLOAT3(eyePos.x, eyePos.y, eyePos.z);
        params.overlaySettings = DirectX::XMFLOAT4(
            settings.enableBoundsViewer ? 1.0f : 0.0f,
            static_cast<float>(settings.boundsMaxObjects),
            0.0f, 0.0f);
        
        // Pack color toggles into float4 (7 bits used)
        float toggleBits = 0.0f;
        if (settings.showBehindCamera) toggleBits += 1.0f;       // bit 0 - earlyOutReason 1
        if (settings.showInvalidRadius) toggleBits += 2.0f;      // bit 1 - earlyOutReason 2
        if (settings.showCameraInside) toggleBits += 4.0f;       // bit 2 - earlyOutReason 4
        if (settings.showInvalidDepth) toggleBits += 8.0f;       // bit 3 - earlyOutReason 5
        if (settings.showNearestOffscreen) toggleBits += 16.0f;  // bit 4 - earlyOutReason 6
        if (settings.showVisible) toggleBits += 32.0f;           // bit 5 - earlyOutReason 0
        if (settings.showOccluded) toggleBits += 64.0f;          // bit 6 - earlyOutReason -1
        params.overlayColorToggles = DirectX::XMFLOAT4(toggleBits, 0.0f, 0.0f, 0.0f);

        if (!prevframeCamDataValid) {
            auto vd = Util::GetCameraData(0);
            // Extract matrices from vd (whatever accessors you currently use)
            // Make sure to convert to row-major XMFLOAT4X4 for the cbuffers:
            DirectX::XMMATRIX V = vd.viewMat;
            DirectX::XMMATRIX P = vd.projMat;
            DirectX::XMMATRIX VP = DirectX::XMMatrixMultiply(V, P);
        
            DirectX::XMStoreFloat4x4(&prevframeCam.view,     V);  // row-major copy
            DirectX::XMStoreFloat4x4(&prevframeCam.proj,     P);
            DirectX::XMStoreFloat4x4(&prevframeCam.viewProj, VP);
        
            prevframeCamDataValid = true;
            return;
        }

        // Camera Data because FrameBuffer might not be set up yet
        auto camData = prevframeCam;
        {
            // Match FrameBuffer layout (row_major in HLSL): store row-major matrices.
            // Our camData matrices appear transposed relative to FrameBuffer, so transpose before upload.
            DirectX::XMMATRIX V  = DirectX::XMLoadFloat4x4(&camData.view);
            DirectX::XMMATRIX P  = DirectX::XMLoadFloat4x4(&camData.proj);
            DirectX::XMMATRIX VP = DirectX::XMLoadFloat4x4(&camData.viewProj);

            DirectX::XMStoreFloat4x4(&params.cameraViewMat,     DirectX::XMMatrixTranspose(V));
            DirectX::XMStoreFloat4x4(&params.cameraProjMat,     DirectX::XMMatrixTranspose(P));
            DirectX::XMStoreFloat4x4(&params.cameraViewProjMat, DirectX::XMMatrixTranspose(VP));

            if (settings.debugMode) {
                // Log camera world position
                logger::info("HiZ Params - CameraWorldPos: [{}, {}, {}]",
                    params.cameraWorldPos.x, params.cameraWorldPos.y, params.cameraWorldPos.z);

                // Log View matrix (row-major layout)
                const auto& VM = params.cameraViewMat;
                logger::info("Frame {}: HiZ Params - ViewMat r0: [{}, {}, {}, {}]", globals::state->frameCount, VM._11, VM._12, VM._13, VM._14);
                logger::info("Frame {}: HiZ Params - ViewMat r1: [{}, {}, {}, {}]", globals::state->frameCount, VM._21, VM._22, VM._23, VM._24);
                logger::info("Frame {}: HiZ Params - ViewMat r2: [{}, {}, {}, {}]", globals::state->frameCount, VM._31, VM._32, VM._33, VM._34);
                logger::info("Frame {}: HiZ Params - ViewMat r3: [{}, {}, {}, {}]", globals::state->frameCount, VM._41, VM._42, VM._43, VM._44);

                // Log Proj matrix
                const auto& PM = params.cameraProjMat;
                logger::info("Frame {}: HiZ Params - ProjMat r0: [{}, {}, {}, {}]", globals::state->frameCount, PM._11, PM._12, PM._13, PM._14);
                logger::info("Frame {}: HiZ Params - ProjMat r1: [{}, {}, {}, {}]", globals::state->frameCount, PM._21, PM._22, PM._23, PM._24);
                logger::info("Frame {}: HiZ Params - ProjMat r2: [{}, {}, {}, {}]", globals::state->frameCount, PM._31, PM._32, PM._33, PM._34);
                logger::info("Frame {}: HiZ Params - ProjMat r3: [{}, {}, {}, {}]", globals::state->frameCount, PM._41, PM._42, PM._43, PM._44);

                // Log ViewProj matrix
                const auto& VPM = params.cameraViewProjMat;
                logger::info("Frame {}: HiZ Params - ViewProjMat r0: [{}, {}, {}, {}]", globals::state->frameCount, VPM._11, VPM._12, VPM._13, VPM._14);
                logger::info("Frame {}: HiZ Params - ViewProjMat r1: [{}, {}, {}, {}]", globals::state->frameCount, VPM._21, VPM._22, VPM._23, VPM._24);
                logger::info("Frame {}: HiZ Params - ViewProjMat r2: [{}, {}, {}, {}]", globals::state->frameCount, VPM._31, VPM._32, VPM._33, VPM._34);
                logger::info("Frame {}: HiZ Params - ViewProjMat r3: [{}, {}, {}, {}]", globals::state->frameCount, VPM._41, VPM._42, VPM._43, VPM._44);

                // Log View Inverse matrix (row-major, matching FrameBuffer::CameraViewInverse)
                // We uploaded View as transpose(V), so inverse(View) row-major is transpose(inverse(V))
                DirectX::XMMATRIX V_inv = DirectX::XMMatrixInverse(nullptr, V);
                DirectX::XMFLOAT4X4 VInvM;
                DirectX::XMStoreFloat4x4(&VInvM, DirectX::XMMatrixTranspose(V_inv));
                logger::info("Frame {}: HiZ Params - ViewInvMat r0: [{}, {}, {}, {}]", globals::state->frameCount, VInvM._11, VInvM._12, VInvM._13, VInvM._14);
                logger::info("Frame {}: HiZ Params - ViewInvMat r1: [{}, {}, {}, {}]", globals::state->frameCount, VInvM._21, VInvM._22, VInvM._23, VInvM._24);
                logger::info("Frame {}: HiZ Params - ViewInvMat r2: [{}, {}, {}, {}]", globals::state->frameCount, VInvM._31, VInvM._32, VInvM._33, VInvM._34);
                logger::info("Frame {}: HiZ Params - ViewInvMat r3: [{}, {}, {}, {}]", globals::state->frameCount, VInvM._41, VInvM._42, VInvM._43, VInvM._44);
            }
        }

        // Update prevframeCam
        {
            auto vd = Util::GetCameraData(0);
            DirectX::XMMATRIX V = vd.viewMat;
            DirectX::XMMATRIX P = vd.projMat;
            DirectX::XMMATRIX VP = DirectX::XMMatrixMultiply(V, P);

            DirectX::XMStoreFloat4x4(&prevframeCam.view,     V);  // row-major copy
            DirectX::XMStoreFloat4x4(&prevframeCam.proj,     P);
            DirectX::XMStoreFloat4x4(&prevframeCam.viewProj, VP);
        }

        auto renderer = globals::game::renderer;
        D3D11_TEXTURE2D_DESC texDesc{};
        renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&texDesc);

        params.bufferDim = { (float)texDesc.Width, (float)texDesc.Height };

        //logger::info("HiZ Params - BufferDim: [{}, {}]", params.bufferDim.x, params.bufferDim.y);

        params.bufferDimInv = { 1.0f / params.bufferDim.x, 1.0f / params.bufferDim.y };

        //logger::info("HiZ Params - BufferDimInv: [{}, {}]", params.bufferDimInv.x, params.bufferDimInv.y);

        if (SUCCEEDED(context->Map(hiZTestParamsBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &params, sizeof(HiZSettings));
            context->Unmap(hiZTestParamsBuffer, 0);
        } else {
            logger::warn("ExecuteVisibilityTests: failed to map hiZTestParamsBuffer");
            return;
        }

        // Set up framebuffer

        ID3D11Buffer* buffers[1] = { *globals::game::perFrame.get() };

        ID3D11Buffer* vrBuffer = nullptr;

        if (REL::Module::IsVR()) {
            static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
            vrBuffer = *VRValues.get();
        }
        if (vrBuffer) {
            context->CSSetConstantBuffers(12, 1, buffers);
            context->CSSetConstantBuffers(13, 1, &vrBuffer);
        } else {
            context->CSSetConstantBuffers(12, 1, buffers);
        }

        // Set up shared data
        globals::state->UpdateSharedData(true, false);
    }
    
    // Bind resources and dispatch Hi-Z test compute shader for batch processing
    {
        if (settings.enableBoundsViewer) {
            if (!boundsOverlayTex || boundsOverlayW != hiZWidth || boundsOverlayH != hiZHeight) {
                ReleaseBoundsOverlayResources();
                SetupBoundsOverlayResources(hiZWidth, hiZHeight);
            }
            ClearBoundsOverlay();
        }

        const bool overlayEnabled = settings.enableBoundsViewer && (boundsOverlayUAV != nullptr);

        UINT uavCount = overlayEnabled ? 3u : 2u;
        ID3D11UnorderedAccessView* uavs[3] = {
            visibilityResultsUAV,
            (settings.debugMode || settings.enableBoundsViewer) ? debugResultsUAV : nullptr,
            overlayEnabled ? boundsOverlayUAV : nullptr
        };
        context->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);

        ID3D11ShaderResourceView* srvs[] = { hiZSRV, geometryBoundsSRV };
        context->CSSetShaderResources(0, 2, srvs);
        context->CSSetConstantBuffers(0, 1, &hiZTestParamsBuffer);

        if (hiZSampler) {
            context->CSSetSamplers(0, 1, &hiZSampler);
        }
        context->CSSetShader(hiZTestCS, nullptr, 0);

        // Dispatch for batch processing
        {
            const uint32_t threadGroupSize = 256;
            uint32_t numGroups = (numGeometry + threadGroupSize - 1) / threadGroupSize;
            // Profile the dispatch to GPU
            auto startDispatch = std::chrono::high_resolution_clock::now();
            context->Dispatch(numGroups, 1, 1);
            auto endDispatch = std::chrono::high_resolution_clock::now();
            stats.gpuCullingTimeMs = static_cast<float>(
                std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(endDispatch - startDispatch).count()
            );
        }

        // Unbind resources (must pass arrays of nulls)
        ID3D11ShaderResourceView* nullSRVs_tests[2] = { nullptr, nullptr };
        context->CSSetShaderResources(0, 2, nullSRVs_tests);
        ID3D11UnorderedAccessView* nullUAVs_tests[2] = { nullptr, nullptr };
        context->CSSetUnorderedAccessViews(0, 2, nullUAVs_tests, nullptr);
        ID3D11SamplerState* nullSamplers_tests[1] = { nullptr };
        context->CSSetSamplers(0, 1, nullSamplers_tests);
        context->CSSetShader(nullptr, nullptr, 0);
    }
}

void HiZOcclusion::ProcessVisibilityResults(uint32_t bufferIndex) {

    unCullNextFrame.clear();

    // Read from the correct triple-buffered staging buffer
    const HiZOcclusion::OcclusionResult* visibilityData = static_cast<const HiZOcclusion::OcclusionResult*>(readbackState.mappedData[bufferIndex].pData);
    
    // Use the geometry snapshot that was stored with this buffer
    const auto& geometrySnapshot = readbackState.geometrySnapshots[bufferIndex];
    const uint32_t geometryCount = readbackState.geometryCount[bufferIndex];
    
    if (settings.debugMode) {
        logger::info("Processing {} results from buffer {} (frame {})",
                    geometryCount, bufferIndex, readbackState.pendingFrameIndex[bufferIndex]);
    }

    for (uint32_t i = 0; i < geometryCount && i < geometrySnapshot.size(); ++i) {
        if (!geometrySnapshot[i]) continue;
        stats.totalTested++;
        
        auto* geo = geometrySnapshot[i];
        const auto& result = visibilityData[i];
        bool currentlyOccluded = (result.objectDepth > result.sceneDepth + settings.conservativeBias);
        
        // Get or create temporal state
        auto& temporal = temporalStates[geo];
        
        // Update confidence counters
        if (currentlyOccluded) {
            temporal.occludedFrames = std::min<uint8_t>(temporal.occludedFrames + 1, 255);
            temporal.visibleFrames = 0;
        } else {
            temporal.visibleFrames = std::min<uint8_t>(temporal.visibleFrames + 1, 255);
            temporal.occludedFrames = 0;
        }
        
        // Apply hysteresis - different thresholds for hiding vs showing
        bool shouldHide = (temporal.occludedFrames >= FRAMES_TO_CULL && temporal.wasVisible);
        bool shouldShow = (temporal.visibleFrames >= FRAMES_TO_UNCULL && !temporal.wasVisible);
        if (shouldHide) {
            geo->GetFlags().set(RE::NiAVObject::Flag::kHidden);
            temporal.wasVisible = false;
            unCullNextFrame.push_back(geo);
            stats.culled++;
        } else if (shouldShow) {
            geo->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
            temporal.wasVisible = true;
            stats.visible++;
        } else {
            // No state change - keep current visibility
            if (temporal.wasVisible) {
                stats.visible++;
                // If currently occluded but not confident yet, keep testing
                if (currentlyOccluded) {
                    unCullNextFrame.push_back(geo);
                }
            } else {
                stats.culled++;
                unCullNextFrame.push_back(geo);  // Keep testing
            }
        }
    }
}