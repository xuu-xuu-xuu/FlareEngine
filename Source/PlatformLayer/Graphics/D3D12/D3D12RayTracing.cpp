#include "D3D12RayTracing.h"
#include "D3D12AmbientOcclusion.h"
#include "D3D12Context.h"
#include "D3D12Memory.h"
#include "D3D12Presentation.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <dxcapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace rhi = flare::rhi;

namespace
{
using Json = nlohmann::json;

constexpr char RtArtifactMagic[8]{'S', 'E', 'R', 'T', '1', '\0', '\0', '\0'};
constexpr UINT MaxUniqueRtMeshes = 64;
constexpr UINT MaxRtInstances = 4096;
constexpr UINT PassSrvCount = 10; // t2..t11
constexpr UINT PassUavCount = 4;  // u0..u3
constexpr UINT PassStride = PassSrvCount + PassUavCount;
constexpr UINT MeshTableCount = MaxUniqueRtMeshes * 3; // 64 VB + 64 IB + 64 Albedo
constexpr UINT TotalPassCount = 10;                    // 5 visibility + 5 indirect
constexpr UINT CompositeSrvBase = MeshTableCount + TotalPassCount * PassStride;
constexpr UINT TotalHeapDescriptors = CompositeSrvBase + PassSrvCount;
constexpr std::size_t MaxShaderFileBytes = 16 * 1024 * 1024;

constexpr const wchar_t* ComputeEntryNames[6]{
    L"CSTraceVisibility",
    L"CSTemporalVisibility",
    L"CSATrousVisibility",
    L"CSTraceIndirect",
    L"CSTemporalIndirect",
    L"CSATrousIndirect"};

enum ComputeStage : UINT
{
    StageTraceVis = 0,
    StageTemporalVis = 1,
    StageATrousVis = 2,
    StageTraceIndirect = 3,
    StageTemporalIndirect = 4,
    StageATrousIndirect = 5
};

UINT64 AlignUp(UINT64 value, UINT64 alignment) noexcept
{
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

void AppendU32(std::vector<std::uint8_t>& out, UINT value)
{
    for (UINT shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

UINT ReadU32(const std::uint8_t* bytes) noexcept
{
    return UINT(bytes[0]) | (UINT(bytes[1]) << 8) | (UINT(bytes[2]) << 16) | (UINT(bytes[3]) << 24);
}

std::string PathUtf8(const fs::path& path)
{
    const auto u8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

bool ReadBytes(const fs::path& path, std::vector<std::uint8_t>& output)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto count = input.tellg();
    if (count <= 0 || static_cast<UINT64>(count) > MaxShaderFileBytes) return false;
    output.resize(static_cast<std::size_t>(count));
    input.seekg(0);
    return static_cast<bool>(input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(count)));
}

bool AtomicWrite(const fs::path& path, std::span<const std::uint8_t> bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec || bytes.size() > MAXDWORD) return false;
    static std::atomic<UINT64> sequence{1};
    auto tmp = path;
    tmp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence++);
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (!ok || !closed || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::tuple<unsigned, unsigned, unsigned, unsigned> ParseSdkVersion(std::wstring_view text)
{
    unsigned parts[4]{};
    std::size_t part = 0;
    unsigned current = 0;
    for (wchar_t ch : text)
    {
        if (ch >= L'0' && ch <= L'9')
            current = current * 10u + unsigned(ch - L'0');
        else if (ch == L'.')
        {
            if (part < 4) parts[part++] = current;
            current = 0;
        }
        else
            return {};
    }
    if (part < 4) parts[part] = current;
    return {parts[0], parts[1], parts[2], parts[3]};
}

fs::path FindDxcDirectory()
{
    std::array<wchar_t, 32768> pathBuffer{};
    const DWORD found = SearchPathW(nullptr, L"dxcompiler.dll", nullptr,
        static_cast<DWORD>(pathBuffer.size()), pathBuffer.data(), nullptr);
    if (found > 0 && found < pathBuffer.size())
    {
        std::error_code dxilEc;
        const fs::path candidate = fs::path(pathBuffer.data()).parent_path();
        if (fs::exists(candidate / "dxil.dll", dxilEc) && !dxilEc) return candidate;
    }

    std::vector<fs::path> searchRoots;
    wchar_t programFiles86[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles86, MAX_PATH) > 0)
        searchRoots.push_back(fs::path(programFiles86) / "Windows Kits/10/bin");
    searchRoots.push_back(L"C:/Program Files (x86)/Windows Kits/10/bin");

    std::tuple<unsigned, unsigned, unsigned, unsigned> bestVersion{};
    fs::path bestDir;
    std::error_code ec;
    for (const auto& root : searchRoots)
    {
        if (!fs::exists(root, ec) || ec) continue;
        for (const auto& entry : fs::directory_iterator(root, ec))
        {
            if (ec || !entry.is_directory(ec)) continue;
            const auto candidate = entry.path() / "x64";
            if (!fs::exists(candidate / "dxcompiler.dll", ec) || !fs::exists(candidate / "dxil.dll", ec))
                continue;
            const auto version = ParseSdkVersion(entry.path().filename().wstring());
            if (version > bestVersion)
            {
                bestVersion = version;
                bestDir = candidate;
            }
        }
    }
    return bestDir;
}

struct RtDxcSession
{
    HMODULE dxilModule = nullptr;
    HMODULE compilerModule = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includes;

    ~RtDxcSession()
    {
        includes.Reset();
        compiler.Reset();
        utils.Reset();
        if (compilerModule) FreeLibrary(compilerModule);
        if (dxilModule) FreeLibrary(dxilModule);
    }

    bool Initialize(const Localization& text)
    {
        const fs::path sdkDir = FindDxcDirectory();
        if (sdkDir.empty())
        {
            Log(LogLevel::Error, text.GetText("Shader.DxcUnavailable"));
            return false;
        }
        dxilModule = LoadLibraryW((sdkDir / "dxil.dll").c_str());
        compilerModule = LoadLibraryW((sdkDir / "dxcompiler.dll").c_str());
        if (!dxilModule || !compilerModule)
        {
            Log(LogLevel::Error, text.GetText("Shader.DxcUnavailable") + ": " + PathUtf8(sdkDir));
            return false;
        }
        const auto createInstance = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(compilerModule, "DxcCreateInstance"));
        if (!createInstance
            || FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.GetAddressOf())))
            || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())))
            || FAILED(utils->CreateDefaultIncludeHandler(includes.GetAddressOf())))
        {
            Log(LogLevel::Error, text.GetText("Shader.DxcUnavailable"));
            return false;
        }
        return true;
    }

    bool CompileCs65(const fs::path& shaderPath, std::span<const std::uint8_t> sourceBytes,
        const wchar_t* entry, std::vector<std::uint8_t>& output, const Localization& text) const
    {
        const std::wstring nameW = shaderPath.filename().wstring();
        const std::wstring includeW = shaderPath.parent_path().wstring();
        const wchar_t* args[]{
            nameW.c_str(),
            L"-E", entry,
            L"-T", L"cs_6_5",
            L"-HV", L"2018",
            L"-O3",
            L"-Ges",
            L"-I", includeW.c_str()};
        DxcBuffer srcBuffer{sourceBytes.data(), sourceBytes.size(), DXC_CP_UTF8};
        ComPtr<IDxcResult> result;
        if (FAILED(compiler->Compile(&srcBuffer, args, static_cast<UINT32>(std::size(args)),
            includes.Get(), IID_PPV_ARGS(result.GetAddressOf()))) || !result)
        {
            Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + PathUtf8(shaderPath));
            return false;
        }
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        ComPtr<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr);
        if (FAILED(status))
        {
            const std::string detail = (errors && errors->GetStringLength())
                ? std::string(errors->GetStringPointer(), errors->GetStringLength()) : std::string{};
            Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + detail);
            return false;
        }
        ComPtr<IDxcBlob> object;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.GetAddressOf()), nullptr))
            || !object || object->GetBufferSize() == 0)
        {
            return false;
        }
        const auto* ptr = static_cast<const std::uint8_t*>(object->GetBufferPointer());
        output.assign(ptr, ptr + object->GetBufferSize());
        return true;
    }
};

struct RtShaderBundle
{
    std::array<std::vector<std::uint8_t>, 6> computeBlobs;
    std::vector<std::uint8_t> vsComposite;
    std::vector<std::uint8_t> psComposite;
};

Json MakeRtShaderIdentity(std::span<const std::uint8_t> sourceBytes)
{
    const auto sha = flare::Sha256(sourceBytes);
    if (sha.empty()) return {};
    return {
        {"schema", 1},
        {"kind", "dxr11-hybrid-rt"},
        {"constants_bytes", sizeof(D3D12RayTracingConstants)},
        {"instance_bytes", sizeof(D3D12RayTracingInstanceData)},
        {"source_sha256", sha}
    };
}

bool CompileRtShaderBundle(const fs::path& sourcePath, std::span<const std::uint8_t> sourceBytes,
    RtShaderBundle& outBundle, const Localization& text)
{
    RtDxcSession dxc;
    if (!dxc.Initialize(text)) return false;
    for (std::size_t i = 0; i < std::size(ComputeEntryNames); ++i)
    {
        if (!dxc.CompileCs65(sourcePath, sourceBytes, ComputeEntryNames[i], outBundle.computeBlobs[i], text))
            return false;
    }
    const D3D_SHADER_MACRO compositeDefines[]{{"SE_RT_COMPOSITE_ONLY", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileD3D12Shader(sourcePath, "VSComposite", "vs_5_1", vsBlob, text, 0, compositeDefines)
        || !CompileD3D12Shader(sourcePath, "PSComposite", "ps_5_1", psBlob, text, 0, compositeDefines))
    {
        return false;
    }
    const auto* vsPtr = static_cast<const std::uint8_t*>(vsBlob->GetBufferPointer());
    outBundle.vsComposite.assign(vsPtr, vsPtr + vsBlob->GetBufferSize());
    const auto* psPtr = static_cast<const std::uint8_t*>(psBlob->GetBufferPointer());
    outBundle.psComposite.assign(psPtr, psPtr + psBlob->GetBufferSize());
    return true;
}

bool ReadRtShaderArtifact(const fs::path& artifactPath, const Json& expectedIdentity, RtShaderBundle& outBundle)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(artifactPath, bytes) || bytes.size() < 16
        || std::memcmp(bytes.data(), RtArtifactMagic, sizeof(RtArtifactMagic)) != 0)
    {
        return false;
    }
    const UINT jsonLen = ReadU32(bytes.data() + 8);
    const UINT blobCount = ReadU32(bytes.data() + 12);
    if (jsonLen == 0 || jsonLen > 65536 || blobCount != 8 || 16ull + jsonLen > bytes.size())
        return false;

    try
    {
        const auto meta = Json::parse(bytes.begin() + 16, bytes.begin() + 16 + jsonLen);
        if (meta.at("identity") != expectedIdentity) return false;
        const auto& hashes = meta.at("blob_sha256");
        if (!hashes.is_array() || hashes.size() != 8) return false;

        std::size_t cursor = 16u + jsonLen;
        std::vector<std::uint8_t>* targets[8]{
            &outBundle.computeBlobs[0], &outBundle.computeBlobs[1], &outBundle.computeBlobs[2],
            &outBundle.computeBlobs[3], &outBundle.computeBlobs[4], &outBundle.computeBlobs[5],
            &outBundle.vsComposite, &outBundle.psComposite};
        for (std::size_t i = 0; i < 8; ++i)
        {
            if (cursor + 4 > bytes.size()) return false;
            const UINT len = ReadU32(bytes.data() + cursor);
            cursor += 4;
            if (len == 0 || cursor + len > bytes.size()) return false;
            targets[i]->assign(bytes.data() + cursor, bytes.data() + cursor + len);
            cursor += len;
            if (flare::Sha256(*targets[i]) != hashes[i].get<std::string>()) return false;
        }
        return cursor == bytes.size();
    }
    catch (...)
    {
        return false;
    }
}

struct BlasKey
{
    const ID3D12Resource* vb = nullptr;
    UINT64 vbGpuVa = 0;
    UINT vbBytes = 0;
    UINT vbStride = 0;
    const ID3D12Resource* ib = nullptr;
    UINT64 ibGpuVa = 0;
    UINT ibBytes = 0;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_R32_UINT;
    UINT indexCount = 0;

    bool operator==(const BlasKey&) const = default;
};

struct BlasKeyHash
{
    std::size_t operator()(const BlasKey& k) const noexcept
    {
        std::size_t h = std::hash<const void*>{}(k.vb);
        auto mix = [&](std::size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<UINT64>{}(k.vbGpuVa));
        mix(k.vbBytes);
        mix(k.vbStride);
        mix(std::hash<const void*>{}(k.ib));
        mix(std::hash<UINT64>{}(k.ibGpuVa));
        mix(k.indexCount);
        mix(static_cast<std::size_t>(k.ibFormat));
        return h;
    }
};

struct BlasEntry
{
    ComPtr<ID3D12Resource> vbKeepAlive;
    ComPtr<ID3D12Resource> ibKeepAlive;
    ComPtr<ID3D12Resource> blas;
    UINT64 bytes = 0;
    UINT64 lastUsedSubmission = 0;
    UINT64 lastFenceValue = 0;
    bool built = false;
};

struct PendingBlasBuild
{
    BlasEntry* entry = nullptr;
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
    UINT64 scratchOffset = 0;
    UINT64 scratchBytes = 0;
};

struct UniqueMeshBinding
{
    ID3D12Resource* vb = nullptr;
    ID3D12Resource* ib = nullptr;
    ID3D12Resource* albedoTex = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv{};
    UINT64 vbFirstWord = 0;
    UINT vbWords = 0;
    UINT64 ibFirstIndex = 0;
    UINT indexCount = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
};

struct FrameSlotResources
{
    ComPtr<ID3D12Resource> tlas;
    UINT64 tlasCapacity = 0;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT descriptorIncrement = 0;
    std::vector<PendingBlasBuild> pendingBlasBuilds;
    std::vector<UniqueMeshBinding> uniqueMeshes;
    std::vector<ComPtr<ID3D12Resource>> frameLeases;
    UINT64 requiredScratchBytes = 0;
    UINT64 tlasScratchOffset = 0;
};
} // namespace

struct D3D12RayTracingSystem
{
    ComPtr<ID3D12Device5> device5;
    ComPtr<ID3D12RootSignature> computeRoot;
    ComPtr<ID3D12RootSignature> compositeRoot;
    std::array<ComPtr<ID3D12PipelineState>, 6> computePipelines;
    ComPtr<ID3D12PipelineState> compositePipeline;

    std::unordered_map<BlasKey, BlasEntry, BlasKeyHash> blasCache;
    std::vector<std::pair<UINT64, ComPtr<ID3D12Resource>>> retiringResources;
    ComPtr<ID3D12Resource> scratchBuffer;
    UINT64 scratchCapacity = 0;
    std::array<FrameSlotResources, D3D12Presentation::frameCount> slots;
    std::shared_ptr<D3D12RayTracingTargets> targets;
    bool initialized = false;
    bool initFailed = false;

    bool EnsureInitialized(ID3D12Device& baseDevice, const Localization& text)
    {
        if (initialized) return true;
        if (initFailed) return false;

        if (FAILED(baseDevice.QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()))) || !device5)
        {
            initFailed = true;
            return false;
        }

        const fs::path exeDir = GetExecutableDirectory();
        fs::path shaderPath = exeDir / "Resources/Shaders/RayTracing.hlsl";
        std::error_code shaderEc;
        if (!fs::exists(shaderPath, shaderEc) || shaderEc)
            shaderPath = fs::path("Resources/Shaders/RayTracing.hlsl");
        fs::path artifactPath = shaderPath;
        artifactPath += L".sebin";

        std::vector<std::uint8_t> sourceBytes;
        if (!ReadBytes(shaderPath, sourceBytes))
        {
            initFailed = true;
            return false;
        }
        const Json identity = MakeRtShaderIdentity(sourceBytes);
        RtShaderBundle bundle;
        if (identity.is_null()
            || (!ReadRtShaderArtifact(artifactPath, identity, bundle)
                && !CompileRtShaderBundle(shaderPath, sourceBytes, bundle, text)))
        {
            initFailed = true;
            return false;
        }

        // 1. 计算根签名（DXR 1.1 内联 RayQuery + SVGF 滤波共用）
        D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, PassSrvCount, 2, 0, 0};
        D3D12_DESCRIPTOR_RANGE uavRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, PassUavCount, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE meshRanges[3]{
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MaxUniqueRtMeshes, 0, 1, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MaxUniqueRtMeshes, 0, 2, MaxUniqueRtMeshes},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MaxUniqueRtMeshes, 0, 3, MaxUniqueRtMeshes * 2}};

        D3D12_ROOT_PARAMETER computeParams[7]{};
        computeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeParams[0].Descriptor = {0, 0};
        computeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeParams[1].Descriptor = {1, 0};
        computeParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        computeParams[2].Descriptor = {0, 0};
        computeParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        computeParams[3].Constants = {1, 0, 4};
        computeParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeParams[4].DescriptorTable = {1, &srvRange};
        computeParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeParams[5].DescriptorTable = {1, &uavRange};
        computeParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeParams[6].DescriptorTable = {3, meshRanges};
        for (auto& p : computeParams) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (UINT s = 0; s < 2; ++s)
        {
            samplers[s].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[s].AddressU = samplers[s].AddressV = samplers[s].AddressW =
                (s == 0) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[s].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[s].MaxAnisotropy = 1;
            samplers[s].ShaderRegister = s;
            samplers[s].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC computeSigDesc{7, computeParams, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> sigBlob, sigErr;
        if (FAILED(D3D12SerializeRootSignature(&computeSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr))
            || FAILED(baseDevice.CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(computeRoot.GetAddressOf()))))
        {
            initFailed = true;
            return false;
        }

        for (std::size_t i = 0; i < 6; ++i)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.pRootSignature = computeRoot.Get();
            psoDesc.CS = {bundle.computeBlobs[i].data(), bundle.computeBlobs[i].size()};
            if (FAILED(baseDevice.CreateComputePipelineState(&psoDesc,
                IID_PPV_ARGS(computePipelines[i].GetAddressOf()))))
            {
                initFailed = true;
                return false;
            }
        }

        // 2. 全屏双边升采样 HDR 合成图形根签名与 PSO
        D3D12_ROOT_PARAMETER gfxParams[2]{};
        gfxParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        gfxParams[0].Descriptor = {0, 0};
        gfxParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        gfxParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        gfxParams[1].DescriptorTable = {1, &srvRange};
        gfxParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC gfxSampler = samplers[0];
        gfxSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC gfxSigDesc{2, gfxParams, 1, &gfxSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> gfxSigBlob, gfxSigErr;
        if (FAILED(D3D12SerializeRootSignature(&gfxSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &gfxSigBlob, &gfxSigErr))
            || FAILED(baseDevice.CreateRootSignature(0, gfxSigBlob->GetBufferPointer(), gfxSigBlob->GetBufferSize(),
                IID_PPV_ARGS(compositeRoot.GetAddressOf()))))
        {
            initFailed = true;
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gfxPso{};
        gfxPso.pRootSignature = compositeRoot.Get();
        gfxPso.VS = {bundle.vsComposite.data(), bundle.vsComposite.size()};
        gfxPso.PS = {bundle.psComposite.data(), bundle.psComposite.size()};
        gfxPso.SampleDesc = {1, 0};
        gfxPso.SampleMask = UINT_MAX;
        gfxPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gfxPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gfxPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gfxPso.RasterizerState.DepthClipEnable = TRUE;
        gfxPso.NumRenderTargets = 1;
        gfxPso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        gfxPso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(baseDevice.CreateGraphicsPipelineState(&gfxPso, IID_PPV_ARGS(compositePipeline.GetAddressOf()))))
        {
            initFailed = true;
            return false;
        }

        initialized = true;
        return true;
    }
};

bool WriteD3D12RayTracingShaders(const fs::path& sourcePath, const fs::path& outputPath, const Localization& text)
{
    std::vector<std::uint8_t> sourceBytes;
    if (!ReadBytes(sourcePath, sourceBytes)) return false;
    const Json identity = MakeRtShaderIdentity(sourceBytes);
    RtShaderBundle bundle;
    if (identity.is_null() || !CompileRtShaderBundle(sourcePath, sourceBytes, bundle, text))
        return false;

    const std::vector<std::uint8_t>* blobs[8]{
        &bundle.computeBlobs[0], &bundle.computeBlobs[1], &bundle.computeBlobs[2],
        &bundle.computeBlobs[3], &bundle.computeBlobs[4], &bundle.computeBlobs[5],
        &bundle.vsComposite, &bundle.psComposite};
    Json hashes = Json::array();
    for (const auto* b : blobs) hashes.push_back(flare::Sha256(*b));
    const std::string metaText = Json{{"identity", identity}, {"blob_sha256", hashes}}.dump(2);

    std::vector<std::uint8_t> payload(RtArtifactMagic, RtArtifactMagic + sizeof(RtArtifactMagic));
    AppendU32(payload, static_cast<UINT>(metaText.size()));
    AppendU32(payload, 8u);
    payload.insert(payload.end(), metaText.begin(), metaText.end());
    for (const auto* b : blobs)
    {
        AppendU32(payload, static_cast<UINT>(b->size()));
        payload.insert(payload.end(), b->begin(), b->end());
    }

    fs::path manifest = outputPath;
    manifest += L".json";
    return AtomicWrite(manifest, std::span(reinterpret_cast<const std::uint8_t*>(metaText.data()), metaText.size()))
        && AtomicWrite(outputPath, payload);
}

bool ValidD3D12RayTracingSettings(const rhi::RayTracingSettings& s)
{
    if (!s.enabled) return true;
    const auto inRange = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!inRange(s.shadowStrength, 0.f, 1.f)
        || !inRange(s.reflectionStrength, 0.f, 4.f)
        || !inRange(s.aoStrength, 0.f, 1.f)
        || !inRange(s.giStrength, 0.f, 4.f)
        || !inRange(s.aoRadius, 0.01f, 100.f)
        || !inRange(s.maxDistance, 0.5f, 10000.f)
        || !inRange(s.sunAngularRadius, 0.f, 0.2f)
        || !inRange(s.normalBias, 0.0005f, 0.5f)
        || !inRange(s.maxRoughness, 0.05f, 1.f)
        || !inRange(s.historyBlendWeight, 0.f, 0.98f))
    {
        return false;
    }
    if (s.Active())
    {
        if (!flare::IsFinite(s.view) || !flare::IsFinite(s.clipFromView)
            || !flare::Inverse(s.view) || !flare::Inverse(s.clipFromView)
            || (s.historyValid && !flare::IsFinite(s.previousClipFromWorld)))
        {
            return false;
        }
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE GetD3D12RayTracingVisibilityView(const D3D12FrameContext& frame)
{
    if (!frame.rayTracing || !frame.rayTracing->visibilityValid
        || !frame.rayTracing->targets || !frame.rayTracing->targets->visibilityGraphicsSrv)
    {
        return {};
    }
    return frame.rayTracing->targets->visibilityGraphicsSrv->Cpu();
}

void SyncD3D12RayTracingScreenFallback(D3D12FrameContext& frame)
{
    if (!frame.rayTracing || !frame.rayTracing->valid || !frame.rayTracing->visibilityValid
        || !frame.rayTracing->constants.cpu)
    {
        return;
    }
    UINT fallbackMode = 0u;
    if (frame.ambientOcclusion && frame.ambientOcclusion->statistics.valid)
    {
        if (!frame.rayTracing->statistics.aoValid && frame.ambientOcclusion->statistics.aoValid)
            fallbackMode |= 1u;
        if (!frame.rayTracing->statistics.shadowsValid && frame.ambientOcclusion->statistics.contactValid)
            fallbackMode |= 2u;
    }
    auto* cb = reinterpret_cast<D3D12RayTracingConstants*>(frame.rayTracing->constants.cpu);
    cb->temporalParams.w = float(fallbackMode);
    frame.rayTracing->aoValid = frame.rayTracing->statistics.aoValid || ((fallbackMode & 1u) != 0u);
    frame.rayTracing->shadowsValid = frame.rayTracing->statistics.shadowsValid || ((fallbackMode & 2u) != 0u);
}

void ResetD3D12RayTracingTargets(D3D12Presentation& presentation)
{
    presentation.rayTracingStatistics = {};
    if (!presentation.rayTracing) return;
    presentation.rayTracing->targets.reset();
    presentation.rayTracing->retiringResources.clear();
    for (auto& slot : presentation.rayTracing->slots)
    {
        slot.pendingBlasBuilds.clear();
        slot.uniqueMeshes.clear();
        slot.frameLeases.clear();
        slot.requiredScratchBytes = 0;
    }
}

namespace
{
bool EnsureRayTracingTargets(D3D12Presentation& p, ID3D12Device& device, UINT width, UINT height,
    std::shared_ptr<D3D12RayTracingTargets>& output, const Localization& text)
{
    const UINT halfW = (std::max)(1u, (width + 1u) / 2u);
    const UINT halfH = (std::max)(1u, (height + 1u) / 2u);
    if (output && output->width == width && output->height == height
        && output->halfWidth == halfW && output->halfHeight == halfH)
    {
        return true;
    }

    auto next = std::make_shared<D3D12RayTracingTargets>();
    next->width = width;
    next->height = height;
    next->halfWidth = halfW;
    next->halfHeight = halfH;

    auto makeTex = [&](UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                       ComPtr<ID3D12Resource>& outRes, const char* debugName) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = fmt;
        desc.SampleDesc = {1, 0};
        desc.Flags = flags;
        if (!CheckGraphicsResult(p.memory->Create(desc, D3D12_RESOURCE_STATE_COMMON, nullptr, outRes, debugName),
            text, "D3D12.PostTargetsFailed"))
        {
            return false;
        }
        if (const auto alloc = p.memory->Allocation(*outRes.Get()))
            next->allocatedBytes += alloc->bytes;
        return true;
    };

    constexpr auto uavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!makeTex(width, height, DXGI_FORMAT_R16G16_FLOAT, uavFlag, next->visibility, "RT.Visibility")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->visRaw, "RT.VisRaw")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->visScratch, "RT.VisScratch")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->visHistory[0], "RT.VisHistoryA")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->visHistory[1], "RT.VisHistoryB")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->visMoments[0], "RT.VisMomentsA")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->visMoments[1], "RT.VisMomentsB")
        || !makeTex(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, next->sourceHdr, "RT.SourceHDR")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->specRaw, "RT.SpecRaw")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->giRaw, "RT.GiRaw")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->indirectMoments, "RT.IndirectMoments")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->specHistory[0], "RT.SpecHistoryA")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->specHistory[1], "RT.SpecHistoryB")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->giHistory[0], "RT.GiHistoryA")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->giHistory[1], "RT.GiHistoryB")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->specPing[0], "RT.SpecPingA")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->specPing[1], "RT.SpecPingB")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->giPing[0], "RT.GiPingA")
        || !makeTex(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT, uavFlag, next->giPing[1], "RT.GiPingB"))
    {
        return false;
    }

    if (!CheckGraphicsResult(p.srvPool->Allocate(1, next->visibilityGraphicsSrv),
        text, "D3D12.DescriptorCapacityExceeded"))
    {
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC visSrv{};
    visSrv.Format = DXGI_FORMAT_R16G16_FLOAT;
    visSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    visSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    visSrv.Texture2D.MipLevels = 1;
    device.CreateShaderResourceView(next->visibility.Get(), &visSrv, next->visibilityGraphicsSrv->Cpu());

    output = std::move(next);
    return true;
}

bool EnsureAsBuffer(D3D12Presentation& p, UINT64 minBytes, ComPtr<ID3D12Resource>& buffer,
    UINT64& capacity, const char* name, std::vector<std::pair<UINT64, ComPtr<ID3D12Resource>>>& retiring,
    UINT64 retireFence, const Localization& text)
{
    const UINT64 aligned = AlignUp((std::max)(UINT64{256}, minBytes),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (buffer && capacity >= aligned) return true;
    if (buffer) retiring.emplace_back(retireFence, std::move(buffer));

    const UINT64 allocBytes = AlignUp((std::max)(aligned, capacity * 2ull), 65536ull);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = allocBytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> created;
    if (!CheckGraphicsResult(p.memory->Create(desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        nullptr, created, name), text, "D3D12.RayTracingBuildFailed"))
    {
        capacity = 0;
        return false;
    }
    buffer = std::move(created);
    capacity = allocBytes;
    return true;
}

bool EnsureScratchBuffer(D3D12Presentation& p, UINT64 minBytes, ComPtr<ID3D12Resource>& buffer,
    UINT64& capacity, std::vector<std::pair<UINT64, ComPtr<ID3D12Resource>>>& retiring,
    UINT64 retireFence, const Localization& text)
{
    const UINT64 aligned = AlignUp((std::max)(UINT64{256}, minBytes),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (buffer && capacity >= aligned) return true;
    if (buffer) retiring.emplace_back(retireFence, std::move(buffer));

    const UINT64 allocBytes = AlignUp((std::max)(aligned, (std::max)(UINT64{256 * 1024}, capacity * 2ull)), 65536ull);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = allocBytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> created;
    if (!CheckGraphicsResult(p.memory->Create(desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, created, "RT.Scratch"), text, "D3D12.RayTracingBuildFailed"))
    {
        capacity = 0;
        return false;
    }
    buffer = std::move(created);
    capacity = allocBytes;
    return true;
}
} // namespace

bool PrepareD3D12RayTracing(D3D12Presentation& p,
    D3D12FrameContext& frame, const D3D12FrameContent& content,
    UINT width, UINT height, const Localization& text)
{
    frame.rayTracing.reset();
    p.rayTracingStatistics = {};
    if (!content.forward || !content.forward->rayTracing.Active())
    {
        if (p.rayTracing) p.rayTracing->targets.reset();
        return true;
    }

    const auto& lighting = *content.forward;
    const auto& rt = lighting.rayTracing;
    const bool aoActive = rt.ambientOcclusion && rt.aoStrength > 0.f && rt.aoRadius > 0.f;
    const bool shadowsActive = rt.shadows && rt.shadowStrength > 0.f && lighting.directionalEnabled;
    const bool reflActive = rt.reflections && rt.reflectionStrength > 0.f;
    const bool giActive = rt.globalIllumination && rt.giStrength > 0.f;
    if (!aoActive && !shadowsActive && !reflActive && !giActive)
    {
        if (p.rayTracing) p.rayTracing->targets.reset();
        return true;
    }

    rhi::RayTracingStatistics stats{};
    stats.requested = true;
    stats.supported = p.rayTracingTier1_1;

    ComPtr<ID3D12Device> device;
    if (!p.rayTracingTier1_1 || !p.queue || FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))) || !device)
    {
        stats.fallback = rhi::RayTracingFallback::UnsupportedHardware;
        p.rayTracingStatistics = stats;
        return true;
    }
    if (!frame.depthNormals || !frame.depthNormals->statistics.valid || !frame.depthNormals->target)
    {
        stats.fallback = rhi::RayTracingFallback::MissingDepthNormals;
        p.rayTracingStatistics = stats;
        return true;
    }
    if (!frame.depthNormals->statistics.complete
        || (rt.IndirectEnabled() && !frame.depthNormals->target->material))
    {
        stats.fallback = rhi::RayTracingFallback::IncompleteGeometry;
        p.rayTracingStatistics = stats;
        return true;
    }

    if (!p.rayTracing) p.rayTracing = std::make_shared<D3D12RayTracingSystem>();
    auto& sys = *p.rayTracing;
    if (!sys.EnsureInitialized(*device.Get(), text))
    {
        stats.fallback = rhi::RayTracingFallback::ShaderUnavailable;
        p.rayTracingStatistics = stats;
        return true;
    }

    // 回收已完成 Fence 的退役加速结构与长时间未使用的 BLAS。
    const UINT64 completedFence = p.fence ? p.fence->GetCompletedValue() : 0;
    std::erase_if(sys.retiringResources, [&](const auto& item) { return completedFence >= item.first; });
    for (auto it = sys.blasCache.begin(); it != sys.blasCache.end();)
    {
        if (p.submittedFrames > it->second.lastUsedSubmission + 180ull
            && completedFence >= it->second.lastFenceValue)
        {
            it = sys.blasCache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    const UINT slotIdx = p.frameCursor % D3D12Presentation::frameCount;
    auto& slot = sys.slots[slotIdx];
    slot.pendingBlasBuilds.clear();
    slot.uniqueMeshes.clear();
    slot.frameLeases.clear();
    slot.requiredScratchBytes = 0;

    struct MeshSurfaceKey
    {
        ID3D12Resource* vb = nullptr;
        UINT64 vbLoc = 0;
        UINT vbBytes = 0;
        ID3D12Resource* ib = nullptr;
        UINT64 ibLoc = 0;
        UINT indexCount = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* albedoTex = nullptr;
        SIZE_T albedoSrvPtr = 0;
        bool operator==(const MeshSurfaceKey&) const = default;
    };
    struct MeshSurfaceKeyHash
    {
        std::size_t operator()(const MeshSurfaceKey& k) const noexcept
        {
            std::size_t h = std::hash<void*>{}(k.vb);
            auto mix = [&](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
            mix(std::hash<UINT64>{}(k.vbLoc));
            mix(std::hash<UINT>{}(k.vbBytes));
            mix(std::hash<void*>{}(k.ib));
            mix(std::hash<UINT64>{}(k.ibLoc));
            mix(std::hash<UINT>{}(k.indexCount));
            mix(std::hash<int>{}(static_cast<int>(k.indexFormat)));
            mix(std::hash<void*>{}(k.albedoTex));
            mix(std::hash<SIZE_T>{}(k.albedoSrvPtr));
            return h;
        }
    };
    struct InstanceDedupeKey
    {
        ID3D12Resource* vb = nullptr;
        UINT64 vbLoc = 0;
        ID3D12Resource* ib = nullptr;
        UINT64 ibLoc = 0;
        UINT indexCount = 0;
        std::array<std::uint32_t, 12> worldBits{};
        bool operator==(const InstanceDedupeKey&) const = default;
    };
    struct InstanceDedupeKeyHash
    {
        std::size_t operator()(const InstanceDedupeKey& k) const noexcept
        {
            std::size_t h = std::hash<void*>{}(k.vb);
            auto mix = [&](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
            mix(std::hash<UINT64>{}(k.vbLoc));
            mix(std::hash<void*>{}(k.ib));
            mix(std::hash<UINT64>{}(k.ibLoc));
            mix(std::hash<UINT>{}(k.indexCount));
            for (std::uint32_t bits : k.worldBits)
                mix(std::hash<std::uint32_t>{}(bits));
            return h;
        }
    };
    const auto makeDedupeKey = [](const D3D12Mesh& mesh, const flare::Mat4& world) {
        InstanceDedupeKey key{};
        key.vb = mesh.vertices.Get();
        key.vbLoc = mesh.vertexView.BufferLocation;
        key.ib = mesh.indices.Get();
        key.ibLoc = mesh.indexView.BufferLocation;
        key.indexCount = mesh.indexCount;
        UINT k = 0;
        for (UINT r = 0; r < 3; ++r)
        {
            for (UINT c = 0; c < 4; ++c)
            {
                float v = world(r, c);
                if (v == 0.f) v = 0.f;
                std::memcpy(&key.worldBits[k++], &v, sizeof(float));
            }
        }
        return key;
    };

    std::unordered_map<MeshSurfaceKey, UINT, MeshSurfaceKeyHash> meshToSlot;
    std::unordered_set<InstanceDedupeKey, InstanceDedupeKeyHash> emittedInstances;
    std::unordered_set<BlasEntry*> queuedBlas;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> tlasInstances;
    std::vector<D3D12RayTracingInstanceData> surfaceInstances;

    const std::size_t opaqueCount = (std::min)(content.opaqueDrawCount, content.meshes.size());
    UINT64 scratchCursor = 0;
    std::uint32_t unsupportedCasters = 0;

    const auto failToRaster = [&](rhi::RayTracingFallback reason) {
        slot.pendingBlasBuilds.clear();
        slot.uniqueMeshes.clear();
        slot.frameLeases.clear();
        slot.requiredScratchBytes = 0;
        frame.rayTracing.reset();
        stats.valid = stats.aoValid = stats.shadowsValid = stats.reflectionsValid = stats.giValid = false;
        stats.fallback = reason;
        p.rayTracingStatistics = stats;
        return true;
    };

    const auto processDraw = [&](const D3D12MeshDraw& draw, bool fromShadowCasters) -> bool {
        if (!draw.mesh || !draw.mesh->vertices || !draw.mesh->indices
            || draw.alphaMode == flare::MaterialAlphaMode::Blend
            || draw.compressedVertices || !draw.skinPalette.empty())
        {
            if (fromShadowCasters) ++unsupportedCasters;
            else ++stats.unsupportedDraws;
            return true;
        }
        const auto& mesh = *draw.mesh;
        const UINT stride = mesh.vertexView.StrideInBytes;
        const UINT vertCount = stride > 0 ? (mesh.vertexView.SizeInBytes / stride) : 0;
        if ((stride != sizeof(rhi::Vertex) && stride != sizeof(rhi::SkinnedVertex))
            || vertCount < 3 || mesh.indexCount < 3 || (mesh.indexCount % 3) != 0)
        {
            if (fromShadowCasters) ++unsupportedCasters;
            else ++stats.unsupportedDraws;
            return true;
        }
        if (fromShadowCasters && draw.instances.empty()
            && emittedInstances.contains(makeDedupeKey(mesh, draw.world)))
        {
            return true;
        }

        BlasKey key{
            mesh.vertices.Get(), mesh.vertexView.BufferLocation, mesh.vertexView.SizeInBytes, stride,
            mesh.indices.Get(), mesh.indexView.BufferLocation, mesh.indexView.SizeInBytes,
            mesh.indexView.Format, mesh.indexCount};

        auto [blasIt, inserted] = sys.blasCache.try_emplace(key);
        auto& blasEntry = blasIt->second;
        blasEntry.lastUsedSubmission = p.submittedFrames + 1;
        blasEntry.lastFenceValue = p.nextFenceValue;

        if ((inserted || !blasEntry.blas || !blasEntry.built) && !queuedBlas.contains(&blasEntry))
        {
            D3D12_RAYTRACING_GEOMETRY_DESC geom{};
            geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geom.Triangles.Transform3x4 = 0;
            geom.Triangles.IndexFormat = mesh.indexView.Format;
            geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geom.Triangles.IndexCount = mesh.indexCount;
            geom.Triangles.VertexCount = vertCount;
            geom.Triangles.IndexBuffer = mesh.indexView.BufferLocation;
            geom.Triangles.VertexBuffer = {mesh.vertexView.BufferLocation, stride};

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = 1;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.pGeometryDescs = &geom;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            sys.device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
            if (prebuild.ResultDataMaxSizeInBytes == 0)
            {
                sys.blasCache.erase(blasIt);
                if (fromShadowCasters) ++unsupportedCasters;
                else ++stats.unsupportedDraws;
                return true;
            }

            if (!EnsureAsBuffer(p, prebuild.ResultDataMaxSizeInBytes, blasEntry.blas, blasEntry.bytes,
                "RT.BLAS", sys.retiringResources, p.nextFenceValue, text))
            {
                sys.blasCache.erase(blasIt);
                return false;
            }
            blasEntry.vbKeepAlive = mesh.vertices;
            blasEntry.ibKeepAlive = mesh.indices;
            blasEntry.built = false;

            const UINT64 alignedScratch = AlignUp((std::max)(UINT64{256}, prebuild.ScratchDataSizeInBytes),
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
            slot.pendingBlasBuilds.push_back({&blasEntry, geom, scratchCursor, alignedScratch});
            queuedBlas.insert(&blasEntry);
            scratchCursor += alignedScratch;
            ++stats.blasBuilds;
        }
        else
        {
            ++stats.blasCacheHits;
        }

        slot.frameLeases.push_back(blasEntry.blas);
        slot.frameLeases.push_back(mesh.vertices);
        slot.frameLeases.push_back(mesh.indices);
        if (mesh.texture) slot.frameLeases.push_back(mesh.texture);

        const D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv = mesh.textureViews
            ? mesh.textureViews->Cpu(draw.textureIsData ? 1 : 0)
            : D3D12_CPU_DESCRIPTOR_HANDLE{};
        const MeshSurfaceKey surfKey{
            mesh.vertices.Get(), mesh.vertexView.BufferLocation, mesh.vertexView.SizeInBytes,
            mesh.indices.Get(), mesh.indexView.BufferLocation, mesh.indexCount, mesh.indexView.Format,
            mesh.texture.Get(), albedoSrv.ptr};

        UINT meshSlot = 0xFFFFFFFFu;
        if (auto foundSlot = meshToSlot.find(surfKey); foundSlot != meshToSlot.end())
        {
            meshSlot = foundSlot->second;
        }
        else if (slot.uniqueMeshes.size() < MaxUniqueRtMeshes && (mesh.vertexView.SizeInBytes % 4u) == 0u)
        {
            meshSlot = static_cast<UINT>(slot.uniqueMeshes.size());
            meshToSlot.emplace(surfKey, meshSlot);
            UniqueMeshBinding binding{};
            binding.vb = mesh.vertices.Get();
            binding.ib = mesh.indices.Get();
            binding.albedoTex = mesh.texture.Get();
            binding.albedoSrv = albedoSrv;
            const UINT64 vbBase = mesh.vertices->GetGPUVirtualAddress();
            const UINT64 ibBase = mesh.indices->GetGPUVirtualAddress();
            const UINT ibStride = mesh.indexView.Format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
            binding.vbFirstWord = mesh.vertexView.BufferLocation >= vbBase
                ? (mesh.vertexView.BufferLocation - vbBase) / 4u : 0u;
            binding.vbWords = mesh.vertexView.SizeInBytes / 4u;
            binding.ibFirstIndex = mesh.indexView.BufferLocation >= ibBase
                ? (mesh.indexView.BufferLocation - ibBase) / ibStride : 0u;
            binding.indexCount = mesh.indexCount;
            binding.indexFormat = mesh.indexView.Format;
            slot.uniqueMeshes.push_back(binding);
        }

        const UINT hasAlbedoTex = (meshSlot < MaxUniqueRtMeshes && albedoSrv.ptr != 0) ? 1u : 0u;

        const auto appendInstance = [&](const flare::Mat4& world, flare::Vec4 tint, float metallic,
                                        float roughness, bool unlit, bool doubleSided) {
            const auto dedupeKey = makeDedupeKey(mesh, world);
            if (emittedInstances.contains(dedupeKey)) return;
            if (tlasInstances.size() >= MaxRtInstances || !flare::IsFinite(world))
            {
                if (fromShadowCasters) ++unsupportedCasters;
                else ++stats.unsupportedDraws;
                return;
            }
            const auto invWorld = flare::Inverse(world);
            if (!invWorld || !flare::IsFinite(*invWorld))
            {
                if (fromShadowCasters) ++unsupportedCasters;
                else ++stats.unsupportedDraws;
                return;
            }
            const flare::Mat4 normalMat = flare::Transpose(*invWorld);
            emittedInstances.insert(dedupeKey);

            const UINT instIndex = static_cast<UINT>(tlasInstances.size());
            D3D12_RAYTRACING_INSTANCE_DESC desc{};
            for (UINT r = 0; r < 3; ++r)
                for (UINT c = 0; c < 4; ++c)
                    desc.Transform[r][c] = world(r, c);
            desc.InstanceID = instIndex;
            desc.InstanceMask = 0xFF;
            desc.InstanceContributionToHitGroupIndex = 0;
            desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE
                | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            desc.AccelerationStructure = blasEntry.blas->GetGPUVirtualAddress();
            tlasInstances.push_back(desc);

            D3D12RayTracingInstanceData surf{};
            surf.worldRow0 = {world(0, 0), world(0, 1), world(0, 2), world(0, 3)};
            surf.worldRow1 = {world(1, 0), world(1, 1), world(1, 2), world(1, 3)};
            surf.worldRow2 = {world(2, 0), world(2, 1), world(2, 2), world(2, 3)};
            surf.normalRow0 = {normalMat(0, 0), normalMat(0, 1), normalMat(0, 2), 0.f};
            surf.normalRow1 = {normalMat(1, 0), normalMat(1, 1), normalMat(1, 2), 0.f};
            surf.normalRow2 = {normalMat(2, 0), normalMat(2, 1), normalMat(2, 2), 0.f};
            surf.baseColor = tint;
            surf.materialParams = {
                std::clamp(metallic, 0.f, 1.f),
                std::clamp(roughness, 0.045f, 1.f),
                unlit ? 1.f : 0.f,
                doubleSided ? 1.f : 0.f};
            surf.meshMeta = {meshSlot, stride, mesh.indexCount, hasAlbedoTex};
            surfaceInstances.push_back(surf);
        };

        if (draw.instances.empty())
        {
            appendInstance(draw.world, draw.tint, draw.pbr.metallic, draw.pbr.roughness,
                           draw.unlit, draw.pbr.doubleSided);
        }
        else
        {
            for (const auto& inst : draw.instances)
                appendInstance(inst.world, inst.tint, inst.metallic, inst.roughness, inst.unlit, inst.doubleSided);
        }
        return true;
    };

    for (std::size_t drawIdx = 0; drawIdx < opaqueCount; ++drawIdx)
    {
        if (!processDraw(content.meshes[drawIdx], false))
            return failToRaster(rhi::RayTracingFallback::ResourceFailure);
    }
    for (const auto& shadowDraw : content.shadowMeshes)
    {
        if (!processDraw(shadowDraw, true))
            return failToRaster(rhi::RayTracingFallback::ResourceFailure);
    }

    if (tlasInstances.empty())
    {
        return failToRaster(rhi::RayTracingFallback::EmptyAccelerationStructure);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasInputs.NumDescs = static_cast<UINT>(tlasInstances.size());
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild{};
    sys.device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);
    if (tlasPrebuild.ResultDataMaxSizeInBytes == 0
        || !EnsureAsBuffer(p, tlasPrebuild.ResultDataMaxSizeInBytes, slot.tlas, slot.tlasCapacity,
            "RT.TLAS", sys.retiringResources, p.nextFenceValue, text))
    {
        return failToRaster(rhi::RayTracingFallback::ResourceFailure);
    }

    const UINT64 tlasScratch = AlignUp((std::max)(UINT64{256}, tlasPrebuild.ScratchDataSizeInBytes),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    slot.tlasScratchOffset = 0;
    slot.requiredScratchBytes = (std::max)(scratchCursor, tlasScratch);
    if (!EnsureScratchBuffer(p, slot.requiredScratchBytes, sys.scratchBuffer, sys.scratchCapacity,
        sys.retiringResources, p.nextFenceValue, text))
    {
        return failToRaster(rhi::RayTracingFallback::ResourceFailure);
    }

    if (!EnsureRayTracingTargets(p, *device.Get(), width, height, sys.targets, text))
    {
        return failToRaster(rhi::RayTracingFallback::ResourceFailure);
    }

    if (!slot.descriptors)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = TotalHeapDescriptors;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!CheckGraphicsResult(device->CreateDescriptorHeap(&heapDesc,
            IID_PPV_ARGS(slot.descriptors.GetAddressOf())), text, "D3D12.DescriptorCapacityExceeded"))
        {
            return failToRaster(rhi::RayTracingFallback::ResourceFailure);
        }
        slot.descriptorIncrement = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    auto candidate = std::make_shared<D3D12RayTracingFrame>();
    candidate->targets = sys.targets;
    candidate->slotIndex = slotIdx;
    candidate->instanceCount = static_cast<UINT>(tlasInstances.size());
    candidate->uniqueMeshCount = static_cast<UINT>(slot.uniqueMeshes.size());
    candidate->allShadowCastersTraced = (stats.unsupportedDraws == 0 && unsupportedCasters == 0);

    const UINT64 tlasDescBytes = tlasInstances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    const UINT64 surfDataBytes = surfaceInstances.size() * sizeof(D3D12RayTracingInstanceData);
    if (!CheckGraphicsResult(frame.uploads.Allocate(tlasDescBytes,
            D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT, candidate->tlasInstancesUpload),
            text, "D3D12.UploadCapacityExceeded")
        || !CheckGraphicsResult(frame.uploads.Allocate(surfDataBytes, 16, candidate->surfaceInstancesUpload),
            text, "D3D12.UploadCapacityExceeded")
        || !CheckGraphicsResult(frame.uploads.Allocate(sizeof(D3D12RayTracingConstants), 256, candidate->constants),
            text, "D3D12.UploadCapacityExceeded"))
    {
        return failToRaster(rhi::RayTracingFallback::ResourceFailure);
    }
    std::memcpy(candidate->tlasInstancesUpload.cpu, tlasInstances.data(), tlasDescBytes);
    std::memcpy(candidate->surfaceInstancesUpload.cpu, surfaceInstances.data(), surfDataBytes);

    const auto invView = flare::Inverse(rt.view);
    const auto invProj = flare::Inverse(rt.clipFromView);
    if (!invView || !invProj)
        return failToRaster(rhi::RayTracingFallback::ResourceFailure);

    const UINT halfW = sys.targets->halfWidth;
    const UINT halfH = sys.targets->halfHeight;

    const bool baseHistoryOk = rt.historyValid && flare::IsFinite(rt.previousClipFromWorld);
    const bool visHistoryValid = baseHistoryOk && (aoActive || shadowsActive)
        && sys.targets->visFrames > 0
        && sys.targets->visLastSubmission == p.submittedFrames
        && sys.targets->visSceneToken == rt.sceneToken;
    const bool indirectHistoryValid = baseHistoryOk && (reflActive || giActive)
        && sys.targets->indirectFrames > 0
        && sys.targets->indirectLastSubmission == p.submittedFrames
        && sys.targets->indirectSceneToken == rt.sceneToken;
    const UINT historyMask = (visHistoryValid ? 1u : 0u) | (indirectHistoryValid ? 2u : 0u);

    D3D12RayTracingConstants cb{};
    cb.viewFromWorld = rt.view;
    cb.clipFromView = rt.clipFromView;
    cb.viewFromClip = *invProj;
    cb.worldFromView = *invView;
    cb.previousClipFromWorld = baseHistoryOk ? rt.previousClipFromWorld : flare::Mat4::Identity();
    cb.viewport = {float(width), float(height), 1.f / float(width), 1.f / float(height)};
    cb.halfViewport = {float(halfW), float(halfH), 1.f / float(halfW), 1.f / float(halfH)};
    const flare::Vec3 sunDir = flare::Normalize(lighting.directionToLight);
    cb.sunDirection = {sunDir.x, sunDir.y, sunDir.z, lighting.directionalEnabled ? 1.f : 0.f};
    cb.sunRadiance = {lighting.directionalRadiance.x, lighting.directionalRadiance.y,
                      lighting.directionalRadiance.z, rt.sunAngularRadius};

    float envIntensity = 0.f, envCos = 1.f, envSin = 0.f, envMaxMip = 0.f;
    if (content.environment && content.environmentTextures[1] && content.environmentTextures[2])
    {
        envIntensity = content.environment->intensity;
        envCos = std::cos(content.environment->yaw);
        envSin = std::sin(content.environment->yaw);
        envMaxMip = float((std::max)(UINT{1}, static_cast<UINT>(content.environmentTextures[1]->resource->GetDesc().MipLevels)) - 1u);
    }
    cb.skyZenith = {lighting.skySettings.zenithColor.x, lighting.skySettings.zenithColor.y,
                    lighting.skySettings.zenithColor.z, envIntensity};
    cb.skyHorizon = {lighting.skySettings.horizonColor.x, lighting.skySettings.horizonColor.y,
                     lighting.skySettings.horizonColor.z, envCos};
    cb.skyGround = {lighting.skySettings.groundColor.x, lighting.skySettings.groundColor.y,
                    lighting.skySettings.groundColor.z, envSin};
    cb.strengths = {rt.shadowStrength, rt.aoStrength, rt.reflectionStrength, rt.giStrength};
    cb.tracingParams = {rt.aoRadius, rt.maxDistance, rt.normalBias, rt.maxRoughness};

    UINT fallbackMode = 0u;
    if (frame.ambientOcclusion && frame.ambientOcclusion->statistics.valid)
    {
        if (!aoActive && frame.ambientOcclusion->statistics.aoValid) fallbackMode |= 1u;
        if (!shadowsActive && frame.ambientOcclusion->statistics.contactValid) fallbackMode |= 2u;
    }

    cb.temporalParams = {rt.historyBlendWeight, float(historyMask), envMaxMip, float(fallbackMode)};
    const std::uint32_t ptCount = (std::min)(lighting.pointCount, 8u);
    cb.flags = {rt.frameIndex, shadowsActive ? 1u : 0u, aoActive ? 1u : 0u, ptCount};
    const std::uint32_t localCount = static_cast<std::uint32_t>((std::min)(lighting.localLights.size(), std::size_t{8}));
    cb.indirectFlags = {reflActive ? 1u : 0u, giActive ? 1u : 0u, candidate->instanceCount, localCount};

    for (std::uint32_t i = 0; i < ptCount; ++i)
    {
        const auto& pt = lighting.points[i];
        cb.pointPosRange[i] = {pt.position.x, pt.position.y, pt.position.z, pt.range};
        cb.pointRadiance[i] = {pt.radiance.x, pt.radiance.y, pt.radiance.z, 0.f};
    }
    for (std::uint32_t i = 0; i < localCount; ++i)
    {
        const auto& loc = lighting.localLights[i];
        cb.localPosRange[i] = {loc.position.x, loc.position.y, loc.position.z, loc.range};
        cb.localRadiance[i] = {loc.radiance.x, loc.radiance.y, loc.radiance.z, loc.spot ? 1.f : 0.f};
        cb.localDirection[i] = {loc.direction.x, loc.direction.y, loc.direction.z, loc.innerCos};
        cb.localCone[i] = {loc.outerCos, 0.f, 0.f, 0.f};
    }
    std::memcpy(candidate->constants.cpu, &cb, sizeof(cb));

    candidate->valid = true;
    candidate->aoValid = aoActive || ((fallbackMode & 1u) != 0u);
    candidate->shadowsValid = shadowsActive || ((fallbackMode & 2u) != 0u);
    candidate->visibilityValid = aoActive || shadowsActive;
    candidate->reflectionsValid = reflActive;
    candidate->giValid = giActive;
    candidate->indirectValid = reflActive || giActive;

    UINT64 totalBlasBytes = 0;
    for (const auto& [_, entry] : sys.blasCache) totalBlasBytes += entry.bytes;
    stats.valid = true;
    stats.aoValid = aoActive;
    stats.shadowsValid = shadowsActive;
    stats.reflectionsValid = reflActive;
    stats.giValid = giActive;
    stats.blasCount = static_cast<std::uint32_t>(sys.blasCache.size());
    stats.tlasInstances = candidate->instanceCount;
    stats.uniqueMeshes = candidate->uniqueMeshCount;
    stats.accelerationBytes = totalBlasBytes + slot.tlasCapacity;
    stats.scratchBytes = sys.scratchCapacity;
    stats.denoiserBytes = sys.targets->allocatedBytes;

    candidate->statistics = stats;
    p.rayTracingStatistics = stats;
    frame.rayTracing = std::move(candidate);
    return true;
}

namespace
{
void WriteCommonSceneSrvs(ID3D12Device& device, const D3D12FrameContent& content,
    const D3D12DepthNormalTarget& depthTarget, ID3D12Resource* sourceHdr,
    D3D12_CPU_DESCRIPTOR_HANDLE passCpuBase, UINT increment)
{
    auto cpuAt = [&](UINT offset) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = passCpuBase;
        h.ptr += SIZE_T(offset) * increment;
        return h;
    };
    auto srv2d = [&](UINT offset, ID3D12Resource* res, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(res, &d, cpuAt(offset));
    };

    // t2=depth, t3=normal, t4=material, t5=sourceHdr, t6=envCube, t7=envBrdf
    device.CopyDescriptorsSimple(1, cpuAt(0), depthTarget.views->Cpu(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device.CopyDescriptorsSimple(1, cpuAt(1), depthTarget.views->Cpu(1), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (depthTarget.material)
        device.CopyDescriptorsSimple(1, cpuAt(2), depthTarget.views->Cpu(2), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    else
        srv2d(2, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);

    srv2d(3, sourceHdr, DXGI_FORMAT_R16G16B16A16_FLOAT);

    if (content.environment && content.environmentTextures[1] && content.environmentTextures[2])
    {
        device.CopyDescriptorsSimple(1, cpuAt(4), content.environmentTextures[1]->views->Cpu(1),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device.CopyDescriptorsSimple(1, cpuAt(5), content.environmentTextures[2]->views->Cpu(1),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC cubeNull{};
        cubeNull.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cubeNull.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeNull.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeNull.TextureCube.MipLevels = 1;
        device.CreateShaderResourceView(nullptr, &cubeNull, cpuAt(4));
        srv2d(5, nullptr, DXGI_FORMAT_R16G16_FLOAT);
    }
}

void ClearPassExtraSlots(ID3D12Device& device, D3D12_CPU_DESCRIPTOR_HANDLE passCpuBase, UINT increment, bool includeUavs)
{
    auto cpuAt = [&](UINT offset) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = passCpuBase;
        h.ptr += SIZE_T(offset) * increment;
        return h;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    for (UINT i = 6; i < PassSrvCount; ++i)
        device.CreateShaderResourceView(nullptr, &nullSrv, cpuAt(i));

    if (includeUavs)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav4{};
        nullUav4.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        nullUav4.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT u = 0; u < 3; ++u)
            device.CreateUnorderedAccessView(nullptr, nullptr, &nullUav4, cpuAt(PassSrvCount + u));

        D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav2{};
        nullUav2.Format = DXGI_FORMAT_R16G16_FLOAT;
        nullUav2.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device.CreateUnorderedAccessView(nullptr, nullptr, &nullUav2, cpuAt(PassSrvCount + 3));
    }
}
} // namespace

bool RecordD3D12RayTracingPreOpaque(D3D12Presentation& p, D3D12FrameContext& frame,
    const D3D12FrameContent& content, D3D12StateRecording& states, const Localization& text)
{
    if (!frame.rayTracing || !frame.rayTracing->valid || !p.rayTracing) return true;
    auto& sys = *p.rayTracing;
    auto& rtFrame = *frame.rayTracing;
    auto& slot = sys.slots[rtFrame.slotIndex];
    auto& targets = *rtFrame.targets;
    auto& depthTarget = *frame.depthNormals->target;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    if (FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))
        || FAILED(p.commandList.As(&cmd4)) || !cmd4)
    {
        return false;
    }
    auto& commands = *p.commandList.Get();
    const auto use = [&](ID3D12Resource& res, D3D12_RESOURCE_STATES desired, const char* pass) {
        return CheckGraphicsResult(states.Use(commands, res, desired, pass), text, "D3D12.StateTrackingFailed");
    };
    const auto uavBarrier = [&](ID3D12Resource& res, const char* pass) {
        return CheckGraphicsResult(states.UavBarrier(commands, res, pass), text, "D3D12.StateTrackingFailed");
    };

    // 1. 确保几何缓冲与纹理同时具备光栅输入与非像素着色器读取状态。
    for (const auto& m : slot.uniqueMeshes)
    {
        if (!use(*m.vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.MeshVB")
            || !use(*m.ib, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.MeshIB"))
        {
            return false;
        }
        if (m.albedoTex
            && !use(*m.albedoTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.MeshAlbedo"))
        {
            return false;
        }
    }
    for (const auto& build : slot.pendingBlasBuilds)
    {
        if (!use(*build.entry->vbKeepAlive.Get(),
                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.BlasVB")
            || !use(*build.entry->ibKeepAlive.Get(),
                    D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.BlasIB"))
        {
            return false;
        }
    }

    if (!use(*sys.scratchBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.Scratch")
        || !use(*rtFrame.tlasInstancesUpload.resource, D3D12_RESOURCE_STATE_GENERIC_READ, "RT.InstanceDescs")
        || !use(*rtFrame.surfaceInstancesUpload.resource, D3D12_RESOURCE_STATE_GENERIC_READ, "RT.SurfaceInstances")
        || !use(*rtFrame.constants.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "RT.Constants"))
    {
        return false;
    }

    // 2. 并行构建所有新增静态网格 BLAS，随后通过单次统一屏障衔接 TLAS 构建。
    if (!slot.pendingBlasBuilds.empty())
    {
        for (const auto& build : slot.pendingBlasBuilds)
        {
            if (!use(*build.entry->blas.Get(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "RT.BlasBuild"))
                return false;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc{};
            blasDesc.DestAccelerationStructureData = build.entry->blas->GetGPUVirtualAddress();
            blasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            blasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            blasDesc.Inputs.NumDescs = 1;
            blasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            blasDesc.Inputs.pGeometryDescs = &build.geomDesc;
            blasDesc.ScratchAccelerationStructureData = sys.scratchBuffer->GetGPUVirtualAddress() + build.scratchOffset;
            cmd4->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);
        }
        if (!uavBarrier(*sys.scratchBuffer.Get(), "RT.BlasScratchSync")) return false;
        for (const auto& build : slot.pendingBlasBuilds)
            if (!uavBarrier(*build.entry->blas.Get(), "RT.BlasReady")) return false;
    }
    const auto commitBlasBuilds = [&]() {
        for (const auto& build : slot.pendingBlasBuilds)
            build.entry->built = true;
    };

    // 3. 构建当前帧 TLAS。
    if (!use(*slot.tlas.Get(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "RT.TlasBuild"))
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc{};
    tlasDesc.DestAccelerationStructureData = slot.tlas->GetGPUVirtualAddress();
    tlasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasDesc.Inputs.NumDescs = rtFrame.instanceCount;
    tlasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasDesc.Inputs.InstanceDescs = rtFrame.tlasInstancesUpload.Gpu();
    tlasDesc.ScratchAccelerationStructureData = sys.scratchBuffer->GetGPUVirtualAddress() + slot.tlasScratchOffset;
    cmd4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
    if (!uavBarrier(*slot.tlas.Get(), "RT.TlasReady")
        || !uavBarrier(*sys.scratchBuffer.Get(), "RT.TlasScratchSync"))
    {
        return false;
    }

    // 4. 填充 64 槽网格 VB / IB / Albedo 表（未用槽写入类型匹配的安全空描述符）。
    const UINT inc = slot.descriptorIncrement;
    const auto heapCpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto heapGpu = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    auto cpuSlot = [&](UINT idx) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heapCpu;
        h.ptr += SIZE_T(idx) * inc;
        return h;
    };
    auto gpuSlot = [&](UINT idx) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heapGpu;
        h.ptr += UINT64(idx) * inc;
        return h;
    };

    for (UINT i = 0; i < MaxUniqueRtMeshes; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC vbSrv{};
        vbSrv.Format = DXGI_FORMAT_R32_TYPELESS;
        vbSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        vbSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vbSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

        D3D12_SHADER_RESOURCE_VIEW_DESC ibSrv{};
        ibSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ibSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (i < slot.uniqueMeshes.size())
        {
            const auto& m = slot.uniqueMeshes[i];
            vbSrv.Buffer.FirstElement = m.vbFirstWord;
            vbSrv.Buffer.NumElements = (std::max)(1u, m.vbWords);
            device->CreateShaderResourceView(m.vb, &vbSrv, cpuSlot(i));

            ibSrv.Format = m.indexFormat;
            ibSrv.Buffer.FirstElement = m.ibFirstIndex;
            ibSrv.Buffer.NumElements = (std::max)(1u, m.indexCount);
            device->CreateShaderResourceView(m.ib, &ibSrv, cpuSlot(MaxUniqueRtMeshes + i));

            if (m.albedoSrv.ptr != 0)
            {
                device->CopyDescriptorsSimple(1, cpuSlot(MaxUniqueRtMeshes * 2 + i), m.albedoSrv,
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            else
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC nullTex{};
                nullTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                nullTex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nullTex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nullTex.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(nullptr, &nullTex, cpuSlot(MaxUniqueRtMeshes * 2 + i));
            }
        }
        else
        {
            vbSrv.Buffer.NumElements = 1;
            device->CreateShaderResourceView(nullptr, &vbSrv, cpuSlot(i));
            ibSrv.Format = DXGI_FORMAT_R32_UINT;
            ibSrv.Buffer.NumElements = 1;
            device->CreateShaderResourceView(nullptr, &ibSrv, cpuSlot(MaxUniqueRtMeshes + i));

            D3D12_SHADER_RESOURCE_VIEW_DESC nullTex{};
            nullTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullTex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullTex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullTex.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &nullTex, cpuSlot(MaxUniqueRtMeshes * 2 + i));
        }
    }

    if (!rtFrame.visibilityValid)
    {
        commitBlasBuilds();
        return true;
    }

    // 5. 阶段 1：可见性光追（RTAO + 光追方向光软阴影）-> 时域累积 -> 3 趟 À-Trous -> RG16F 可见性输出。
    if (!use(*depthTarget.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisDepth")
        || !use(*depthTarget.normal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisNormal"))
    {
        return false;
    }
    if (depthTarget.material
        && !use(*depthTarget.material.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisMaterial"))
    {
        return false;
    }
    if (content.environment)
    {
        for (UINT e = 1; e <= 2; ++e)
        {
            if (content.environmentTextures[e]
                && !use(*content.environmentTextures[e]->resource.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        "RT.VisEnv"))
            {
                return false;
            }
        }
    }

    const UINT prevVis = targets.visHistoryIndex;
    const UINT currVis = 1u - prevVis;
    const UINT fullW = targets.width;
    const UINT fullH = targets.height;

    auto passBase = [&](UINT passIdx) { return MeshTableCount + passIdx * PassStride; };
    auto setSrv2d = [&](UINT absSlot, ID3D12Resource* res, DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(res, &d, cpuSlot(absSlot));
    };
    auto setUav2d = [&](UINT absSlot, ID3D12Resource* res, DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = fmt;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(res, nullptr, &u, cpuSlot(absSlot));
    };

    for (UINT passIdx = 0; passIdx < 5; ++passIdx)
    {
        const UINT base = passBase(passIdx);
        WriteCommonSceneSrvs(*device.Get(), content, depthTarget, nullptr, cpuSlot(base), inc);
        ClearPassExtraSlots(*device.Get(), cpuSlot(base), inc, true);
    }

    // Pass 0: CSTraceVisibility
    {
        const UINT b = passBase(0);
        if (frame.ambientOcclusion && frame.ambientOcclusion->statistics.valid
            && frame.ambientOcclusion->output && frame.ambientOcclusion->output->visibility)
        {
            if (!use(*frame.ambientOcclusion->output->visibility.Get(),
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     "RT.FallbackScreenVis"))
            {
                return false;
            }
            setSrv2d(b + 9, frame.ambientOcclusion->output->visibility.Get(), DXGI_FORMAT_R16G16_FLOAT);
        }
        setUav2d(b + PassSrvCount + 0, targets.visRaw.Get());
    }
    // Pass 1: CSTemporalVisibility
    {
        const UINT b = passBase(1);
        setSrv2d(b + 6, targets.visRaw.Get());
        setSrv2d(b + 7, targets.visHistory[prevVis].Get());
        setSrv2d(b + 8, targets.visMoments[prevVis].Get());
        setUav2d(b + PassSrvCount + 0, targets.visHistory[currVis].Get());
        setUav2d(b + PassSrvCount + 1, targets.visMoments[currVis].Get());
    }
    // Pass 2..4: CSATrousVisibility (steps 1, 2, 4)
    {
        const UINT b2 = passBase(2);
        setSrv2d(b2 + 6, targets.visHistory[currVis].Get());
        setSrv2d(b2 + 7, targets.visMoments[currVis].Get());
        setUav2d(b2 + PassSrvCount + 0, targets.visScratch.Get());

        const UINT b3 = passBase(3);
        setSrv2d(b3 + 6, targets.visScratch.Get());
        setSrv2d(b3 + 7, targets.visMoments[currVis].Get());
        setUav2d(b3 + PassSrvCount + 0, targets.visRaw.Get());

        const UINT b4 = passBase(4);
        setSrv2d(b4 + 6, targets.visRaw.Get());
        setSrv2d(b4 + 7, targets.visMoments[currVis].Get());
        setUav2d(b4 + PassSrvCount + 0, targets.visScratch.Get());
        setUav2d(b4 + PassSrvCount + 3, targets.visibility.Get(), DXGI_FORMAT_R16G16_FLOAT);
    }

    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get()};
    commands.SetDescriptorHeaps(1, heaps);
    commands.SetComputeRootSignature(sys.computeRoot.Get());
    commands.SetComputeRootShaderResourceView(0, slot.tlas->GetGPUVirtualAddress());
    commands.SetComputeRootShaderResourceView(1, rtFrame.surfaceInstancesUpload.Gpu());
    commands.SetComputeRootConstantBufferView(2, rtFrame.constants.Gpu());
    commands.SetComputeRootDescriptorTable(6, gpuSlot(0));

    const UINT gx = (fullW + 7u) / 8u;
    const UINT gy = (fullH + 7u) / 8u;
    const auto dispatchPass = [&](ComputeStage stage, UINT passIdx, UINT stepSize, UINT passSubIdx) {
        const UINT base = passBase(passIdx);
        const UINT params[4]{stepSize, fullW, fullH, passSubIdx};
        commands.SetComputeRoot32BitConstants(3, 4, params, 0);
        commands.SetComputeRootDescriptorTable(4, gpuSlot(base));
        commands.SetComputeRootDescriptorTable(5, gpuSlot(base + PassSrvCount));
        commands.SetPipelineState(sys.computePipelines[stage].Get());
        commands.Dispatch(gx, gy, 1);
    };

    if (!use(*targets.visRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.TraceVisWrite")) return false;
    dispatchPass(StageTraceVis, 0, 1, 0);
    if (!uavBarrier(*targets.visRaw.Get(), "RT.TraceVisSync")
        || !use(*targets.visRaw.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.TraceVisRead"))
    {
        return false;
    }

    if (!use(*targets.visHistory[prevVis].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisHistRead")
        || !use(*targets.visMoments[prevVis].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisMomRead")
        || !use(*targets.visHistory[currVis].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.VisHistWrite")
        || !use(*targets.visMoments[currVis].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.VisMomWrite"))
    {
        return false;
    }
    dispatchPass(StageTemporalVis, 1, 1, 0);
    if (!uavBarrier(*targets.visHistory[currVis].Get(), "RT.VisHistSync")
        || !uavBarrier(*targets.visMoments[currVis].Get(), "RT.VisMomSync")
        || !use(*targets.visHistory[currVis].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisHistCurrRead")
        || !use(*targets.visMoments[currVis].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisMomCurrRead"))
    {
        return false;
    }

    if (!use(*targets.visScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.VisATrous0Write")) return false;
    dispatchPass(StageATrousVis, 2, 1, 0);
    if (!uavBarrier(*targets.visScratch.Get(), "RT.VisATrous0Sync")
        || !use(*targets.visScratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisATrous0Read")
        || !use(*targets.visRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.VisATrous1Write"))
    {
        return false;
    }
    dispatchPass(StageATrousVis, 3, 2, 1);
    if (!uavBarrier(*targets.visRaw.Get(), "RT.VisATrous1Sync")
        || !use(*targets.visRaw.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.VisATrous1Read")
        || !use(*targets.visScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.VisATrous2Scratch")
        || !use(*targets.visibility.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.VisFinalWrite"))
    {
        return false;
    }
    dispatchPass(StageATrousVis, 4, 4, 2);
    if (!uavBarrier(*targets.visibility.Get(), "RT.VisFinalSync")
        || !use(*targets.visibility.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "RT.VisGraphicsRead"))
    {
        return false;
    }

    commitBlasBuilds();
    targets.visHistoryIndex = currVis;
    ++targets.visFrames;
    targets.visLastSubmission = p.submittedFrames + 1;
    targets.visSceneToken = content.forward->rayTracing.sceneToken;
    return true;
}

bool RecordD3D12RayTracingPostOpaque(D3D12Presentation& p, D3D12FrameContext& frame,
    const D3D12FrameContent& content, D3D12StateRecording& states,
    ID3D12Resource& rasterColor, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    UINT width, UINT height, const Localization& text)
{
    if (!frame.rayTracing || !frame.rayTracing->valid || !frame.rayTracing->indirectValid || !p.rayTracing)
        return true;

    auto& sys = *p.rayTracing;
    auto& rtFrame = *frame.rayTracing;
    auto& slot = sys.slots[rtFrame.slotIndex];
    auto& targets = *rtFrame.targets;
    auto& depthTarget = *frame.depthNormals->target;
    if (targets.width != width || targets.height != height || !depthTarget.material)
        return true;

    ComPtr<ID3D12Device> device;
    if (FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))) return false;

    auto& commands = *p.commandList.Get();
    const auto use = [&](ID3D12Resource& res, D3D12_RESOURCE_STATES desired, const char* pass) {
        return CheckGraphicsResult(states.Use(commands, res, desired, pass), text, "D3D12.StateTrackingFailed");
    };
    const auto uavBarrier = [&](ID3D12Resource& res, const char* pass) {
        return CheckGraphicsResult(states.UavBarrier(commands, res, pass), text, "D3D12.StateTrackingFailed");
    };

    // 1. 拷贝已着色的不透明 HDR 颜色快照，避免读写同源并供屏内命中直接复用多光源着色。
    if (!use(rasterColor, D3D12_RESOURCE_STATE_COPY_SOURCE, "RT.CopySourceHDR")
        || !use(*targets.sourceHdr.Get(), D3D12_RESOURCE_STATE_COPY_DEST, "RT.CopySourceHDR"))
    {
        return false;
    }
    commands.CopyResource(targets.sourceHdr.Get(), &rasterColor);

    if (!use(*targets.sourceHdr.Get(),
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             "RT.SourceHdrRead")
        || !use(*depthTarget.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndDepth")
        || !use(*depthTarget.normal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndNormal")
        || !use(*depthTarget.material.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndMaterial")
        || !use(*rtFrame.surfaceInstancesUpload.resource, D3D12_RESOURCE_STATE_GENERIC_READ, "RT.IndSurface")
        || !use(*rtFrame.constants.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "RT.IndConstants"))
    {
        return false;
    }
    for (const auto& m : slot.uniqueMeshes)
    {
        if (!use(*m.vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndMeshVB")
            || !use(*m.ib, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndMeshIB"))
        {
            return false;
        }
        if (m.albedoTex
            && !use(*m.albedoTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndMeshAlbedo"))
        {
            return false;
        }
    }
    if (content.environment)
    {
        for (UINT e = 1; e <= 2; ++e)
        {
            if (content.environmentTextures[e]
                && !use(*content.environmentTextures[e]->resource.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        "RT.IndEnv"))
            {
                return false;
            }
        }
    }

    const UINT inc = slot.descriptorIncrement;
    const auto heapCpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto heapGpu = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    auto cpuSlot = [&](UINT idx) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heapCpu;
        h.ptr += SIZE_T(idx) * inc;
        return h;
    };
    auto gpuSlot = [&](UINT idx) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heapGpu;
        h.ptr += UINT64(idx) * inc;
        return h;
    };
    auto passBase = [&](UINT passIdx) { return MeshTableCount + passIdx * PassStride; };
    auto setSrv2d = [&](UINT absSlot, ID3D12Resource* res) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(res, &d, cpuSlot(absSlot));
    };
    auto setUav2d = [&](UINT absSlot, ID3D12Resource* res) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(res, nullptr, &u, cpuSlot(absSlot));
    };

    const UINT prevInd = targets.indirectHistoryIndex;
    const UINT currInd = 1u - prevInd;
    const UINT halfW = targets.halfWidth;
    const UINT halfH = targets.halfHeight;

    for (UINT passIdx = 5; passIdx < 10; ++passIdx)
    {
        const UINT base = passBase(passIdx);
        WriteCommonSceneSrvs(*device.Get(), content, depthTarget, targets.sourceHdr.Get(), cpuSlot(base), inc);
        ClearPassExtraSlots(*device.Get(), cpuSlot(base), inc, true);
    }

    // Pass 5: CSTraceIndirect
    {
        const UINT b = passBase(5);
        setUav2d(b + PassSrvCount + 0, targets.specRaw.Get());
        setUav2d(b + PassSrvCount + 1, targets.giRaw.Get());
    }
    // Pass 6: CSTemporalIndirect
    {
        const UINT b = passBase(6);
        setSrv2d(b + 6, targets.specRaw.Get());
        setSrv2d(b + 7, targets.giRaw.Get());
        setSrv2d(b + 8, targets.specHistory[prevInd].Get());
        setSrv2d(b + 9, targets.giHistory[prevInd].Get());
        setUav2d(b + PassSrvCount + 0, targets.specHistory[currInd].Get());
        setUav2d(b + PassSrvCount + 1, targets.giHistory[currInd].Get());
        setUav2d(b + PassSrvCount + 2, targets.indirectMoments.Get());
    }
    // Pass 7..9: CSATrousIndirect (steps 1, 2, 4)
    {
        const UINT b7 = passBase(7);
        setSrv2d(b7 + 6, targets.specHistory[currInd].Get());
        setSrv2d(b7 + 7, targets.giHistory[currInd].Get());
        setSrv2d(b7 + 8, targets.indirectMoments.Get());
        setUav2d(b7 + PassSrvCount + 0, targets.specPing[0].Get());
        setUav2d(b7 + PassSrvCount + 1, targets.giPing[0].Get());

        const UINT b8 = passBase(8);
        setSrv2d(b8 + 6, targets.specPing[0].Get());
        setSrv2d(b8 + 7, targets.giPing[0].Get());
        setSrv2d(b8 + 8, targets.indirectMoments.Get());
        setUav2d(b8 + PassSrvCount + 0, targets.specPing[1].Get());
        setUav2d(b8 + PassSrvCount + 1, targets.giPing[1].Get());

        const UINT b9 = passBase(9);
        setSrv2d(b9 + 6, targets.specPing[1].Get());
        setSrv2d(b9 + 7, targets.giPing[1].Get());
        setSrv2d(b9 + 8, targets.indirectMoments.Get());
        setUav2d(b9 + PassSrvCount + 0, targets.specPing[0].Get());
        setUav2d(b9 + PassSrvCount + 1, targets.giPing[0].Get());
    }
    // Composite SRV table
    {
        WriteCommonSceneSrvs(*device.Get(), content, depthTarget, targets.sourceHdr.Get(),
                             cpuSlot(CompositeSrvBase), inc);
        ClearPassExtraSlots(*device.Get(), cpuSlot(CompositeSrvBase), inc, false);
        setSrv2d(CompositeSrvBase + 6, targets.specPing[0].Get());
        setSrv2d(CompositeSrvBase + 7, targets.giPing[0].Get());
    }

    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get()};
    commands.SetDescriptorHeaps(1, heaps);
    commands.SetComputeRootSignature(sys.computeRoot.Get());
    commands.SetComputeRootShaderResourceView(0, slot.tlas->GetGPUVirtualAddress());
    commands.SetComputeRootShaderResourceView(1, rtFrame.surfaceInstancesUpload.Gpu());
    commands.SetComputeRootConstantBufferView(2, rtFrame.constants.Gpu());
    commands.SetComputeRootDescriptorTable(6, gpuSlot(0));

    const UINT gx = (halfW + 7u) / 8u;
    const UINT gy = (halfH + 7u) / 8u;
    const auto dispatchPass = [&](ComputeStage stage, UINT passIdx, UINT stepSize, UINT passSubIdx) {
        const UINT base = passBase(passIdx);
        const UINT params[4]{stepSize, halfW, halfH, passSubIdx};
        commands.SetComputeRoot32BitConstants(3, 4, params, 0);
        commands.SetComputeRootDescriptorTable(4, gpuSlot(base));
        commands.SetComputeRootDescriptorTable(5, gpuSlot(base + PassSrvCount));
        commands.SetPipelineState(sys.computePipelines[stage].Get());
        commands.Dispatch(gx, gy, 1);
    };

    if (!use(*targets.specRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.SpecRawWrite")
        || !use(*targets.giRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.GiRawWrite"))
    {
        return false;
    }
    dispatchPass(StageTraceIndirect, 5, 1, 0);
    if (!uavBarrier(*targets.specRaw.Get(), "RT.SpecRawSync")
        || !uavBarrier(*targets.giRaw.Get(), "RT.GiRawSync")
        || !use(*targets.specRaw.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.SpecRawRead")
        || !use(*targets.giRaw.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.GiRawRead"))
    {
        return false;
    }

    if (!use(*targets.specHistory[prevInd].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.SpecHistRead")
        || !use(*targets.giHistory[prevInd].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.GiHistRead")
        || !use(*targets.specHistory[currInd].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.SpecHistWrite")
        || !use(*targets.giHistory[currInd].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.GiHistWrite")
        || !use(*targets.indirectMoments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.IndMomWrite"))
    {
        return false;
    }
    dispatchPass(StageTemporalIndirect, 6, 1, 0);
    if (!uavBarrier(*targets.specHistory[currInd].Get(), "RT.SpecHistSync")
        || !uavBarrier(*targets.giHistory[currInd].Get(), "RT.GiHistSync")
        || !uavBarrier(*targets.indirectMoments.Get(), "RT.IndMomSync")
        || !use(*targets.specHistory[currInd].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.SpecHistCurrRead")
        || !use(*targets.giHistory[currInd].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.GiHistCurrRead")
        || !use(*targets.indirectMoments.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "RT.IndMomRead"))
    {
        return false;
    }

    const UINT steps[3]{1u, 2u, 4u};
    for (UINT pass = 0; pass < 3; ++pass)
    {
        ID3D12Resource& outSpec = *targets.specPing[pass % 2u].Get();
        ID3D12Resource& outGi = *targets.giPing[pass % 2u].Get();
        if (!use(outSpec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.SpecATrousWrite")
            || !use(outGi, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "RT.GiATrousWrite"))
        {
            return false;
        }
        dispatchPass(StageATrousIndirect, 7u + pass, steps[pass], pass);
        if (!uavBarrier(outSpec, "RT.SpecATrousSync") || !uavBarrier(outGi, "RT.GiATrousSync"))
            return false;
        const auto nextState = (pass == 2u)
            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (!use(outSpec, nextState, "RT.SpecATrousRead") || !use(outGi, nextState, "RT.GiATrousRead"))
            return false;
    }

    // 2. 全屏双边升采样与物理能量守恒 HDR 合成。
    if (!use(*depthTarget.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "RT.CompDepth")
        || !use(*depthTarget.normal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "RT.CompNormal")
        || !use(*depthTarget.material.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "RT.CompMaterial")
        || !use(rasterColor, D3D12_RESOURCE_STATE_RENDER_TARGET, "RT.CompHDR"))
    {
        return false;
    }

    commands.SetGraphicsRootSignature(sys.compositeRoot.Get());
    commands.SetDescriptorHeaps(1, heaps);
    commands.SetGraphicsRootConstantBufferView(0, rtFrame.constants.Gpu());
    commands.SetGraphicsRootDescriptorTable(1, gpuSlot(CompositeSrvBase));
    commands.SetPipelineState(sys.compositePipeline.Get());
    const D3D12_VIEWPORT vp{0.f, 0.f, float(width), float(height), 0.f, 1.f};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    commands.RSSetViewports(1, &vp);
    commands.RSSetScissorRects(1, &scissor);
    commands.OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands.DrawInstanced(3, 1, 0, 0);

    targets.indirectHistoryIndex = currInd;
    ++targets.indirectFrames;
    targets.indirectLastSubmission = p.submittedFrames + 1;
    targets.indirectSceneToken = content.forward->rayTracing.sceneToken;
    return true;
}
