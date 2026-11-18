#include "D3D12Particles.h"
#include "D3D12PipelineCache.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include <d3d12shader.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace rhi = se::rhi;
namespace
{
using Json = nlohmann::json;
constexpr char ParticleArtifactMagic[8]{'S', 'E', 'P', 'R', 'T', '1', '\0', '\0'};

void U32(std::vector<std::uint8_t>& out, UINT value)
{
    for (UINT shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

UINT U32(const std::uint8_t* bytes)
{
    return UINT(bytes[0]) | (UINT(bytes[1]) << 8) | (UINT(bytes[2]) << 16) | (UINT(bytes[3]) << 24);
}

bool ReadBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& output)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto count = input.tellg();
    if (count <= 0 || count > 8 * 1024 * 1024) return false;
    output.resize(static_cast<std::size_t>(count));
    input.seekg(0);
    return static_cast<bool>(input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(count)));
}

bool AtomicWrite(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || bytes.size() > MAXDWORD) return false;
    static std::atomic<UINT64> seq{1};
    auto tmp = path;
    tmp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(seq++);
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

Json ParticleIdentity(const std::filesystem::path& source)
{
    std::vector<std::uint8_t> mainBytes, bindingsBytes;
    if (!ReadBytes(source, mainBytes) || !ReadBytes(source.parent_path() / "ParticleBindings.hlsli", bindingsBytes))
        return {};
    return {
        {"schema", 1},
        {"layout", rhi::ParticleLayoutVersion},
        {"flags", D3D12ShaderCompileFlags() | D3DCOMPILE_IEEE_STRICTNESS},
        {"constants", sizeof(rhi::ParticleFrameConstants)},
        {"instance_stride", sizeof(rhi::ParticleInstanceGpuData)},
        {"source_sha256", se::Sha256(mainBytes)},
        {"bindings_sha256", se::Sha256(bindingsBytes)}
    };
}

bool ValidateShaderStage(ID3DBlob& code, bool vertexStage)
{
    ComPtr<ID3D12ShaderReflection> reflection;
    if (FAILED(D3DReflect(code.GetBufferPointer(), code.GetBufferSize(), IID_PPV_ARGS(reflection.GetAddressOf()))))
        return false;
    D3D12_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc))) return false;
    const UINT expectedType = vertexStage ? D3D12_SHVER_VERTEX_SHADER : D3D12_SHVER_PIXEL_SHADER;
    if (D3D12_SHVER_GET_TYPE(desc.Version) != expectedType || D3D12_SHVER_GET_MAJOR(desc.Version) != 5
        || D3D12_SHVER_GET_MINOR(desc.Version) != 1)
        return false;

    if (!vertexStage) return desc.BoundResources == 0 && desc.OutputParameters == 1;

    bool hasConstants = false, hasInstances = false;
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bind{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &bind)) || bind.Space != 0 || bind.BindCount != 1)
            return false;
        if (bind.Type == D3D_SIT_CBUFFER && bind.BindPoint == 0 && !hasConstants)
        {
            auto* cb = reflection->GetConstantBufferByName("ParticleFrameConstants");
            D3D12_SHADER_BUFFER_DESC cbDesc{};
            if (!cb || FAILED(cb->GetDesc(&cbDesc)) || cbDesc.Size != sizeof(rhi::ParticleFrameConstants) || cbDesc.Variables != 4)
                return false;
            hasConstants = true;
        }
        else if (bind.Type == D3D_SIT_STRUCTURED && bind.BindPoint == 0 && !hasInstances)
        {
            if (bind.NumSamples != sizeof(rhi::ParticleInstanceGpuData)) return false;
            hasInstances = true;
        }
        else return false;
    }
    return hasConstants && hasInstances;
}

bool TryLoadArtifact(const std::filesystem::path& source, ComPtr<ID3DBlob>& vs, ComPtr<ID3DBlob>& ps)
{
    const auto identity = ParticleIdentity(source);
    if (identity.empty()) return false;

    auto artifact = source;
    artifact += ".sebin";
    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(artifact, bytes) || bytes.size() < 20 || std::memcmp(bytes.data(), ParticleArtifactMagic, 8) != 0)
        return false;

    const UINT jsonLen = U32(bytes.data() + 8);
    const UINT vsLen = U32(bytes.data() + 12);
    const UINT psLen = U32(bytes.data() + 16);
    if (!jsonLen || !vsLen || !psLen || jsonLen > 65536
        || UINT64(20) + jsonLen + vsLen + psLen != bytes.size())
        return false;

    try
    {
        const auto metadata = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLen,
            [](int depth, Json::parse_event_t, Json&) {
                if (depth > 32) throw std::invalid_argument("particle metadata depth");
                return true;
            });
        if (metadata.at("identity") != identity) return false;
    }
    catch (const std::exception&)
    {
        return false;
    }

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (FAILED(D3DCreateBlob(vsLen, vsBlob.GetAddressOf())) || FAILED(D3DCreateBlob(psLen, psBlob.GetAddressOf())))
        return false;
    const auto* payload = bytes.data() + 20 + jsonLen;
    std::memcpy(vsBlob->GetBufferPointer(), payload, vsLen);
    std::memcpy(psBlob->GetBufferPointer(), payload + vsLen, psLen);
    if (!ValidateShaderStage(*vsBlob.Get(), true) || !ValidateShaderStage(*psBlob.Get(), false))
        return false;

    vs = std::move(vsBlob);
    ps = std::move(psBlob);
    return true;
}

se::Vec3 SafeNormalized(se::Vec3 value, se::Vec3 fallback) noexcept
{
    const float lenSq = se::LengthSquared(value);
    return lenSq > 1e-6f ? value * (1.0f / std::sqrt(lenSq)) : fallback;
}

void ResolveCameraBasis(const D3D12FrameContent& content, se::Vec3& rightOut, se::Vec3& upOut) noexcept
{
    if (se::LengthSquared(content.particles.cameraRight) > 1e-6f
        && se::LengthSquared(content.particles.cameraUp) > 1e-6f)
    {
        rightOut = SafeNormalized(content.particles.cameraRight, {1.0f, 0.0f, 0.0f});
        upOut = SafeNormalized(content.particles.cameraUp, {0.0f, 1.0f, 0.0f});
        return;
    }
    if (const auto inv = se::Inverse(content.viewProjection))
    {
        const auto center = se::TransformPoint(*inv, {0.0f, 0.0f, 0.5f});
        const auto rightPt = se::TransformPoint(*inv, {0.5f, 0.0f, 0.5f});
        const auto upPt = se::TransformPoint(*inv, {0.0f, 0.5f, 0.5f});
        rightOut = SafeNormalized(rightPt - center, {1.0f, 0.0f, 0.0f});
        upOut = SafeNormalized(upPt - center, {0.0f, 1.0f, 0.0f});
        return;
    }
    rightOut = {1.0f, 0.0f, 0.0f};
    upOut = {0.0f, 1.0f, 0.0f};
}
} // namespace

bool WriteD3D12ParticleShader(const std::filesystem::path& source, const std::filesystem::path& destination, const Localization& text)
{
    const auto identity = ParticleIdentity(source);
    if (identity.empty()) return false;

    ComPtr<ID3DBlob> vs, ps;
    if (!CompileD3D12Shader(source, "VSMain", "vs_5_1", vs, text, D3DCOMPILE_IEEE_STRICTNESS)
        || !CompileD3D12Shader(source, "PSMain", "ps_5_1", ps, text, D3DCOMPILE_IEEE_STRICTNESS)
        || !ValidateShaderStage(*vs.Get(), true) || !ValidateShaderStage(*ps.Get(), false))
        return false;

    const Json metadata{
        {"schema", 1},
        {"identity", identity},
        {"vs_sha256", se::Sha256({static_cast<const std::uint8_t*>(vs->GetBufferPointer()), vs->GetBufferSize()})},
        {"ps_sha256", se::Sha256({static_cast<const std::uint8_t*>(ps->GetBufferPointer()), ps->GetBufferSize()})}
    };
    const auto jsonText = metadata.dump();
    std::vector<std::uint8_t> artifact;
    artifact.insert(artifact.end(), ParticleArtifactMagic, ParticleArtifactMagic + 8);
    U32(artifact, static_cast<UINT>(jsonText.size()));
    U32(artifact, static_cast<UINT>(vs->GetBufferSize()));
    U32(artifact, static_cast<UINT>(ps->GetBufferSize()));
    artifact.insert(artifact.end(), jsonText.begin(), jsonText.end());
    const auto* vsBytes = static_cast<const std::uint8_t*>(vs->GetBufferPointer());
    const auto* psBytes = static_cast<const std::uint8_t*>(ps->GetBufferPointer());
    artifact.insert(artifact.end(), vsBytes, vsBytes + vs->GetBufferSize());
    artifact.insert(artifact.end(), psBytes, psBytes + ps->GetBufferSize());
    if (!AtomicWrite(destination, artifact)) return false;

    auto sidecarPath = destination;
    sidecarPath += ".json";
    const auto sidecarBytes = metadata.dump(2);
    return AtomicWrite(sidecarPath, {reinterpret_cast<const std::uint8_t*>(sidecarBytes.data()), sidecarBytes.size()});
}

bool D3D12Particles::Initialize(ID3D12Device& device, const std::filesystem::path& shader, const Localization& text)
{
    if (root) return true;
    const auto check = [&](HRESULT hr) { return CheckGraphicsResult(hr, text, "D3D12.ParticlePassFailed"); };

    ComPtr<ID3DBlob> vs, ps;
    if (!TryLoadArtifact(shader, vs, ps))
    {
        if (!CompileD3D12Shader(shader, "VSMain", "vs_5_1", vs, text, D3DCOMPILE_IEEE_STRICTNESS)
            || !CompileD3D12Shader(shader, "PSMain", "ps_5_1", ps, text, D3DCOMPILE_IEEE_STRICTNESS)
            || !ValidateShaderStage(*vs.Get(), true) || !ValidateShaderStage(*ps.Get(), false))
        {
            return false;
        }
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &srvRange};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized, error;
    ComPtr<ID3D12RootSignature> nextRoot;
    if (!check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error))
        || !check(device.CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(nextRoot.GetAddressOf()))))
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = nextRoot.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc = {1, 0};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};

    auto& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;

    ComPtr<ID3D12PipelineState> alphaPso, additivePso;
    if (!check(device.CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(alphaPso.GetAddressOf())))) return false;

    blend.DestBlend = D3D12_BLEND_ONE;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    if (!check(device.CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(additivePso.GetAddressOf())))) return false;

    root = std::move(nextRoot);
    alphaPipeline = std::move(alphaPso);
    additivePipeline = std::move(additivePso);
    return true;
}

bool RecordD3D12Particles(D3D12Presentation& p, D3D12FrameContext& frame, const D3D12FrameContent& content,
    D3D12StateRecording& states, ID3D12Resource& color, ID3D12Resource& depth,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv,
    UINT width, UINT height, const Localization& text)
{
    if (content.particles.batches.empty()) return true;

    std::size_t totalInstances = 0;
    for (const auto& batch : content.particles.batches) totalInstances += batch.instances.size();
    if (totalInstances == 0) return true;

    const auto shader = !content.particleShader.empty()
        ? content.particleShader
        : content.outputPipeline && content.outputPipeline->prepared
            ? content.outputPipeline->prepared->program->source.path.parent_path() / "ParticleBillboard.hlsl"
            : std::filesystem::path{};
    if (shader.empty()) return true;

    ComPtr<ID3D12Device> device;
    if (!CheckGraphicsResult(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())), text, "D3D12.ParticlePassFailed"))
        return false;
    if (!p.particlePass) p.particlePass = std::make_shared<D3D12Particles>();
    if (!p.particlePass->Initialize(*device.Get(), shader, text)) return false;

    auto& commands = *p.commandList.Get();
    const auto check = [&](HRESULT hr) { return CheckGraphicsResult(hr, text, "D3D12.ParticlePassFailed"); };
    const auto use = [&](ID3D12Resource& resource, D3D12_RESOURCE_STATES state, const char* reason)
    {
        return CheckGraphicsResult(states.Use(commands, resource, state, reason), text, "D3D12.StateTrackingFailed");
    };

    if (!use(depth, D3D12_RESOURCE_STATE_DEPTH_READ, "Particles.Depth")
        || !use(color, D3D12_RESOURCE_STATE_RENDER_TARGET, "Particles.HDR"))
        return false;

    constexpr UINT stride = sizeof(rhi::ParticleInstanceGpuData);
    const UINT64 allocBytes = UINT64(totalInstances) * stride + stride;
    D3D12UploadSlice instanceUpload{};
    if (!CheckGraphicsResult(frame.uploads.Allocate(allocBytes, 16, instanceUpload), text, "D3D12.UploadCapacityExceeded"))
        return false;
    if (!use(*instanceUpload.resource, D3D12_RESOURCE_STATE_GENERIC_READ, "Particles.Instances"))
        return false;

    const UINT64 remainder = instanceUpload.offset % stride;
    const UINT64 padding = remainder == 0 ? 0 : (stride - remainder);
    const UINT64 alignedOffset = instanceUpload.offset + padding;
    auto* dst = reinterpret_cast<rhi::ParticleInstanceGpuData*>(instanceUpload.cpu + padding);

    std::size_t cursor = 0;
    for (const auto& batch : content.particles.batches)
    {
        if (batch.instances.empty()) continue;
        std::memcpy(dst + cursor, batch.instances.data(), batch.instances.size() * stride);
        cursor += batch.instances.size();
    }

    se::Vec3 cameraRight{}, cameraUp{};
    ResolveCameraBasis(content, cameraRight, cameraUp);

    rhi::ParticleFrameConstants frameConstants{};
    frameConstants.clipFromWorld = content.viewProjection;
    frameConstants.cameraRight[0] = cameraRight.x;
    frameConstants.cameraRight[1] = cameraRight.y;
    frameConstants.cameraRight[2] = cameraRight.z;
    frameConstants.cameraUp[0] = cameraUp.x;
    frameConstants.cameraUp[1] = cameraUp.y;
    frameConstants.cameraUp[2] = cameraUp.z;
    frameConstants.viewport[0] = static_cast<float>(width);
    frameConstants.viewport[1] = static_cast<float>(height);
    frameConstants.viewport[2] = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
    frameConstants.viewport[3] = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;

    D3D12UploadSlice constantUpload{};
    if (!CheckGraphicsResult(frame.uploads.Allocate(256, 256, constantUpload), text, "D3D12.UploadCapacityExceeded"))
        return false;
    std::memset(constantUpload.cpu, 0, 256);
    std::memcpy(constantUpload.cpu, &frameConstants, sizeof(frameConstants));
    if (!use(*constantUpload.resource, D3D12_RESOURCE_STATE_GENERIC_READ, "Particles.Constants"))
        return false;

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvHandles;
    std::vector<std::shared_ptr<D3D12DescriptorRange>> srvRanges;
    std::size_t batchFirst = 0;
    for (const auto& batch : content.particles.batches)
    {
        if (batch.instances.empty()) continue;
        std::shared_ptr<D3D12DescriptorRange> range;
        if (!CheckGraphicsResult(p.srvPool->Allocate(1, range), text, "D3D12.DescriptorCapacityExceeded"))
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = (alignedOffset / stride) + batchFirst;
        srv.Buffer.NumElements = static_cast<UINT>(batch.instances.size());
        srv.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(instanceUpload.resource, &srv, range->Cpu());
        srvHandles.push_back(range->Cpu());
        srvRanges.push_back(std::move(range));
        batchFirst += batch.instances.size();
    }

    if (!CheckGraphicsResult(frame.particleDescriptors.Prepare(*device.Get(), srvHandles, 1), text, "D3D12.DescriptorCapacityExceeded"))
        return false;

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands.RSSetViewports(1, &viewport);
    commands.RSSetScissorRects(1, &scissor);
    commands.OMSetRenderTargets(1, &rtv, FALSE, &readOnlyDsv);

    ID3D12DescriptorHeap* heaps[]{frame.particleDescriptors.Heap()};
    commands.SetDescriptorHeaps(1, heaps);
    commands.SetGraphicsRootSignature(p.particlePass->root.Get());
    commands.SetGraphicsRootConstantBufferView(0, constantUpload.Gpu());
    commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT drawIndex = 0;
    for (const auto& batch : content.particles.batches)
    {
        if (batch.instances.empty()) continue;
        commands.SetPipelineState((batch.additive ? p.particlePass->additivePipeline : p.particlePass->alphaPipeline).Get());
        commands.SetGraphicsRootDescriptorTable(1, frame.particleDescriptors.Draw(drawIndex++));
        commands.DrawInstanced(6u, static_cast<UINT>(batch.instances.size()), 0u, 0u);
    }
    return check(S_OK);
}
