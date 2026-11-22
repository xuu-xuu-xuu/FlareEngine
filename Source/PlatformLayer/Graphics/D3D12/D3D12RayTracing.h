#pragma once

#include "D3D12DepthNormals.h"
#include "D3D12UploadArena.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct D3D12Context;

struct alignas(16) D3D12RayTracingInstanceData
{
    flare::Vec4 worldRow0;
    flare::Vec4 worldRow1;
    flare::Vec4 worldRow2;
    flare::Vec4 normalRow0;
    flare::Vec4 normalRow1;
    flare::Vec4 normalRow2;
    flare::Vec4 baseColor;
    flare::Vec4 materialParams;
    std::array<std::uint32_t, 4> meshMeta{};
};
static_assert(sizeof(D3D12RayTracingInstanceData) == 144);

struct alignas(16) D3D12RayTracingConstants
{
    flare::Mat4 viewFromWorld;
    flare::Mat4 clipFromView;
    flare::Mat4 viewFromClip;
    flare::Mat4 worldFromView;
    flare::Mat4 previousClipFromWorld;
    flare::Vec4 viewport;
    flare::Vec4 halfViewport;
    flare::Vec4 sunDirection;
    flare::Vec4 sunRadiance;
    flare::Vec4 skyZenith;
    flare::Vec4 skyHorizon;
    flare::Vec4 skyGround;
    flare::Vec4 strengths;
    flare::Vec4 tracingParams;
    flare::Vec4 temporalParams;
    std::array<std::uint32_t, 4> flags{};
    std::array<std::uint32_t, 4> indirectFlags{};
    std::array<flare::Vec4, 8> pointPosRange{};
    std::array<flare::Vec4, 8> pointRadiance{};
    std::array<flare::Vec4, 8> localPosRange{};
    std::array<flare::Vec4, 8> localRadiance{};
    std::array<flare::Vec4, 8> localDirection{};
    std::array<flare::Vec4, 8> localCone{};
};
static_assert(sizeof(D3D12RayTracingConstants) == 1280);

struct D3D12RayTracingTargets
{
    Microsoft::WRL::ComPtr<ID3D12Resource> visibility; // RG16_FLOAT (AO, SunShadow)
    std::shared_ptr<D3D12DescriptorRange> visibilityGraphicsSrv;
    Microsoft::WRL::ComPtr<ID3D12Resource> visRaw;
    Microsoft::WRL::ComPtr<ID3D12Resource> visScratch;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> visHistory;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> visMoments;

    Microsoft::WRL::ComPtr<ID3D12Resource> sourceHdr;
    Microsoft::WRL::ComPtr<ID3D12Resource> specRaw;
    Microsoft::WRL::ComPtr<ID3D12Resource> giRaw;
    Microsoft::WRL::ComPtr<ID3D12Resource> indirectMoments;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> specHistory;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> giHistory;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> specPing;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> giPing;

    UINT width = 0, height = 0, halfWidth = 0, halfHeight = 0;
    UINT visHistoryIndex = 0, indirectHistoryIndex = 0;
    UINT64 visFrames = 0, indirectFrames = 0;
    UINT64 visLastSubmission = 0, indirectLastSubmission = 0;
    UINT64 visSceneToken = 0, indirectSceneToken = 0;
    UINT64 allocatedBytes = 0;
};

struct D3D12RayTracingSystem;

struct D3D12RayTracingFrame
{
    std::shared_ptr<D3D12RayTracingTargets> targets;
    D3D12UploadSlice constants;
    D3D12UploadSlice tlasInstancesUpload;
    D3D12UploadSlice surfaceInstancesUpload;
    bool valid = false;
    bool visibilityValid = false;
    bool aoValid = false;
    bool shadowsValid = false;
    bool allShadowCastersTraced = false;
    bool indirectValid = false;
    bool reflectionsValid = false;
    bool giValid = false;
    UINT slotIndex = 0;
    UINT instanceCount = 0;
    UINT uniqueMeshCount = 0;
    flare::rhi::RayTracingStatistics statistics;
};

bool WriteD3D12RayTracingShaders(const std::filesystem::path& sourcePath,
    const std::filesystem::path& outputPath, const Localization& text);

bool ValidD3D12RayTracingSettings(const flare::rhi::RayTracingSettings& settings);

D3D12_CPU_DESCRIPTOR_HANDLE GetD3D12RayTracingVisibilityView(const D3D12FrameContext& frame);

void SyncD3D12RayTracingScreenFallback(D3D12FrameContext& frame);

void ResetD3D12RayTracingTargets(D3D12Presentation& presentation);

bool PrepareD3D12RayTracing(D3D12Presentation& presentation,
    D3D12FrameContext& frame, const D3D12FrameContent& content,
    UINT width, UINT height, const Localization& text);

bool RecordD3D12RayTracingPreOpaque(D3D12Presentation& presentation, D3D12FrameContext& frame,
    const D3D12FrameContent& content, D3D12StateRecording& states, const Localization& text);

bool RecordD3D12RayTracingPostOpaque(D3D12Presentation& presentation, D3D12FrameContext& frame,
    const D3D12FrameContent& content, D3D12StateRecording& states,
    ID3D12Resource& rasterColor, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    UINT width, UINT height, const Localization& text);
