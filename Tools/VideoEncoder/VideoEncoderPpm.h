#pragma once
// W08 独立视频编码器：编号 PPM 序列的模式解析、预检与 RGB→NV12 转换。
// 只实现引擎 WritePpmImage 实际写出的二进制 P6 / 8 位格式，避免依赖引擎库。
#include "VideoEncoderSupport.h"

namespace se::videoencoder
{
// 帧名模式只允许一个零填充的整数转换，例如 frame_%04d.ppm。
struct FramePattern
{
    std::string text;
    std::string scanRegex;
};

// 预检通过后的序列信息。宽度/高度取首帧，其余帧必须一致。
struct FrameSequence
{
    std::filesystem::path directory;
    std::string pattern;
    std::string firstFileName;
    std::string lastFileName;
    int firstIndex = 0;
    int frameCount = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t ignoredFileCount = 0;
    // 预检中确认存在的帧路径，编码阶段直接复用，避免再次拼接非 ASCII 文件名。
    std::vector<std::filesystem::path> frames;
};

struct PpmFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t maxValue = 0;
    std::uint64_t fileBytes = 0;
    std::uint64_t headerBytes = 0;
    std::uint64_t pixelBytes = 0;
};

bool BuildFramePattern(const std::string& pattern, FramePattern& compiled,
    Diagnostic& diagnostic);
// 按已校验的模式生成帧文件名；索引越界或格式过长时返回空串。
std::string FrameFileName(const std::string& pattern, int index);
// 只解析文件头并核对文件大小，不读取像素。
bool ProbePpmFile(const std::filesystem::path& path, PpmFrame& frame,
    Diagnostic& diagnostic);
bool ScanFrameSequence(const std::filesystem::path& directory, const std::string& pattern,
    int firstIndex, int frameCount, FrameSequence& sequence, Diagnostic& diagnostic);
bool LoadPpmFrame(const std::filesystem::path& path, std::uint32_t expectedWidth,
    std::uint32_t expectedHeight, std::vector<std::uint8_t>& rgb,
    Diagnostic& diagnostic);
// BT.709 有限范围（Y: 16..235，Cb/Cr: 16..240）的整数近似转换。
void ConvertRgbToNv12(std::span<const std::uint8_t> rgb, std::uint32_t width,
    std::uint32_t height, std::span<std::uint8_t> nv12);
} // namespace se::videoencoder
