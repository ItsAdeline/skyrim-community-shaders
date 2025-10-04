#pragma once

#include "OverlayFeature.h"
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <chrono>

struct HiZOcclusion : OverlayFeature
{
    virtual inline std::string GetName() override { return "HiZ Occlusion Culling"; }
    virtual inline std::string GetShortName() override { return "HiZOcclusion"; }
    virtual inline std::string_view GetShaderDefineName() override { return "HIZ_OCCLUSION"; }
    virtual std::string_view GetCategory() const override { return "Performance"; }

    virtual bool SupportsVR() override { return true; }
    virtual bool IsCore() const override { return false; }

    // Preserve Feature base-class contract; implementation will call InitShaders()
    virtual void SetupResources() override;
    // Explicit shader initialization entry point
    void InitShaders();
    // Ensure/create Hi-Z texture + per-mip SRVs/UAVs sized to current depth
    bool InitHiZResources();
    // Unbind all compute stage resources used by Hi-Z passes
    void UnbindD3DResources();
    virtual void EarlyPrepass() override;
    void Prepass();

    void CreateDebugBuffer();
    void ReleaseDebugBuffer();

    virtual void RestoreDefaultSettings() override;
    virtual void DrawSettings() override;
    virtual void LoadSettings(json& o_json) override;
    virtual void SaveSettings(json& o_json) override;
    virtual void ClearShaderCache() override;
    
    // OverlayFeature interface
    virtual void DrawOverlay() override;
    virtual bool IsOverlayVisible() const override;

    void NewFrame();
    uint32_t currentFrame = 0;

	std::string status = "init";     // status message for debug display
	bool resourcesSetup = false;    // whether resources have been initialized
	bool resourcesValid = false;     // whether current resources are valid and safe to use

    // Per-frame tracking to verify build/draw order and availability
    uint32_t lastBuiltFrame = 0;   // frame index when the pyramid was last built
    bool builtThisFrame = false;   // true if EarlyPrepass() successfully built the pyramid this frame
    bool preserveResourcesForUI = true;  // prevent Reset() from destroying resources needed for debug viewer
    uint32_t resourceCreationFrame = 0;  // frame when resources were last created
    bool skipValidationThisFrame = false; // skip resource validation to prevent crashes during compilation

    struct Settings {
        // Debug viewer settings
        bool enableHiZViewer = false;     // show Hi-Z mip viewer window
        uint32_t hizViewerMip = 0;        // selected mip level to view
        float hizViewerScale = 1.0f;      // scale factor for display size
        bool debugMode = false;           // enable debug mode for Hi-Z testing
        
        // Hi-Z culling settings
        bool enableHiZCulling = true;     // enable Hi-Z occlusion culling
        float conservativeBias = 0.01f;   // depth bias for conservative testing (0.01 = 1% bias)
        bool showCullingStats = false;    // show Hi-Z culling statistics in UI

        // Bounds overlay viewer (draw tested bounds and closest point)
        bool enableBoundsViewer = false;  // enable per-object bounds debug overlay
        uint32_t boundsMaxObjects = 256;  // maximum objects to draw outlines for (subsampled)
        
        // Individual toggles for each early-out reason color
        bool showBehindCamera = true;      // Magenta - earlyOutReason 1
        bool showInvalidRadius = true;     // Dark Yellow - earlyOutReason 2
        bool showCameraInside = true;      // Orange - earlyOutReason 4
        bool showInvalidDepth = true;      // Pink - earlyOutReason 5
        bool showNearestOffscreen = true;  // Yellow - earlyOutReason 6
        bool showVisible = true;           // Green - earlyOutReason 0
        bool showOccluded = true;          // Red - earlyOutReason -1 (0xFFFFFFFF)
    };

    Settings settings;

    struct CameraMatrices {
        DirectX::XMFLOAT4X4 view;   // row-major
        DirectX::XMFLOAT4X4 proj;   // row-major
        DirectX::XMFLOAT4X4 viewProj; // optional but handy
    };
    
    CameraMatrices prevframeCam = {};
    bool prevframeCamDataValid = false;

    // Hi-Z pyramid resources
    ID3D11Texture2D* hiZTexture = nullptr;                       // R32_FLOAT, full mip chain
    ID3D11ShaderResourceView* hiZSRV = nullptr;                  // SRV over all mips
    std::vector<ID3D11ShaderResourceView*> hiZSRVsPerMip;        // SRV per mip slice (MostDetailedMip=i, MipLevels=1)
    std::vector<ID3D11UnorderedAccessView*> hiZUAVs;             // UAV per mip slice
    uint32_t hiZWidth = 0, hiZHeight = 0, hiZMipCount = 0;       // cached dims

    // Shaders for building the Hi-Z pyramid
    ID3D11ComputeShader* hiZBuildLevel0CS = nullptr;             // depth -> mip 0
    ID3D11ComputeShader* hiZDownsampleCS = nullptr;              // mip n -> mip n+1 (min-reduction)
    ID3D11ComputeShader* hiZTestCS = nullptr;                    // GPU-based occlusion testing
    
    // GPU culling resources
    ID3D11Buffer* geometryBoundsBuffer = nullptr;                // Input: geometry bounding spheres
    ID3D11ShaderResourceView* geometryBoundsSRV = nullptr;
    ID3D11Buffer* visibilityResultsBuffer = nullptr;             // Output: visibility results
    ID3D11UnorderedAccessView* visibilityResultsUAV = nullptr;
    ID3D11Buffer* debugResultsBuffer = nullptr;                 // Staging buffer for CPU readback
    ID3D11UnorderedAccessView* debugResultsUAV = nullptr;
    ID3D11Buffer* hiZTestParamsBuffer = nullptr;                 // Constant buffer for test parameters
    
    ID3D11Buffer* debugReadbackBuffer = nullptr;
    ID3D11Buffer* visibilityReadbackBuffer = nullptr;
    uint32_t readbackFrameIndex = 0;

    ID3D11SamplerState* hiZSampler = nullptr;                    // Sampler for Hi-Z sampling in CS
    uint32_t maxGeometryCount = 16384;                           // Maximum geometry objects per frame
    ID3D11Buffer* nullCBs[1] = { nullptr };
    ID3D11SamplerState* nullSamplers[1] = { nullptr };
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUAV = nullptr;

    // Bounds debug overlay resources (RGBA8, size = HiZ base dimensions)
    ID3D11Texture2D* boundsOverlayTex = nullptr;
    ID3D11ShaderResourceView* boundsOverlaySRV = nullptr;
    ID3D11UnorderedAccessView* boundsOverlayUAV = nullptr;
    uint32_t boundsOverlayW = 0, boundsOverlayH = 0;

    // Overlay resource management
    bool SetupBoundsOverlayResources(uint32_t width, uint32_t height);
    void ReleaseBoundsOverlayResources();
    void ClearBoundsOverlay();

    // Hi-Z occlusion culling functionality
    bool TestGeometryOcclusion(RE::NiBound* bound);
    void ProcessCulledGeometry(RE::BSGeometry* geometry, uint32_t renderFlags);
    bool SetupGPUCullingResources();
    void PerformGPUCulling();

    void DispatchComputeShader();
    void ProcessVisibilityResults();
    
    // Get current camera for culling tests
    RE::NiCamera* GetCurrentCamera();
    
    // Integration with rendering pipeline (no hooks needed)
    void IntegrateWithRenderPipeline();
    
    // Accessors for culling step
    inline ID3D11ShaderResourceView* GetHiZSRV() const { return hiZSRV; }
    inline uint32_t GetHiZMipCount() const { return hiZMipCount; }
    
    // Debugging result struct (x = object depth, y = max scene depth)
    struct OcclusionResult {
        float objectDepth;
        float sceneDepth;
    };
    
    // Comprehensive debug data from GPU (matches shader DebugData struct)
    struct DebugData {
        DirectX::XMFLOAT4 centerWS_radius;       // xyz=centerWS, w=radius
        DirectX::XMFLOAT4 centerRel_objDepth;    // xyz=centerWSCameraRelative, w=objDepth
        float sceneDepth;
        uint32_t earlyOutReason;
        DirectX::XMFLOAT2 padding;               // Total: 48 bytes
    };
    
    // Depth buffer verification
    void VerifyDepthBufferContents();
    
    // Performance metrics calculation
    void UpdatePerformanceMetrics();
    
    // Timing utility for performance measurement
    class ScopedTimer {
    public:
        ScopedTimer(float& target) : target_(target) {
            start_ = std::chrono::high_resolution_clock::now();
        }
        ~ScopedTimer() {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
            target_ = duration.count() / 1000.0f; // Convert to milliseconds
        }
    private:
        float& target_;
        std::chrono::high_resolution_clock::time_point start_;
    };

    // Culling statistics
    struct CullingStats {
        uint32_t totalTested = 0;
        uint32_t culled = 0;
        uint32_t visible = 0;
        uint32_t frameIndex = 0;
        uint32_t geometryListSize = 0;
        
        // Per early-out reason statistics
        uint32_t behindCamera = 0;       // earlyOutReason 1
        uint32_t invalidRadius = 0;      // earlyOutReason 2
        uint32_t cameraInside = 0;       // earlyOutReason 4
        uint32_t invalidDepth = 0;       // earlyOutReason 5
        uint32_t nearestOffscreen = 0;   // earlyOutReason 6
        uint32_t visibleCount = 0;       // earlyOutReason 0
        uint32_t occludedCount = 0;      // earlyOutReason -1
        
        // Timing statistics (in milliseconds)
        float resourceSetupDurationMS = 0.0f;
        float recreateDurationMS = 0.0f;
        float gpuCullingTimeMs = 0.0f;
        float hiZBuildTimeMs = 0.0f;
        float geometryProcessingTimeMs = 0.0f;
        float readbackTimeMs = 0.0f;
        float copyTimeMs = 0.0f;
        float mapTimeMs = 0.0f;
        float unmapTimeMs = 0.0f;
        float copyDataTimeMs = 0.0f;
        
        // Performance metrics
        float cullingEfficiency = 0.0f;        // percentage of geometry culled
        float cullingOverheadMs = 0.0f;        // total overhead per frame
        float avgGeometryPerMs = 0.0f;         // geometry processed per millisecond
        uint32_t pointsTestedPerObject = 17;   // 8 corners + 8 extended + 1 nearest sphere point
        
        // Running averages (over last 60 frames)
        float avgCullingEfficiency = 0.0f;
        float avgOverheadMs = 0.0f;
        float avgGeometryCount = 0.0f;
        
        // Frame timing for averaging
        std::vector<float> recentEfficiency;
        std::vector<float> recentOverhead;
        std::vector<uint32_t> recentGeometryCount;
        uint32_t maxHistoryFrames = 60;
    };
    CullingStats stats;

    struct AsyncReadbackState {
        ID3D11Buffer* stagingBuffer = nullptr;
        D3D11_MAPPED_SUBRESOURCE mappedData = {};
        bool hasPendingRead = false;
        uint32_t pendingFrameIndex = 0;
    };
    AsyncReadbackState readbackState;
    
    // Shared state for async pipeline
    uint32_t numGeometry = 0;  // Number of geometry objects in current batch
    uint32_t numGeometryPending = 0;  // Number of geometry in pending results
    std::vector<RE::BSGeometry*> pendingGeometrySnapshot;  // Snapshot for current dispatch
    std::vector<RE::BSGeometry*> pendingGeometryResults;  // Snapshot for async result processing
    
    // Geometry batch for GPU culling
    std::vector<RE::BSGeometry*> pendingGeometry;
    std::unordered_set<RE::BSGeometry*> pendingGeometrySet;  // For fast lookup
    std::vector<RE::BSGeometry*> unCullNextFrame;
    std::vector<DirectX::XMFLOAT4> geometryBounds;  // xyz=center, w=radius
    std::vector<OcclusionResult> visibilityResultsCPU;     // CPU-side visibility results (readback)
    // Mapping and per-frame state for result lookup
    std::unordered_map<RE::BSGeometry*, uint32_t> geometryIndexMap;   // geometry -> batch index (current frame pending)
    std::unordered_map<RE::BSGeometry*, OcclusionResult> visibilityResultsMap; // stores all results for debugging

    // Temporal coherence tracking - prevent flickering
    struct TemporalState {
        bool wasVisible = true;  // Default visible
        uint8_t visibleFrames = 0;  // Consecutive frames visible
        uint8_t occludedFrames = 0;  // Consecutive frames occluded
    };
    std::unordered_map<RE::BSGeometry*, TemporalState> temporalStates;

    // Settings for temporal stability
    const uint8_t FRAMES_TO_CULL = 3;    // Must be occluded 3 frames before hiding
    const uint8_t FRAMES_TO_UNCULL = 1;  // Must be visible 1 frame to unhide (faster response)

    // Previous-frame results used during rendering to avoid feedback/flicker
    std::unordered_map<RE::BSGeometry*, OcclusionResult> visibilityResultsPrev; // stores all results for debugging
    bool batchDispatchedThisFrame = false;                             // guards single dispatch + readback per frame
    uint32_t batchFrame = 0;                                           // frame index that pendingGeometry belongs to

    void ExecuteVisibilityTests();

    struct HiZSettings {
        DirectX::XMFLOAT4 hiZParams;           // 16 (mipCount, conservativeBias, geometryCount, debugMode)
        DirectX::XMFLOAT4 overlaySettings;     // 16 (overlayEnabled, maxObjectsToDraw, unused, unused)
        DirectX::XMFLOAT4 overlayColorToggles; // 16 (8 bits per toggle: behind|invalid|centerOff|camInside|invalidDepth|nearestOff|visible|occluded)
    
        DirectX::XMFLOAT3 cameraWorldPos;      // 12
        float              pad0 = 0.0f;        //  4 -> 32
    
        DirectX::XMFLOAT4X4 cameraViewMat;     // 64 -> 96
        DirectX::XMFLOAT4X4 cameraProjMat;     // 64 -> 160
        DirectX::XMFLOAT4X4 cameraViewProjMat; // 64 -> 224
    
        DirectX::XMFLOAT2 bufferDim;           //  8
        DirectX::XMFLOAT2 bufferDimInv;        //  8 -> 240
        // Total: 240 + 16 (two XMFLOAT2) = 256 bytes (multiple of 16)
    };
    static_assert(sizeof(HiZSettings) % 16 == 0, "HiZSettings must be 16B-sized");
    static_assert(alignof(HiZSettings) <= 16,  "HiZSettings alignment should not exceed 16");
};

