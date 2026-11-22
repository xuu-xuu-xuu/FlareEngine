#pragma once

#include "ResourceLayer/Assets/AssetIO.h"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flare
{
struct AudioClip
{
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0; // 1 = 单声道，2 = 双声道
    std::vector<std::int16_t> samples;

    [[nodiscard]] bool Valid() const noexcept
    {
        return sampleRate >= 8000 && sampleRate <= 192000 && (channels == 1 || channels == 2) &&
               !samples.empty() && (samples.size() % channels) == 0;
    }

    [[nodiscard]] std::uint64_t FrameCount() const noexcept
    {
        return channels ? static_cast<std::uint64_t>(samples.size() / channels) : 0;
    }

    [[nodiscard]] double DurationSeconds() const noexcept
    {
        return (sampleRate && channels) ? static_cast<double>(FrameCount()) / static_cast<double>(sampleRate) : 0.0;
    }
};

[[nodiscard]] bool IsAudioExtension(const std::filesystem::path& path);

[[nodiscard]] bool DecodeAudioSource(
    const std::filesystem::path& path,
    AudioClip& output,
    std::vector<AssetDiagnostic>& diagnostics);

[[nodiscard]] bool DecodeAudioSource(
    std::span<const std::uint8_t> encoded,
    std::string_view extensionHint,
    AudioClip& output,
    std::vector<AssetDiagnostic>& diagnostics);

[[nodiscard]] bool EncodeAudioContainer(
    const AudioClip& clip,
    std::string_view sourceSha256,
    std::vector<std::uint8_t>& output,
    std::vector<AssetDiagnostic>& diagnostics);

[[nodiscard]] bool DecodeAudioContainer(
    std::span<const std::uint8_t> bytes,
    std::string_view expectedSourceSha256,
    AudioClip& output,
    std::vector<AssetDiagnostic>& diagnostics);
} // namespace flare
