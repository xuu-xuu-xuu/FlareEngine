#pragma once
// W08 独立视频编码器：Media Foundation H.264/MP4 写入与 Source Reader 验证。
// 仅使用系统 Media Foundation，不依赖引擎的 D3D12 或资源层。
#include "VideoEncoderSupport.h"

#include <filesystem>
#include <memory>

struct IMFSample;

namespace flare::videoencoder
{
// 负责 COM 与 Media Foundation 的成对初始化/清理，失败路径也会回到干净状态。
class MediaFoundationRuntime
{
public:
    MediaFoundationRuntime() = default;
    ~MediaFoundationRuntime();
    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

    bool Start(Diagnostic& diagnostic);

private:
    bool comInitialized_ = false;
    bool mfStarted_ = false;
};

struct EncodeSettings
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
    std::uint32_t bitrate = 0;
    std::uint32_t keyframeInterval = 30;
};

struct EncodeResult
{
    std::uint64_t encodedSamples = 0;
    // 部分机器的 H.264 MFT 会拒绝可选的输出类型属性；此时降级为最小类型集。
    bool usedMinimalOutputType = false;
    // 编码器属性（GOP、B 帧、码率控制）是否全部被 ICodecAPI 接受。
    bool encoderConfigApplied = false;
    // "CBR" 或 "average"，写入清单以便说明实际码率控制方式。
    std::string rateControlMode;
    // 编码器是否接受单线程设置；只有单线程时同一批帧才可能编出相同字节（E20）。
    bool singleThreaded = false;
    // 是否把 mp4 的 creation/modification 时间戳归一为固定值（E20：同帧同字节）。
    bool normalizedTimestamps = false;
};

// 把 moov 下 mvhd/tkhd/mdhd 的 creation/modification 置零，使编码产物字节可复现（E20）。
bool NormalizeMp4Timestamps(const std::filesystem::path& path, Diagnostic& diagnostic);

class H264Mp4Writer
{
public:
    H264Mp4Writer();
    ~H264Mp4Writer();
    H264Mp4Writer(const H264Mp4Writer&) = delete;
    H264Mp4Writer& operator=(const H264Mp4Writer&) = delete;

    bool Open(const EncodeSettings& settings, const std::filesystem::path& path,
        EncodeResult& result, Diagnostic& diagnostic);
    bool WriteFrame(std::span<const std::uint8_t> nv12, int frameIndex,
        Diagnostic& diagnostic);
    bool Finish(EncodeResult& result, Diagnostic& diagnostic);

private:
    bool CollectOutputSamples(bool draining, Diagnostic& diagnostic);
    bool StoreCompressedSample(IMFSample* sample, Diagnostic& diagnostic);
    static bool IsAnnexBIdrSample(IMFSample* sample);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct VerificationResult
{
    std::uint64_t decodedSamples = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double containerFps = 0.0;
    double measuredFps = 0.0;
    bool constantFrameRate = false;
    double firstTimestampSeconds = 0.0;
    double lastTimestampSeconds = 0.0;
    double durationSeconds = 0.0;
    std::uint32_t bitrate = 0;
    bool firstLastDiffer = false;
    bool sourceFirstLastIdentical = false;
    std::string firstFrameSha256;
    std::string lastFrameSha256;
    // 容器/原生媒体类型报告的实际 H.264 profile（100 = High）。
    std::uint32_t profile = 0;
    // 额外证据：压缩样本的关键帧间隔与时间戳单调性（可检测 B 帧重排）。
    std::uint64_t keyframeCount = 0;
    std::uint64_t compressedSampleCount = 0;
    std::uint32_t maxKeyframeInterval = 0;
    bool compressedTimestampsMonotonic = false;
    bool compressedStatisticsAvailable = false;
    std::string compressedNote;
};

// 重新打开 MP4 并解码全部样本，核对数量/尺寸/帧率/首末帧差异/容器时长。
bool VerifyMp4File(const std::filesystem::path& path, const EncodeSettings& expected,
    std::uint64_t expectedSamples, bool sourceFirstLastIdentical,
    VerificationResult& result, Diagnostic& diagnostic);
} // namespace flare::videoencoder
