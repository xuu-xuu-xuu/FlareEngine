// W08 独立视频编码器：把编号 PPM 帧序列编码为可交付的 H.264 MP4。
// 只用 Windows Media Foundation；不依赖引擎渲染层、D3D12 或外部工具。
#include "VideoEncoderMp4.h"
#include "VideoEncoderPpm.h"
#include "VideoEncoderSupport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace se::videoencoder
{
namespace
{
constexpr std::uint32_t kDefaultFps = 30;
constexpr std::uint32_t kDefaultKeyframeInterval = 30;
constexpr std::uint32_t kTargetBitrate1080p = 12'000'000;
constexpr std::uint32_t kMinimumBitrate = 200'000;
constexpr std::uint32_t kMaximumBitrate = 60'000'000;
constexpr std::uint32_t kSelftestWidth = 64;
constexpr std::uint32_t kSelftestHeight = 64;
constexpr int kSelftestFrameCount = 8;
constexpr std::uint32_t kSelftestFps = 8;

struct Options
{
    bool help = false;
    bool selftest = false;
    std::filesystem::path framesDirectory;
    std::string pattern = "frame_%04d.ppm";
    int firstIndex = 0;
    int frameCount = 0;
    std::uint32_t fps = kDefaultFps;
    std::filesystem::path outputPath;
    std::filesystem::path manifestPath;
    bool manifestRequested = false;
    std::filesystem::path selftestDirectory;
};

// 一次运行的完整状态；JSON 清单由它生成，失败时也保留已得到的数字。
struct Session
{
    bool selftest = false;
    std::filesystem::path framesDirectory;
    std::string pattern;
    int firstIndex = 0;
    int frameCount = 0;
    std::uint32_t fps = 0;
    std::filesystem::path outputPath;
    std::filesystem::path manifestPath;
    bool manifestRequested = false;

    bool sequenceScanned = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string firstFileName;
    std::string lastFileName;
    std::uint64_t ignoredFileCount = 0;
    std::string sourceFirstSha256;
    std::string sourceLastSha256;
    bool sourceFirstLastIdentical = false;

    EncodeSettings settings;
    EncodeResult encodeResult;
    bool encodeCompleted = false;
    bool verificationCompleted = false;
    VerificationResult verification;

    std::uint64_t outputBytes = 0;
    std::string outputSha256;
};

class TempFileGuard
{
public:
    explicit TempFileGuard(std::filesystem::path path)
        : path_(std::move(path))
    {
    }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    ~TempFileGuard()
    {
        if (keep_)
        {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void Keep()
    {
        keep_ = true;
    }

private:
    std::filesystem::path path_;
    bool keep_ = false;
};

void PrintHelp()
{
    std::cout
        << "FlareVideoEncoder (VideoEncoder.exe) - standalone H.264/MP4 encoder for PPM frames\n"
        << "\n"
        << "Encodes a contiguous numbered sequence of binary P6 (8-bit) PPM frames - the\n"
        << "format written by the engine's WritePpmImage - into an H.264 MP4 using Windows\n"
        << "Media Foundation. No ffmpeg and no external downloads are required.\n"
        << "\n"
        << "Usage:\n"
        << "  VideoEncoder --frames <dir> --count <n> --output <file.mp4> [options]\n"
        << "  VideoEncoder --selftest <tempdir>\n"
        << "  VideoEncoder --help\n"
        << "\n"
        << "Required for encoding:\n"
        << "  --frames <dir>        Directory containing the numbered PPM frames.\n"
        << "  --count <n>           Number of frames to encode, 1..1000000. Frame numbers\n"
        << "                        must be contiguous starting at --first.\n"
        << "  --output <file.mp4>   Destination MP4. Missing parent directories are created.\n"
        << "\n"
        << "Options:\n"
        << "  --pattern <pattern>   File name pattern with exactly one zero-padded integer\n"
        << "                        conversion. Default: frame_%04d.ppm\n"
        << "  --first <index>       First frame number, 0..9999999. Default: 0\n"
        << "  --fps <n>             Constant frame rate, integer 1..1000. Default: 30\n"
        << "  --manifest <file>     Write the same JSON summary to this file.\n"
        << "  --selftest <tempdir>  Generate 8 frames (64x64), encode, verify, exit 0 or 1.\n"
        << "  --help                Print this text and exit 0.\n"
        << "\n"
        << "Encoding profile (fixed):\n"
        << "  H.264 High profile, yuv420p (NV12 input), MP4, constant frame rate. The tool\n"
        << "  drives the Microsoft H.264 encoder MFT directly, so GOP 30, B-frame count 0\n"
        << "  and CBR rate control are applied through ICodecAPI; the MP4 is muxed by a\n"
        << "  Sink Writer in transcode mode. Bitrate: 12000000 bps at 1920x1080, scaled\n"
        << "  linearly by pixel count with a 200000 bps floor and a 60000000 bps ceiling.\n"
        << "  The encoder may undershoot the target on simple content; the manifest reports\n"
        << "  both the requested target and the measured average bitrate.\n"
        << "  Resolution follows the first PPM frame; every frame must match.\n"
        << "\n"
        << "Validation and exit codes:\n"
        << "   0  success\n"
        << "   1  selftest failed\n"
        << "   2  Encoder.Usage\n"
        << "   3  Encoder.FrameMissing / Encoder.InputDirectoryMissing\n"
        << "   4  Encoder.FrameDuplicate\n"
        << "   5  Encoder.FrameOversized\n"
        << "   6  Encoder.FrameUnsupported / Encoder.DimensionsUnsupported\n"
        << "   7  Encoder.FrameTruncated\n"
        << "   8  Encoder.FrameDimensionMismatch\n"
        << "   9  Encoder.FrameCountMismatch\n"
        << "  10  Encoder.FpsInvalid\n"
        << "  11  Encoder.OutputNotWritable\n"
        << "  12  Encoder.PatternInvalid\n"
        << "  13  Encoder.EncodeFailed (includes Media Foundation HRESULTs)\n"
        << "  14  Encoder.VerificationFailed\n"
        << "  15  Encoder.ManifestNotWritable\n"
        << "  16  Encoder.InternalError\n"
        << "\n"
        << "The JSON summary is always printed to stdout: input frame count, first/last\n"
        << "file, width/height, fps, encoded samples, duration, output bytes and SHA-256,\n"
        << "plus the Media Foundation source-reader verification numbers. Diagnostics use\n"
        << "stable English keys, for example \"Encoder.FrameMissing: frame_0123.ppm\".\n";
}

bool ParseInteger(const std::wstring& text, long long minimum, long long maximum,
    long long& value)
{
    const std::string narrow = WideToUtf8(text);
    if (narrow.empty())
    {
        return false;
    }
    long long parsed = 0;
    const auto result = std::from_chars(narrow.data(), narrow.data() + narrow.size(),
        parsed);
    if (result.ec != std::errc{} || result.ptr != narrow.data() + narrow.size() ||
        parsed < minimum || parsed > maximum)
    {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseArguments(int argc, wchar_t** argv, Options& options, Diagnostic& diagnostic)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring token = argv[index];
        const auto requireValue = [&](std::wstring& value) {
            if (index + 1 >= argc)
            {
                return Fail(diagnostic, "Encoder.Usage",
                    "option " + WideToUtf8(token) + " needs a value", ExitCode::Usage);
            }
            value = argv[++index];
            return true;
        };

        if (token == L"--help" || token == L"-h")
        {
            options.help = true;
            continue;
        }
        if (token == L"--selftest")
        {
            std::wstring value;
            if (!requireValue(value))
            {
                return false;
            }
            options.selftest = true;
            options.selftestDirectory = value;
            continue;
        }
        if (token == L"--frames" || token == L"--pattern" || token == L"--first" ||
            token == L"--count" || token == L"--fps" || token == L"--output" ||
            token == L"--manifest")
        {
            std::wstring value;
            if (!requireValue(value))
            {
                return false;
            }
            if (token == L"--frames")
            {
                options.framesDirectory = value;
            }
            else if (token == L"--pattern")
            {
                options.pattern = WideToUtf8(value);
            }
            else if (token == L"--output")
            {
                options.outputPath = value;
            }
            else if (token == L"--manifest")
            {
                options.manifestPath = value;
                options.manifestRequested = true;
            }
            else if (token == L"--first")
            {
                long long parsed = 0;
                if (!ParseInteger(value, 0, 9'999'999, parsed))
                {
                    return Fail(diagnostic, "Encoder.Usage",
                        "--first must be an integer between 0 and 9999999",
                        ExitCode::Usage);
                }
                options.firstIndex = static_cast<int>(parsed);
            }
            else if (token == L"--count")
            {
                long long parsed = 0;
                if (!ParseInteger(value, 1, 1'000'000, parsed))
                {
                    return Fail(diagnostic, "Encoder.FrameCountMismatch",
                        "--count must be an integer between 1 and 1000000",
                        ExitCode::FrameCountMismatch);
                }
                options.frameCount = static_cast<int>(parsed);
            }
            else
            {
                long long parsed = 0;
                if (!ParseInteger(value, 1, 1000, parsed))
                {
                    return Fail(diagnostic, "Encoder.FpsInvalid",
                        "--fps must be an integer between 1 and 1000",
                        ExitCode::FpsInvalid);
                }
                options.fps = static_cast<std::uint32_t>(parsed);
            }
            continue;
        }

        return Fail(diagnostic, "Encoder.Usage",
            "unknown option: " + WideToUtf8(token), ExitCode::Usage);
    }

    if (options.help)
    {
        return true;
    }
    if (options.selftest)
    {
        if (!options.framesDirectory.empty() || !options.outputPath.empty() ||
            options.manifestRequested)
        {
            return Fail(diagnostic, "Encoder.Usage",
                "--selftest cannot be combined with --frames, --output or --manifest",
                ExitCode::Usage);
        }
        if (options.selftestDirectory.empty())
        {
            return Fail(diagnostic, "Encoder.Usage", "--selftest needs a temporary directory",
                ExitCode::Usage);
        }
        return true;
    }
    if (options.framesDirectory.empty())
    {
        return Fail(diagnostic, "Encoder.Usage", "--frames is required", ExitCode::Usage);
    }
    if (options.frameCount <= 0)
    {
        return Fail(diagnostic, "Encoder.FrameCountMismatch", "--count is required",
            ExitCode::FrameCountMismatch);
    }
    if (options.outputPath.empty())
    {
        return Fail(diagnostic, "Encoder.Usage", "--output is required", ExitCode::Usage);
    }
    return true;
}

std::uint32_t BitrateForDimensions(std::uint32_t width, std::uint32_t height)
{
    const double ratio = (static_cast<double>(width) * static_cast<double>(height)) /
        (1920.0 * 1080.0);
    const double scaled = static_cast<double>(kTargetBitrate1080p) * ratio;
    const double clamped = std::clamp(scaled, static_cast<double>(kMinimumBitrate),
        static_cast<double>(kMaximumBitrate));
    return static_cast<std::uint32_t>(clamped + 0.5);
}

bool ValidateOutputPath(const std::filesystem::path& path, Diagnostic& diagnostic)
{
    if (path.empty() || path.filename().empty())
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            "output path has no file name", ExitCode::OutputNotWritable);
    }
    std::error_code error;
    if (std::filesystem::is_directory(path, error))
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            PathText(path) + " is a directory", ExitCode::OutputNotWritable);
    }
    // 路径不存在时 status 会带回 ERROR_PATH_NOT_FOUND；这不是失败，接着创建目录。
    error.clear();
    std::filesystem::path directory = path.parent_path();
    if (directory.empty())
    {
        directory = std::filesystem::current_path(error);
        if (error)
        {
            return Fail(diagnostic, "Encoder.OutputNotWritable",
                PathText(path) + " (cannot resolve the output directory)",
                ExitCode::OutputNotWritable);
        }
    }
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            PathText(directory) + " (cannot create the output directory)",
            ExitCode::OutputNotWritable);
    }

    // 真正试写一个临时探测文件，避免编码完才发现目录不可写。
    const std::filesystem::path probe = directory /
        ("flare-videoencoder-probe-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + ".tmp");
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return Fail(diagnostic, "Encoder.OutputNotWritable",
                PathText(directory) + " (output directory is not writable)",
                ExitCode::OutputNotWritable);
        }
        output.put('\n');
        output.flush();
        if (!output)
        {
            std::error_code ignored;
            std::filesystem::remove(probe, ignored);
            return Fail(diagnostic, "Encoder.OutputNotWritable",
                PathText(directory) + " (cannot write to the output directory)",
                ExitCode::OutputNotWritable);
        }
    }
    std::filesystem::remove(probe, error);
    return true;
}

bool WriteManifest(const std::filesystem::path& path, const std::string& text,
    Diagnostic& diagnostic)
{
    if (path.empty() || path.filename().empty())
    {
        return Fail(diagnostic, "Encoder.ManifestNotWritable",
            "manifest path has no file name", ExitCode::ManifestNotWritable);
    }
    std::error_code error;
    const std::filesystem::path directory = path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : path.parent_path();
    if (!error)
    {
        std::filesystem::create_directories(directory, error);
    }
    if (error)
    {
        return Fail(diagnostic, "Encoder.ManifestNotWritable",
            PathText(directory) + " (cannot create the manifest directory)",
            ExitCode::ManifestNotWritable);
    }

    const std::filesystem::path temporary = directory /
        (path.filename().wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return Fail(diagnostic, "Encoder.ManifestNotWritable",
                PathText(temporary) + " (cannot open the manifest file)",
                ExitCode::ManifestNotWritable);
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.put('\n');
        output.flush();
        if (!output)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return Fail(diagnostic, "Encoder.ManifestNotWritable",
                PathText(temporary) + " (cannot write the manifest file)",
                ExitCode::ManifestNotWritable);
        }
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        const DWORD code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Fail(diagnostic, "Encoder.ManifestNotWritable",
            PathText(path) + " (Win32 error " + std::to_string(code) + ")",
            ExitCode::ManifestNotWritable);
    }
    return true;
}

bool GenerateSelftestFrames(const std::filesystem::path& directory, Diagnostic& diagnostic)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            PathText(directory) + " (cannot create the selftest frames directory)",
            ExitCode::OutputNotWritable);
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSelftestWidth) *
        static_cast<std::size_t>(kSelftestHeight) * 3u);
    for (int frame = 0; frame < kSelftestFrameCount; ++frame)
    {
        for (std::uint32_t y = 0; y < kSelftestHeight; ++y)
        {
            for (std::uint32_t x = 0; x < kSelftestWidth; ++x)
            {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kSelftestWidth + x) * 3u;
                pixels[offset] = static_cast<std::uint8_t>((x * 4u + frame * 17) & 0xFF);
                pixels[offset + 1] = static_cast<std::uint8_t>((y * 4u + frame * 29) & 0xFF);
                pixels[offset + 2] = static_cast<std::uint8_t>(
                    ((x + y) * 2u + frame * 43) & 0xFF);
            }
        }

        const std::string name = FrameFileName("frame_%04d.ppm", frame);
        // 自检文件名全为 ASCII，直接转宽字符即可，无需再经过 UTF-8 路径转换。
        const std::filesystem::path file = directory /
            std::filesystem::path(std::wstring(name.begin(), name.end()));
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return Fail(diagnostic, "Encoder.OutputNotWritable",
                PathText(file) + " (cannot write a selftest frame)",
                ExitCode::OutputNotWritable);
        }
        output << "P6\n" << kSelftestWidth << " " << kSelftestHeight << "\n255\n";
        output.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
        if (!output)
        {
            return Fail(diagnostic, "Encoder.OutputNotWritable",
                PathText(file) + " (cannot write a selftest frame)",
                ExitCode::OutputNotWritable);
        }
    }
    return true;
}

// 扫描 + 预检 + 编码 + 重新解码验证。session 的字段在失败时也尽量保留，
// 以便 JSON 清单包含已得到的数字。
bool RunEncode(Session& session, Diagnostic& diagnostic)
{
    FramePattern compiled;
    if (!BuildFramePattern(session.pattern, compiled, diagnostic))
    {
        return false;
    }
    if (session.fps == 0 || session.fps > 1000)
    {
        return Fail(diagnostic, "Encoder.FpsInvalid", "fps must be between 1 and 1000",
            ExitCode::FpsInvalid);
    }
    if (session.frameCount <= 0)
    {
        return Fail(diagnostic, "Encoder.FrameCountMismatch", "frame count must be positive",
            ExitCode::FrameCountMismatch);
    }
    // 即使预检失败，清单里也保留计划使用的帧率与关键帧间隔。
    session.settings.fps = session.fps;
    session.settings.keyframeInterval = kDefaultKeyframeInterval;
    if (!ValidateOutputPath(session.outputPath, diagnostic))
    {
        return false;
    }

    FrameSequence sequence;
    if (!ScanFrameSequence(session.framesDirectory, session.pattern, session.firstIndex,
            session.frameCount, sequence, diagnostic))
    {
        return false;
    }
    session.sequenceScanned = true;
    session.width = sequence.width;
    session.height = sequence.height;
    session.firstFileName = sequence.firstFileName;
    session.lastFileName = sequence.lastFileName;
    session.ignoredFileCount = sequence.ignoredFileCount;

    std::vector<std::uint8_t> firstPixels;
    std::vector<std::uint8_t> lastPixels;
    if (!LoadPpmFrame(sequence.frames.front(), sequence.width, sequence.height, firstPixels,
            diagnostic) ||
        !LoadPpmFrame(sequence.frames.back(), sequence.width, sequence.height, lastPixels,
            diagnostic))
    {
        return false;
    }
    session.sourceFirstSha256 = Sha256Hex(firstPixels);
    session.sourceLastSha256 = Sha256Hex(lastPixels);
    session.sourceFirstLastIdentical = session.sourceFirstSha256 == session.sourceLastSha256;

    session.settings.width = sequence.width;
    session.settings.height = sequence.height;
    session.settings.fps = session.fps;
    session.settings.keyframeInterval = kDefaultKeyframeInterval;
    session.settings.bitrate = BitrateForDimensions(sequence.width, sequence.height);

    const std::filesystem::path outputDirectory = session.outputPath.parent_path().empty()
        ? std::filesystem::current_path()
        : session.outputPath.parent_path();
    const std::filesystem::path temporaryPath = outputDirectory /
        (session.outputPath.filename().wstring() + L".partial-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L".mp4");
    TempFileGuard temporaryGuard(temporaryPath);

    MediaFoundationRuntime runtime;
    if (!runtime.Start(diagnostic))
    {
        return false;
    }

    H264Mp4Writer writer;
    if (!writer.Open(session.settings, temporaryPath, session.encodeResult, diagnostic))
    {
        return false;
    }

    const std::size_t nv12Bytes = static_cast<std::size_t>(sequence.width) *
        static_cast<std::size_t>(sequence.height) * 3u / 2u;
    std::vector<std::uint8_t> rgb;
    std::vector<std::uint8_t> nv12(nv12Bytes);
    for (int offset = 0; offset < sequence.frameCount; ++offset)
    {
        if (!LoadPpmFrame(sequence.frames[static_cast<std::size_t>(offset)],
                sequence.width, sequence.height, rgb, diagnostic))
        {
            return false;
        }
        ConvertRgbToNv12(rgb, sequence.width, sequence.height, nv12);
        if (!writer.WriteFrame(nv12, offset, diagnostic))
        {
            return false;
        }
    }
    if (!writer.Finish(session.encodeResult, diagnostic))
    {
        return false;
    }
    session.encodeCompleted = true;

    // Media Foundation 的 MP4 muxer 会把“当前时间”写进 mvhd/tkhd/mdhd 的 creation/modification
    // 字段，同一批帧两次编码就差这几字节。这里把三处时间戳归一为固定值，让产物字节可复现（E20）。
    if (!NormalizeMp4Timestamps(temporaryPath, diagnostic))
    {
        return false;
    }
    session.encodeResult.normalizedTimestamps = true;

    if (MoveFileExW(temporaryPath.c_str(), session.outputPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        const DWORD code = GetLastError();
        return Fail(diagnostic, "Encoder.OutputNotWritable",
            "cannot replace " + PathText(session.outputPath) + " (Win32 error " +
                std::to_string(code) + ")",
            ExitCode::OutputNotWritable);
    }
    temporaryGuard.Keep();

    session.outputBytes = FileSizeBytes(session.outputPath);
    std::string hashError;
    session.outputSha256 = Sha256FileHex(session.outputPath, hashError);
    if (session.outputSha256.empty())
    {
        return Fail(diagnostic, "Encoder.InternalError",
            "cannot hash the output MP4: " + hashError, ExitCode::Internal);
    }

    if (!VerifyMp4File(session.outputPath, session.settings, session.frameCount,
            session.sourceFirstLastIdentical, session.verification, diagnostic))
    {
        return false;
    }
    session.verificationCompleted = true;
    return true;
}

void WriteReport(JsonWriter& writer, const Session& session, const Diagnostic* failure)
{
    const auto stringOrNull = [&writer](std::string_view key, const std::string& value) {
        writer.Key(key);
        if (value.empty())
        {
            writer.Null();
        }
        else
        {
            writer.String(value);
        }
    };

    writer.BeginObject();
    writer.Key("tool");
    writer.String("FlareVideoEncoder");
    writer.Key("reportVersion");
    writer.Integer(1);
    writer.Key("status");
    writer.String(failure != nullptr ? "failed" : "ok");
    writer.Key("command");
    writer.String(session.selftest ? "selftest" : "encode");
    if (failure != nullptr)
    {
        writer.Key("key");
        writer.String(failure->key);
        writer.Key("detail");
        writer.String(failure->detail);
        writer.Key("exitCode");
        writer.Integer(static_cast<std::int64_t>(failure->exitCode));
    }

    writer.Key("input");
    writer.BeginObject();
    writer.Key("directory");
    if (session.framesDirectory.empty())
    {
        writer.Null();
    }
    else
    {
        writer.String(PathText(session.framesDirectory));
    }
    writer.Key("pattern");
    writer.String(session.pattern);
    writer.Key("firstIndex");
    writer.Integer(session.firstIndex);
    writer.Key("frameCount");
    writer.Integer(session.frameCount);
    writer.Key("fps");
    writer.Unsigned(session.fps);
    stringOrNull("firstFile", session.firstFileName);
    stringOrNull("lastFile", session.lastFileName);
    writer.Key("width");
    writer.Unsigned(session.width);
    writer.Key("height");
    writer.Unsigned(session.height);
    writer.Key("ignoredFileCount");
    writer.Unsigned(session.ignoredFileCount);
    stringOrNull("sourceFirstSha256", session.sourceFirstSha256);
    stringOrNull("sourceLastSha256", session.sourceLastSha256);
    writer.Key("sourceFirstLastIdentical");
    writer.Bool(session.sourceFirstLastIdentical);
    writer.EndObject();

    writer.Key("encode");
    writer.BeginObject();
    writer.Key("codec");
    writer.String("H.264");
    writer.Key("profile");
    writer.String("high");
    writer.Key("pixelFormat");
    writer.String("yuv420p");
    writer.Key("container");
    writer.String("MP4");
    writer.Key("bitrate");
    writer.Unsigned(session.settings.bitrate);
    writer.Key("bitrateMode");
    writer.String(session.encodeResult.rateControlMode.empty()
        ? "average"
        : session.encodeResult.rateControlMode);
    writer.Key("keyframeInterval");
    writer.Unsigned(session.settings.keyframeInterval);
    writer.Key("frameRateMode");
    writer.String("constant");
    // 单线程编码是“同帧同字节”的前提，写进清单便于复现实验判断（E20）。
    writer.Key("singleThreaded");
    writer.Bool(session.encodeResult.singleThreaded);
    writer.Key("normalizedTimestamps");
    writer.Bool(session.encodeResult.normalizedTimestamps);
    writer.Key("encodedSamples");
    writer.Unsigned(session.encodeResult.encodedSamples);
    writer.Key("durationSeconds");
    writer.Number(session.fps != 0
        ? static_cast<double>(session.frameCount) / static_cast<double>(session.fps)
        : 0.0);
    writer.Key("output");
    if (session.outputPath.empty())
    {
        writer.Null();
    }
    else
    {
        writer.String(PathText(session.outputPath));
    }
    writer.Key("outputBytes");
    writer.Unsigned(session.outputBytes);
    writer.Key("averageBitrate");
    writer.Unsigned(session.encodeCompleted && session.fps != 0 && session.frameCount > 0
        ? static_cast<std::uint64_t>((static_cast<double>(session.outputBytes) * 8.0 *
              static_cast<double>(session.fps)) / static_cast<double>(session.frameCount) +
              0.5)
        : 0);
    stringOrNull("sha256", session.outputSha256);
    writer.Key("encoderConfigApplied");
    writer.Bool(session.encodeResult.encoderConfigApplied);
    writer.Key("outputTypeFallback");
    writer.Bool(session.encodeResult.usedMinimalOutputType);
    writer.EndObject();

    writer.Key("verification");
    writer.BeginObject();
    writer.Key("status");
    if (session.verificationCompleted)
    {
        writer.String("ok");
    }
    else if (session.verification.decodedSamples > 0)
    {
        writer.String("failed");
    }
    else
    {
        writer.String("not-run");
    }
    writer.Key("decodedSamples");
    writer.Unsigned(session.verification.decodedSamples);
    writer.Key("width");
    writer.Unsigned(session.verification.width);
    writer.Key("height");
    writer.Unsigned(session.verification.height);
    writer.Key("containerFps");
    writer.Number(session.verification.containerFps);
    writer.Key("measuredFps");
    writer.Number(session.verification.measuredFps);
    writer.Key("constantFrameRate");
    writer.Bool(session.verification.constantFrameRate);
    writer.Key("firstTimestampSeconds");
    writer.Number(session.verification.firstTimestampSeconds);
    writer.Key("lastTimestampSeconds");
    writer.Number(session.verification.lastTimestampSeconds);
    writer.Key("durationSeconds");
    writer.Number(session.verification.durationSeconds);
    writer.Key("bitrate");
    writer.Unsigned(session.verification.bitrate);
    writer.Key("profile");
    writer.Unsigned(session.verification.profile);
    writer.Key("profileName");
    switch (session.verification.profile)
    {
    case 66: writer.String("baseline"); break;
    case 77: writer.String("main"); break;
    case 100: writer.String("high"); break;
    case 244: writer.String("high-444"); break;
    default: writer.String("unknown"); break;
    }
    writer.Key("sourceFirstLastIdentical");
    writer.Bool(session.verification.sourceFirstLastIdentical);
    writer.Key("firstLastDiffer");
    writer.Bool(session.verification.firstLastDiffer);
    stringOrNull("firstFrameSha256", session.verification.firstFrameSha256);
    stringOrNull("lastFrameSha256", session.verification.lastFrameSha256);
    writer.Key("keyframeCount");
    writer.Unsigned(session.verification.keyframeCount);
    writer.Key("compressedSampleCount");
    writer.Unsigned(session.verification.compressedSampleCount);
    writer.Key("maxKeyframeInterval");
    writer.Unsigned(session.verification.maxKeyframeInterval);
    writer.Key("compressedTimestampsMonotonic");
    writer.Bool(session.verification.compressedTimestampsMonotonic);
    writer.Key("compressedStatisticsAvailable");
    writer.Bool(session.verification.compressedStatisticsAvailable);
    stringOrNull("compressedNote", session.verification.compressedNote);
    writer.EndObject();

    writer.Key("hresults");
    writer.BeginArray();
    if (failure != nullptr)
    {
        for (const HrFailure& failureRecord : failure->hresults)
        {
            writer.BeginObject();
            writer.Key("stage");
            writer.String(failureRecord.stage);
            writer.Key("hresult");
            writer.String(failureRecord.text);
            writer.Key("value");
            writer.Integer(static_cast<std::int64_t>(failureRecord.value));
            writer.EndObject();
        }
    }
    writer.EndArray();
    writer.EndObject();
}

void PrintDiagnostic(const Diagnostic& diagnostic)
{
    std::cerr << diagnostic.key << ": " << diagnostic.detail << "\n";
    for (const HrFailure& failure : diagnostic.hresults)
    {
        std::cerr << "  at " << failure.stage << ": " << failure.text << " (" << failure.value
                  << ")\n";
    }
}

int RunSelftest(const std::filesystem::path& temporaryDirectory)
{
    Session session;
    session.selftest = true;
    session.framesDirectory = temporaryDirectory / "frames";
    session.pattern = "frame_%04d.ppm";
    session.firstIndex = 0;
    session.frameCount = kSelftestFrameCount;
    session.fps = kSelftestFps;
    session.outputPath = temporaryDirectory / "selftest.mp4";
    session.manifestPath = temporaryDirectory / "selftest-manifest.json";
    session.manifestRequested = true;

    Diagnostic diagnostic;
    bool success = GenerateSelftestFrames(session.framesDirectory, diagnostic) &&
        RunEncode(session, diagnostic);

    std::string json = [&]() {
        JsonWriter writer;
        WriteReport(writer, session, success ? nullptr : &diagnostic);
        return writer.Text();
    }();

    if (session.manifestRequested)
    {
        Diagnostic manifestDiagnostic;
        if (!WriteManifest(session.manifestPath, json, manifestDiagnostic))
        {
            if (success)
            {
                diagnostic = manifestDiagnostic;
                success = false;
                JsonWriter writer;
                WriteReport(writer, session, &diagnostic);
                json = writer.Text();
            }
            else
            {
                std::cerr << manifestDiagnostic.key << ": " << manifestDiagnostic.detail
                          << "\n";
            }
        }
    }

    std::cout << json << "\n";
    if (!success)
    {
        PrintDiagnostic(diagnostic);
        return static_cast<int>(ExitCode::SelftestFailed);
    }
    return static_cast<int>(ExitCode::Ok);
}
} // namespace
} // namespace se::videoencoder

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    using namespace se::videoencoder;

    try
    {
        Options options;
        Diagnostic diagnostic;
        if (!ParseArguments(argc, argv, options, diagnostic))
        {
            PrintDiagnostic(diagnostic);
            std::cerr << "Run VideoEncoder --help for usage.\n";
            return static_cast<int>(diagnostic.exitCode == ExitCode::Usage
                    ? ExitCode::Usage
                    : diagnostic.exitCode);
        }
        if (options.help)
        {
            PrintHelp();
            return static_cast<int>(ExitCode::Ok);
        }
        if (options.selftest)
        {
            return RunSelftest(options.selftestDirectory);
        }

        Session session;
        session.selftest = false;
        session.framesDirectory = options.framesDirectory;
        session.pattern = options.pattern;
        session.firstIndex = options.firstIndex;
        session.frameCount = options.frameCount;
        session.fps = options.fps;
        session.outputPath = options.outputPath;
        session.manifestPath = options.manifestPath;
        session.manifestRequested = options.manifestRequested;

        const bool success = RunEncode(session, diagnostic);
        JsonWriter writer;
        WriteReport(writer, session, success ? nullptr : &diagnostic);
        std::string json = writer.Text();

        if (session.manifestRequested)
        {
            Diagnostic manifestDiagnostic;
            if (!WriteManifest(session.manifestPath, json, manifestDiagnostic))
            {
                if (success)
                {
                    diagnostic = manifestDiagnostic;
                    diagnostic.exitCode = ExitCode::ManifestNotWritable;
                    JsonWriter failureWriter;
                    WriteReport(failureWriter, session, &diagnostic);
                    json = failureWriter.Text();
                }
                else
                {
                    std::cerr << manifestDiagnostic.key << ": " << manifestDiagnostic.detail
                              << "\n";
                }
            }
        }

        std::cout << json << "\n";
        if (!success)
        {
            PrintDiagnostic(diagnostic);
            return static_cast<int>(diagnostic.exitCode);
        }
        return static_cast<int>(ExitCode::Ok);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Encoder.InternalError: unhandled exception: " << exception.what()
                  << "\n";
        return static_cast<int>(ExitCode::Internal);
    }
    catch (...)
    {
        std::cerr << "Encoder.InternalError: unhandled exception\n";
        return static_cast<int>(ExitCode::Internal);
    }
}
