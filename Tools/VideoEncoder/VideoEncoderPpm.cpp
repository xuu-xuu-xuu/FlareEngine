#include "VideoEncoderPpm.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <utility>

namespace se::videoencoder
{
namespace
{
// 与 WritePpmImage 的实际输出一致：8 位 P6、无额外压缩。
constexpr std::uint32_t kMaxPpmDimension = 8192;
constexpr std::uint64_t kMaxPpmPixels = 8192ull * 8192ull;
constexpr std::uint64_t kMaxTrailingBytes = 1024ull * 1024ull;
constexpr int kMaxFrameCount = 1'000'000;
constexpr int kMaxFrameIndex = 9'999'999;
constexpr std::size_t kMaxPatternLength = 240;

bool SkipWhitespaceAndComments(std::istream& input)
{
    for (;;)
    {
        const int value = input.get();
        if (value == std::char_traits<char>::eof())
        {
            return false;
        }
        const char character = static_cast<char>(value);
        if (character == '#')
        {
            input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
            continue;
        }
        input.putback(character);
        return true;
    }
}

bool ReadToken(std::istream& input, std::string& token, char& terminator)
{
    token.clear();
    terminator = '\0';
    if (!SkipWhitespaceAndComments(input))
    {
        return false;
    }
    for (;;)
    {
        const int value = input.get();
        if (value == std::char_traits<char>::eof())
        {
            return !token.empty();
        }
        const char character = static_cast<char>(value);
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
            terminator = character;
            return !token.empty();
        }
        token.push_back(character);
        if (token.size() > 32)
        {
            return false;
        }
    }
}

bool ParseUnsigned(const std::string& token, std::uint64_t& value)
{
    if (token.empty())
    {
        return false;
    }
    value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool IsRegexSpecial(char character)
{
    constexpr const char* kSpecial = R"(\^$.|?*+()[]{}")";
    return std::strchr(kSpecial, character) != nullptr;
}

std::string EscapeRegex(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2u);
    for (const char character : text)
    {
        if (IsRegexSpecial(character))
        {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

// 解析文件头并核对文件长度；detail 使用文件名以保持诊断简短稳定。
bool ParsePpmHeader(std::ifstream& input, const std::filesystem::path& path, PpmFrame& frame,
    Diagnostic& diagnostic)
{
    const std::string fileName = PathText(path.filename());
    std::string magic;
    std::string widthToken;
    std::string heightToken;
    std::string maxToken;
    char terminator = '\0';

    if (!ReadToken(input, magic, terminator))
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (unreadable PPM header)", ExitCode::FrameUnsupported);
    }
    if (magic != "P6")
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (magic " + magic + "; only binary P6 is supported)",
            ExitCode::FrameUnsupported);
    }
    std::uint64_t width = 0;
    if (!ReadToken(input, widthToken, terminator) || !ParseUnsigned(widthToken, width))
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (width field is not a positive integer)", ExitCode::FrameUnsupported);
    }
    std::uint64_t height = 0;
    if (!ReadToken(input, heightToken, terminator) || !ParseUnsigned(heightToken, height))
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (height field is not a positive integer)", ExitCode::FrameUnsupported);
    }
    std::uint64_t maxValue = 0;
    if (!ReadToken(input, maxToken, terminator) || !ParseUnsigned(maxToken, maxValue))
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (maxval field is not an integer)", ExitCode::FrameUnsupported);
    }
    // ReadToken 已消费 maxval 后的一个空白字符；CRLF 时还要吃掉 LF。
    if (terminator == '\r' && input.peek() == '\n')
    {
        input.get();
    }

    if (width == 0 || height == 0)
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (" + widthToken + "x" + heightToken + "; empty frames are not supported)",
            ExitCode::FrameUnsupported);
    }
    if (width > kMaxPpmDimension || height > kMaxPpmDimension ||
        width * height > kMaxPpmPixels)
    {
        return Fail(diagnostic, "Encoder.FrameOversized",
            fileName + " (" + widthToken + "x" + heightToken +
                "; maximum supported frame is 8192x8192)",
            ExitCode::FrameOversized);
    }
    if (maxValue != 255)
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (maxval " + maxToken + "; only 8-bit P6 is supported)",
            ExitCode::FrameUnsupported);
    }

    const std::streampos position = input.tellg();
    if (position < 0)
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (cannot determine PPM header length)", ExitCode::FrameUnsupported);
    }

    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.maxValue = static_cast<std::uint32_t>(maxValue);
    frame.headerBytes = static_cast<std::uint64_t>(static_cast<std::streamoff>(position));
    frame.pixelBytes = width * height * 3ull;
    return true;
}

bool CheckFileExtent(const std::filesystem::path& path, PpmFrame& frame,
    Diagnostic& diagnostic)
{
    const std::string fileName = PathText(path.filename());
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            fileName + " (cannot read file size)", ExitCode::FrameUnsupported);
    }
    frame.fileBytes = static_cast<std::uint64_t>(size);
    if (frame.fileBytes < frame.headerBytes + frame.pixelBytes)
    {
        return Fail(diagnostic, "Encoder.FrameTruncated",
            fileName + " (" + std::to_string(frame.fileBytes) + " bytes; expected " +
                std::to_string(frame.headerBytes + frame.pixelBytes) + ")",
            ExitCode::FrameTruncated);
    }
    if (frame.fileBytes > frame.headerBytes + frame.pixelBytes + kMaxTrailingBytes)
    {
        return Fail(diagnostic, "Encoder.FrameOversized",
            fileName + " (" + std::to_string(frame.fileBytes) + " bytes; expected " +
                std::to_string(frame.headerBytes + frame.pixelBytes) + ")",
            ExitCode::FrameOversized);
    }
    return true;
}

bool ProbePpmFileInternal(const std::filesystem::path& path, PpmFrame& frame,
    Diagnostic& diagnostic)
{
    frame = {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            PathText(path) + " (cannot open frame file)", ExitCode::FrameUnsupported);
    }
    if (!ParsePpmHeader(input, path, frame, diagnostic))
    {
        return false;
    }
    return CheckFileExtent(path, frame, diagnostic);
}
} // namespace

bool BuildFramePattern(const std::string& pattern, FramePattern& compiled,
    Diagnostic& diagnostic)
{
    compiled = {};
    if (pattern.empty())
    {
        return Fail(diagnostic, "Encoder.PatternInvalid", "pattern is empty",
            ExitCode::PatternInvalid);
    }
    if (pattern.size() > kMaxPatternLength)
    {
        return Fail(diagnostic, "Encoder.PatternInvalid",
            "pattern is longer than " + std::to_string(kMaxPatternLength) + " bytes",
            ExitCode::PatternInvalid);
    }
    if (pattern.find('/') != std::string::npos || pattern.find('\\') != std::string::npos)
    {
        return Fail(diagnostic, "Encoder.PatternInvalid",
            "pattern must be a file name without path separators: " + pattern,
            ExitCode::PatternInvalid);
    }

    std::string regexText = "^";
    int conversionCount = 0;
    for (std::size_t index = 0; index < pattern.size();)
    {
        const char character = pattern[index];
        if (character != '%')
        {
            const std::string literal(1, character);
            regexText += EscapeRegex(literal);
            ++index;
            continue;
        }

        if (index + 1 < pattern.size() && pattern[index + 1] == '%')
        {
            regexText += '%';
            index += 2;
            continue;
        }

        std::size_t cursor = index + 1;
        if (cursor < pattern.size() && pattern[cursor] == '0')
        {
            ++cursor;
            std::size_t digits = 0;
            while (cursor < pattern.size() && std::isdigit(
                static_cast<unsigned char>(pattern[cursor])) != 0 && digits < 3)
            {
                ++cursor;
                ++digits;
            }
            if (digits == 0)
            {
                return Fail(diagnostic, "Encoder.PatternInvalid",
                    "zero-padded conversion needs a width, for example %04d: " + pattern,
                    ExitCode::PatternInvalid);
            }
        }
        else if (cursor < pattern.size() && std::isdigit(
            static_cast<unsigned char>(pattern[cursor])) != 0)
        {
            return Fail(diagnostic, "Encoder.PatternInvalid",
                "space-padded conversions are not supported, use zero padding like %04d: " +
                    pattern, ExitCode::PatternInvalid);
        }
        if (cursor >= pattern.size() ||
            (pattern[cursor] != 'd' && pattern[cursor] != 'i' && pattern[cursor] != 'u'))
        {
            return Fail(diagnostic, "Encoder.PatternInvalid",
                "pattern must contain one integer conversion such as %04d: " + pattern,
                ExitCode::PatternInvalid);
        }
        if (conversionCount != 0)
        {
            return Fail(diagnostic, "Encoder.PatternInvalid",
                "pattern contains more than one integer conversion: " + pattern,
                ExitCode::PatternInvalid);
        }
        ++conversionCount;
        regexText += "([0-9]+)";
        index = cursor + 1;
    }

    if (conversionCount != 1)
    {
        return Fail(diagnostic, "Encoder.PatternInvalid",
            "pattern must contain exactly one integer conversion such as %04d: " + pattern,
            ExitCode::PatternInvalid);
    }
    regexText += "$";
    compiled.text = pattern;
    compiled.scanRegex = regexText;
    return true;
}

std::string FrameFileName(const std::string& pattern, int index)
{
    if (index < 0 || index > kMaxFrameIndex)
    {
        return {};
    }
    char buffer[512] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), pattern.c_str(), index);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer))
    {
        return {};
    }
    return buffer;
}

bool ProbePpmFile(const std::filesystem::path& path, PpmFrame& frame, Diagnostic& diagnostic)
{
    return ProbePpmFileInternal(path, frame, diagnostic);
}

bool ScanFrameSequence(const std::filesystem::path& directory, const std::string& pattern,
    int firstIndex, int frameCount, FrameSequence& sequence, Diagnostic& diagnostic)
{
    sequence = {};
    FramePattern compiled;
    if (!BuildFramePattern(pattern, compiled, diagnostic))
    {
        return false;
    }
    if (frameCount < 1 || frameCount > kMaxFrameCount)
    {
        return Fail(diagnostic, "Encoder.FrameCountMismatch",
            "count must be between 1 and " + std::to_string(kMaxFrameCount) + ", got " +
                std::to_string(frameCount),
            ExitCode::FrameCountMismatch);
    }
    if (firstIndex < 0 || firstIndex > kMaxFrameIndex - (frameCount - 1))
    {
        return Fail(diagnostic, "Encoder.FrameCountMismatch",
            "frame range " + std::to_string(firstIndex) + ".." +
                std::to_string(static_cast<long long>(firstIndex) + frameCount - 1) +
                " is outside 0.." + std::to_string(kMaxFrameIndex),
            ExitCode::FrameCountMismatch);
    }

    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        return Fail(diagnostic, "Encoder.InputDirectoryMissing",
            PathText(directory) + " (frames directory does not exist)",
            ExitCode::FrameMissing);
    }

    std::regex expression;
    try
    {
        expression = std::regex(compiled.scanRegex,
            std::regex_constants::ECMAScript | std::regex_constants::optimize);
    }
    catch (const std::regex_error&)
    {
        return Fail(diagnostic, "Encoder.PatternInvalid",
            "cannot compile frame pattern: " + pattern, ExitCode::PatternInvalid);
    }

    // 同一编号出现多个文件（例如 frame_0012.ppm 与 frame_12.ppm）会让输入有歧义，
    // 必须在编码前判为重复而不是任意挑一个。
    std::map<int, std::vector<std::filesystem::path>> filesByIndex;
    std::uint64_t ignoredFileCount = 0;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error)
    {
        return Fail(diagnostic, "Encoder.InputDirectoryUnreadable",
            PathText(directory) + " (cannot enumerate directory)",
            ExitCode::FrameMissing);
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        const std::filesystem::directory_entry entry = *iterator;
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && !entryError)
        {
            const std::filesystem::path entryPath = entry.path();
            const std::string fileName = PathText(entryPath.filename());
            std::smatch match;
            if (std::regex_match(fileName, match, expression))
            {
                const std::string digits = match[1].str();
                std::uint64_t parsedIndex = 0;
                const auto result = std::from_chars(digits.data(),
                    digits.data() + digits.size(), parsedIndex);
                if (result.ec == std::errc{} &&
                    result.ptr == digits.data() + digits.size() &&
                    parsedIndex <= static_cast<std::uint64_t>(kMaxFrameIndex))
                {
                    const int index = static_cast<int>(parsedIndex);
                    if (index >= firstIndex && index < firstIndex + frameCount)
                    {
                        filesByIndex[index].push_back(entryPath);
                    }
                    else
                    {
                        ++ignoredFileCount;
                    }
                }
                else
                {
                    ++ignoredFileCount;
                }
            }
        }
        iterator.increment(error);
        if (error)
        {
            return Fail(diagnostic, "Encoder.InputDirectoryUnreadable",
                PathText(directory) + " (directory enumeration failed)",
                ExitCode::FrameMissing);
        }
    }

    for (const auto& [index, paths] : filesByIndex)
    {
        if (paths.size() > 1)
        {
            std::string detail = "index " + std::to_string(index) + " is provided by ";
            for (std::size_t position = 0; position < paths.size(); ++position)
            {
                if (position != 0)
                {
                    detail += " and ";
                }
                detail += PathText(paths[position]);
            }
            return Fail(diagnostic, "Encoder.FrameDuplicate", detail,
                ExitCode::FrameDuplicate);
        }
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    sequence.frames.clear();
    sequence.frames.reserve(static_cast<std::size_t>(frameCount));
    for (int offset = 0; offset < frameCount; ++offset)
    {
        const int index = firstIndex + offset;
        const std::string expectedName = FrameFileName(pattern, index);
        const auto found = filesByIndex.find(index);
        if (found == filesByIndex.end())
        {
            return Fail(diagnostic, "Encoder.FrameMissing",
                expectedName.empty() ? ("index " + std::to_string(index)) : expectedName,
                ExitCode::FrameMissing);
        }
        const std::filesystem::path& framePath = found->second.front();
        PpmFrame frame;
        if (!ProbePpmFile(framePath, frame, diagnostic))
        {
            return false;
        }
        sequence.frames.push_back(framePath);
        if (offset == 0)
        {
            width = frame.width;
            height = frame.height;
            if ((width % 2u) != 0 || (height % 2u) != 0)
            {
                return Fail(diagnostic, "Encoder.DimensionsUnsupported",
                    PathText(framePath.filename()) + " is " + std::to_string(width) + "x" +
                        std::to_string(height) +
                        "; yuv420p requires even width and height",
                    ExitCode::FrameUnsupported);
            }
        }
        else if (frame.width != width || frame.height != height)
        {
            return Fail(diagnostic, "Encoder.FrameDimensionMismatch",
                PathText(framePath.filename()) + " is " + std::to_string(frame.width) + "x" +
                    std::to_string(frame.height) + ", expected " + std::to_string(width) + "x" +
                    std::to_string(height),
                ExitCode::FrameDimensionMismatch);
        }
    }

    sequence.directory = directory;
    sequence.pattern = pattern;
    sequence.firstIndex = firstIndex;
    sequence.frameCount = frameCount;
    sequence.width = width;
    sequence.height = height;
    sequence.firstFileName = FrameFileName(pattern, firstIndex);
    sequence.lastFileName = FrameFileName(pattern, firstIndex + frameCount - 1);
    sequence.ignoredFileCount = ignoredFileCount;
    return true;
}

bool LoadPpmFrame(const std::filesystem::path& path, std::uint32_t expectedWidth,
    std::uint32_t expectedHeight, std::vector<std::uint8_t>& rgb, Diagnostic& diagnostic)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return Fail(diagnostic, "Encoder.FrameUnsupported",
            PathText(path) + " (cannot open frame file)", ExitCode::FrameUnsupported);
    }
    PpmFrame frame;
    if (!ParsePpmHeader(input, path, frame, diagnostic))
    {
        return false;
    }
    if (frame.width != expectedWidth || frame.height != expectedHeight)
    {
        return Fail(diagnostic, "Encoder.FrameDimensionMismatch",
            PathText(path.filename()) + " is " + std::to_string(frame.width) + "x" +
                std::to_string(frame.height) + ", expected " +
                std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight),
            ExitCode::FrameDimensionMismatch);
    }
    rgb.resize(static_cast<std::size_t>(frame.pixelBytes));
    input.read(reinterpret_cast<char*>(rgb.data()),
        static_cast<std::streamsize>(frame.pixelBytes));
    if (input.gcount() != static_cast<std::streamsize>(frame.pixelBytes))
    {
        return Fail(diagnostic, "Encoder.FrameTruncated",
            PathText(path.filename()) + " (pixel data is shorter than the declared size)",
            ExitCode::FrameTruncated);
    }
    return true;
}

void ConvertRgbToNv12(std::span<const std::uint8_t> rgb, std::uint32_t width,
    std::uint32_t height, std::span<std::uint8_t> nv12)
{
    const std::size_t expectedRgb = static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 3u;
    const std::size_t expectedNv12 = static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 3u / 2u;
    if (width == 0 || height == 0 || (width % 2u) != 0 || (height % 2u) != 0 ||
        rgb.size() < expectedRgb || nv12.size() < expectedNv12)
    {
        return;
    }

    std::uint8_t* lumaPlane = nv12.data();
    std::uint8_t* chromaPlane = nv12.data() +
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    for (std::uint32_t y = 0; y < height; y += 2)
    {
        const std::uint8_t* row0 = rgb.data() + static_cast<std::size_t>(y) * width * 3u;
        const std::uint8_t* row1 = row0 + static_cast<std::size_t>(width) * 3u;
        std::uint8_t* luma0 = lumaPlane + static_cast<std::size_t>(y) * width;
        std::uint8_t* luma1 = luma0 + width;
        std::uint8_t* chroma = chromaPlane + static_cast<std::size_t>(y / 2) * width;

        for (std::uint32_t x = 0; x < width; x += 2)
        {
            const std::uint8_t* pixel00 = row0 + static_cast<std::size_t>(x) * 3u;
            const std::uint8_t* pixel01 = pixel00 + 3u;
            const std::uint8_t* pixel10 = row1 + static_cast<std::size_t>(x) * 3u;
            const std::uint8_t* pixel11 = pixel10 + 3u;

            const int r00 = pixel00[0];
            const int g00 = pixel00[1];
            const int b00 = pixel00[2];
            const int r01 = pixel01[0];
            const int g01 = pixel01[1];
            const int b01 = pixel01[2];
            const int r10 = pixel10[0];
            const int g10 = pixel10[1];
            const int b10 = pixel10[2];
            const int r11 = pixel11[0];
            const int g11 = pixel11[1];
            const int b11 = pixel11[2];

            const auto luma = [](int red, int green, int blue) {
                const int value = ((47 * red + 157 * green + 16 * blue + 128) >> 8) + 16;
                return static_cast<std::uint8_t>(std::clamp(value, 16, 235));
            };
            luma0[x] = luma(r00, g00, b00);
            luma0[x + 1] = luma(r01, g01, b01);
            luma1[x] = luma(r10, g10, b10);
            luma1[x + 1] = luma(r11, g11, b11);

            const int red = (r00 + r01 + r10 + r11 + 2) >> 2;
            const int green = (g00 + g01 + g10 + g11 + 2) >> 2;
            const int blue = (b00 + b01 + b10 + b11 + 2) >> 2;
            chroma[x] = static_cast<std::uint8_t>(std::clamp(
                ((-26 * red - 86 * green + 112 * blue + 128) >> 8) + 128, 16, 240));
            chroma[x + 1] = static_cast<std::uint8_t>(std::clamp(
                ((112 * red - 102 * green - 10 * blue + 128) >> 8) + 128, 16, 240));
        }
    }
}
} // namespace se::videoencoder
