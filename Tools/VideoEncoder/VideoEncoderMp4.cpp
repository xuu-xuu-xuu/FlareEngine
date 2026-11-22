#include "VideoEncoderMp4.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <Windows.h>
#include <objbase.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace flare::videoencoder
{
// ---- MP4 时间戳归一（E20） ----
// Media Foundation 的 muxer 把“写出时刻”写进 mvhd/tkhd/mdhd 的 creation/modification 字段，
// 于是同一批帧两次编码只有这几字节不同。这里按 ISO BMFF 盒子结构定位这些字段并置零，
// 让“同帧 → 同字节”成立；盒子结构不合法就原样返回、不猜偏移。
namespace
{
constexpr std::uint32_t MakeFourCC(char a,char b,char c,char d){return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a))<<24)|(static_cast<std::uint32_t>(static_cast<std::uint8_t>(b))<<16)|(static_cast<std::uint32_t>(static_cast<std::uint8_t>(c))<<8)|static_cast<std::uint32_t>(static_cast<std::uint8_t>(d));}
struct Mp4Box
{
    std::uint64_t size = 0;
    std::uint32_t type = 0;
    std::size_t payload = 0;
    std::size_t end = 0;
};

std::uint32_t ReadBigEndian32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

bool ReadBox(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t limit, Mp4Box& box)
{
    if (offset + 8 > limit) return false;
    std::uint64_t size = ReadBigEndian32(bytes.data() + offset);
    const std::uint32_t type = ReadBigEndian32(bytes.data() + offset + 4);
    std::size_t header = 8;
    if (size == 1)
    {
        if (offset + 16 > limit) return false;
        size = (static_cast<std::uint64_t>(ReadBigEndian32(bytes.data() + offset + 8)) << 32) |
               ReadBigEndian32(bytes.data() + offset + 12);
        header = 16;
    }
    else if (size == 0)
    {
        size = limit - offset;
    }
    if (size < header || offset + size > limit) return false;
    box.size = size;
    box.type = type;
    box.payload = offset + header;
    box.end = static_cast<std::size_t>(offset + size);
    return true;
}

// 把 creation/modification 写成固定值（1904 起算的 0 = 未设时间）。
void ZeroTimes(std::vector<std::uint8_t>& bytes, const Mp4Box& box)
{
    if (box.payload + 4 > box.end) return;
    const std::uint8_t version = bytes[box.payload];
    if (version != 0) return;  // v1 用 64 位时间，本项目产出的都是 v0
    const std::size_t stamps = box.payload + 4;  // version(1) + flags(3)
    if (stamps + 8 > box.end) return;
    std::memset(bytes.data() + stamps, 0, 8);
}
}
namespace
{
using Microsoft::WRL::ComPtr;

constexpr LONGLONG kOneSecond = 10'000'000;
constexpr std::uint32_t kH264ProfileHigh = 100; // eAVEncH264VProfile_High
constexpr std::uint32_t kMaxEncodeDimension = 8192;
constexpr std::uint64_t kMaxDecodedPixels = 8192ull * 8192ull;
constexpr std::uint64_t kMaxSampleReads = 10'000'000;
// 时间戳以 100ns 为单位；整数除法让相邻帧间隔相差 1 tick 属于正常。
constexpr LONGLONG kTimestampTolerance = 2;
// 编码器自备样本时（MFT_OUTPUT_STREAM_PROVIDES_SAMPLES 未置位但 cbSize 为 0）的兜底大小。
constexpr std::uint32_t kFallbackEncoderBufferBytes = 4u * 1024u * 1024u;

bool FailVerify(Diagnostic& diagnostic, std::string detail)
{
    return Fail(diagnostic, "Encoder.VerificationFailed", std::move(detail),
        ExitCode::VerifyFailed);
}

bool MergeUint32Attribute(IMFMediaType* type, const GUID& key, UINT32& target)
{
    UINT32 value = 0;
    if (type != nullptr && SUCCEEDED(type->GetUINT32(key, &value)) && value != 0)
    {
        target = value;
        return true;
    }
    return false;
}

void MergeGeometry(IMFMediaType* type, UINT32& width, UINT32& height)
{
    UINT32 sizeWidth = 0;
    UINT32 sizeHeight = 0;
    if (type != nullptr &&
        SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &sizeWidth, &sizeHeight)) &&
        sizeWidth != 0 && sizeHeight != 0)
    {
        width = sizeWidth;
        height = sizeHeight;
    }
}

void MergeFrameRate(IMFMediaType* type, UINT32& numerator, UINT32& denominator)
{
    UINT32 rateNumerator = 0;
    UINT32 rateDenominator = 0;
    if (type != nullptr &&
        SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rateNumerator,
            &rateDenominator)) &&
        rateNumerator != 0 && rateDenominator != 0)
    {
        numerator = rateNumerator;
        denominator = rateDenominator;
    }
}

struct StreamMetadata
{
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsNumerator = 0;
    UINT32 fpsDenominator = 0;
    UINT32 bitrate = 0;
    UINT32 profile = 0;
    UINT64 duration = 0;
};

double FpsFromRatio(const StreamMetadata& metadata)
{
    if (metadata.fpsNumerator == 0 || metadata.fpsDenominator == 0)
    {
        return 0.0;
    }
    return static_cast<double>(metadata.fpsNumerator) /
        static_cast<double>(metadata.fpsDenominator);
}

bool FillVideoType(IMFMediaType* type, const GUID& subtype, const EncodeSettings& settings,
    Diagnostic& diagnostic)
{
    HRESULT result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result))
    {
        result = type->SetGUID(MF_MT_SUBTYPE, subtype);
    }
    if (SUCCEEDED(result))
    {
        result = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, settings.width, settings.height);
    }
    if (SUCCEEDED(result))
    {
        result = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, settings.fps, 1);
    }
    if (SUCCEEDED(result))
    {
        result = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot configure media type " + std::to_string(settings.width) + "x" +
                std::to_string(settings.height) + " @" + std::to_string(settings.fps) + "fps",
            "IMFMediaType::SetGUID/SetUINT32", result);
    }
    return true;
}

bool ConfigureNv12InputType(IMFMediaType* type, const EncodeSettings& settings,
    Diagnostic& diagnostic)
{
    if (!FillVideoType(type, MFVideoFormat_NV12, settings, diagnostic))
    {
        return false;
    }
    const std::uint64_t lumaBytes = static_cast<std::uint64_t>(settings.width) *
        static_cast<std::uint64_t>(settings.height);
    const std::uint64_t frameBytes = lumaBytes + lumaBytes / 2u;
    if (frameBytes > 0xFFFFFFFFull)
    {
        return Fail(diagnostic, "Encoder.MediaFoundationCallFailed",
            "NV12 frame exceeds a single media buffer", ExitCode::EncodeFailed);
    }

    HRESULT result = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_DEFAULT_STRIDE, settings.width);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(frameBytes));
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot configure NV12 input media type", "IMFMediaType::SetUINT32", result);
    }
    return true;
}

// 输出类型：H.264 High + 固定 GOP。minimal 版本去掉可选属性，用于兼容旧编码器。
bool ConfigureCompressedOutputType(IMFMediaType* type, const EncodeSettings& settings,
    bool minimal, Diagnostic& diagnostic)
{
    if (!FillVideoType(type, MFVideoFormat_H264, settings, diagnostic))
    {
        return false;
    }
    HRESULT result = type->SetUINT32(MF_MT_AVG_BITRATE, settings.bitrate);
    if (SUCCEEDED(result) && !minimal)
    {
        result = type->SetUINT32(MF_MT_MPEG2_PROFILE, kH264ProfileHigh);
    }
    if (SUCCEEDED(result) && !minimal)
    {
        result = type->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, settings.keyframeInterval);
    }
    if (SUCCEEDED(result) && !minimal)
    {
        result = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    }
    if (SUCCEEDED(result) && !minimal)
    {
        result = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    }
    if (SUCCEEDED(result) && !minimal)
    {
        result = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    }
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot configure the H.264 output media type", "IMFMediaType::SetUINT32",
            result);
    }
    return true;
}

// ICodecAPI 是唯一可靠的编码器控制入口：固定 GOP、禁用 B 帧、CBR 码率控制。
bool ConfigureEncoderCodecApi(ICodecAPI* codec, const EncodeSettings& settings,
    EncodeResult& result, Diagnostic& diagnostic)
{
    const auto setUint32 = [codec](const GUID& api, UINT32 value) {
        VARIANT variant = {};
        variant.vt = VT_UI4;
        variant.ulVal = value;
        return codec->SetValue(&api, &variant);
    };

    HRESULT hr = setUint32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    if (FAILED(hr))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the H.264 encoder rejected B-frame count 0",
            "ICodecAPI::SetValue(CODECAPI_AVEncMPVDefaultBPictureCount)", hr);
    }
    hr = setUint32(CODECAPI_AVEncMPVGOPSize, settings.keyframeInterval);
    if (FAILED(hr))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the H.264 encoder rejected the keyframe interval",
            "ICodecAPI::SetValue(CODECAPI_AVEncMPVGOPSize)", hr);
    }
    hr = setUint32(CODECAPI_AVEncCommonRateControlMode,
        eAVEncCommonRateControlMode_CBR);
    if (SUCCEEDED(hr))
    {
        result.rateControlMode = "CBR";
        if (FAILED(setUint32(CODECAPI_AVEncCommonMeanBitRate, settings.bitrate)))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                "the H.264 encoder rejected the target bitrate",
                "ICodecAPI::SetValue(CODECAPI_AVEncCommonMeanBitRate)", hr);
        }
        // 一秒 VBV 缓冲；编码器可拒绝该可选设置，不影响 CBR 主路径。
        setUint32(CODECAPI_AVEncCommonBufferSize, settings.bitrate);
    }
    else
    {
        result.rateControlMode = "average";
    }
    // High profile 下启用 CABAC；失败只是少一个质量选项，不算编码失败。
    setUint32(CODECAPI_AVEncH264CABACEnable, TRUE);
    // 单线程编码：Media Foundation 的 H.264 MFT 默认多线程，同一批帧两次编码的字节会不同，
    // 交付就永远拿不到可复现的哈希。编码器可拒绝该设置，此时如实记在多线程标记里。
    result.singleThreaded = SUCCEEDED(setUint32(CODECAPI_AVEncNumWorkerThreads, 1));
    return true;
}

// 遍历盒子树，把 moov 下的三个时间戳盒子清零；返回是否改动过。
bool NormalizeMp4TimestampsInternal(const std::filesystem::path& path, Diagnostic& diagnostic)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            "cannot reopen " + PathText(path) + " to normalize timestamps", ExitCode::OutputNotWritable);
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    if (bytes.size() < 16)
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            "encoded file is too small to contain an MP4 header: " + PathText(path),
            ExitCode::OutputNotWritable);
    }

    bool touched = false;
    const auto walk = [&](auto&& self, std::size_t offset, std::size_t limit, int depth) -> void {
        while (offset + 8 <= limit)
        {
            Mp4Box box;
            if (!ReadBox(bytes, offset, limit, box)) return;
            if (box.type == MakeFourCC('m', 'v', 'h', 'd') || box.type == MakeFourCC('t', 'k', 'h', 'd') ||
                box.type == MakeFourCC('m', 'd', 'h', 'd'))
            {
                ZeroTimes(bytes, box);
                touched = true;
            }
            else if (depth < 3 && (box.type == MakeFourCC('m', 'o', 'o', 'v') || box.type == MakeFourCC('t', 'r', 'a', 'k') ||
                box.type == MakeFourCC('m', 'd', 'i', 'a')))
            {
                self(self, box.payload, box.end, depth + 1);
            }
            offset = box.end;
        }
    };
    walk(walk, 0, bytes.size(), 0);
    if (!touched) return true;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            "cannot rewrite " + PathText(path) + " after normalizing timestamps", ExitCode::OutputNotWritable);
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            "short write while normalizing " + PathText(path), ExitCode::OutputNotWritable);
    }
    return true;
}
// 重新套用输出类型：编码器在首个样本后会发 MF_E_TRANSFORM_STREAM_CHANGE。
bool ReapplyEncoderOutputType(IMFTransform* encoder, IMFMediaType* outputType,
    MFT_OUTPUT_STREAM_INFO& streamInfo, Diagnostic& diagnostic)
{
    HRESULT result = encoder->SetOutputType(0, outputType, 0);
    if (FAILED(result))
    {
        ComPtr<IMFMediaType> available;
        result = encoder->GetOutputAvailableType(0, 0, &available);
        if (SUCCEEDED(result) && available)
        {
            UINT32 availableWidth = 0;
            UINT32 availableHeight = 0;
            if (FAILED(MFGetAttributeSize(available.Get(), MF_MT_FRAME_SIZE, &availableWidth,
                    &availableHeight)) ||
                availableWidth == 0 || availableHeight == 0)
            {
                UINT32 width = 0;
                UINT32 height = 0;
                if (outputType != nullptr &&
                    SUCCEEDED(MFGetAttributeSize(outputType, MF_MT_FRAME_SIZE, &width,
                        &height)))
                {
                    MFSetAttributeSize(available.Get(), MF_MT_FRAME_SIZE, width, height);
                }
            }
            result = encoder->SetOutputType(0, available.Get(), 0);
        }
    }
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot renegotiate the encoder output type",
            "IMFTransform::SetOutputType", result);
    }
    result = encoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot query the encoder output stream", "IMFTransform::GetOutputStreamInfo",
            result);
    }
    return true;
}

bool OpenVideoReader(const std::filesystem::path& file, bool advancedProcessing,
    ComPtr<IMFSourceReader>& reader, DWORD& streamIndex, StreamMetadata& metadata,
    Diagnostic& diagnostic)
{
    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 2);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create source reader attributes", "MFCreateAttributes", result);
    }
    const GUID& processingAttribute = advancedProcessing
        ? MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
        : MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING;
    result = attributes->SetUINT32(processingAttribute, TRUE);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot enable source reader video processing",
            "IMFAttributes::SetUINT32", result);
    }
    result = MFCreateSourceReaderFromURL(file.c_str(), attributes.Get(), &reader);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot open MP4 with the source reader: " + PathText(file),
            "MFCreateSourceReaderFromURL", result);
    }

    ComPtr<IMFMediaSource> mediaSource;
    result = reader->GetServiceForStream(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
        GUID_NULL, __uuidof(IMFMediaSource),
        reinterpret_cast<void**>(mediaSource.GetAddressOf()));
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot query the MP4 media source", "IMFSourceReader::GetServiceForStream",
            result);
    }
    ComPtr<IMFPresentationDescriptor> presentation;
    result = mediaSource->CreatePresentationDescriptor(&presentation);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot read the MP4 presentation descriptor",
            "IMFMediaSource::CreatePresentationDescriptor", result);
    }

    DWORD streamCount = 0;
    result = presentation->GetStreamDescriptorCount(&streamCount);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot enumerate MP4 streams",
            "IMFPresentationDescriptor::GetStreamDescriptorCount", result);
    }

    bool foundVideo = false;
    for (DWORD index = 0; index < streamCount; ++index)
    {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> streamDescriptor;
        result = presentation->GetStreamDescriptorByIndex(index, &selected,
            &streamDescriptor);
        if (FAILED(result))
        {
            continue;
        }
        ComPtr<IMFMediaTypeHandler> handler;
        result = streamDescriptor->GetMediaTypeHandler(&handler);
        if (FAILED(result))
        {
            continue;
        }
        GUID majorType = GUID_NULL;
        result = handler->GetMajorType(&majorType);
        if (FAILED(result) || !IsEqualGUID(majorType, MFMediaType_Video))
        {
            continue;
        }

        streamIndex = index;
        foundVideo = true;
        ComPtr<IMFMediaType> handlerType;
        if (SUCCEEDED(handler->GetMediaTypeByIndex(0, &handlerType)))
        {
            handlerType->GetGUID(MF_MT_SUBTYPE, &metadata.subtype);
            MergeGeometry(handlerType.Get(), metadata.width, metadata.height);
            MergeFrameRate(handlerType.Get(), metadata.fpsNumerator,
                metadata.fpsDenominator);
            MergeUint32Attribute(handlerType.Get(), MF_MT_AVG_BITRATE, metadata.bitrate);
            MergeUint32Attribute(handlerType.Get(), MF_MT_MPEG2_PROFILE, metadata.profile);
        }
        ComPtr<IMFMediaType> nativeType;
        if (SUCCEEDED(reader->GetNativeMediaType(index, 0, &nativeType)))
        {
            nativeType->GetGUID(MF_MT_SUBTYPE, &metadata.subtype);
            MergeGeometry(nativeType.Get(), metadata.width, metadata.height);
            MergeFrameRate(nativeType.Get(), metadata.fpsNumerator,
                metadata.fpsDenominator);
            MergeUint32Attribute(nativeType.Get(), MF_MT_AVG_BITRATE, metadata.bitrate);
            MergeUint32Attribute(nativeType.Get(), MF_MT_MPEG2_PROFILE, metadata.profile);
        }
        break;
    }
    if (!foundVideo)
    {
        return FailVerify(diagnostic, "MP4 has no video stream: " + PathText(file));
    }

    result = reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(result))
    {
        result = reader->SetStreamSelection(streamIndex, TRUE);
    }
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot select the MP4 video stream", "IMFSourceReader::SetStreamSelection",
            result);
    }

    UINT64 duration = 0;
    if (SUCCEEDED(presentation->GetUINT64(MF_PD_DURATION, &duration)))
    {
        metadata.duration = duration;
    }
    return true;
}

bool CopySampleToRgb32(IMFSample* sample, UINT32 width, UINT32 height, LONG strideHint,
    std::vector<std::uint8_t>& bgra, Diagnostic& diagnostic)
{
    if (width == 0 || height == 0)
    {
        return FailVerify(diagnostic, "decoded frame size is zero");
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    bgra.resize(rowBytes * static_cast<std::size_t>(height));

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(result) || !buffer)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot read the decoded sample buffer", "IMFSample::GetBufferByIndex", result);
    }

    BYTE* scanline = nullptr;
    LONG pitch = 0;
    bool locked2d = false;
    DWORD currentLength = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)))
    {
        if (SUCCEEDED(buffer2d->Lock2D(&scanline, &pitch)))
        {
            locked2d = true;
        }
    }
    if (!locked2d)
    {
        DWORD maxLength = 0;
        result = buffer->Lock(&scanline, &maxLength, &currentLength);
        if (FAILED(result))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                "cannot lock the decoded sample buffer", "IMFMediaBuffer::Lock", result);
        }
        pitch = strideHint != 0 ? strideHint : static_cast<LONG>(rowBytes);
    }

    const LONG absolutePitch = pitch < 0 ? -pitch : pitch;
    if (pitch == 0 || absolutePitch < static_cast<LONG>(rowBytes) ||
        absolutePitch > (1 << 20))
    {
        if (locked2d)
        {
            buffer2d->Unlock2D();
        }
        else
        {
            buffer->Unlock();
        }
        return FailVerify(diagnostic, "decoded buffer stride is invalid: " +
            std::to_string(absolutePitch));
    }
    if (!locked2d)
    {
        const std::uint64_t required = static_cast<std::uint64_t>(height - 1) *
            static_cast<std::uint64_t>(absolutePitch) + rowBytes;
        if (static_cast<std::uint64_t>(currentLength) < required)
        {
            buffer->Unlock();
            return FailVerify(diagnostic, "decoded buffer is shorter than one frame");
        }
    }

    const BYTE* rowBase = scanline;
    if (pitch < 0)
    {
        rowBase += static_cast<std::ptrdiff_t>(height - 1) * absolutePitch;
    }
    for (UINT32 row = 0; row < height; ++row)
    {
        std::memcpy(bgra.data() + static_cast<std::size_t>(row) * rowBytes,
            rowBase + static_cast<std::ptrdiff_t>(row) * pitch, rowBytes);
    }
    if (locked2d)
    {
        buffer2d->Unlock2D();
    }
    else
    {
        buffer->Unlock();
    }
    return true;
}

bool ConfigureRgb32(IMFSourceReader* reader, DWORD streamIndex,
    const StreamMetadata& metadata, UINT32& decodeWidth, UINT32& decodeHeight,
    LONG& stride, Diagnostic& diagnostic)
{
    if (metadata.width == 0 || metadata.height == 0)
    {
        return FailVerify(diagnostic, "cannot determine MP4 frame size");
    }
    ComPtr<IMFMediaType> type;
    HRESULT result = MFCreateMediaType(&type);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create RGB32 media type", "MFCreateMediaType", result);
    }
    result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result))
    {
        result = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(result))
    {
        result = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, metadata.width,
            metadata.height);
    }
    if (SUCCEEDED(result))
    {
        result = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(result))
    {
        result = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result) && metadata.fpsNumerator != 0 && metadata.fpsDenominator != 0)
    {
        result = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, metadata.fpsNumerator,
            metadata.fpsDenominator);
    }
    if (SUCCEEDED(result))
    {
        result = reader->SetCurrentMediaType(streamIndex, nullptr, type.Get());
    }
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot configure RGB32 decoding", "IMFSourceReader::SetCurrentMediaType",
            result);
    }

    ComPtr<IMFMediaType> actual;
    result = reader->GetCurrentMediaType(streamIndex, &actual);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot read the decoded media type", "IMFSourceReader::GetCurrentMediaType",
            result);
    }
    GUID subtype = GUID_NULL;
    if (FAILED(actual->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        !IsEqualGUID(subtype, MFVideoFormat_RGB32))
    {
        return FailVerify(diagnostic, "decoder did not produce RGB32 frames");
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        width == 0 || height == 0)
    {
        width = metadata.width;
        height = metadata.height;
    }
    // 解码缓冲可能按 16 像素对齐（1080 -> 1088）；显示尺寸以容器/原生类型为准。
    if (width < metadata.width || height < metadata.height)
    {
        return FailVerify(diagnostic, "decoder output is smaller than the container size: " +
            std::to_string(width) + "x" + std::to_string(height));
    }
    width = metadata.width;
    height = metadata.height;
    if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
        kMaxDecodedPixels)
    {
        return FailVerify(diagnostic, "decoded frame size is too large: " +
            std::to_string(width) + "x" + std::to_string(height));
    }

    UINT32 strideAttribute = 0;
    if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttribute)) &&
        strideAttribute != 0)
    {
        stride = static_cast<LONG>(strideAttribute);
    }
    else
    {
        stride = static_cast<LONG>(width * 4u);
    }
    decodeWidth = width;
    decodeHeight = height;
    return true;
}

bool RefreshDecodedFormat(IMFSourceReader* reader, DWORD streamIndex,
    const StreamMetadata& metadata, UINT32& decodeWidth, UINT32& decodeHeight,
    LONG& stride, Diagnostic& diagnostic)
{
    ComPtr<IMFMediaType> actual;
    HRESULT result = reader->GetCurrentMediaType(streamIndex, &actual);
    if (FAILED(result))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot refresh the decoded media type",
            "IMFSourceReader::GetCurrentMediaType", result);
    }
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        width < metadata.width || height < metadata.height)
    {
        return FailVerify(diagnostic, "decoded video geometry changed during playback");
    }
    width = metadata.width;
    height = metadata.height;
    UINT32 strideAttribute = 0;
    if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttribute)) &&
        strideAttribute != 0)
    {
        stride = static_cast<LONG>(strideAttribute);
    }
    decodeWidth = width;
    decodeHeight = height;
    return true;
}

bool ReadDecodedSamples(const std::filesystem::path& file, UINT32 expectedFps,
    VerificationResult& result, std::vector<std::uint8_t>& firstFrame,
    std::vector<std::uint8_t>& lastFrame, Diagnostic& diagnostic)
{
    if (expectedFps == 0)
    {
        return FailVerify(diagnostic, "expected frame rate is zero");
    }
    ComPtr<IMFSourceReader> reader;
    DWORD streamIndex = 0;
    StreamMetadata metadata;
    if (!OpenVideoReader(file, false, reader, streamIndex, metadata, diagnostic))
    {
        return false;
    }

    UINT32 decodeWidth = 0;
    UINT32 decodeHeight = 0;
    LONG stride = 0;
    if (!ConfigureRgb32(reader.Get(), streamIndex, metadata, decodeWidth, decodeHeight,
            stride, diagnostic))
    {
        // 部分机器的普通视频处理链不支持 RGB32，换高级视频处理重试一次。
        Diagnostic advancedDiagnostic;
        if (!OpenVideoReader(file, true, reader, streamIndex, metadata, advancedDiagnostic))
        {
            return false;
        }
        if (!ConfigureRgb32(reader.Get(), streamIndex, metadata, decodeWidth, decodeHeight,
                stride, diagnostic))
        {
            return false;
        }
    }

    std::uint64_t sampleCount = 0;
    LONGLONG firstTimestamp = 0;
    LONGLONG lastTimestamp = 0;
    LONGLONG lastDuration = 0;
    LONGLONG minimumDelta = std::numeric_limits<LONGLONG>::max();
    LONGLONG maximumDelta = 0;
    bool constant = true;
    const LONGLONG nominalDelta = kOneSecond / static_cast<LONGLONG>(expectedFps);

    for (std::uint64_t guard = 0; guard < kMaxSampleReads; ++guard)
    {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT readResult = reader->ReadSample(streamIndex, 0, nullptr, &flags,
            &timestamp, sample.GetAddressOf());
        if (FAILED(readResult))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                "cannot read a decoded video sample", "IMFSourceReader::ReadSample",
                readResult);
        }
        if ((flags & MF_SOURCE_READERF_ERROR) != 0)
        {
            return FailVerify(diagnostic, "source reader reported a decode error");
        }

        if (sample)
        {
            if (sampleCount > 0)
            {
                const LONGLONG delta = timestamp - lastTimestamp;
                if (delta <= 0)
                {
                    return FailVerify(diagnostic,
                        "decoded timestamps are not strictly increasing at sample " +
                            std::to_string(sampleCount + 1));
                }
                minimumDelta = std::min(minimumDelta, delta);
                maximumDelta = std::max(maximumDelta, delta);
                if (delta < nominalDelta - kTimestampTolerance ||
                    delta > nominalDelta + kTimestampTolerance)
                {
                    constant = false;
                }
            }
            else
            {
                firstTimestamp = timestamp;
            }
            lastTimestamp = timestamp;
            LONGLONG duration = 0;
            if (SUCCEEDED(sample->GetSampleDuration(&duration)) && duration > 0)
            {
                lastDuration = duration;
            }
            if (!CopySampleToRgb32(sample.Get(), decodeWidth, decodeHeight, stride,
                    lastFrame, diagnostic))
            {
                return false;
            }
            if (sampleCount == 0)
            {
                firstFrame = lastFrame;
            }
            ++sampleCount;
        }

        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
        {
            if (!RefreshDecodedFormat(reader.Get(), streamIndex, metadata, decodeWidth,
                    decodeHeight, stride, diagnostic))
            {
                return false;
            }
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            break;
        }
    }

    result.decodedSamples = sampleCount;
    result.width = decodeWidth;
    result.height = decodeHeight;
    result.containerFps = FpsFromRatio(metadata);
    result.bitrate = metadata.bitrate;
    result.profile = metadata.profile;
    result.firstTimestampSeconds = static_cast<double>(firstTimestamp) /
        static_cast<double>(kOneSecond);
    result.lastTimestampSeconds = static_cast<double>(lastTimestamp) /
        static_cast<double>(kOneSecond);
    result.durationSeconds = static_cast<double>(metadata.duration) /
        static_cast<double>(kOneSecond);
    if (lastDuration == 0 && sampleCount > 0)
    {
        lastDuration = nominalDelta;
    }
    const LONGLONG span = lastTimestamp + lastDuration - firstTimestamp;
    result.measuredFps = span > 0
        ? (static_cast<double>(sampleCount) * static_cast<double>(kOneSecond)) /
            static_cast<double>(span)
        : 0.0;
    result.constantFrameRate = constant && sampleCount > 1 &&
        minimumDelta != std::numeric_limits<LONGLONG>::max() &&
        (maximumDelta - minimumDelta) <= kTimestampTolerance;
    result.firstFrameSha256 = Sha256Hex(firstFrame);
    result.lastFrameSha256 = Sha256Hex(lastFrame);
    result.firstLastDiffer = firstFrame != lastFrame;
    return true;
}

// 只读取压缩样本的 CleanPoint 与时间戳，作为 GOP 与 B 帧重排的辅助证据。
bool CollectCompressedStatistics(const std::filesystem::path& file,
    VerificationResult& result)
{
    try
    {
        ComPtr<IMFSourceReader> reader;
        DWORD streamIndex = 0;
        StreamMetadata metadata;
        Diagnostic diagnostic;
        if (!OpenVideoReader(file, false, reader, streamIndex, metadata, diagnostic))
        {
            result.compressedNote = diagnostic.key + ": " + diagnostic.detail;
            return false;
        }
        ComPtr<IMFMediaType> nativeType;
        HRESULT resultCode = reader->GetNativeMediaType(streamIndex, 0, &nativeType);
        if (FAILED(resultCode) || !nativeType)
        {
            result.compressedNote = "GetNativeMediaType failed";
            return false;
        }
        resultCode = reader->SetCurrentMediaType(streamIndex, nullptr, nativeType.Get());
        if (FAILED(resultCode))
        {
            result.compressedNote = "the compressed media type was rejected";
            return false;
        }

        std::uint64_t sampleCount = 0;
        std::uint64_t keyframeCount = 0;
        std::uint64_t lastKeyframeIndex = 0;
        bool haveKeyframe = false;
        std::uint32_t maximumInterval = 0;
        LONGLONG previousTimestamp = 0;
        bool haveTimestamp = false;
        bool monotonic = true;

        for (std::uint64_t guard = 0; guard < kMaxSampleReads; ++guard)
        {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            const HRESULT readResult = reader->ReadSample(streamIndex, 0, nullptr, &flags,
                &timestamp, sample.GetAddressOf());
            if (FAILED(readResult) || (flags & MF_SOURCE_READERF_ERROR) != 0)
            {
                result.compressedNote = "cannot read compressed samples";
                return false;
            }
            if (sample)
            {
                UINT32 cleanPoint = 0;
                const bool isKeyframe = SUCCEEDED(sample->GetUINT32(
                    MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0;
                if (isKeyframe)
                {
                    ++keyframeCount;
                    if (haveKeyframe)
                    {
                        const std::uint64_t interval = sampleCount - lastKeyframeIndex;
                        maximumInterval = static_cast<std::uint32_t>(std::max<std::uint64_t>(
                            maximumInterval, interval));
                    }
                    lastKeyframeIndex = sampleCount;
                    haveKeyframe = true;
                }
                if (haveTimestamp && timestamp < previousTimestamp)
                {
                    monotonic = false;
                }
                previousTimestamp = timestamp;
                haveTimestamp = true;
                ++sampleCount;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            {
                break;
            }
        }

        result.keyframeCount = keyframeCount;
        result.compressedSampleCount = sampleCount;
        result.maxKeyframeInterval = maximumInterval;
        result.compressedTimestampsMonotonic = monotonic;
        result.compressedStatisticsAvailable = sampleCount > 0;
        if (!result.compressedStatisticsAvailable)
        {
            result.compressedNote = "source reader returned no compressed samples";
        }
        return result.compressedStatisticsAvailable;
    }
    catch (const std::exception& exception)
    {
        result.compressedNote = std::string("compressed scan failed: ") + exception.what();
        return false;
    }
    catch (...)
    {
        result.compressedNote = "compressed scan failed";
        return false;
    }
}

bool ValidateVerification(const EncodeSettings& expected, std::uint64_t expectedSamples,
    VerificationResult& result, Diagnostic& diagnostic)
{
    if (result.decodedSamples == 0)
    {
        return FailVerify(diagnostic, "MP4 has no decodable video samples");
    }
    if (result.decodedSamples != expectedSamples)
    {
        return FailVerify(diagnostic, "decoded sample count " +
            std::to_string(result.decodedSamples) + ", expected " +
            std::to_string(expectedSamples));
    }
    if (result.width != expected.width || result.height != expected.height)
    {
        return FailVerify(diagnostic, "decoded size " + std::to_string(result.width) + "x" +
            std::to_string(result.height) + ", expected " + std::to_string(expected.width) +
            "x" + std::to_string(expected.height));
    }
    if (std::fabs(result.containerFps - static_cast<double>(expected.fps)) > 0.001)
    {
        return FailVerify(diagnostic, "container frame rate " +
            FormatNumber(result.containerFps) + ", expected " +
            std::to_string(expected.fps));
    }
    if (!result.constantFrameRate)
    {
        return FailVerify(diagnostic, "decoded frame timestamps are not constant rate");
    }
    if (std::fabs(result.firstTimestampSeconds) > 0.000001)
    {
        return FailVerify(diagnostic, "first decoded timestamp is " +
            FormatNumber(result.firstTimestampSeconds) +
            "s, expected 0 (encoder reordered the timeline)");
    }
    const double expectedDuration = static_cast<double>(expectedSamples) /
        static_cast<double>(expected.fps);
    const double durationTolerance = 1.0 / static_cast<double>(expected.fps) + 0.001;
    if (std::fabs(result.durationSeconds - expectedDuration) > durationTolerance)
    {
        return FailVerify(diagnostic, "container duration " +
            FormatNumber(result.durationSeconds) + "s, expected " +
            FormatNumber(expectedDuration) + "s");
    }
    if (!result.sourceFirstLastIdentical && !result.firstLastDiffer)
    {
        return FailVerify(diagnostic,
            "decoded first and last frames are identical although the source frames differ");
    }
    return true;
}
} // namespace

MediaFoundationRuntime::~MediaFoundationRuntime()
{
    if (mfStarted_)
    {
        MFShutdown();
    }
    if (comInitialized_)
    {
        CoUninitialize();
    }
}

bool MediaFoundationRuntime::Start(Diagnostic& diagnostic)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult))
    {
        comInitialized_ = true;
    }
    else if (comResult != RPC_E_CHANGED_MODE)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "COM initialization failed", "CoInitializeEx", comResult);
    }
    // RPC_E_CHANGED_MODE 表示当前线程已用其它模型初始化 COM：继续使用但不反初始化。

    const HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfResult))
    {
        if (comInitialized_)
        {
            CoUninitialize();
            comInitialized_ = false;
        }
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "Media Foundation startup failed", "MFStartup", mfResult);
    }
    mfStarted_ = true;
    return true;
}

struct H264Mp4Writer::Impl
{
    ComPtr<IMFTransform> encoder;
    ComPtr<IMFMediaType> encoderOutputType;
    ComPtr<IMFSinkWriter> muxer;
    MFT_OUTPUT_STREAM_INFO outputInfo = {};
    std::vector<ComPtr<IMFSample>> compressedSamples;
    EncodeSettings settings;
    std::uint64_t frameBytes = 0;
    std::uint64_t framesWritten = 0;
    LONGLONG lastTimestamp = -1;
    bool streaming = false;
};

H264Mp4Writer::H264Mp4Writer()
    : impl_(std::make_unique<Impl>())
{
}

H264Mp4Writer::~H264Mp4Writer() = default;

bool H264Mp4Writer::Open(const EncodeSettings& settings, const std::filesystem::path& path,
    EncodeResult& result, Diagnostic& diagnostic)
{
    if (!impl_ || impl_->encoder)
    {
        return Fail(diagnostic, "Encoder.InternalError", "writer is already open or invalid",
            ExitCode::Internal);
    }
    if (settings.width == 0 || settings.height == 0 || settings.fps == 0 ||
        settings.bitrate == 0 || settings.width > kMaxEncodeDimension ||
        settings.height > kMaxEncodeDimension)
    {
        return Fail(diagnostic, "Encoder.InternalError", "invalid encode settings",
            ExitCode::Internal);
    }
    impl_->settings = settings;
    const std::uint64_t lumaBytes = static_cast<std::uint64_t>(settings.width) *
        static_cast<std::uint64_t>(settings.height);
    impl_->frameBytes = lumaBytes + lumaBytes / 2u;

    // 1) 直接创建 H.264 编码器 MFT：Sink Writer 内部的编码器无法可靠地关闭 B 帧，
    //    会导致首帧 CTS 偏移一帧。这里用 ICodecAPI 明确控制 GOP 与 B 帧。
    HRESULT resultCode = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->encoder));
    if (FAILED(resultCode) || !impl_->encoder)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create the H.264 encoder MFT",
            "CoCreateInstance(CLSID_CMSH264EncoderMFT)", resultCode);
    }

    ComPtr<ICodecAPI> codec;
    resultCode = impl_->encoder.As(&codec);
    if (FAILED(resultCode) || !codec)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the H.264 encoder does not expose ICodecAPI",
            "QueryInterface(ICodecAPI)", resultCode);
    }
    if (!ConfigureEncoderCodecApi(codec.Get(), settings, result, diagnostic))
    {
        return false;
    }
    result.encoderConfigApplied = true;

    ComPtr<IMFMediaType> outputType;
    resultCode = MFCreateMediaType(&outputType);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create the H.264 output media type", "MFCreateMediaType", resultCode);
    }
    if (!ConfigureCompressedOutputType(outputType.Get(), settings, false, diagnostic))
    {
        return false;
    }
    resultCode = impl_->encoder->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(resultCode))
    {
        // 兼容拒绝可选属性的旧编码器：保留基本尺寸/帧率/码率再试一次。
        ComPtr<IMFMediaType> minimalType;
        if (FAILED(MFCreateMediaType(&minimalType)) ||
            !ConfigureCompressedOutputType(minimalType.Get(), settings, true, diagnostic))
        {
            return false;
        }
        resultCode = impl_->encoder->SetOutputType(0, minimalType.Get(), 0);
        if (FAILED(resultCode))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                "the H.264 encoder rejected the output media type",
                "IMFTransform::SetOutputType", resultCode);
        }
        outputType = minimalType;
        result.usedMinimalOutputType = true;
    }
    impl_->encoderOutputType = outputType;

    ComPtr<IMFMediaType> inputType;
    resultCode = MFCreateMediaType(&inputType);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create the NV12 input media type", "MFCreateMediaType", resultCode);
    }
    if (!ConfigureNv12InputType(inputType.Get(), settings, diagnostic))
    {
        return false;
    }
    resultCode = impl_->encoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the H.264 encoder rejected the NV12 input media type",
            "IMFTransform::SetInputType", resultCode);
    }

    resultCode = impl_->encoder->GetOutputStreamInfo(0, &impl_->outputInfo);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot query the encoder output stream",
            "IMFTransform::GetOutputStreamInfo", resultCode);
    }
    resultCode = impl_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(resultCode))
    {
        resultCode = impl_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot start the H.264 encoder", "IMFTransform::ProcessMessage", resultCode);
    }
    impl_->streaming = true;

    // 2) 提前创建 MP4 Sink Writer：输出路径不可写等问题在编码前就失败。
    ComPtr<IMFAttributes> muxerAttributes;
    resultCode = MFCreateAttributes(&muxerAttributes, 2);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create sink writer attributes", "MFCreateAttributes", resultCode);
    }
    muxerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    resultCode = MFCreateSinkWriterFromURL(path.c_str(), nullptr, muxerAttributes.Get(),
        &impl_->muxer);
    if (FAILED(resultCode) || !impl_->muxer)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create the MP4 sink writer: " + PathText(path),
            "MFCreateSinkWriterFromURL", resultCode);
    }
    return true;
}

bool H264Mp4Writer::WriteFrame(std::span<const std::uint8_t> nv12, int frameIndex,
    Diagnostic& diagnostic)
{
    if (!impl_ || !impl_->encoder || !impl_->muxer)
    {
        return Fail(diagnostic, "Encoder.InternalError", "MP4 writer is not open",
            ExitCode::Internal);
    }
    if (nv12.size() != impl_->frameBytes || impl_->settings.fps == 0)
    {
        return Fail(diagnostic, "Encoder.InternalError",
            "NV12 frame size does not match the encoder configuration", ExitCode::Internal);
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT resultCode = MFCreateMemoryBuffer(static_cast<DWORD>(impl_->frameBytes),
        &buffer);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create a media buffer for frame " + std::to_string(frameIndex),
            "MFCreateMemoryBuffer", resultCode);
    }
    BYTE* destination = nullptr;
    resultCode = buffer->Lock(&destination, nullptr, nullptr);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot lock a media buffer for frame " + std::to_string(frameIndex),
            "IMFMediaBuffer::Lock", resultCode);
    }
    std::memcpy(destination, nv12.data(), nv12.size());
    buffer->Unlock();
    resultCode = buffer->SetCurrentLength(static_cast<DWORD>(impl_->frameBytes));
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot set the media buffer length", "IMFMediaBuffer::SetCurrentLength",
            resultCode);
    }

    ComPtr<IMFSample> sample;
    resultCode = MFCreateSample(&sample);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot create a video sample", "MFCreateSample", resultCode);
    }
    resultCode = sample->AddBuffer(buffer.Get());
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot attach the media buffer to the sample", "IMFSample::AddBuffer",
            resultCode);
    }

    const LONGLONG start = static_cast<LONGLONG>(frameIndex) * kOneSecond /
        static_cast<LONGLONG>(impl_->settings.fps);
    const LONGLONG end = (static_cast<LONGLONG>(frameIndex) + 1) * kOneSecond /
        static_cast<LONGLONG>(impl_->settings.fps);
    resultCode = sample->SetSampleTime(start);
    if (SUCCEEDED(resultCode))
    {
        resultCode = sample->SetSampleDuration(end - start);
    }
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot set the sample timestamp", "IMFSample::SetSampleTime", resultCode);
    }

    resultCode = impl_->encoder->ProcessInput(0, sample.Get(), 0);
    if (resultCode == MF_E_NOTACCEPTING)
    {
        if (!CollectOutputSamples(false, diagnostic))
        {
            return false;
        }
        resultCode = impl_->encoder->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot feed frame " + std::to_string(frameIndex) + " to the encoder",
            "IMFTransform::ProcessInput", resultCode);
    }
    ++impl_->framesWritten;
    return CollectOutputSamples(false, diagnostic);
}

bool H264Mp4Writer::CollectOutputSamples(bool draining, Diagnostic& diagnostic)
{
    if (!impl_ || !impl_->encoder)
    {
        return Fail(diagnostic, "Encoder.InternalError", "encoder is not open",
            ExitCode::Internal);
    }
    const bool providesSamples =
        (impl_->outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    for (std::uint64_t guard = 0; guard < kMaxSampleReads; ++guard)
    {
        MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
        DWORD status = 0;
        ComPtr<IMFSample> providedSample;
        ComPtr<IMFMediaBuffer> providedBuffer;
        if (!providesSamples)
        {
            const UINT32 bufferSize = impl_->outputInfo.cbSize != 0
                ? impl_->outputInfo.cbSize
                : kFallbackEncoderBufferBytes;
            if (FAILED(MFCreateSample(&providedSample)) ||
                FAILED(MFCreateMemoryBuffer(bufferSize, &providedBuffer)))
            {
                return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                    "cannot allocate an encoder output buffer", "MFCreateMemoryBuffer",
                    E_OUTOFMEMORY);
            }
            providedSample->AddBuffer(providedBuffer.Get());
            outputBuffer.pSample = providedSample.Get();
        }

        const HRESULT resultCode = impl_->encoder->ProcessOutput(0, 1, &outputBuffer,
            &status);
        if (resultCode == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            return true;
        }
        if (resultCode == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            if (!ReapplyEncoderOutputType(impl_->encoder.Get(),
                    impl_->encoderOutputType.Get(), impl_->outputInfo, diagnostic))
            {
                return false;
            }
            continue;
        }
        if (FAILED(resultCode))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                draining ? "cannot drain the encoder" : "cannot read an encoded sample",
                "IMFTransform::ProcessOutput", resultCode);
        }

        if (outputBuffer.pSample != nullptr)
        {
            if (!StoreCompressedSample(outputBuffer.pSample, diagnostic))
            {
                if (providesSamples)
                {
                    outputBuffer.pSample->Release();
                }
                return false;
            }
            if (providesSamples)
            {
                outputBuffer.pSample->Release();
            }
        }
        if (outputBuffer.pEvents != nullptr)
        {
            outputBuffer.pEvents->Release();
        }
        if ((status & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) != 0)
        {
            // 单个输出流不应返回 INCOMPLETE；明确失败比输出截断的码流安全。
            return Fail(diagnostic, "Encoder.EncodeFailed",
                "the H.264 encoder returned an incomplete output buffer",
                ExitCode::EncodeFailed);
        }
    }
    return Fail(diagnostic, "Encoder.EncodeFailed",
        "the H.264 encoder produced too many samples", ExitCode::EncodeFailed);
}

bool H264Mp4Writer::StoreCompressedSample(IMFSample* sample, Diagnostic& diagnostic)
{
    if (!impl_ || sample == nullptr)
    {
        return Fail(diagnostic, "Encoder.InternalError", "encoder returned no sample",
            ExitCode::Internal);
    }
    LONGLONG timestamp = 0;
    LONGLONG duration = 0;
    HRESULT resultCode = sample->GetSampleTime(&timestamp);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the encoder returned a sample without a timestamp",
            "IMFSample::GetSampleTime", resultCode);
    }
    if (impl_->lastTimestamp >= 0 && timestamp <= impl_->lastTimestamp)
    {
        // 输出顺序与显示顺序不一致说明编码器启用了 B 帧；这会让首帧 CTS 偏移。
        return Fail(diagnostic, "Encoder.EncodeFailed",
            "the encoder reordered output samples (B frames are still enabled)",
            ExitCode::EncodeFailed);
    }
    impl_->lastTimestamp = timestamp;
    sample->GetSampleDuration(&duration);

    ComPtr<IMFMediaBuffer> sourceBuffer;
    resultCode = sample->ConvertToContiguousBuffer(&sourceBuffer);
    if (FAILED(resultCode) || !sourceBuffer)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot access the encoded sample data",
            "IMFSample::ConvertToContiguousBuffer", resultCode);
    }
    DWORD length = 0;
    resultCode = sourceBuffer->GetCurrentLength(&length);
    if (FAILED(resultCode) || length == 0)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the encoder returned an empty sample", "IMFMediaBuffer::GetCurrentLength",
            resultCode);
    }
    BYTE* source = nullptr;
    resultCode = sourceBuffer->Lock(&source, nullptr, nullptr);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot lock the encoded sample data", "IMFMediaBuffer::Lock", resultCode);
    }
    // 深拷贝：编码器输出的缓冲区在后续 ProcessOutput 中可能被复用。
    ComPtr<IMFMediaBuffer> ownedBuffer;
    ComPtr<IMFSample> ownedSample;
    BYTE* destination = nullptr;
    if (SUCCEEDED(MFCreateMemoryBuffer(length, &ownedBuffer)) &&
        SUCCEEDED(ownedBuffer->Lock(&destination, nullptr, nullptr)))
    {
        std::memcpy(destination, source, length);
        ownedBuffer->Unlock();
        ownedBuffer->SetCurrentLength(length);
    }
    else
    {
        ownedBuffer.Reset();
    }
    sourceBuffer->Unlock();
    if (!ownedBuffer || FAILED(MFCreateSample(&ownedSample)))
    {
        return Fail(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot copy an encoded sample", ExitCode::EncodeFailed);
    }
    ownedSample->AddBuffer(ownedBuffer.Get());
    ownedSample->SetSampleTime(timestamp);
    if (duration > 0)
    {
        ownedSample->SetSampleDuration(duration);
    }
    UINT32 cleanPoint = 0;
    if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)))
    {
        ownedSample->SetUINT32(MFSampleExtension_CleanPoint, cleanPoint);
    }
    // 部分编码器不在样本属性里标记关键帧，此时按 Annex B NAL 的 IDR 检测。
    else if (IsAnnexBIdrSample(ownedSample.Get()))
    {
        ownedSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }
    impl_->compressedSamples.push_back(std::move(ownedSample));
    return true;
}

bool H264Mp4Writer::Finish(EncodeResult& result, Diagnostic& diagnostic)
{
    if (!impl_ || !impl_->encoder || !impl_->muxer)
    {
        return Fail(diagnostic, "Encoder.InternalError", "MP4 writer is not open",
            ExitCode::Internal);
    }
    if (impl_->streaming)
    {
        const HRESULT resultCode = impl_->encoder->ProcessMessage(
            MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(resultCode))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                "cannot drain the H.264 encoder", "IMFTransform::ProcessMessage",
                resultCode);
        }
        impl_->streaming = false;
    }
    if (!CollectOutputSamples(true, diagnostic))
    {
        return false;
    }
    if (impl_->compressedSamples.empty())
    {
        return Fail(diagnostic, "Encoder.EncodeFailed",
            "the H.264 encoder produced no samples", ExitCode::EncodeFailed);
    }
    if (impl_->compressedSamples.size() != impl_->framesWritten)
    {
        return Fail(diagnostic, "Encoder.EncodeFailed",
            "the H.264 encoder produced " +
                std::to_string(impl_->compressedSamples.size()) + " samples for " +
                std::to_string(impl_->framesWritten) + " frames",
            ExitCode::EncodeFailed);
    }

    ComPtr<IMFMediaType> compressedType;
    HRESULT resultCode = impl_->encoder->GetOutputCurrentType(0, &compressedType);
    if (FAILED(resultCode) || !compressedType)
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot read the negotiated H.264 output type",
            "IMFTransform::GetOutputCurrentType", resultCode);
    }

    DWORD muxStreamIndex = 0;
    resultCode = impl_->muxer->AddStream(compressedType.Get(), &muxStreamIndex);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the MP4 muxer rejected the H.264 stream", "IMFSinkWriter::AddStream",
            resultCode);
    }
    ComPtr<IMFMediaType> muxInputType;
    resultCode = MFCreateMediaType(&muxInputType);
    if (SUCCEEDED(resultCode))
    {
        resultCode = compressedType->CopyAllItems(muxInputType.Get());
    }
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot build the MP4 muxer input type", "IMFMediaType::CopyAllItems",
            resultCode);
    }
    // 输入类型 == 输出类型：Sink Writer 只封装、不再编码（转码模式）。
    resultCode = impl_->muxer->SetInputMediaType(muxStreamIndex, muxInputType.Get(),
        nullptr);
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "the MP4 muxer rejected the compressed H.264 input type",
            "IMFSinkWriter::SetInputMediaType", resultCode);
    }
    resultCode = impl_->muxer->BeginWriting();
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot start MP4 writing", "IMFSinkWriter::BeginWriting", resultCode);
    }
    for (const ComPtr<IMFSample>& sample : impl_->compressedSamples)
    {
        resultCode = impl_->muxer->WriteSample(muxStreamIndex, sample.Get());
        if (FAILED(resultCode))
        {
            return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
                "the MP4 muxer rejected an encoded sample",
                "IMFSinkWriter::WriteSample", resultCode);
        }
    }
    resultCode = impl_->muxer->Finalize();
    if (FAILED(resultCode))
    {
        return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed",
            "cannot finalize the MP4 file", "IMFSinkWriter::Finalize", resultCode);
    }
    impl_->muxer.Reset();
    impl_->compressedSamples.clear();
    result.encodedSamples = impl_->framesWritten;
    return true;
}

bool H264Mp4Writer::IsAnnexBIdrSample(IMFSample* sample)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
    {
        return false;
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)) || length < 5)
    {
        if (data != nullptr)
        {
            buffer->Unlock();
        }
        return false;
    }
    bool idr = false;
    // Annex B：00 00 00 01 或 00 00 01 之后是 nal_unit_type(低 5 位)，5 = IDR。
    for (DWORD index = 0; index + 4 < length && index < 16; ++index)
    {
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1)
        {
            const std::uint8_t nalType = static_cast<std::uint8_t>(data[index + 3] & 0x1F);
            idr = nalType == 5;
            break;
        }
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 0 &&
            data[index + 3] == 1)
        {
            const std::uint8_t nalType = static_cast<std::uint8_t>(data[index + 4] & 0x1F);
            idr = nalType == 5;
            break;
        }
    }
    buffer->Unlock();
    return idr;
}

bool VerifyMp4File(const std::filesystem::path& path, const EncodeSettings& expected,
    std::uint64_t expectedSamples, bool sourceFirstLastIdentical,
    VerificationResult& result, Diagnostic& diagnostic)
{
    result = {};
    result.sourceFirstLastIdentical = sourceFirstLastIdentical;
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError) || existsError)
    {
        return FailVerify(diagnostic, "MP4 file does not exist: " + PathText(path));
    }

    std::vector<std::uint8_t> firstFrame;
    std::vector<std::uint8_t> lastFrame;
    if (!ReadDecodedSamples(path, expected.fps, result, firstFrame, lastFrame, diagnostic))
    {
        return false;
    }
    // 压缩样本统计（GOP、时间戳单调性）先收集，失败验证时清单也能保留这些证据。
    CollectCompressedStatistics(path, result);
    if (!ValidateVerification(expected, expectedSamples, result, diagnostic))
    {
        return false;
    }
    return true;
}
// 对外入口：内部实现放在文件级匿名命名空间里，避免污染头文件的可见符号。
bool NormalizeMp4Timestamps(const std::filesystem::path& path, Diagnostic& diagnostic)
{
    return NormalizeMp4TimestampsInternal(path, diagnostic);
}
} // namespace flare::videoencoder
