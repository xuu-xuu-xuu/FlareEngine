#include "VideoEncoderSupport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace flare::videoencoder
{
namespace
{
constexpr std::uint32_t kSha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

std::uint32_t RotateRight(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

std::string EscapeJson(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char character : text)
    {
        const unsigned char byte = static_cast<unsigned char>(character);
        switch (character)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (byte < 0x20)
            {
                char buffer[8] = {};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                escaped += buffer;
            }
            else
            {
                escaped.push_back(character);
            }
            break;
        }
    }
    return escaped;
}
} // namespace

bool Fail(Diagnostic& diagnostic, std::string key, std::string detail, ExitCode exitCode)
{
    diagnostic.key = std::move(key);
    diagnostic.detail = std::move(detail);
    diagnostic.exitCode = exitCode;
    return false;
}

bool FailHr(Diagnostic& diagnostic, std::string key, std::string detail, std::string stage,
    long hresult)
{
    HrFailure failure;
    failure.stage = std::move(stage);
    failure.value = hresult;
    failure.text = HresultText(hresult);
    diagnostic.hresults.push_back(std::move(failure));
    return Fail(diagnostic, std::move(key), std::move(detail), ExitCode::EncodeFailed);
}

bool CheckHr(Diagnostic& diagnostic, std::string detail, std::string stage, long hresult)
{
    if (SUCCEEDED(static_cast<HRESULT>(hresult)))
    {
        return true;
    }
    return FailHr(diagnostic, "Encoder.MediaFoundationCallFailed", std::move(detail),
        std::move(stage), hresult);
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return "<unconvertible-path>";
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string PathText(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::string HresultText(long value)
{
    char text[16] = {};
    std::snprintf(text, sizeof(text), "0x%08lX",
        static_cast<unsigned long>(static_cast<std::uint32_t>(value)));
    return text;
}

std::string FormatNumber(double value, int maxDecimals)
{
    if (!std::isfinite(value))
    {
        return "0";
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::clamp(maxDecimals, 0, 9), value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos)
    {
        while (!text.empty() && text.back() == '0')
        {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.')
        {
            text.pop_back();
        }
    }
    if (text == "-0")
    {
        text = "0";
    }
    return text;
}

Sha256::Sha256()
{
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
}

void Sha256::Transform(const std::uint8_t* block)
{
    std::uint32_t words[64] = {};
    for (int index = 0; index < 16; ++index)
    {
        words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
            (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
            (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
            static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (int index = 16; index < 64; ++index)
    {
        const std::uint32_t s0 = RotateRight(words[index - 15], 7) ^
            RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
        const std::uint32_t s1 = RotateRight(words[index - 2], 17) ^
            RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (int index = 0; index < 64; ++index)
    {
        const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choose + kSha256RoundConstants[index] + words[index];
        const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(std::span<const std::uint8_t> bytes)
{
    if (finalized_ || bytes.empty())
    {
        return;
    }
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t chunk = std::min<std::size_t>(64 - bufferSize_, bytes.size() - offset);
        std::memcpy(buffer_ + bufferSize_, bytes.data() + offset, chunk);
        bufferSize_ += chunk;
        offset += chunk;
        if (bufferSize_ == 64)
        {
            Transform(buffer_);
            bitCount_ += 512;
            bufferSize_ = 0;
        }
    }
}

std::string Sha256::FinalHex()
{
    if (!finalized_)
    {
        bitCount_ += static_cast<std::uint64_t>(bufferSize_) * 8u;
        buffer_[bufferSize_++] = 0x80;
        if (bufferSize_ > 56)
        {
            std::fill(buffer_ + bufferSize_, buffer_ + 64, 0);
            Transform(buffer_);
            bufferSize_ = 0;
        }
        std::fill(buffer_ + bufferSize_, buffer_ + 56, 0);
        for (int index = 0; index < 8; ++index)
        {
            buffer_[56 + index] = static_cast<std::uint8_t>(bitCount_ >> (56 - index * 8));
        }
        Transform(buffer_);
        bufferSize_ = 0;
        finalized_ = true;
    }

    std::string hex;
    hex.reserve(64);
    char part[16] = {};
    for (const std::uint32_t word : state_)
    {
        std::snprintf(part, sizeof(part), "%08x", word);
        hex += part;
    }
    return hex;
}

std::string Sha256Hex(std::span<const std::uint8_t> bytes)
{
    Sha256 hasher;
    hasher.Update(bytes);
    return hasher.FinalHex();
}

std::string Sha256FileHex(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "cannot open file: " + PathText(path);
        return {};
    }
    Sha256 hasher;
    std::vector<std::uint8_t> buffer(1u << 20);
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read > 0)
        {
            hasher.Update(std::span<const std::uint8_t>(buffer.data(),
                static_cast<std::size_t>(read)));
        }
    }
    if (!input.eof())
    {
        error = "read failed: " + PathText(path);
        return {};
    }
    return hasher.FinalHex();
}

std::uint64_t FileSizeBytes(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(size);
}

void JsonWriter::Indent(std::size_t depth)
{
    text_.append(depth * 2u, ' ');
}

void JsonWriter::PushKey(std::string_view key)
{
    pendingKey_ = "\"";
    pendingKey_ += EscapeJson(key);
    pendingKey_ += "\": ";
}

void JsonWriter::WritePrefix()
{
    if (!stack_.empty())
    {
        Frame& parent = stack_.back();
        if (!parent.empty)
        {
            text_ += ',';
        }
        parent.empty = false;
        text_ += '\n';
        Indent(stack_.size());
    }
    if (!pendingKey_.empty())
    {
        text_ += pendingKey_;
        pendingKey_.clear();
    }
}

void JsonWriter::BeginObject()
{
    WritePrefix();
    text_ += '{';
    stack_.push_back({false, true});
}

void JsonWriter::EndObject()
{
    if (stack_.empty() || stack_.back().array)
    {
        return;
    }
    const bool empty = stack_.back().empty;
    stack_.pop_back();
    if (!empty)
    {
        text_ += '\n';
        Indent(stack_.size());
    }
    text_ += '}';
}

void JsonWriter::BeginArray()
{
    WritePrefix();
    text_ += '[';
    stack_.push_back({true, true});
}

void JsonWriter::EndArray()
{
    if (stack_.empty() || !stack_.back().array)
    {
        return;
    }
    const bool empty = stack_.back().empty;
    stack_.pop_back();
    if (!empty)
    {
        text_ += '\n';
        Indent(stack_.size());
    }
    text_ += ']';
}

void JsonWriter::Key(std::string_view key)
{
    PushKey(key);
}

void JsonWriter::String(std::string_view value)
{
    WritePrefix();
    text_ += '"';
    text_ += EscapeJson(value);
    text_ += '"';
}

void JsonWriter::Integer(std::int64_t value)
{
    WritePrefix();
    text_ += std::to_string(value);
}

void JsonWriter::Unsigned(std::uint64_t value)
{
    WritePrefix();
    text_ += std::to_string(value);
}

void JsonWriter::Number(double value, int maxDecimals)
{
    WritePrefix();
    text_ += FormatNumber(value, maxDecimals);
}

void JsonWriter::Bool(bool value)
{
    WritePrefix();
    text_ += value ? "true" : "false";
}

void JsonWriter::Null()
{
    WritePrefix();
    text_ += "null";
}
} // namespace flare::videoencoder
