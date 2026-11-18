#include "ResourceLayer/Assets/AudioClip.h"

#include "CoreLayer/Hash.h"
#include "dr_wav/dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>

namespace se
{
namespace
{
constexpr std::uint8_t kAudioContainerMagic[]{'S', 'E', 'A', 'U', 'D', '0', '1', 0};
constexpr std::uint32_t kAudioContainerVersion = 1;
constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 2 + 2 + 8 + 64 + 64; // 156 bytes
constexpr std::uint64_t kMaxDecodedSamples = 64ull * 1024ull * 1024ull; // 128 MiB s16 PCM cap
constexpr std::uint32_t kMinSampleRate = 8000;
constexpr std::uint32_t kMaxSampleRate = 192000;

std::string LowerString(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool FailAudio(std::vector<AssetDiagnostic>& diagnostics, const char* key, std::string detail = {})
{
    asset_io::Diagnostic(diagnostics, key, std::move(detail));
    return false;
}

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t ReadU16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) noexcept
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* p) noexcept
{
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
    {
        value |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    }
    return value;
}

bool ValidHashText(std::string_view text) noexcept
{
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool DecodeWav(std::span<const std::uint8_t> encoded, AudioClip& output, std::vector<AssetDiagnostic>& diagnostics)
{
    drwav wav{};
    if (!drwav_init_memory(&wav, encoded.data(), encoded.size(), nullptr))
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "wav header");
    }
    struct Guard
    {
        drwav* ptr;
        ~Guard() { drwav_uninit(ptr); }
    } guard{&wav};

    if (wav.sampleRate < kMinSampleRate || wav.sampleRate > kMaxSampleRate ||
        (wav.channels != 1 && wav.channels != 2) || wav.totalPCMFrameCount == 0)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "wav format");
    }

    const std::uint64_t totalSamples = wav.totalPCMFrameCount * static_cast<std::uint64_t>(wav.channels);
    if (wav.totalPCMFrameCount > kMaxDecodedSamples / static_cast<std::uint64_t>(wav.channels) ||
        totalSamples > kMaxDecodedSamples)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "wav frame budget exceeded");
    }

    AudioClip clip;
    clip.sampleRate = wav.sampleRate;
    clip.channels = wav.channels;
    clip.samples.resize(static_cast<std::size_t>(totalSamples));
    const drwav_uint64 readFrames = drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, clip.samples.data());
    if (readFrames == 0)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "wav empty pcm");
    }
    clip.samples.resize(static_cast<std::size_t>(readFrames * clip.channels));
    if (!clip.Valid())
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "wav validation");
    }
    output = std::move(clip);
    return true;
}

bool DecodeOgg(std::span<const std::uint8_t> encoded, AudioClip& output, std::vector<AssetDiagnostic>& diagnostics)
{
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "ogg size overflow");
    }
    int error = VORBIS__no_error;
    stb_vorbis* vorbis = stb_vorbis_open_memory(encoded.data(), static_cast<int>(encoded.size()), &error, nullptr);
    if (!vorbis)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "ogg open (" + std::to_string(error) + ")");
    }
    struct Guard
    {
        stb_vorbis* ptr;
        ~Guard() { stb_vorbis_close(ptr); }
    } guard{vorbis};

    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.sample_rate < kMinSampleRate || info.sample_rate > kMaxSampleRate ||
        (info.channels != 1 && info.channels != 2))
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "ogg format");
    }

    AudioClip clip;
    clip.sampleRate = info.sample_rate;
    clip.channels = static_cast<std::uint16_t>(info.channels);

    constexpr std::size_t kChunkFrames = 4096;
    std::vector<std::int16_t> chunk(kChunkFrames * clip.channels);
    while (true)
    {
        const int frames = stb_vorbis_get_samples_short_interleaved(
            vorbis,
            info.channels,
            chunk.data(),
            static_cast<int>(chunk.size()));
        if (frames < 0)
        {
            return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "ogg stream error");
        }
        if (frames == 0)
        {
            break;
        }
        const std::size_t newSamples = static_cast<std::size_t>(frames) * clip.channels;
        if (clip.samples.size() + newSamples > kMaxDecodedSamples)
        {
            return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "ogg sample budget exceeded");
        }
        clip.samples.insert(clip.samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(newSamples));
    }

    if (!clip.Valid())
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "ogg empty pcm");
    }
    output = std::move(clip);
    return true;
}
} // namespace

bool IsAudioExtension(const std::filesystem::path& path)
{
    const auto ext = LowerString(asset_io::Utf8(path.extension()));
    return ext == ".wav" || ext == ".ogg" || ext == ".seaud";
}

bool DecodeAudioSource(
    const std::filesystem::path& path,
    AudioClip& output,
    std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(path, bytes))
    {
        return FailAudio(diagnostics, "Assets.ReadFailed", asset_io::Utf8(path));
    }
    return DecodeAudioSource(bytes, asset_io::Utf8(path.extension()), output, diagnostics);
}

bool DecodeAudioSource(
    std::span<const std::uint8_t> encoded,
    std::string_view extensionHint,
    AudioClip& output,
    std::vector<AssetDiagnostic>& diagnostics)
{
    if (encoded.empty() || encoded.size() > asset_io::MaxFileBytes)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "encoded audio size");
    }

    std::string ext = LowerString(std::string(extensionHint));
    if (ext.empty())
    {
        if (encoded.size() >= sizeof(kAudioContainerMagic) &&
            std::memcmp(encoded.data(), kAudioContainerMagic, sizeof(kAudioContainerMagic)) == 0)
        {
            ext = ".seaud";
        }
        else if (encoded.size() >= 12 && std::memcmp(encoded.data(), "RIFF", 4) == 0 &&
                 std::memcmp(encoded.data() + 8, "WAVE", 4) == 0)
        {
            ext = ".wav";
        }
        else if (encoded.size() >= 4 && std::memcmp(encoded.data(), "OggS", 4) == 0)
        {
            ext = ".ogg";
        }
    }

    if (ext == ".wav")
    {
        return DecodeWav(encoded, output, diagnostics);
    }
    if (ext == ".ogg")
    {
        return DecodeOgg(encoded, output, diagnostics);
    }
    if (ext == ".seaud")
    {
        return DecodeAudioContainer(encoded, {}, output, diagnostics);
    }
    return FailAudio(diagnostics, "Assets.AudioUnsupportedFormat", std::string(extensionHint));
}

bool EncodeAudioContainer(
    const AudioClip& clip,
    std::string_view sourceSha256,
    std::vector<std::uint8_t>& output,
    std::vector<AssetDiagnostic>& diagnostics)
{
    if (!clip.Valid() || !ValidHashText(sourceSha256) || clip.samples.size() > kMaxDecodedSamples)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "invalid clip container encode");
    }
    const std::size_t payloadBytes = clip.samples.size() * sizeof(std::int16_t);
    if (kHeaderSize + payloadBytes > asset_io::MaxFileBytes)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "container file budget");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize + payloadBytes);
    bytes.insert(bytes.end(), std::begin(kAudioContainerMagic), std::end(kAudioContainerMagic));
    AppendU32(bytes, kAudioContainerVersion);
    AppendU32(bytes, clip.sampleRate);
    AppendU16(bytes, clip.channels);
    AppendU16(bytes, 0);
    AppendU64(bytes, clip.FrameCount());
    bytes.insert(bytes.end(), sourceSha256.begin(), sourceSha256.end());

    // Compute hash over canonical little-endian PCM byte representation (identical on Windows x64).
    std::vector<std::uint8_t> pcmBytes(payloadBytes);
    for (std::size_t i = 0; i < clip.samples.size(); ++i)
    {
        const auto sample = static_cast<std::uint16_t>(clip.samples[i]);
        pcmBytes[i * 2 + 0] = static_cast<std::uint8_t>(sample);
        pcmBytes[i * 2 + 1] = static_cast<std::uint8_t>(sample >> 8);
    }
    const std::string payloadHash = Sha256(pcmBytes);
    if (!ValidHashText(payloadHash))
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "container sha256");
    }
    bytes.insert(bytes.end(), payloadHash.begin(), payloadHash.end());
    bytes.insert(bytes.end(), pcmBytes.begin(), pcmBytes.end());
    output = std::move(bytes);
    return true;
}

bool DecodeAudioContainer(
    std::span<const std::uint8_t> bytes,
    std::string_view expectedSourceSha256,
    AudioClip& output,
    std::vector<AssetDiagnostic>& diagnostics)
{
    if (bytes.size() < kHeaderSize || bytes.size() > asset_io::MaxFileBytes ||
        std::memcmp(bytes.data(), kAudioContainerMagic, sizeof(kAudioContainerMagic)) != 0)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "seaud header");
    }

    const std::uint32_t version = ReadU32(bytes.data() + 8);
    const std::uint32_t sampleRate = ReadU32(bytes.data() + 12);
    const std::uint16_t channels = ReadU16(bytes.data() + 16);
    const std::uint16_t reserved = ReadU16(bytes.data() + 18);
    const std::uint64_t frameCount = ReadU64(bytes.data() + 20);
    const std::string_view sourceHash(reinterpret_cast<const char*>(bytes.data() + 28), 64);
    const std::string_view payloadHash(reinterpret_cast<const char*>(bytes.data() + 92), 64);

    if (version != kAudioContainerVersion || reserved != 0 ||
        sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate ||
        (channels != 1 && channels != 2) || frameCount == 0 ||
        frameCount > kMaxDecodedSamples / channels ||
        !ValidHashText(sourceHash) || !ValidHashText(payloadHash))
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "seaud fields");
    }
    if (!expectedSourceSha256.empty() && sourceHash != expectedSourceSha256)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "seaud stale source hash");
    }

    const std::uint64_t expectedPayloadBytes = frameCount * channels * sizeof(std::int16_t);
    if (bytes.size() - kHeaderSize != expectedPayloadBytes)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "seaud payload length");
    }

    const auto payloadSpan = bytes.subspan(kHeaderSize);
    if (Sha256(payloadSpan) != payloadHash)
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "seaud payload checksum");
    }

    AudioClip clip;
    clip.sampleRate = sampleRate;
    clip.channels = channels;
    clip.samples.resize(static_cast<std::size_t>(frameCount * channels));
    for (std::size_t i = 0; i < clip.samples.size(); ++i)
    {
        clip.samples[i] = static_cast<std::int16_t>(ReadU16(payloadSpan.data() + i * 2));
    }
    if (!clip.Valid())
    {
        return FailAudio(diagnostics, "Assets.AudioDecodeFailed", "seaud invalid clip");
    }
    output = std::move(clip);
    return true;
}
} // namespace se
