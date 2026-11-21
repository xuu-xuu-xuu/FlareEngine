#include "D3D12Meshlet.h"
#include "D3D12Context.h"
#include "D3D12HiZ.h"
#include "D3D12PipelineCache.h"
#include "D3D12ShaderProgram.h"
#include "D3D12StateTracker.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <dxcapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace rhi = se::rhi;

namespace
{
using Json = nlohmann::json;

constexpr char MeshletArtifactMagic[8]{'S', 'E', 'M', 'S', 'H', '1', '\0', '\0'};
constexpr UINT MeshletMaxTrianglesPerItem = 64;
constexpr UINT MaxGpuMeshletsPerBatch = 262144;
constexpr UINT MaxAsGroupsPerDispatch = 65535;
constexpr std::size_t MaxShaderFileBytes = 16 * 1024 * 1024;

void AppendU32(std::vector<std::uint8_t>& out, UINT value)
{
    for (UINT shift = 0; shift < 32; shift += 8)
    {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

UINT ReadU32(const std::uint8_t* bytes)
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

Json HashShaderFiles(const fs::path& shaderDirectory, std::span<const char* const> files)
{
    Json hashes = Json::object();
    for (const char* name : files)
    {
        std::vector<std::uint8_t> bytes;
        if (!ReadBytes(shaderDirectory / name, bytes)) return {};
        const auto digest = se::Sha256(bytes);
        if (digest.empty()) return {};
        hashes[name] = digest;
    }
    return hashes;
}

Json MeshletAsIdentity(const fs::path& shaderDirectory)
{
    static constexpr const char* kFiles[] = {
        "MeshletCull.as.hlsl",
        "MeshletBindings.hlsli",
        "VisibilityPredicate.hlsli"
    };
    const auto hashes = HashShaderFiles(shaderDirectory, kFiles);
    if (hashes.empty()) return {};
    return {
        {"schema", 1},
        {"kind", "meshlet-as"},
        {"layout", rhi::MeshletLayoutVersion},
        {"files", hashes}
    };
}

Json MeshletMsIdentity(const fs::path& shaderDirectory)
{
    static constexpr const char* kFiles[] = {
        "MeshletDraw.ms.hlsl",
        "MeshletBindings.hlsli",
        "Pbr.hlsl",
        "Forward.hlsl",
        "ShadowedForward.hlsl",
        "EnvironmentForward.hlsl",
        "BasicLit.hlsl",
        "PbrBindings.hlsli",
        "ForwardBindings.hlsli",
        "ShadowBindings.hlsli",
        "EnvironmentBindings.hlsli",
        "GeometryCoverage.hlsli"
    };
    const auto hashes = HashShaderFiles(shaderDirectory, kFiles);
    if (hashes.empty()) return {};
    return {
        {"schema", 1},
        {"kind", "meshlet-ms"},
        {"layout", rhi::MeshletLayoutVersion},
        {"files", hashes}
    };
}

bool HasSignedDxilContainer(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() < 24) return false;
    if (bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' || bytes[3] != 'C') return false;
    bool nonZeroHash = false;
    for (std::size_t i = 4; i < 20; ++i) nonZeroHash |= (bytes[i] != 0);
    return nonZeroHash;
}

std::tuple<unsigned, unsigned, unsigned, unsigned> ParseSdkVersion(const std::wstring& name)
{
    std::array<unsigned, 4> parts{};
    std::size_t part = 0;
    unsigned current = 0;
    for (const wchar_t c : name)
    {
        if (c >= L'0' && c <= L'9')
        {
            current = current * 10u + static_cast<unsigned>(c - L'0');
        }
        else if (c == L'.')
        {
            if (part < 4) parts[part++] = current;
            current = 0;
        }
        else
        {
            return {};
        }
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
        const fs::path candidate = fs::path(pathBuffer.data()).parent_path();
        if (fs::exists(candidate / "dxil.dll")) return candidate;
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

struct DxcSession
{
    HMODULE dxilModule = nullptr;
    HMODULE compilerModule = nullptr;
    DxcCreateInstanceProc createInstance = nullptr;
    DxcCreateInstanceProc dxilCreateInstance = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includes;

    ~DxcSession()
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
        createInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compilerModule, "DxcCreateInstance"));
        dxilCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxilModule, "DxcCreateInstance"));
        if (!createInstance)
        {
            Log(LogLevel::Error, text.GetText("Shader.DxcUnavailable"));
            return false;
        }
        if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.GetAddressOf())))
            || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())))
            || FAILED(utils->CreateDefaultIncludeHandler(includes.GetAddressOf())))
        {
            Log(LogLevel::Error, text.GetText("Shader.DxcUnavailable"));
            return false;
        }
        return true;
    }

    bool Compile(const fs::path& shaderPath, const fs::path& includeDir, const wchar_t* entry, const wchar_t* profile,
        const wchar_t* defineMacro, std::vector<std::uint8_t>& output, const Localization& text) const
    {
        std::vector<std::uint8_t> sourceBytes;
        if (!ReadBytes(shaderPath, sourceBytes))
        {
            Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + PathUtf8(shaderPath));
            return false;
        }
        const std::wstring nameW = shaderPath.filename().wstring();
        const std::wstring includeW = includeDir.wstring();
        std::vector<const wchar_t*> args{
            nameW.c_str(),
            L"-E", entry,
            L"-T", profile,
            L"-HV", L"2018",
            L"-O3",
            L"-Ges",
            L"-Gis",
            L"-I", includeW.c_str()
        };
        if (defineMacro)
        {
            args.push_back(L"-D");
            args.push_back(defineMacro);
        }

        DxcBuffer sourceBuffer{sourceBytes.data(), sourceBytes.size(), DXC_CP_UTF8};
        ComPtr<IDxcResult> compileResult;
        HRESULT hr = compiler->Compile(&sourceBuffer, args.data(), static_cast<UINT32>(args.size()),
            includes.Get(), IID_PPV_ARGS(compileResult.GetAddressOf()));
        if (FAILED(hr) || !compileResult)
        {
            Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + PathUtf8(shaderPath));
            return false;
        }

        ComPtr<IDxcBlobUtf8> errors;
        if (SUCCEEDED(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr))
            && errors && errors->GetStringLength() > 0)
        {
            const std::string errText(errors->GetStringPointer(), errors->GetStringLength());
            if (!errText.empty()) Log(LogLevel::Warning, errText);
        }

        HRESULT status = E_FAIL;
        if (FAILED(compileResult->GetStatus(&status)) || FAILED(status))
        {
            Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + PathUtf8(shaderPath));
            return false;
        }

        ComPtr<IDxcBlob> object;
        if (FAILED(compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.GetAddressOf()), nullptr))
            || !object || !object->GetBufferSize())
        {
            Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + PathUtf8(shaderPath));
            return false;
        }

        const auto* ptr = static_cast<const std::uint8_t*>(object->GetBufferPointer());
        output.assign(ptr, ptr + object->GetBufferSize());
        if (!HasSignedDxilContainer(output) && dxilCreateInstance)
        {
            ComPtr<IDxcValidator> validator;
            if (SUCCEEDED(dxilCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(validator.GetAddressOf()))))
            {
                ComPtr<IDxcBlobEncoding> writableBlob;
                if (SUCCEEDED(utils->CreateBlob(output.data(), static_cast<UINT32>(output.size()), DXC_CP_ACP, writableBlob.GetAddressOf())))
                {
                    ComPtr<IDxcOperationResult> valResult;
                    HRESULT valStatus = E_FAIL;
                    if (SUCCEEDED(validator->Validate(writableBlob.Get(), DxcValidatorFlags_Default, valResult.GetAddressOf()))
                        && valResult && SUCCEEDED(valResult->GetStatus(&valStatus)) && SUCCEEDED(valStatus))
                    {
                        ComPtr<IDxcBlob> validated;
                        if (SUCCEEDED(valResult->GetResult(validated.GetAddressOf())) && validated)
                        {
                            const auto* vPtr = static_cast<const std::uint8_t*>(validated->GetBufferPointer());
                            output.assign(vPtr, vPtr + validated->GetBufferSize());
                        }
                    }
                }
            }
        }
        return HasSignedDxilContainer(output);
    }
};

struct StageBlobInput
{
    std::string name;
    std::vector<std::uint8_t> dxil;
};

bool WriteMeshletPackage(const Json& identity, std::span<const StageBlobInput> stages,
    const fs::path& destination, const Localization& text)
{
    Json stageList = Json::array();
    std::vector<std::uint8_t> payload;
    for (const auto& stage : stages)
    {
        if (stage.dxil.empty()) return false;
        const UINT offset = static_cast<UINT>(payload.size());
        const UINT size = static_cast<UINT>(stage.dxil.size());
        payload.insert(payload.end(), stage.dxil.begin(), stage.dxil.end());
        stageList.push_back({
            {"name", stage.name},
            {"offset", offset},
            {"size", size},
            {"sha256", se::Sha256(stage.dxil)}
        });
    }

    const Json metadata{
        {"schema", 1},
        {"identity", identity},
        {"stages", std::move(stageList)}
    };
    const std::string jsonText = metadata.dump();
    std::vector<std::uint8_t> artifact;
    artifact.reserve(16 + jsonText.size() + payload.size());
    artifact.insert(artifact.end(), MeshletArtifactMagic, MeshletArtifactMagic + 8);
    AppendU32(artifact, static_cast<UINT>(jsonText.size()));
    AppendU32(artifact, static_cast<UINT>(stages.size()));
    artifact.insert(artifact.end(), jsonText.begin(), jsonText.end());
    artifact.insert(artifact.end(), payload.begin(), payload.end());
    if (!AtomicWrite(destination, artifact))
    {
        Log(LogLevel::Error, text.GetText("Shader.WriteFailed") + ": " + PathUtf8(destination));
        return false;
    }

    auto sidecarPath = destination;
    sidecarPath += ".json";
    const std::string sidecarText = metadata.dump(2);
    return AtomicWrite(sidecarPath, {reinterpret_cast<const std::uint8_t*>(sidecarText.data()), sidecarText.size()});
}

bool LoadMeshletPackage(const fs::path& artifactPath, const Json& expectedIdentity,
    std::unordered_map<std::string, std::vector<std::uint8_t>>& stagesOut)
{
    if (expectedIdentity.empty()) return false;
    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(artifactPath, bytes) || bytes.size() < 16
        || std::memcmp(bytes.data(), MeshletArtifactMagic, 8) != 0)
        return false;

    const UINT jsonLen = ReadU32(bytes.data() + 8);
    const UINT stageCount = ReadU32(bytes.data() + 12);
    if (!jsonLen || !stageCount || jsonLen > 256 * 1024 || UINT64(16) + jsonLen > bytes.size())
        return false;

    const std::size_t payloadOffset = std::size_t(16) + jsonLen;
    const std::size_t payloadSize = bytes.size() - payloadOffset;
    try
    {
        const auto metadata = Json::parse(bytes.begin() + 16, bytes.begin() + payloadOffset,
            [](int depth, Json::parse_event_t, Json&)
            {
                if (depth > 32) throw std::invalid_argument("meshlet metadata depth");
                return true;
            });
        if (metadata.at("schema").get<int>() != 1 || metadata.at("identity") != expectedIdentity)
            return false;
        const auto& stageList = metadata.at("stages");
        if (!stageList.is_array() || stageList.size() != stageCount)
            return false;

        std::unordered_map<std::string, std::vector<std::uint8_t>> loaded;
        for (const auto& entry : stageList)
        {
            const std::string name = entry.at("name").get<std::string>();
            const UINT offset = entry.at("offset").get<UINT>();
            const UINT size = entry.at("size").get<UINT>();
            const std::string expectedSha = entry.at("sha256").get<std::string>();
            if (!size || UINT64(offset) + size > payloadSize) return false;
            std::vector<std::uint8_t> blob(bytes.begin() + payloadOffset + offset,
                bytes.begin() + payloadOffset + offset + size);
            if (!HasSignedDxilContainer(blob) || se::Sha256(blob) != expectedSha) return false;
            loaded.insert_or_assign(name, std::move(blob));
        }
        stagesOut = std::move(loaded);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool BufferRange(ID3D12Resource* resource, UINT64 address, UINT64 bytes)
{
    if (!resource || !bytes || resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return false;
    const auto begin = resource->GetGPUVirtualAddress(), size = resource->GetDesc().Width;
    return begin && address >= begin && address - begin <= size && bytes <= size - (address - begin);
}

const D3D12MeshPipeline* ResolvePipeline(const D3D12MeshletInput& item, const D3D12MeshPipeline& fallback)
{
    return item.draw && item.draw->pipeline ? item.draw->pipeline : &fallback;
}

bool TrustedGeometryProgram(const D3D12ShaderSource& source, const fs::path& builtinDirectory, bool& outBc5Normals)
{
    outBc5Normals = false;
    if (builtinDirectory.empty() || source.path.empty()) return false;
    const wchar_t* file = nullptr;
    switch (source.kind)
    {
    case rhi::PipelineKind::PbrMesh: file = L"Pbr.hlsl"; break;
    case rhi::PipelineKind::ForwardMesh: file = L"Forward.hlsl"; break;
    case rhi::PipelineKind::ShadowedMesh: file = L"ShadowedForward.hlsl"; break;
    case rhi::PipelineKind::EnvironmentMesh: file = L"EnvironmentForward.hlsl"; break;
    case rhi::PipelineKind::BasicLitMesh: file = L"BasicLit.hlsl"; break;
    default: return false;
    }
    const auto expected = (builtinDirectory / file).lexically_normal().native();
    std::error_code error;
    const auto actual = fs::weakly_canonical(source.path, error).native();
    if (error) return false;
    if (actual.size() > INT_MAX || expected.size() > INT_MAX
        || CompareStringOrdinal(actual.data(), static_cast<int>(actual.size()),
            expected.data(), static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL)
        return false;
    bool bc5 = false;
    for (const auto& macro : source.macros)
    {
        if (macro.name != "SE_NORMAL_BC5" || macro.value != "1" || bc5) return false;
        bc5 = true;
    }
    if (bc5 && source.kind != rhi::PipelineKind::PbrMesh) return false;
    outBc5Normals = bc5;
    return true;
}

bool SameBindings(const D3D12MeshletInput& a, const D3D12MeshletInput& b, const D3D12MeshPipeline& fallback)
{
    const auto* pa = ResolvePipeline(a, fallback);
    const auto* pb = ResolvePipeline(b, fallback);
    if (pa->prepared != pb->prepared || pa->pipeline.Get() != pb->pipeline.Get() || pa->rootSignature.Get() != pb->rootSignature.Get())
        return false;
    if (!a.draw || !b.draw || !a.draw->mesh || !b.draw->mesh) return false;
    const auto& ma = *a.draw->mesh;
    const auto& mb = *b.draw->mesh;
    if (a.draw->textureIsData != b.draw->textureIsData || ma.texture.Get() != mb.texture.Get() || ma.textureViews != mb.textureViews)
        return false;
    for (size_t slot = 0; slot < ma.pbrTextures.size(); ++slot)
    {
        if (ma.pbrTextures[slot].resource.Get() != mb.pbrTextures[slot].resource.Get()
            || ma.pbrTextures[slot].views != mb.pbrTextures[slot].views)
            return false;
    }
    return true;
}

UINT RequiredMaterialViews(rhi::PipelineKind kind) noexcept
{
    return rhi::IsEnvironment(kind) ? 7u : rhi::IsShadowed(kind) ? 4u : 3u;
}

D3D12HeterogeneousFallback Eligible(const D3D12MeshletInput& item, const D3D12MeshPipeline& fallback, bool trustedGeometry)
{
    using Reason = D3D12HeterogeneousFallback;
    if (!item.draw || !item.draw->mesh) return Reason::InvalidGeometry;
    const auto& draw = *item.draw;
    const auto& mesh = *draw.mesh;
    const auto* pipeline = ResolvePipeline(item, fallback);
    if (draw.alphaMode != se::MaterialAlphaMode::Opaque) return Reason::Alpha;
    if (!draw.instances.empty()) return Reason::Instances;
    if (!draw.skinPalette.empty() || draw.skinJointCount || draw.skinJointOffset) return Reason::Skin;
    if (draw.outline) return Reason::Outline;
    if (draw.impostorView != 0 || draw.impostorNext != 0 || draw.impostorBlend != 0) return Reason::Impostor;
    if (!pipeline->prepared || !pipeline->prepared->program || !pipeline->pipeline || !pipeline->rootSignature)
        return Reason::UnsupportedPipeline;
    const auto& source = pipeline->prepared->program->source;
    const auto kind = source.kind;
    if (source.materialLayoutVersion || source.shadingLayoutVersion
        || pipeline->prepared->materialRootParameter != UINT_MAX || source.depthNormals)
        return Reason::MaterialExtension;
    if (source.skinnedVertex || source.compressedVertex || rhi::IsInstanced(kind))
        return Reason::UnsupportedPipeline;
    if (kind != rhi::PipelineKind::PbrMesh && kind != rhi::PipelineKind::ForwardMesh
        && kind != rhi::PipelineKind::ShadowedMesh && kind != rhi::PipelineKind::EnvironmentMesh
        && kind != rhi::PipelineKind::BasicLitMesh)
        return Reason::UnsupportedPipeline;
    if (source.layoutVersion != rhi::BindingLayout(kind) || pipeline->prepared->blend != rhi::BlendMode::Opaque
        || pipeline->prepared->alphaMode != se::MaterialAlphaMode::Opaque)
        return Reason::UnsupportedPipeline;
    if (!trustedGeometry) return Reason::UnsupportedPipeline;
    const UINT viewCount = RequiredMaterialViews(kind);
    if (item.materialViewCount < viewCount) return Reason::InvalidGeometry;
    for (UINT i = 0; i < viewCount; ++i)
        if (!item.materialViews[i].ptr) return Reason::InvalidGeometry;
    const UINT expectedStride = static_cast<UINT>(sizeof(rhi::Vertex));
    if (!mesh.vertices || !mesh.indices || mesh.vertexView.StrideInBytes != expectedStride
        || !mesh.vertexView.SizeInBytes || mesh.vertexView.SizeInBytes % expectedStride
        || !BufferRange(mesh.vertices.Get(), mesh.vertexView.BufferLocation, mesh.vertexView.SizeInBytes)
        || !BufferRange(mesh.indices.Get(), mesh.indexView.BufferLocation, mesh.indexView.SizeInBytes)
        || !se::IsFinite(draw.world) || item.baseVertex != 0)
        return Reason::InvalidGeometry;
    const UINT stride = mesh.indexView.Format == DXGI_FORMAT_R16_UINT ? 2u : mesh.indexView.Format == DXGI_FORMAT_R32_UINT ? 4u : 0u;
    const UINT count = item.indexCount ? item.indexCount : mesh.indexCount;
    if (!stride || !count || count % 3 || item.firstIndex % 3 || (!item.indexCount && item.firstIndex)
        || mesh.indexView.BufferLocation % stride || mesh.indexView.SizeInBytes % stride
        || item.firstIndex > mesh.indexCount || count > mesh.indexCount - item.firstIndex
        || item.firstIndex > mesh.indexView.SizeInBytes / stride || count > mesh.indexView.SizeInBytes / stride - item.firstIndex)
        return Reason::InvalidGeometry;
    if (!se::IsValid(item.worldBounds)) return Reason::InvalidBounds;
    if (!item.b0.resource || item.b0.offset % 256 || item.b0.size < sizeof(rhi::PbrDrawConstants)
        || item.b0.offset > item.b0.resource->GetDesc().Width
        || item.b0.size > item.b0.resource->GetDesc().Width - item.b0.offset
        || !BufferRange(item.b0.resource, item.b0.Gpu(), item.b0.size) || item.b0.Gpu() % 256)
        return Reason::InvalidConstants;
    return Reason::None;
}

D3D12HeterogeneousFallback EligibleRangeOnly(const D3D12MeshletInput& item)
{
    using Reason = D3D12HeterogeneousFallback;
    const auto& mesh = *item.draw->mesh;
    const UINT stride = mesh.indexView.Format == DXGI_FORMAT_R16_UINT ? 2u : mesh.indexView.Format == DXGI_FORMAT_R32_UINT ? 4u : 0u;
    const UINT count = item.indexCount ? item.indexCount : mesh.indexCount;
    if (item.baseVertex != 0 || !stride || !count || count % 3 || item.firstIndex % 3 || (!item.indexCount && item.firstIndex)
        || item.firstIndex > mesh.indexCount || count > mesh.indexCount - item.firstIndex
        || item.firstIndex > mesh.indexView.SizeInBytes / stride || count > mesh.indexView.SizeInBytes / stride - item.firstIndex)
        return Reason::InvalidGeometry;
    if (!se::IsValid(item.worldBounds)) return Reason::InvalidBounds;
    return Reason::None;
}

bool ValidOcclusion(ID3D12Device& device, const D3D12HeterogeneousOcclusion& value)
{
    const auto inverse = se::Inverse(value.clipFromWorld);
    if (!value.output || !se::IsFinite(value.clipFromWorld) || !inverse || !se::IsFinite(*inverse)
        || !se::IsFinite(value.depthBias) || value.depthBias < 1e-6f || value.depthBias > .01f)
        return false;
    const auto& input = *value.output;
    if (!input.depth || !input.texture || !input.width || !input.height
        || D3D12HiZ::Levels(input.width, input.height) != input.mipCount)
        return false;
    const auto texture = input.texture->GetDesc(), depth = input.depth->GetDesc();
    if (texture.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || texture.Width != input.width || texture.Height != input.height
        || texture.MipLevels != input.mipCount || texture.Format != DXGI_FORMAT_R32_FLOAT
        || texture.DepthOrArraySize != 1 || texture.SampleDesc.Count != 1
        || depth.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || depth.Width != input.width || depth.Height != input.height
        || depth.MipLevels != 1 || depth.DepthOrArraySize != 1 || depth.SampleDesc.Count != 1
        || (depth.Format != DXGI_FORMAT_R32_FLOAT && depth.Format != DXGI_FORMAT_R32_TYPELESS && depth.Format != DXGI_FORMAT_D32_FLOAT))
        return false;
    ComPtr<ID3D12Device> owner;
    if (FAILED(input.texture->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))) || owner.Get() != &device) return false;
    owner.Reset();
    if (FAILED(input.depth->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))) || owner.Get() != &device) return false;
    return true;
}

const char* PixelStageName(rhi::PipelineKind kind, bool bc5Normals) noexcept
{
    switch (kind)
    {
    case rhi::PipelineKind::PbrMesh: return bc5Normals ? "PbrBc5" : "Pbr";
    case rhi::PipelineKind::ForwardMesh: return "Forward";
    case rhi::PipelineKind::ShadowedMesh: return "Shadowed";
    case rhi::PipelineKind::EnvironmentMesh: return "Environment";
    case rhi::PipelineKind::BasicLitMesh: return "BasicLit";
    default: return nullptr;
    }
}

std::size_t PixelStageIndex(rhi::PipelineKind kind, bool bc5Normals) noexcept
{
    switch (kind)
    {
    case rhi::PipelineKind::PbrMesh: return bc5Normals ? 1u : 0u;
    case rhi::PipelineKind::ForwardMesh: return 2u;
    case rhi::PipelineKind::ShadowedMesh: return 3u;
    case rhi::PipelineKind::EnvironmentMesh: return 4u;
    case rhi::PipelineKind::BasicLitMesh: return 5u;
    default: return SIZE_MAX;
    }
}

std::uint64_t MakePipelineKey(rhi::PipelineKind kind, bool bc5Normals, rhi::ColorFormat colorFormat, rhi::DepthMode depthMode) noexcept
{
    return (static_cast<std::uint64_t>(kind) << 16)
        | (static_cast<std::uint64_t>(bc5Normals ? 1u : 0u) << 12)
        | (static_cast<std::uint64_t>(colorFormat) << 8)
        | static_cast<std::uint64_t>(depthMode);
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename Value, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) PsoSubobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Value value{};
};

struct alignas(void*) MeshShaderPipelineStream
{
    PsoSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rootSignature;
    PsoSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> as;
    PsoSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
    PsoSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    PsoSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    PsoSubobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sampleMask;
    PsoSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    PsoSubobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depthStencil;
    PsoSubobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rtvFormats;
    PsoSubobject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsvFormat;
    PsoSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sampleDesc;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpu(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT increment, UINT index) noexcept
{
    handle.ptr += static_cast<SIZE_T>(increment) * index;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE OffsetGpu(D3D12_GPU_DESCRIPTOR_HANDLE handle, UINT increment, UINT index) noexcept
{
    handle.ptr += static_cast<UINT64>(increment) * index;
    return handle;
}
} // namespace

bool WriteD3D12MeshletShader(const fs::path& source, const fs::path& destination, const Localization& text)
{
    const fs::path shaderDir = source.parent_path();
    const fs::path filename = source.filename();
    DxcSession dxc;
    if (!dxc.Initialize(text)) return false;

    if (filename == L"MeshletCull.as.hlsl")
    {
        const auto identity = MeshletAsIdentity(shaderDir);
        if (identity.empty()) return false;
        std::vector<std::uint8_t> asDxil;
        if (!dxc.Compile(source, shaderDir, L"ASMain", L"as_6_5", nullptr, asDxil, text)) return false;
        const std::array<StageBlobInput, 1> stages{{{"ASMain", std::move(asDxil)}}};
        return WriteMeshletPackage(identity, stages, destination, text);
    }

    if (filename == L"MeshletDraw.ms.hlsl")
    {
        const auto identity = MeshletMsIdentity(shaderDir);
        if (identity.empty()) return false;

        std::vector<StageBlobInput> stages;
        stages.reserve(7);

        std::vector<std::uint8_t> msDxil;
        if (!dxc.Compile(source, shaderDir, L"MSMain", L"ms_6_5", nullptr, msDxil, text)) return false;
        stages.push_back({"MSMain", std::move(msDxil)});

        struct PsTarget
        {
            const char* name;
            const char* file;
            const wchar_t* macro;
        };
        static constexpr PsTarget kPsTargets[] = {
            {"Pbr", "Pbr.hlsl", nullptr},
            {"PbrBc5", "Pbr.hlsl", L"SE_NORMAL_BC5=1"},
            {"Forward", "Forward.hlsl", nullptr},
            {"Shadowed", "ShadowedForward.hlsl", nullptr},
            {"Environment", "EnvironmentForward.hlsl", nullptr},
            {"BasicLit", "BasicLit.hlsl", nullptr}
        };
        for (const auto& ps : kPsTargets)
        {
            std::vector<std::uint8_t> psDxil;
            if (!dxc.Compile(shaderDir / ps.file, shaderDir, L"PSMain", L"ps_6_5", ps.macro, psDxil, text))
                return false;
            stages.push_back({ps.name, std::move(psDxil)});
        }
        return WriteMeshletPackage(identity, stages, destination, text);
    }

    Log(LogLevel::Error, text.GetText("Shader.SourceInvalid") + ": " + PathUtf8(source));
    return false;
}

HRESULT D3D12Meshlet::Initialize(D3D12Context& context, const fs::path& shaderDirectory, const Localization& text)
{
    if (initialized_) return S_OK;
    if (!context.meshShaderTier1 || !context.device || !context.states || !context.srvDescriptors)
        return E_FAIL;

    ComPtr<ID3D12Device2> dev2;
    if (FAILED(context.device.As(&dev2)) || !dev2) return E_NOINTERFACE;

    fs::path resolvedDir = shaderDirectory;
    const auto hasArtifacts = [](const fs::path& dir)
    {
        std::error_code ec;
        return !dir.empty()
            && fs::exists(dir / "MeshletCull.as.hlsl.sebin", ec)
            && fs::exists(dir / "MeshletDraw.ms.hlsl.sebin", ec);
    };
    if (!hasArtifacts(resolvedDir))
    {
        const fs::path exeShaders = GetExecutableDirectory() / "Resources/Shaders";
        if (hasArtifacts(exeShaders)) resolvedDir = exeShaders;
    }
    if (!hasArtifacts(resolvedDir))
    {
        Log(LogLevel::Warning, text.GetText("D3D12.MeshShaderInitializeFailed"));
        return E_FAIL;
    }

    std::unordered_map<std::string, std::vector<std::uint8_t>> asStages, msStages;
    if (!LoadMeshletPackage(resolvedDir / "MeshletCull.as.hlsl.sebin", MeshletAsIdentity(resolvedDir), asStages)
        || !LoadMeshletPackage(resolvedDir / "MeshletDraw.ms.hlsl.sebin", MeshletMsIdentity(resolvedDir), msStages))
    {
        Log(LogLevel::Warning, text.GetText("D3D12.MeshShaderInitializeFailed"));
        return E_FAIL;
    }

    const auto asIt = asStages.find("ASMain");
    const auto msIt = msStages.find("MSMain");
    if (asIt == asStages.end() || msIt == msStages.end())
    {
        Log(LogLevel::Warning, text.GetText("D3D12.MeshShaderInitializeFailed"));
        return E_FAIL;
    }

    static constexpr std::array<const char*, 6> kPsNames{
        "Pbr", "PbrBc5", "Forward", "Shadowed", "Environment", "BasicLit"
    };
    std::array<std::vector<std::uint8_t>, 6> loadedPs{};
    for (std::size_t i = 0; i < kPsNames.size(); ++i)
    {
        const auto found = msStages.find(kPsNames[i]);
        if (found == msStages.end())
        {
            Log(LogLevel::Warning, text.GetText("D3D12.MeshShaderInitializeFailed"));
            return E_FAIL;
        }
        loadedPs[i] = std::move(found->second);
    }

    D3D12_ROOT_SIGNATURE_DESC emptyRootDesc{};
    ComPtr<ID3DBlob> emptyRootBlob, emptyRootError;
    ComPtr<ID3D12RootSignature> emptyRoot;
    if (FAILED(D3D12SerializeRootSignature(&emptyRootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0,
            emptyRootBlob.GetAddressOf(), emptyRootError.GetAddressOf()))
        || FAILED(dev2->CreateRootSignature(0, emptyRootBlob->GetBufferPointer(), emptyRootBlob->GetBufferSize(),
            IID_PPV_ARGS(emptyRoot.GetAddressOf()))))
    {
        Log(LogLevel::Warning, text.GetText("D3D12.MeshShaderInitializeFailed"));
        return E_FAIL;
    }

    device2_ = std::move(dev2);
    emptyComputeRoot_ = std::move(emptyRoot);
    asBlob_[0] = std::move(asIt->second);
    msBlob_ = std::move(msIt->second);
    psBlobs_ = std::move(loadedPs);
    pipelines_.clear();
    initialized_ = true;
    return S_OK;
}

const D3D12Meshlet::PipelineEntry* D3D12Meshlet::GetOrCreatePipeline(
    rhi::PipelineKind kind, bool bc5Normals, rhi::ColorFormat colorFormat, rhi::DepthMode depthMode, const Localization& text)
{
    if (!initialized_ || !device2_) return nullptr;
    const std::size_t psIndex = PixelStageIndex(kind, bc5Normals);
    if (psIndex >= psBlobs_.size() || !PixelStageName(kind, bc5Normals)) return nullptr;

    const auto effectiveDepth = depthMode == rhi::DepthMode::Default ? rhi::DepthMode::ReadWriteLess : depthMode;
    const std::uint64_t key = MakePipelineKey(kind, bc5Normals, colorFormat, effectiveDepth);
    if (const auto found = pipelines_.find(key); found != pipelines_.end())
        return &found->second;

    const bool isForward = rhi::IsForward(kind);
    const bool isShadow = rhi::IsShadowed(kind);
    const bool isEnvironment = rhi::IsEnvironment(kind);
    const UINT numMaterials = RequiredMaterialViews(kind);

    D3D12_DESCRIPTOR_RANGE materialRange{};
    materialRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialRange.NumDescriptors = numMaterials;
    materialRange.BaseShaderRegister = 0;
    materialRange.RegisterSpace = 0;
    materialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE hzbRange{};
    hzbRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    hzbRange.NumDescriptors = 1;
    hzbRange.BaseShaderRegister = 4;
    hzbRange.RegisterSpace = 1;
    hzbRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 10> params{};
    // 0: b0, space0 (PbrDrawConstants)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // 1: t0..tN, space0 (material + shadow + env table)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &materialRange};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // 2: b0, space1 (4x32-bit dispatch constants)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0, 1, 4};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // 3: b1, space1 (MeshletCullConstants)
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].Descriptor = {1, 1};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    // 4: t0, space1 (meshletBounds)
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].Descriptor = {0, 1};
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    // 5: t1, space1 (meshletItems)
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor = {1, 1};
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    // 6: t2, space1 (sourceVertices)
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[6].Descriptor = {2, 1};
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    // 7: t3, space1 (sourceIndices)
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[7].Descriptor = {3, 1};
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    // 8: t4, space1 (currentFrameHiZ table)
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].DescriptorTable = {1, &hzbRange};
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    // 9 (Forward only): b1, space0 (ForwardConstants)
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[9].Descriptor = {1, 0};
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[]{sampler, sampler, sampler};
    samplers[1].ShaderRegister = 1;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[2].ShaderRegister = 2;
    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = isForward ? 10u : 9u;
    rootDesc.pParameters = params.data();
    rootDesc.NumStaticSamplers = isEnvironment ? 3u : isShadow ? 2u : 1u;
    rootDesc.pStaticSamplers = samplers;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, diagnostics;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(), diagnostics.GetAddressOf())))
    {
        Log(LogLevel::Error, text.GetText("D3D12.SerializeRootSignatureFailed"));
        return nullptr;
    }

    PipelineEntry created;
    if (FAILED(device2_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(created.rootSignature.GetAddressOf()))))
    {
        Log(LogLevel::Error, text.GetText("D3D12.CreateRootSignatureFailed"));
        return nullptr;
    }

    D3D12_BLEND_DESC blend{};
    auto& rtBlend = blend.RenderTarget[0];
    rtBlend.BlendEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_ONE;
    rtBlend.DestBlend = D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = TRUE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = effectiveDepth != rhi::DepthMode::Disabled;
    depthStencil.DepthWriteMask = effectiveDepth == rhi::DepthMode::ReadWriteLess ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencil.DepthFunc = effectiveDepth == rhi::DepthMode::ReadOnlyEqual ? D3D12_COMPARISON_FUNC_EQUAL
        : effectiveDepth == rhi::DepthMode::ReadOnlyLessEqual ? D3D12_COMPARISON_FUNC_LESS_EQUAL
        : effectiveDepth == rhi::DepthMode::ReadWriteLess ? D3D12_COMPARISON_FUNC_LESS
        : D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencil.FrontFace = depthStencil.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};

    D3D12_RT_FORMAT_ARRAY rtFormats{};
    rtFormats.NumRenderTargets = 1;
    rtFormats.RTFormats[0] = colorFormat == rhi::ColorFormat::Rgba8Unorm ? DXGI_FORMAT_R8G8B8A8_UNORM
        : colorFormat == rhi::ColorFormat::Rgba8Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        : DXGI_FORMAT_R16G16B16A16_FLOAT;

    MeshShaderPipelineStream stream;
    stream.rootSignature.value = created.rootSignature.Get();
    stream.as.value = {asBlob_[0].data(), asBlob_[0].size()};
    stream.ms.value = {msBlob_.data(), msBlob_.size()};
    stream.ps.value = {psBlobs_[psIndex].data(), psBlobs_[psIndex].size()};
    stream.blend.value = blend;
    stream.sampleMask.value = UINT_MAX;
    stream.rasterizer.value = rasterizer;
    stream.depthStencil.value = depthStencil;
    stream.rtvFormats.value = rtFormats;
    stream.dsvFormat.value = DXGI_FORMAT_D32_FLOAT;
    stream.sampleDesc.value = {1, 0};

    D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{sizeof(stream), &stream};
    if (FAILED(device2_->CreatePipelineState(&streamDesc, IID_PPV_ARGS(created.pipeline.GetAddressOf()))))
    {
        Log(LogLevel::Warning, text.GetText("D3D12.MeshShaderInitializeFailed"));
        return nullptr;
    }

    const auto [it, inserted] = pipelines_.emplace(key, std::move(created));
    return &it->second;
}

HRESULT D3D12Meshlet::Prepare(D3D12UploadArena& arena, D3D12MeshletOutput& output, const D3D12MeshPipeline& defaultPipeline,
    std::span<const D3D12MeshletInput> input, const se::VisibilityFrustum& frustum,
    D3D12MeshletPrepared& prepared, const D3D12HeterogeneousOcclusion* occlusion, const Localization& text)
{
    if (!initialized_ || !device2_) return E_UNEXPECTED;
    if (input.size() > se::MaximumVisibilityItems || !se::ValidVisibilityFrustum(frustum))
        return E_INVALIDARG;
    if (occlusion && !ValidOcclusion(*device2_.Get(), *occlusion)) return E_INVALIDARG;

    std::error_code pathError;
    const auto executable = GetExecutableDirectory();
    auto builtinDirectory = executable.empty() ? fs::path{} : fs::weakly_canonical(executable / "Resources/Shaders", pathError);
    if (pathError) builtinDirectory.clear();

    struct ProgramCheck
    {
        bool trusted = false;
        bool bc5 = false;
    };
    std::unordered_map<const D3D12ShaderProgram*, ProgramCheck> trustedPrograms;
    const auto checkProgram = [&](const D3D12MeshletInput& item) -> ProgramCheck
    {
        const auto* pipeline = ResolvePipeline(item, defaultPipeline);
        if (!pipeline->prepared || !pipeline->prepared->program) return {};
        const auto* program = pipeline->prepared->program.get();
        const auto [entry, inserted] = trustedPrograms.try_emplace(program, ProgramCheck{});
        if (inserted) entry->second.trusted = TrustedGeometryProgram(program->source, builtinDirectory, entry->second.bc5);
        return entry->second;
    };

    D3D12MeshletPrepared next;
    next.statistics_.sourceRanges = static_cast<UINT>(input.size());
    if (occlusion) next.occlusion_ = occlusion->output;

    std::vector<se::VisibilityItem> gpuBounds;
    gpuBounds.reserve(input.size() * 2u);
    std::vector<rhi::GpuMeshletItem> gpuItems;
    gpuItems.reserve(input.size() * 2u);

    struct PendingSegmentMaterial
    {
        std::size_t segmentIndex = 0;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 7> views{};
        UINT viewCount = 0;
    };
    std::vector<PendingSegmentMaterial> segmentMaterials;
    UINT totalDescriptorsNeeded = 1; // slot 0 = HZB or null R32F SRV

    for (UINT first = 0; first < input.size();)
    {
        UINT end = first + 1;
        while (end < input.size() && SameBindings(input[first], input[end], defaultPipeline)) ++end;

        D3D12MeshletSegment segment;
        segment.sourceFirst = first;
        segment.sourceCount = end - first;

        UINT unsupported = 0;
        bool bucketBc5 = false;
        const D3D12MeshDraw* verifiedDraw = nullptr;
        D3D12UploadSlice verifiedB0{};
        bool verifiedBc5 = false;
        for (UINT index = first; index < end; ++index)
        {
            const auto& cur = input[index];
            D3D12HeterogeneousFallback reason;
            if (verifiedDraw && cur.draw == verifiedDraw && cur.b0.resource == verifiedB0.resource
                && cur.b0.offset == verifiedB0.offset && cur.b0.size == verifiedB0.size)
            {
                reason = EligibleRangeOnly(cur);
                bucketBc5 = bucketBc5 || verifiedBc5;
            }
            else
            {
                const auto programCheck = checkProgram(cur);
                reason = Eligible(cur, defaultPipeline, programCheck.trusted);
                bucketBc5 = bucketBc5 || programCheck.bc5;
                if (reason == D3D12HeterogeneousFallback::None)
                {
                    verifiedDraw = cur.draw;
                    verifiedB0 = cur.b0;
                    verifiedBc5 = programCheck.bc5;
                }
            }
            if (reason != D3D12HeterogeneousFallback::None)
            {
                ++unsupported;
                ++next.statistics_.reasons[static_cast<size_t>(reason)];
            }
        }

        if (unsupported)
        {
            ++next.statistics_.cpuBuckets;
            next.segments_.push_back(std::move(segment));
            first = end;
            continue;
        }

        const auto* pipeline = ResolvePipeline(input[first], defaultPipeline);
        const auto kind = pipeline->prepared->program->source.kind;
        const auto colorFormat = pipeline->prepared->colorFormat;
        const auto depthMode = pipeline->prepared->depth;
        if (!GetOrCreatePipeline(kind, bucketBc5, colorFormat, depthMode, text))
        {
            return E_FAIL;
        }

        segment.cpuFallback = false;
        segment.isForward = rhi::IsForward(kind);
        segment.kind = kind;
        segment.bc5Normals = bucketBc5;
        segment.colorFormat = colorFormat;
        segment.depthMode = depthMode;

        const D3D12MeshDraw* leasedDraw = nullptr;
        ID3D12Resource* leasedB0 = nullptr;
        for (UINT index = first; index < end; ++index)
        {
            const auto& item = input[index];
            const auto& mesh = *item.draw->mesh;
            const UINT stride = mesh.indexView.Format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
            const UINT totalIndices = item.indexCount ? item.indexCount : mesh.indexCount;
            const UINT totalTriangles = totalIndices / 3u;
            const UINT flags = kind == rhi::PipelineKind::BasicLitMesh ? 1u : 0u;

            const UINT64 vbAddr = mesh.vertexView.BufferLocation;
            const UINT64 ibRawAddr = mesh.indexView.BufferLocation;
            const UINT64 ibAlignedAddr = ibRawAddr & ~UINT64(3);
            const UINT indexElementOffset = (stride == 2 && (ibRawAddr & UINT64(2)) != 0) ? 1u : 0u;

            const UINT runFirstMeshlet = static_cast<UINT>(gpuItems.size());
            UINT remainingTris = totalTriangles;
            UINT triOffset = 0;
            while (remainingTris > 0)
            {
                if (gpuItems.size() >= MaxGpuMeshletsPerBatch) return E_INVALIDARG;
                const UINT chunkTris = (std::min)(remainingTris, MeshletMaxTrianglesPerItem);
                const UINT meshletId = static_cast<UINT>(gpuItems.size());
                gpuItems.push_back({item.firstIndex + triOffset * 3u, chunkTris, stride, flags});
                gpuBounds.push_back({item.worldBounds.minimum, meshletId, item.worldBounds.maximum, 0});
                triOffset += chunkTris;
                remainingTris -= chunkTris;
            }
            const UINT chunkCount = static_cast<UINT>(gpuItems.size()) - runFirstMeshlet;

            const D3D12_GPU_VIRTUAL_ADDRESS itemB0 = item.b0.Gpu();
            if (!segment.runs.empty())
            {
                auto& prev = segment.runs.back();
                if (prev.vertexSrv == vbAddr && prev.indexSrv == ibAlignedAddr
                    && prev.indexElementOffset == indexElementOffset && prev.b0 == itemB0
                    && prev.firstMeshlet + prev.meshletCount == runFirstMeshlet)
                {
                    prev.meshletCount += chunkCount;
                }
                else
                {
                    segment.runs.push_back({runFirstMeshlet, chunkCount, itemB0, vbAddr, ibAlignedAddr,
                        0u, indexElementOffset, mesh.vertices.Get(), mesh.indices.Get()});
                }
            }
            else
            {
                segment.runs.push_back({runFirstMeshlet, chunkCount, itemB0, vbAddr, ibAlignedAddr,
                    0u, indexElementOffset, mesh.vertices.Get(), mesh.indices.Get()});
            }

            if (item.draw != leasedDraw)
            {
                leasedDraw = item.draw;
                next.vertexLeases_.push_back(mesh.vertices);
                next.indexLeases_.push_back(mesh.indices);
                if (mesh.texture) next.textureLeases_.push_back(mesh.texture);
                if (mesh.textureViews) next.viewLeases_.push_back(mesh.textureViews);
                for (const auto& texture : mesh.pbrTextures)
                {
                    if (texture.resource) next.textureLeases_.push_back(texture.resource);
                    if (texture.views) next.viewLeases_.push_back(texture.views);
                }
            }
            if (item.b0.resource != leasedB0)
            {
                leasedB0 = item.b0.resource;
                next.constantLeases_.emplace_back(item.b0.resource);
            }
            ++next.statistics_.gpuRanges;
        }

        const UINT viewCount = RequiredMaterialViews(kind);
        PendingSegmentMaterial pendingMaterial;
        pendingMaterial.segmentIndex = next.segments_.size();
        pendingMaterial.views = input[first].materialViews;
        pendingMaterial.viewCount = viewCount;
        segmentMaterials.push_back(pendingMaterial);
        totalDescriptorsNeeded += viewCount;

        next.statistics_.dispatchCount += static_cast<UINT>(segment.runs.size());
        ++next.statistics_.gpuBuckets;
        next.segments_.push_back(std::move(segment));
        first = end;
    }

    next.totalMeshlets_ = static_cast<UINT>(gpuItems.size());
    next.statistics_.gpuMeshlets = next.totalMeshlets_;

    if (next.totalMeshlets_)
    {
        if (!output.descriptors || output.descriptorCapacity < totalDescriptorsNeeded)
        {
            const UINT nextCap = (std::max)(totalDescriptorsNeeded, (std::max)(64u, output.descriptorCapacity * 2u));
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = nextCap;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            heapDesc.NodeMask = 1;
            ComPtr<ID3D12DescriptorHeap> heap;
            const auto hr = device2_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.GetAddressOf()));
            if (FAILED(hr)) return hr;
            heap->SetName(L"FlareEngine.MeshletDescriptors");
            output.descriptors = std::move(heap);
            output.descriptorCapacity = nextCap;
        }

        const UINT increment = device2_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto heapCpu = output.descriptors->GetCPUDescriptorHandleForHeapStart();
        const auto heapGpu = output.descriptors->GetGPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC hzbView{};
        hzbView.Format = DXGI_FORMAT_R32_FLOAT;
        hzbView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hzbView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (occlusion)
        {
            hzbView.Texture2D.MipLevels = occlusion->output->mipCount;
            device2_->CreateShaderResourceView(occlusion->output->texture.Get(), &hzbView, OffsetCpu(heapCpu, increment, 0));
        }
        else
        {
            hzbView.Texture2D.MipLevels = 1;
            device2_->CreateShaderResourceView(nullptr, &hzbView, OffsetCpu(heapCpu, increment, 0));
        }
        next.hzbTable_ = OffsetGpu(heapGpu, increment, 0);
        next.descriptorHeap_ = output.descriptors.Get();

        UINT cursor = 1;
        for (const auto& mat : segmentMaterials)
        {
            next.segments_[mat.segmentIndex].materialTable = OffsetGpu(heapGpu, increment, cursor);
            for (UINT i = 0; i < mat.viewCount; ++i)
            {
                device2_->CopyDescriptorsSimple(1, OffsetCpu(heapCpu, increment, cursor + i),
                    mat.views[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            cursor += mat.viewCount;
        }

        auto result = arena.Allocate(gpuBounds.size() * sizeof(se::VisibilityItem), 16, next.bounds_);
        if (SUCCEEDED(result))
            result = arena.Allocate(gpuItems.size() * sizeof(rhi::GpuMeshletItem), 16, next.items_);
        if (SUCCEEDED(result))
            result = arena.Allocate(sizeof(rhi::MeshletCullConstants), 256, next.cullConstants_);
        if (FAILED(result)) return result;

        std::memcpy(next.bounds_.cpu, gpuBounds.data(), gpuBounds.size() * sizeof(se::VisibilityItem));
        std::memcpy(next.items_.cpu, gpuItems.data(), gpuItems.size() * sizeof(rhi::GpuMeshletItem));

        rhi::MeshletCullConstants cullConstants{};
        static_assert(sizeof(cullConstants.planes) == sizeof(frustum.planes));
        std::memcpy(cullConstants.planes.data(), frustum.planes.data(), sizeof(cullConstants.planes));
        cullConstants.totalMeshlets = next.totalMeshlets_;
        if (occlusion)
        {
            cullConstants.clipFromWorld = occlusion->clipFromWorld;
            cullConstants.screen = {
                float(occlusion->output->width),
                float(occlusion->output->height),
                occlusion->depthBias,
                float(occlusion->output->mipCount)
            };
            cullConstants.occlusionEnabled = 1;
        }
        std::memcpy(next.cullConstants_.cpu, &cullConstants, sizeof(cullConstants));
    }

    prepared = std::move(next);
    return S_OK;
}

HRESULT D3D12Meshlet::RecordBucket(ID3D12GraphicsCommandList& commands, D3D12StateRecording& states,
    const D3D12MeshletPrepared& prepared, D3D12_GPU_VIRTUAL_ADDRESS forwardConstantsGpu,
    UINT width, UINT height, UINT segmentIndex, const Localization& text)
{
    if (!initialized_ || !device2_) return E_UNEXPECTED;
    if (segmentIndex >= prepared.segments_.size()) return E_INVALIDARG;
    const auto& segment = prepared.segments_[segmentIndex];
    if (segment.cpuFallback || segment.runs.empty() || !prepared.totalMeshlets_ || !prepared.descriptorHeap_)
        return E_INVALIDARG;

    const auto* entry = GetOrCreatePipeline(segment.kind, segment.bc5Normals, segment.colorFormat, segment.depthMode, text);
    if (!entry || !entry->pipeline || !entry->rootSignature) return E_FAIL;

    ComPtr<ID3D12GraphicsCommandList6> list6;
    if (FAILED(commands.QueryInterface(IID_PPV_ARGS(list6.GetAddressOf()))) || !list6)
    {
        Log(LogLevel::Error, text.GetText("D3D12.MeshShaderRecordFailed"));
        return E_NOINTERFACE;
    }

    for (const auto& run : segment.runs)
    {
        if (!run.vertexResource || !run.indexResource) return E_INVALIDARG;
        auto hr = states.Use(commands, *run.vertexResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Meshlet.VertexSrv");
        if (SUCCEEDED(hr))
            hr = states.Use(commands, *run.indexResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Meshlet.IndexSrv");
        if (FAILED(hr)) return hr;
    }
    if (prepared.occlusion_ && prepared.occlusion_->texture)
    {
        const auto hr = states.Use(commands, *prepared.occlusion_->texture.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Meshlet.CurrentFrameHiZ");
        if (FAILED(hr)) return hr;
    }

    commands.SetPipelineState(entry->pipeline.Get());
    commands.SetGraphicsRootSignature(entry->rootSignature.Get());
    if (emptyComputeRoot_) commands.SetComputeRootSignature(emptyComputeRoot_.Get());
    ID3D12DescriptorHeap* heaps[]{prepared.descriptorHeap_};
    commands.SetDescriptorHeaps(1, heaps);

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands.RSSetViewports(1, &viewport);
    commands.RSSetScissorRects(1, &scissor);

    commands.SetGraphicsRootDescriptorTable(1, segment.materialTable);
    commands.SetGraphicsRootConstantBufferView(3, prepared.cullConstants_.Gpu());
    commands.SetGraphicsRootShaderResourceView(4, prepared.bounds_.Gpu());
    commands.SetGraphicsRootShaderResourceView(5, prepared.items_.Gpu());
    commands.SetGraphicsRootDescriptorTable(8, prepared.hzbTable_);
    if (segment.isForward)
    {
        if (!forwardConstantsGpu) return E_INVALIDARG;
        commands.SetGraphicsRootConstantBufferView(9, forwardConstantsGpu);
    }

    for (const auto& run : segment.runs)
    {
        commands.SetGraphicsRootConstantBufferView(0, run.b0);
        commands.SetGraphicsRootShaderResourceView(6, run.vertexSrv);
        commands.SetGraphicsRootShaderResourceView(7, run.indexSrv);

        UINT offset = 0;
        while (offset < run.meshletCount)
        {
            const UINT remaining = run.meshletCount - offset;
            const UINT maxPerDispatch = MaxAsGroupsPerDispatch * 32u;
            const UINT chunk = (std::min)(remaining, maxPerDispatch);
            const UINT dispatchConstants[4]{
                run.firstMeshlet + offset,
                chunk,
                run.vertexByteOffset,
                run.indexElementOffset
            };
            commands.SetGraphicsRoot32BitConstants(2, 4, dispatchConstants, 0);
            list6->DispatchMesh((chunk + 31u) / 32u, 1u, 1u);
            offset += chunk;
        }
    }
    return S_OK;
}
