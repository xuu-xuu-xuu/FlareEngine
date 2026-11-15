#pragma once
// W08 独立视频编码器：诊断、UTF-8/JSON 输出与 SHA-256 的公共小工具。
// 该工具不链接引擎目标，因此这里只使用 C++ 标准库与 Win32。
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace se::videoencoder
{
// 退出码：成功为 0；自检失败固定为 1（便于 CTest 直接判定）；其余按失败类别区分，
// 调用脚本可以只凭退出码区分“缺帧/重复帧/尺寸不一致/编码失败/验证失败”。
enum class ExitCode : int
{
    Ok = 0,
    SelftestFailed = 1,
    Usage = 2,
    FrameMissing = 3,
    FrameDuplicate = 4,
    FrameOversized = 5,
    FrameUnsupported = 6,
    FrameTruncated = 7,
    FrameDimensionMismatch = 8,
    FrameCountMismatch = 9,
    FpsInvalid = 10,
    OutputNotWritable = 11,
    PatternInvalid = 12,
    EncodeFailed = 13,
    VerifyFailed = 14,
    ManifestNotWritable = 15,
    Internal = 16,
};

// 一条失败的 Media Foundation / COM 调用记录，只在失败摘要中出现。
struct HrFailure
{
    std::string stage;
    long value = 0;
    std::string text;
};

// 结构化失败：key 是稳定英文键（如 Encoder.FrameMissing），detail 是具体细节。
struct Diagnostic
{
    std::string key;
    std::string detail;
    ExitCode exitCode = ExitCode::Internal;
    std::vector<HrFailure> hresults;

    bool Filled() const
    {
        return !key.empty();
    }
};

bool Fail(Diagnostic& diagnostic, std::string key, std::string detail, ExitCode exitCode);
bool FailHr(Diagnostic& diagnostic, std::string key, std::string detail,
    std::string stage, long hresult);
// HRESULT 成功时返回 true；失败时写入固定键 Encoder.MediaFoundationCallFailed。
bool CheckHr(Diagnostic& diagnostic, std::string detail, std::string stage, long hresult);

std::string WideToUtf8(std::wstring_view text);
std::string PathText(const std::filesystem::path& path);
std::string HresultText(long value);
// 去掉多余小数零的十进制格式；JSON 与终端摘要共用。
std::string FormatNumber(double value, int maxDecimals = 6);

// 流式 SHA-256，用于帧缓冲和整个 MP4 文件，避免额外载入整份文件。
class Sha256
{
public:
    Sha256();
    void Update(std::span<const std::uint8_t> bytes);
    std::string FinalHex();

private:
    void Transform(const std::uint8_t* block);

    std::uint32_t state_[8] = {};
    std::uint64_t bitCount_ = 0;
    std::uint8_t buffer_[64] = {};
    std::size_t bufferSize_ = 0;
    bool finalized_ = false;
};

std::string Sha256Hex(std::span<const std::uint8_t> bytes);
// 读取文件失败时返回空字符串并把原因写入 error。
std::string Sha256FileHex(const std::filesystem::path& path, std::string& error);
std::uint64_t FileSizeBytes(const std::filesystem::path& path);

// 有序、缩进固定的 JSON 写出器，足以生成编码器清单而不引入第三方库。
class JsonWriter
{
public:
    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();
    void Key(std::string_view key);
    void String(std::string_view value);
    void Integer(std::int64_t value);
    void Unsigned(std::uint64_t value);
    void Number(double value, int maxDecimals = 6);
    void Bool(bool value);
    void Null();
    std::string Text() const
    {
        return text_;
    }

private:
    struct Frame
    {
        bool array = false;
        bool empty = true;
    };

    void WritePrefix();
    void Indent(std::size_t depth);
    void PushKey(std::string_view key);

    std::string text_;
    std::string pendingKey_;
    std::vector<Frame> stack_;
};
} // namespace se::videoencoder
