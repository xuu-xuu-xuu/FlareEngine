#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <compressapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;

constexpr char kFooterMagic[12] = {'F', 'L', 'A', 'R', 'E', 'S', 'E', 'T', 'U', 'P', '1', '\0'};
constexpr char kUninstallMagic[12] = {'F', 'L', 'A', 'R', 'E', 'U', 'N', 'I', 'N', 'S', '1', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr wchar_t kUninstallRegSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FlareEngine";

constexpr UINT kMsgProgress = WM_APP + 10;
constexpr UINT kMsgFinished = WM_APP + 11;

constexpr int kIdPathEdit = 1001;
constexpr int kIdBrowseBtn = 1002;
constexpr int kIdChkDesktop = 1003;
constexpr int kIdChkStartMenu = 1004;
constexpr int kIdChkLaunch = 1005;
constexpr int kIdInstallBtn = 1006;
constexpr int kIdCancelBtn = 1007;
constexpr int kIdProgress = 1008;

#pragma pack(push, 1)
struct InstallerFooter
{
    char magic[12];
    std::uint32_t version;
    std::uint64_t stubSize;
    std::uint64_t payloadOffset;
    std::uint64_t payloadSize;
    std::uint32_t fileCount;
    std::uint8_t payloadSha256[32];
};
#pragma pack(pop)
static_assert(sizeof(InstallerFooter) == 76, "InstallerFooter size must be 76 bytes");

using StringMap = std::map<std::string, std::string>;

std::wstring ToWide(std::string_view utf8)
{
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string ToUtf8(std::wstring_view wide)
{
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

void WriteStdoutLine(std::string_view line)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::string msg(line);
    msg.push_back('\n');
    DWORD written = 0;
    WriteFile(out, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
}

void WriteStderrLine(std::string_view line)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE) err = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE) return;
    std::string msg(line);
    msg.push_back('\n');
    DWORD written = 0;
    WriteFile(err, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
}

// 仅在 uninstall.dat 被意外删除时作为启动/底层故障兜底文案；正常流程一律使用打包自语言表的 Installer.* 键值。
std::wstring LookupText(const StringMap& table, std::string_view key, std::wstring_view fallback)
{
    const auto it = table.find(std::string(key));
    if (it != table.end() && !it->second.empty()) return ToWide(it->second);
    return std::wstring(fallback);
}

std::wstring FormatArgs(std::wstring templ, std::initializer_list<std::wstring_view> args)
{
    std::size_t idx = 0;
    for (std::wstring_view arg : args)
    {
        const std::wstring token = L"{" + std::to_wstring(idx++) + L"}";
        std::size_t pos = 0;
        while ((pos = templ.find(token, pos)) != std::wstring::npos)
        {
            templ.replace(pos, token.size(), arg);
            pos += arg.size();
        }
    }
    return templ;
}

class Sha256Hasher
{
public:
    Sha256Hasher()
    {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
        {
            DWORD objLen = 0, cbData = 0;
            if (BCryptGetProperty(alg_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbData, 0) >= 0)
            {
                obj_.resize(objLen);
                if (BCryptCreateHash(alg_, &hash_, obj_.data(), objLen, nullptr, 0, 0) < 0) hash_ = nullptr;
            }
        }
    }

    ~Sha256Hasher()
    {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    bool Valid() const noexcept { return hash_ != nullptr; }

    bool Update(const void* data, std::size_t size)
    {
        if (!hash_) return false;
        const auto* ptr = static_cast<const UCHAR*>(data);
        while (size > 0)
        {
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 24));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(ptr), chunk, 0) < 0) return false;
            ptr += chunk;
            size -= chunk;
        }
        return true;
    }

    bool Finalize(std::array<std::uint8_t, 32>& digest)
    {
        if (!hash_) return false;
        return BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> obj_;
};

bool HashBufferSha256(const void* data, std::size_t size, std::array<std::uint8_t, 32>& out)
{
    Sha256Hasher hasher;
    return hasher.Valid() && hasher.Update(data, size) && hasher.Finalize(out);
}

bool HashFileRangeSha256(std::ifstream& in, std::uint64_t offset, std::uint64_t length, std::array<std::uint8_t, 32>& out)
{
    Sha256Hasher hasher;
    if (!hasher.Valid()) return false;
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) return false;

    std::vector<char> buffer(256 * 1024);
    std::uint64_t remaining = length;
    while (remaining > 0)
    {
        const std::size_t toRead = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        in.read(buffer.data(), static_cast<std::streamsize>(toRead));
        if (static_cast<std::size_t>(in.gcount()) != toRead) return false;
        if (!hasher.Update(buffer.data(), toRead)) return false;
        remaining -= toRead;
    }
    return hasher.Finalize(out);
}

template <typename T>
bool WritePod(std::ostream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(out);
}

template <typename T>
bool ReadPod(std::istream& in, T& value)
{
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<std::size_t>(in.gcount()) == sizeof(T);
}

bool WriteString(std::ostream& out, std::string_view s)
{
    const auto len = static_cast<std::uint32_t>(s.size());
    if (!WritePod(out, len)) return false;
    if (len > 0) out.write(s.data(), static_cast<std::streamsize>(len));
    return static_cast<bool>(out);
}

bool ReadString(std::istream& in, std::string& s, std::uint32_t maxLen = 1u << 20)
{
    std::uint32_t len = 0;
    if (!ReadPod(in, len) || len > maxLen) return false;
    s.assign(len, '\0');
    if (len > 0)
    {
        in.read(s.data(), static_cast<std::streamsize>(len));
        if (static_cast<std::uint32_t>(in.gcount()) != len) return false;
    }
    return true;
}

bool WriteStringMap(std::ostream& out, const StringMap& map)
{
    const auto count = static_cast<std::uint32_t>(map.size());
    if (!WritePod(out, count)) return false;
    for (const auto& [k, v] : map)
    {
        if (!WriteString(out, k) || !WriteString(out, v)) return false;
    }
    return true;
}

bool ReadStringMap(std::istream& in, StringMap& map)
{
    map.clear();
    std::uint32_t count = 0;
    if (!ReadPod(in, count) || count > 4096) return false;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::string k, v;
        if (!ReadString(in, k, 4096) || !ReadString(in, v, 65536)) return false;
        map.emplace(std::move(k), std::move(v));
    }
    return true;
}

bool LoadInstallerStringsFromTable(const fs::path& filePath, StringMap& out)
{
    std::ifstream in(filePath, std::ios::binary);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (pos < content.size())
    {
        std::size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        std::string_view line(content.data() + pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.rfind("Installer.", 0) == 0)
        {
            const std::size_t eq = line.find('=');
            if (eq != std::string_view::npos && eq > 0)
            {
                out[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
            }
        }
        pos = end + 1;
    }
    return !out.empty();
}

fs::path SelfExecutablePath()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD copied = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (copied == 0) return {};
        if (copied < buf.size() - 1)
        {
            buf.resize(copied);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

bool TryReadFooter(const fs::path& exePath, InstallerFooter& footer)
{
    std::ifstream in(exePath, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto endPos = in.tellg();
    if (endPos < static_cast<std::streamoff>(sizeof(InstallerFooter))) return false;
    const auto fileSize = static_cast<std::uint64_t>(endPos);
    in.seekg(static_cast<std::streamoff>(fileSize - sizeof(InstallerFooter)), std::ios::beg);
    if (!ReadPod(in, footer)) return false;
    if (std::memcmp(footer.magic, kFooterMagic, sizeof(kFooterMagic)) != 0) return false;
    if (footer.version != kFormatVersion) return false;
    if (footer.payloadOffset < footer.stubSize) return false;
    if (footer.payloadOffset + footer.payloadSize + sizeof(InstallerFooter) != fileSize) return false;
    return true;
}

bool IsSafeRelativePosixPath(std::string_view rel)
{
    if (rel.empty() || rel.front() == '/' || rel.front() == '\\') return false;
    if (rel.find(':') != std::string_view::npos || rel.find('\\') != std::string_view::npos) return false;
    std::size_t pos = 0;
    while (pos <= rel.size())
    {
        std::size_t slash = rel.find('/', pos);
        if (slash == std::string_view::npos) slash = rel.size();
        const std::string_view seg = rel.substr(pos, slash - pos);
        if (seg.empty() || seg == "." || seg == "..") return false;
        pos = slash + 1;
    }
    return true;
}

bool IsEditorLockedInDirectory(const fs::path& targetDir)
{
    const fs::path editorExe = targetDir / L"FlareEditor.exe";
    std::error_code ec;
    if (!fs::exists(editorExe, ec)) return false;
    HANDLE h = CreateFileW(editorExe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION;
    }
    CloseHandle(h);
    return false;
}

fs::path KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw)
    {
        result = fs::path(raw);
    }
    if (raw) CoTaskMemFree(raw);
    return result;
}

bool CreateShortcutLink(const fs::path& shortcutPath, const fs::path& targetExe, const fs::path& workingDir, std::wstring_view arguments, std::wstring_view description)
{
    std::error_code ec;
    fs::create_directories(shortcutPath.parent_path(), ec);

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link)
        return false;

    link->SetPath(targetExe.c_str());
    link->SetWorkingDirectory(workingDir.c_str());
    if (!arguments.empty())
    {
        const std::wstring argStr(arguments);
        link->SetArguments(argStr.c_str());
    }
    if (!description.empty())
    {
        const std::wstring descStr(description);
        link->SetDescription(descStr.c_str());
    }
    link->SetIconLocation(targetExe.c_str(), 0);

    IPersistFile* persist = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))) && persist)
    {
        ok = SUCCEEDED(persist->Save(shortcutPath.c_str(), TRUE));
        persist->Release();
    }
    link->Release();
    return ok;
}

void WriteUninstallRegistry(const fs::path& targetDir, std::string_view version, std::uint64_t totalRawBytes)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallRegSubkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    auto setStr = [&](const wchar_t* name, const std::wstring& val) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                       static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
    };
    auto setDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
    };

    const fs::path editorExe = targetDir / L"FlareEditor.exe";
    const fs::path uninstallExe = targetDir / L"Uninstall.exe";
    const std::wstring verWide = ToWide(version);
    const std::wstring uninstallCmd = L"\"" + uninstallExe.wstring() + L"\" --uninstall";
    const std::wstring quietCmd = uninstallCmd + L" --silent";

    setStr(L"DisplayName", L"Flare Engine");
    setStr(L"DisplayVersion", verWide.empty() ? L"0.1.0" : verWide);
    setStr(L"Publisher", L"Flare Engine");
    setStr(L"DisplayIcon", editorExe.wstring() + L",0");
    setStr(L"InstallLocation", targetDir.wstring());
    setStr(L"UninstallString", uninstallCmd);
    setStr(L"QuietUninstallString", quietCmd);
    setStr(L"URLInfoAbout", L"https://github.com/xuu-xuu-xuu/FlareEngine");
    setDword(L"EstimatedSize", static_cast<DWORD>(std::max<std::uint64_t>(1, totalRawBytes / 1024)));
    setDword(L"NoModify", 1);
    setDword(L"NoRepair", 1);

    RegCloseKey(key);
}

// -----------------------------------------------------------------------------
// Pack Mode: FlareInstaller.exe --pack <packageDir> <outputSetupExe> [--version 0.1.0]
// -----------------------------------------------------------------------------
int RunPackMode(const fs::path& packageDir, const fs::path& outputExe, std::string_view version)
{
    std::error_code ec;
    if (!fs::exists(packageDir / L"FlareEditor.exe", ec))
    {
        WriteStderrLine("Pack failed: FlareEditor.exe not found in package directory.");
        return 2;
    }

    StringMap zhMap, enMap;
    if (!LoadInstallerStringsFromTable(packageDir / L"Resources" / L"Localization" / L"zh-CN.txt", zhMap) ||
        !LoadInstallerStringsFromTable(packageDir / L"Resources" / L"Localization" / L"en-US.txt", enMap))
    {
        WriteStderrLine("Pack failed: Installer.* localization keys missing from package Resources/Localization.");
        return 3;
    }

    const fs::path selfPath = SelfExecutablePath();
    InstallerFooter existingFooter{};
    std::uint64_t stubSize = 0;
    if (TryReadFooter(selfPath, existingFooter))
        stubSize = existingFooter.stubSize;
    else
        stubSize = static_cast<std::uint64_t>(fs::file_size(selfPath, ec));

    if (stubSize == 0)
    {
        WriteStderrLine("Pack failed: unable to read installer stub executable.");
        return 4;
    }

    std::vector<std::pair<std::string, fs::path>> files;
    std::uint64_t totalRawBytes = 0;
    const std::wstring rootNorm = fs::weakly_canonical(packageDir, ec).wstring();
    for (const auto& entry : fs::recursive_directory_iterator(rootNorm, ec))
    {
        if (!entry.is_regular_file(ec)) continue;
        std::wstring relW = entry.path().wstring().substr(rootNorm.size());
        while (!relW.empty() && (relW.front() == L'\\' || relW.front() == L'/')) relW.erase(relW.begin());
        for (wchar_t& ch : relW) if (ch == L'\\') ch = L'/';
        const std::string relUtf8 = ToUtf8(relW);
        if (!IsSafeRelativePosixPath(relUtf8))
        {
            WriteStderrLine("Pack failed: unsafe relative path " + relUtf8);
            return 5;
        }
        totalRawBytes += static_cast<std::uint64_t>(entry.file_size(ec));
        files.emplace_back(relUtf8, entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (files.empty())
    {
        WriteStderrLine("Pack failed: package directory contains no files.");
        return 6;
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor))
    {
        WriteStderrLine("Pack failed: CreateCompressor(LZMS) failed.");
        return 7;
    }

    fs::create_directories(outputExe.parent_path(), ec);
    const fs::path tempOut = outputExe.wstring() + L".tmp";
    fs::remove(tempOut, ec);

    int exitCode = 0;
    {
        std::ifstream stubIn(selfPath, std::ios::binary);
        std::ofstream out(tempOut, std::ios::binary | std::ios::trunc);
        if (!stubIn || !out)
        {
            CloseCompressor(compressor);
            WriteStderrLine("Pack failed: unable to open output installer stream.");
            return 8;
        }

        std::vector<char> copyBuf(256 * 1024);
        std::uint64_t left = stubSize;
        while (left > 0)
        {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, copyBuf.size()));
            stubIn.read(copyBuf.data(), static_cast<std::streamsize>(chunk));
            if (static_cast<std::size_t>(stubIn.gcount()) != chunk) { exitCode = 9; break; }
            out.write(copyBuf.data(), static_cast<std::streamsize>(chunk));
            left -= chunk;
        }

        const auto payloadOffset = static_cast<std::uint64_t>(out.tellp());
        Sha256Hasher payloadHasher;
        if (!payloadHasher.Valid()) exitCode = 10;

        // 同步写入文件并馈入整包 SHA-256，无需在内存中保留整份归档。
        struct HashedSink
        {
            std::ofstream& out;
            Sha256Hasher& hasher;
            bool WriteBytes(const void* data, std::size_t len)
            {
                if (len == 0) return true;
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
                return static_cast<bool>(out) && hasher.Update(data, len);
            }
            bool WriteU32(std::uint32_t v) { return WriteBytes(&v, sizeof(v)); }
            bool WriteU64(std::uint64_t v) { return WriteBytes(&v, sizeof(v)); }
            bool WriteU8(std::uint8_t v) { return WriteBytes(&v, sizeof(v)); }
            bool WriteStr(std::string_view s)
            {
                return WriteU32(static_cast<std::uint32_t>(s.size())) && WriteBytes(s.data(), s.size());
            }
            bool WriteMap(const StringMap& m)
            {
                if (!WriteU32(static_cast<std::uint32_t>(m.size()))) return false;
                for (const auto& [k, v] : m)
                    if (!WriteStr(k) || !WriteStr(v)) return false;
                return true;
            }
        } sink{out, payloadHasher};

        if (exitCode == 0)
        {
            if (!sink.WriteStr(version.empty() ? "0.1.0" : version) ||
                !sink.WriteU64(totalRawBytes) ||
                !sink.WriteMap(zhMap) ||
                !sink.WriteMap(enMap) ||
                !sink.WriteU32(static_cast<std::uint32_t>(files.size())))
            {
                exitCode = 11;
            }
        }

        std::vector<std::uint8_t> rawBuf;
        std::vector<std::uint8_t> compBuf;
        for (std::size_t i = 0; exitCode == 0 && i < files.size(); ++i)
        {
            const auto& [relPath, fullPath] = files[i];
            std::ifstream fin(fullPath, std::ios::binary | std::ios::ate);
            if (!fin) { exitCode = 12; break; }
            const auto rawSize = static_cast<std::uint64_t>(fin.tellg());
            fin.seekg(0, std::ios::beg);
            rawBuf.resize(static_cast<std::size_t>(rawSize));
            if (rawSize > 0)
            {
                fin.read(reinterpret_cast<char*>(rawBuf.data()), static_cast<std::streamsize>(rawSize));
                if (static_cast<std::uint64_t>(fin.gcount()) != rawSize) { exitCode = 13; break; }
            }

            std::array<std::uint8_t, 32> fileSha{};
            if (!HashBufferSha256(rawBuf.data(), rawBuf.size(), fileSha)) { exitCode = 14; break; }

            std::uint8_t compressedFlag = 0;
            const std::uint8_t* storePtr = rawBuf.data();
            std::uint64_t storeSize = rawSize;

            if (rawSize >= 64)
            {
                ResetCompressor(compressor);
                SIZE_T needed = 0;
                Compress(compressor, rawBuf.data(), rawBuf.size(), nullptr, 0, &needed);
                if (needed > 0)
                {
                    compBuf.resize(needed);
                    SIZE_T actualComp = 0;
                    if (Compress(compressor, rawBuf.data(), rawBuf.size(), compBuf.data(), compBuf.size(), &actualComp) &&
                        actualComp > 0 && actualComp < rawSize)
                    {
                        compressedFlag = 1;
                        storePtr = compBuf.data();
                        storeSize = static_cast<std::uint64_t>(actualComp);
                    }
                }
            }

            if (!sink.WriteStr(relPath) ||
                !sink.WriteU64(rawSize) ||
                !sink.WriteU64(storeSize) ||
                !sink.WriteU8(compressedFlag) ||
                !sink.WriteBytes(fileSha.data(), fileSha.size()) ||
                !sink.WriteBytes(storePtr, static_cast<std::size_t>(storeSize)))
            {
                exitCode = 15;
                break;
            }
        }

        if (exitCode == 0)
        {
            const auto payloadEnd = static_cast<std::uint64_t>(out.tellp());
            std::array<std::uint8_t, 32> payloadSha{};
            if (!payloadHasher.Finalize(payloadSha))
            {
                exitCode = 16;
            }
            else
            {
                InstallerFooter footer{};
                std::memcpy(footer.magic, kFooterMagic, sizeof(kFooterMagic));
                footer.version = kFormatVersion;
                footer.stubSize = stubSize;
                footer.payloadOffset = payloadOffset;
                footer.payloadSize = payloadEnd - payloadOffset;
                footer.fileCount = static_cast<std::uint32_t>(files.size());
                std::memcpy(footer.payloadSha256, payloadSha.data(), payloadSha.size());
                if (!WritePod(out, footer)) exitCode = 17;
            }
        }
        out.flush();
    }

    CloseCompressor(compressor);
    if (exitCode != 0)
    {
        fs::remove(tempOut, ec);
        WriteStderrLine("Pack failed with error code " + std::to_string(exitCode));
        return exitCode;
    }

    fs::remove(outputExe, ec);
    fs::rename(tempOut, outputExe, ec);
    if (ec)
    {
        WriteStderrLine("Pack failed: unable to finalize output setup executable.");
        return 18;
    }

    const auto finalBytes = static_cast<std::uint64_t>(fs::file_size(outputExe, ec));
    WriteStdoutLine("Installer created: " + ToUtf8(outputExe.wstring()) +
                    " (files=" + std::to_string(files.size()) +
                    ", raw_bytes=" + std::to_string(totalRawBytes) +
                    ", installer_bytes=" + std::to_string(finalBytes) + ")");
    return 0;
}

// -----------------------------------------------------------------------------
// Archive Header & Extraction Engine
// -----------------------------------------------------------------------------
struct ArchiveHeaderInfo
{
    InstallerFooter footer{};
    std::string version = "0.1.0";
    std::uint64_t totalRawBytes = 0;
    StringMap zhStrings;
    StringMap enStrings;
    std::uint32_t fileCount = 0;
    std::uint64_t entriesStreamOffset = 0;
};

bool LoadArchiveHeader(const fs::path& selfPath, ArchiveHeaderInfo& out)
{
    if (!TryReadFooter(selfPath, out.footer)) return false;
    std::ifstream in(selfPath, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(out.footer.payloadOffset), std::ios::beg);
    if (!ReadString(in, out.version, 256) ||
        !ReadPod(in, out.totalRawBytes) ||
        !ReadStringMap(in, out.zhStrings) ||
        !ReadStringMap(in, out.enStrings) ||
        !ReadPod(in, out.fileCount))
    {
        return false;
    }
    if (out.fileCount != out.footer.fileCount || out.fileCount == 0 || out.fileCount > 200000) return false;
    out.entriesStreamOffset = static_cast<std::uint64_t>(in.tellg());
    return true;
}

struct InstallJobOptions
{
    fs::path selfPath;
    fs::path targetDir;
    bool createDesktopShortcut = true;
    bool createStartMenuShortcut = true;
    bool writeRegistry = true;
    StringMap activeStrings;
    ArchiveHeaderInfo header;
};

struct InstallProgressCallback
{
    std::function<void(int permille, const std::wstring& status)> onProgress;
    std::atomic<bool>* cancelRequested = nullptr;
};

bool ExecuteInstallJob(const InstallJobOptions& job, const InstallProgressCallback& cb, std::wstring& outError)
{
    const auto report = [&](int permille, const std::wstring& text) {
        if (cb.onProgress) cb.onProgress(std::clamp(permille, 0, 1000), text);
    };

    if (job.targetDir.empty() || !job.targetDir.is_absolute())
    {
        outError = LookupText(job.activeStrings, "Installer.ErrorInvalidPath", L"Invalid installation directory.");
        return false;
    }

    if (IsEditorLockedInDirectory(job.targetDir))
    {
        outError = LookupText(job.activeStrings, "Installer.UninstallRunning", L"FlareEditor.exe is currently running.");
        return false;
    }

    report(15, LookupText(job.activeStrings, "Installer.StatusVerifying", L"Verifying installer integrity (SHA-256)..."));

    std::ifstream in(job.selfPath, std::ios::binary);
    if (!in)
    {
        outError = LookupText(job.activeStrings, "Installer.ErrorCorrupt", L"Installer integrity check failed.");
        return false;
    }

    std::array<std::uint8_t, 32> actualPayloadSha{};
    if (!HashFileRangeSha256(in, job.header.footer.payloadOffset, job.header.footer.payloadSize, actualPayloadSha) ||
        std::memcmp(actualPayloadSha.data(), job.header.footer.payloadSha256, 32) != 0)
    {
        outError = LookupText(job.activeStrings, "Installer.ErrorCorrupt", L"Installer integrity check failed.");
        return false;
    }

    std::error_code ec;
    fs::create_directories(job.targetDir, ec);
    if (ec)
    {
        outError = LookupText(job.activeStrings, "Installer.ErrorWriteFailed", L"Failed to write file: ") + job.targetDir.wstring();
        return false;
    }

    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor))
    {
        outError = L"CreateDecompressor(LZMS) failed.";
        return false;
    }

    in.clear();
    in.seekg(static_cast<std::streamoff>(job.header.entriesStreamOffset), std::ios::beg);

    std::vector<std::string> installedFiles;
    installedFiles.reserve(job.header.fileCount);
    std::vector<std::uint8_t> compBuf;
    std::vector<std::uint8_t> rawBuf;

    const std::wstring extractTempl = LookupText(job.activeStrings, "Installer.StatusExtracting", L"Installing ({0}/{1}): {2}");
    const std::wstring totalCountStr = std::to_wstring(job.header.fileCount);
    std::uint64_t extractedBytes = 0;

    bool success = true;
    for (std::uint32_t i = 0; i < job.header.fileCount; ++i)
    {
        if (cb.cancelRequested && cb.cancelRequested->load())
        {
            outError = L"Cancelled";
            success = false;
            break;
        }

        std::string relPath;
        std::uint64_t rawSize = 0, storeSize = 0;
        std::uint8_t compressedFlag = 0;
        std::array<std::uint8_t, 32> expectedSha{};

        if (!ReadString(in, relPath, 4096) ||
            !IsSafeRelativePosixPath(relPath) ||
            !ReadPod(in, rawSize) ||
            !ReadPod(in, storeSize) ||
            !ReadPod(in, compressedFlag) ||
            !ReadPod(in, expectedSha) ||
            rawSize > (1ull << 31) ||
            storeSize > (1ull << 31))
        {
            outError = LookupText(job.activeStrings, "Installer.ErrorCorrupt", L"Installer integrity check failed.");
            success = false;
            break;
        }

        if ((i % 8 == 0) || i + 1 == job.header.fileCount)
        {
            const double ratio = job.header.totalRawBytes > 0
                ? static_cast<double>(extractedBytes) / static_cast<double>(job.header.totalRawBytes)
                : static_cast<double>(i) / static_cast<double>(job.header.fileCount);
            const int permille = 60 + static_cast<int>(ratio * 880.0);
            const std::wstring idxStr = std::to_wstring(i + 1);
            const std::wstring relWide = ToWide(relPath);
            report(permille, FormatArgs(extractTempl, {idxStr, totalCountStr, relWide}));
        }

        compBuf.resize(static_cast<std::size_t>(storeSize));
        if (storeSize > 0)
        {
            in.read(reinterpret_cast<char*>(compBuf.data()), static_cast<std::streamsize>(storeSize));
            if (static_cast<std::uint64_t>(in.gcount()) != storeSize)
            {
                outError = LookupText(job.activeStrings, "Installer.ErrorCorrupt", L"Installer integrity check failed.");
                success = false;
                break;
            }
        }

        const std::uint8_t* writePtr = compBuf.data();
        if (compressedFlag != 0)
        {
            ResetDecompressor(decompressor);
            rawBuf.resize(static_cast<std::size_t>(rawSize));
            SIZE_T decompActual = 0;
            if (!Decompress(decompressor, compBuf.data(), compBuf.size(), rawBuf.data(), rawBuf.size(), &decompActual) ||
                decompActual != rawSize)
            {
                outError = LookupText(job.activeStrings, "Installer.ErrorCorrupt", L"Installer integrity check failed.");
                success = false;
                break;
            }
            writePtr = rawBuf.data();
        }

        std::array<std::uint8_t, 32> actualFileSha{};
        if (!HashBufferSha256(writePtr, static_cast<std::size_t>(rawSize), actualFileSha) ||
            std::memcmp(actualFileSha.data(), expectedSha.data(), 32) != 0)
        {
            outError = LookupText(job.activeStrings, "Installer.ErrorCorrupt", L"Installer integrity check failed.");
            success = false;
            break;
        }

        std::wstring relWin = ToWide(relPath);
        for (wchar_t& ch : relWin) if (ch == L'/') ch = L'\\';
        const fs::path destFile = job.targetDir / relWin;
        fs::create_directories(destFile.parent_path(), ec);

        std::ofstream fout(destFile, std::ios::binary | std::ios::trunc);
        if (!fout)
        {
            outError = LookupText(job.activeStrings, "Installer.ErrorWriteFailed", L"Failed to write file: ") + destFile.wstring();
            success = false;
            break;
        }
        if (rawSize > 0)
        {
            fout.write(reinterpret_cast<const char*>(writePtr), static_cast<std::streamsize>(rawSize));
        }
        fout.flush();
        if (!fout)
        {
            outError = LookupText(job.activeStrings, "Installer.ErrorWriteFailed", L"Failed to write file: ") + destFile.wstring();
            success = false;
            break;
        }

        installedFiles.push_back(std::move(relPath));
        extractedBytes += rawSize;
    }

    CloseDecompressor(decompressor);
    if (!success) return false;

    report(955, LookupText(job.activeStrings, "Installer.StatusRegistering", L"Creating shortcuts and uninstaller..."));

    // 写入纯净卸载程序 Uninstall.exe（即剥离压缩载荷后的安装器本体 Stub）
    const fs::path uninstallExe = job.targetDir / L"Uninstall.exe";
    {
        in.clear();
        in.seekg(0, std::ios::beg);
        std::ofstream uout(uninstallExe, std::ios::binary | std::ios::trunc);
        if (!uout)
        {
            outError = LookupText(job.activeStrings, "Installer.ErrorWriteFailed", L"Failed to write file: ") + uninstallExe.wstring();
            return false;
        }
        std::vector<char> stubBuf(256 * 1024);
        std::uint64_t left = job.header.footer.stubSize;
        while (left > 0)
        {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, stubBuf.size()));
            in.read(stubBuf.data(), static_cast<std::streamsize>(chunk));
            if (static_cast<std::size_t>(in.gcount()) != chunk) return false;
            uout.write(stubBuf.data(), static_cast<std::streamsize>(chunk));
            left -= chunk;
        }
        uout.flush();
        if (!uout) return false;
    }

    std::string desktopShortcutUtf8;
    std::string startMenuDirUtf8;
    const fs::path editorExe = job.targetDir / L"FlareEditor.exe";

    if (job.createDesktopShortcut)
    {
        const fs::path desktop = KnownFolder(FOLDERID_Desktop);
        if (!desktop.empty())
        {
            const fs::path lnk = desktop / L"Flare Engine.lnk";
            if (CreateShortcutLink(lnk, editorExe, job.targetDir, L"", L"Flare Engine Editor"))
                desktopShortcutUtf8 = ToUtf8(lnk.wstring());
        }
    }

    if (job.createStartMenuShortcut)
    {
        const fs::path programs = KnownFolder(FOLDERID_Programs);
        if (!programs.empty())
        {
            const fs::path menuFolder = programs / L"Flare Engine";
            const fs::path mainLnk = menuFolder / L"Flare Engine.lnk";
            const fs::path uninstLnk = menuFolder / L"Uninstall Flare Engine.lnk";
            if (CreateShortcutLink(mainLnk, editorExe, job.targetDir, L"", L"Flare Engine Editor"))
            {
                CreateShortcutLink(uninstLnk, uninstallExe, job.targetDir, L"--uninstall", L"Uninstall Flare Engine");
                startMenuDirUtf8 = ToUtf8(menuFolder.wstring());
            }
        }
    }

    // 写入卸载清单 uninstall.dat，使卸载仅清理引擎自身安装的文件，绝不误删用户数据。
    {
        std::ofstream manifestOut(job.targetDir / L"uninstall.dat", std::ios::binary | std::ios::trunc);
        if (manifestOut)
        {
            manifestOut.write(kUninstallMagic, sizeof(kUninstallMagic));
            WritePod(manifestOut, kFormatVersion);
            WriteString(manifestOut, job.header.version);
            WriteStringMap(manifestOut, job.header.zhStrings);
            WriteStringMap(manifestOut, job.header.enStrings);
            WriteString(manifestOut, desktopShortcutUtf8);
            WriteString(manifestOut, startMenuDirUtf8);
            WritePod(manifestOut, static_cast<std::uint32_t>(installedFiles.size()));
            for (const std::string& f : installedFiles) WriteString(manifestOut, f);
        }
    }

    if (job.writeRegistry)
    {
        WriteUninstallRegistry(job.targetDir, job.header.version, job.header.totalRawBytes);
    }

    report(1000, LookupText(job.activeStrings, "Installer.StatusCompleted", L"Installation complete!"));
    return true;
}

// -----------------------------------------------------------------------------
// Uninstall Mode: Uninstall.exe [--uninstall] [--silent] [--no-registry] [--language zh-CN|en-US]
// -----------------------------------------------------------------------------
int RunUninstallMode(const fs::path& selfPath, bool silent, bool noRegistry, std::string_view langOverride)
{
    const fs::path targetDir = selfPath.parent_path();
    const fs::path manifestPath = targetDir / L"uninstall.dat";

    StringMap zhMap, enMap;
    std::string version = "0.1.0";
    std::string desktopShortcutUtf8;
    std::string startMenuDirUtf8;
    std::vector<std::string> installedFiles;

    {
        std::ifstream in(manifestPath, std::ios::binary);
        char magic[12]{};
        std::uint32_t ver = 0;
        if (in && ReadPod(in, magic) && std::memcmp(magic, kUninstallMagic, sizeof(kUninstallMagic)) == 0 &&
            ReadPod(in, ver) && ver == kFormatVersion)
        {
            ReadString(in, version, 256);
            ReadStringMap(in, zhMap);
            ReadStringMap(in, enMap);
            ReadString(in, desktopShortcutUtf8, 4096);
            ReadString(in, startMenuDirUtf8, 4096);
            std::uint32_t count = 0;
            if (ReadPod(in, count) && count <= 200000)
            {
                installedFiles.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    std::string rel;
                    if (!ReadString(in, rel, 4096)) break;
                    if (IsSafeRelativePosixPath(rel)) installedFiles.push_back(std::move(rel));
                }
            }
        }
    }

    const bool useChinese = langOverride == "zh-CN" ||
        (langOverride.empty() && ((GetUserDefaultUILanguage() & 0x3FF) == LANG_CHINESE));
    const StringMap& strings = useChinese ? (!zhMap.empty() ? zhMap : enMap) : (!enMap.empty() ? enMap : zhMap);
    const std::wstring title = LookupText(strings, "Installer.UninstallTitle", L"Uninstall Flare Engine");

    if (IsEditorLockedInDirectory(targetDir))
    {
        const std::wstring msg = LookupText(strings, "Installer.UninstallRunning", L"FlareEditor.exe is currently running.");
        if (!silent) MessageBoxW(nullptr, msg.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
        else WriteStderrLine(ToUtf8(msg));
        return 3;
    }

    if (!silent)
    {
        const std::wstring confirm = LookupText(strings, "Installer.UninstallConfirm",
                                                L"Uninstall Flare Engine from your computer?");
        if (MessageBoxW(nullptr, confirm.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 0;
    }

    std::error_code ec;
    std::vector<fs::path> parentDirs;
    for (auto it = installedFiles.rbegin(); it != installedFiles.rend(); ++it)
    {
        std::wstring relWin = ToWide(*it);
        for (wchar_t& ch : relWin) if (ch == L'/') ch = L'\\';
        const fs::path file = targetDir / relWin;
        fs::remove(file, ec);
        for (fs::path p = file.parent_path(); !p.empty() && p != targetDir; p = p.parent_path())
            parentDirs.push_back(p);
    }

    std::sort(parentDirs.begin(), parentDirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.wstring().size() > b.wstring().size();
    });
    parentDirs.erase(std::unique(parentDirs.begin(), parentDirs.end()), parentDirs.end());
    for (const fs::path& dir : parentDirs)
    {
        if (fs::exists(dir, ec) && fs::is_empty(dir, ec)) fs::remove(dir, ec);
    }

    if (!desktopShortcutUtf8.empty()) fs::remove(fs::path(ToWide(desktopShortcutUtf8)), ec);
    if (!startMenuDirUtf8.empty())
    {
        const fs::path menuDir(ToWide(startMenuDirUtf8));
        fs::remove(menuDir / L"Flare Engine.lnk", ec);
        fs::remove(menuDir / L"Uninstall Flare Engine.lnk", ec);
        if (fs::exists(menuDir, ec) && fs::is_empty(menuDir, ec)) fs::remove(menuDir, ec);
    }

    if (!noRegistry)
    {
        RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallRegSubkey);
    }

    fs::remove(manifestPath, ec);

    if (!silent)
    {
        const std::wstring done = LookupText(strings, "Installer.UninstallSuccess", L"Flare Engine has been uninstalled.");
        MessageBoxW(nullptr, done.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
    }

    // 进程退出后异步移除自身 Uninstall.exe 并在目录已空时回收目录
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    const fs::path cmdExe = fs::path(sysDir) / L"cmd.exe";
    const std::wstring cmdArgs = L"/d /c ping 127.0.0.1 -n 2 >nul & del /f /q \"" + selfPath.wstring() +
                                 L"\" >nul 2>&1 & rmdir \"" + targetDir.wstring() + L"\" >nul 2>&1";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = L"\"" + cmdExe.wstring() + L"\" " + cmdArgs;
    if (CreateProcessW(cmdExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, sysDir, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Obsidian-Gold GUI Setup Wizard
// -----------------------------------------------------------------------------
struct WizardState
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND pathEdit = nullptr;
    HWND browseBtn = nullptr;
    HWND chkDesktop = nullptr;
    HWND chkStartMenu = nullptr;
    HWND chkLaunch = nullptr;
    HWND installBtn = nullptr;
    HWND cancelBtn = nullptr;
    HWND progressBar = nullptr;

    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT boldFont = nullptr;
    HBRUSH editBrush = nullptr;
    HICON appIconLarge = nullptr;

    UINT dpi = 96;
    bool optDesktop = true;
    bool optStartMenu = true;
    bool optLaunch = true;
    bool writeRegistry = true;

    enum class Phase { Ready, Installing, Completed } phase = Phase::Ready;
    int hoveredControlId = 0;

    ArchiveHeaderInfo header;
    StringMap strings;
    fs::path selfPath;
    fs::path installedDir;

    std::mutex statusMutex;
    std::wstring statusText;
    std::wstring errorText;
    std::atomic<bool> cancelRequested{false};
    std::thread worker;

    int Scale(int dip) const noexcept { return MulDiv(dip, static_cast<int>(dpi), 96); }

    void RebuildFonts()
    {
        if (titleFont) DeleteObject(titleFont);
        if (bodyFont) DeleteObject(bodyFont);
        if (boldFont) DeleteObject(boldFont);
        titleFont = CreateFontW(-Scale(19), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        boldFont = CreateFontW(-Scale(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        bodyFont = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        for (HWND ctrl : {pathEdit, browseBtn, chkDesktop, chkStartMenu, chkLaunch, installBtn, cancelBtn})
        {
            if (ctrl) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(ctrl == installBtn ? boldFont : bodyFont), TRUE);
        }
    }

    void LayoutControls()
    {
        if (!window) return;
        RECT rc{};
        GetClientRect(window, &rc);
        const int w = rc.right - rc.left;
        const int pad = Scale(24);
        const int contentW = std::max(Scale(200), w - pad * 2);

        const int browseW = Scale(92);
        const int editY = Scale(124);
        const int editH = Scale(30);
        const int editW = std::max(Scale(120), contentW - browseW - Scale(10));

        SetWindowPos(pathEdit, nullptr, pad + Scale(6), editY + Scale(5), editW - Scale(12), editH - Scale(10), SWP_NOZORDER);
        SetWindowPos(browseBtn, nullptr, pad + editW + Scale(10), editY, browseW, editH, SWP_NOZORDER);

        const int chkH = Scale(24);
        SetWindowPos(chkDesktop, nullptr, pad, Scale(188), contentW, chkH, SWP_NOZORDER);
        SetWindowPos(chkStartMenu, nullptr, pad, Scale(216), contentW, chkH, SWP_NOZORDER);
        SetWindowPos(chkLaunch, nullptr, pad, Scale(244), contentW, chkH, SWP_NOZORDER);

        SetWindowPos(progressBar, nullptr, pad, Scale(288), contentW, Scale(14), SWP_NOZORDER);

        const int btnW = Scale(116);
        const int btnH = Scale(34);
        const int btnY = rc.bottom - pad - btnH;
        SetWindowPos(cancelBtn, nullptr, w - pad - btnW, btnY, btnW, btnH, SWP_NOZORDER);
        SetWindowPos(installBtn, nullptr, w - pad - btnW * 2 - Scale(12), btnY, btnW, btnH, SWP_NOZORDER);

        InvalidateRect(window, nullptr, FALSE);
    }
};

void ApplyDarkTitleBar(HWND hwnd)
{
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

bool PickFolderDialog(HWND owner, const std::wstring& title, std::wstring& ioPath)
{
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return false;

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title.c_str());

    if (!ioPath.empty())
    {
        IShellItem* initial = nullptr;
        fs::path probe(ioPath);
        std::error_code ec;
        while (!probe.empty() && !fs::exists(probe, ec))
        {
            const fs::path parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
        if (!probe.empty() && SUCCEEDED(SHCreateItemFromParsingName(probe.c_str(), nullptr, IID_PPV_ARGS(&initial))) && initial)
        {
            dlg->SetFolder(initial);
            initial->Release();
        }
    }

    bool picked = false;
    if (SUCCEEDED(dlg->Show(owner)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item)
        {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw)
            {
                fs::path selected(raw);
                const std::wstring leaf = selected.filename().wstring();
                if (_wcsicmp(leaf.c_str(), L"FlareEngine") != 0 && !fs::exists(selected / L"FlareEditor.exe"))
                    selected /= L"FlareEngine";
                ioPath = selected.wstring();
                picked = true;
                CoTaskMemFree(raw);
            }
            item->Release();
        }
    }
    dlg->Release();
    return picked;
}

void DrawOwnerButton(const DRAWITEMSTRUCT* dis, WizardState& st)
{
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const int id = static_cast<int>(dis->CtlID);

    wchar_t text[256]{};
    GetWindowTextW(dis->hwndItem, text, 256);

    if (id == kIdChkDesktop || id == kIdChkStartMenu || id == kIdChkLaunch)
    {
        const HBRUSH bg = CreateSolidBrush(RGB(18, 20, 26));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        const bool checked = (id == kIdChkDesktop && st.optDesktop) ||
                             (id == kIdChkStartMenu && st.optStartMenu) ||
                             (id == kIdChkLaunch && st.optLaunch);

        const int boxSize = st.Scale(16);
        const int boxY = rc.top + ((rc.bottom - rc.top) - boxSize) / 2;
        RECT box{rc.left, boxY, rc.left + boxSize, boxY + boxSize};

        const COLORREF fillCol = checked ? (disabled ? RGB(115, 92, 46) : RGB(226, 168, 62)) : RGB(28, 31, 40);
        const COLORREF borderCol = checked ? fillCol : RGB(78, 84, 102);
        const HBRUSH boxBrush = CreateSolidBrush(fillCol);
        FillRect(dc, &box, boxBrush);
        DeleteObject(boxBrush);
        const HBRUSH borderBrush = CreateSolidBrush(borderCol);
        FrameRect(dc, &box, borderBrush);
        DeleteObject(borderBrush);

        if (checked)
        {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, st.Scale(2)), RGB(18, 20, 26));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, box.left + st.Scale(3), box.top + st.Scale(8), nullptr);
            LineTo(dc, box.left + st.Scale(7), box.bottom - st.Scale(4));
            LineTo(dc, box.right - st.Scale(3), box.top + st.Scale(4));
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        RECT textRc = rc;
        textRc.left += boxSize + st.Scale(10);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? RGB(120, 126, 140) : RGB(230, 234, 242));
        HGDIOBJ oldFont = SelectObject(dc, st.bodyFont);
        DrawTextW(dc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        return;
    }

    const bool primary = (id == kIdInstallBtn);
    COLORREF fill = primary ? RGB(224, 166, 58) : RGB(34, 38, 48);
    COLORREF border = primary ? RGB(240, 190, 92) : RGB(68, 74, 90);
    COLORREF fg = primary ? RGB(18, 18, 22) : RGB(230, 234, 242);

    if (disabled)
    {
        fill = RGB(30, 33, 42);
        border = RGB(52, 56, 68);
        fg = RGB(112, 118, 132);
    }
    else if (pressed)
    {
        fill = primary ? RGB(196, 142, 44) : RGB(46, 52, 66);
    }

    HBRUSH fillBrush = CreateSolidBrush(fill);
    FillRect(dc, &rc, fillBrush);
    DeleteObject(fillBrush);

    HBRUSH frameBrush = CreateSolidBrush(border);
    FrameRect(dc, &rc, frameBrush);
    DeleteObject(frameBrush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HGDIOBJ oldFont = SelectObject(dc, primary ? st.boldFont : st.bodyFont);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void PaintWizardWindow(HWND hwnd, WizardState& st)
{
    PAINTSTRUCT ps{};
    HDC rawDc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    HDC memDc = CreateCompatibleDC(rawDc);
    HBITMAP memBmp = CreateCompatibleBitmap(rawDc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    // 主背景与顶部黑曜金标题横幅
    const HBRUSH bgBrush = CreateSolidBrush(RGB(18, 20, 26));
    FillRect(memDc, &rc, bgBrush);
    DeleteObject(bgBrush);

    RECT headerRc{0, 0, w, st.Scale(86)};
    const HBRUSH headerBrush = CreateSolidBrush(RGB(24, 27, 35));
    FillRect(memDc, &headerRc, headerBrush);
    DeleteObject(headerBrush);

    RECT accentStripe{0, st.Scale(84), w, st.Scale(86)};
    const HBRUSH goldBrush = CreateSolidBrush(RGB(224, 166, 58));
    FillRect(memDc, &accentStripe, goldBrush);
    DeleteObject(goldBrush);

    const int pad = st.Scale(24);
    if (st.appIconLarge)
    {
        DrawIconEx(memDc, pad, st.Scale(19), st.appIconLarge, st.Scale(48), st.Scale(48), 0, nullptr, DI_NORMAL);
    }

    SetBkMode(memDc, TRANSPARENT);
    const int titleLeft = pad + st.Scale(62);
    const std::wstring headerTitle = L"Flare Engine v" + ToWide(st.header.version);
    RECT titleRc{titleLeft, st.Scale(18), w - pad, st.Scale(48)};
    SetTextColor(memDc, RGB(244, 246, 250));
    HGDIOBJ oldFont = SelectObject(memDc, st.titleFont);
    DrawTextW(memDc, headerTitle.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const std::wstring subtitle = LookupText(st.strings, "Installer.Subtitle", L"Modern C++20 / DirectX 12 / .NET 10 3D Game Engine");
    RECT subRc{titleLeft, st.Scale(48), w - pad, st.Scale(72)};
    SetTextColor(memDc, RGB(164, 172, 188));
    SelectObject(memDc, st.bodyFont);
    DrawTextW(memDc, subtitle.c_str(), -1, &subRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 安装位置标签与输入外框
    const std::wstring pathLabel = LookupText(st.strings, "Installer.InstallPathLabel", L"Install Location");
    RECT pathLabelRc{pad, st.Scale(98), w / 2, st.Scale(120)};
    SetTextColor(memDc, RGB(214, 220, 232));
    SelectObject(memDc, st.boldFont);
    DrawTextW(memDc, pathLabel.c_str(), -1, &pathLabelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const std::uint64_t reqMb = std::max<std::uint64_t>(1, (st.header.totalRawBytes + (1024 * 1024 - 1)) / (1024 * 1024));
    const std::wstring reqStr = FormatArgs(LookupText(st.strings, "Installer.RequiredSpace", L"Required space: ~{0} MB"),
                                           {std::to_wstring(reqMb)});
    RECT spaceRc{w / 2, st.Scale(98), w - pad, st.Scale(120)};
    SetTextColor(memDc, RGB(150, 158, 176));
    SelectObject(memDc, st.bodyFont);
    DrawTextW(memDc, reqStr.c_str(), -1, &spaceRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const int browseW = st.Scale(92);
    const int contentW = std::max(st.Scale(200), w - pad * 2);
    const int editW = std::max(st.Scale(120), contentW - browseW - st.Scale(10));
    RECT editBorder{pad, st.Scale(124), pad + editW, st.Scale(154)};
    const HBRUSH editBg = CreateSolidBrush(RGB(28, 31, 40));
    FillRect(memDc, &editBorder, editBg);
    DeleteObject(editBg);
    const HBRUSH editFrame = CreateSolidBrush(RGB(68, 74, 90));
    FrameRect(memDc, &editBorder, editFrame);
    DeleteObject(editFrame);

    // 进度说明状态行
    std::wstring statusCopy;
    {
        std::lock_guard<std::mutex> lock(st.statusMutex);
        statusCopy = st.statusText;
    }
    RECT statusRc{pad, st.Scale(310), w - pad, st.Scale(336)};
    SetTextColor(memDc, st.phase == WizardState::Phase::Completed ? RGB(114, 218, 146) : RGB(180, 186, 200));
    SelectObject(memDc, st.bodyFont);
    DrawTextW(memDc, statusCopy.c_str(), -1, &statusRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    SelectObject(memDc, oldFont);
    BitBlt(rawDc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

void StartGuiInstallWorker(WizardState& st)
{
    int textLen = GetWindowTextLengthW(st.pathEdit);
    std::wstring pathStr(static_cast<std::size_t>(std::max(0, textLen)), L'\0');
    if (textLen > 0) GetWindowTextW(st.pathEdit, pathStr.data(), textLen + 1);

    while (!pathStr.empty() && (pathStr.front() == L' ' || pathStr.front() == L'"')) pathStr.erase(pathStr.begin());
    while (!pathStr.empty() && (pathStr.back() == L' ' || pathStr.back() == L'"')) pathStr.pop_back();

    const fs::path targetDir(pathStr);
    if (targetDir.empty() || !targetDir.is_absolute())
    {
        const std::wstring err = LookupText(st.strings, "Installer.ErrorInvalidPath", L"Please select a valid local directory.");
        MessageBoxW(st.window, err.c_str(), LookupText(st.strings, "Installer.WindowTitle", L"Flare Engine Setup").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }

    st.phase = WizardState::Phase::Installing;
    st.installedDir = targetDir;
    st.cancelRequested.store(false);

    EnableWindow(st.pathEdit, FALSE);
    EnableWindow(st.browseBtn, FALSE);
    EnableWindow(st.chkDesktop, FALSE);
    EnableWindow(st.chkStartMenu, FALSE);
    EnableWindow(st.installBtn, FALSE);
    SetWindowTextW(st.installBtn, LookupText(st.strings, "Installer.InstallingButton", L"Installing...").c_str());
    InvalidateRect(st.window, nullptr, FALSE);

    if (st.worker.joinable()) st.worker.join();

    InstallJobOptions job;
    job.selfPath = st.selfPath;
    job.targetDir = targetDir;
    job.createDesktopShortcut = st.optDesktop;
    job.createStartMenuShortcut = st.optStartMenu;
    job.writeRegistry = st.writeRegistry;
    job.activeStrings = st.strings;
    job.header = st.header;

    HWND notifyHwnd = st.window;
    st.worker = std::thread([&st, job = std::move(job), notifyHwnd]() {
        InstallProgressCallback cb;
        cb.cancelRequested = &st.cancelRequested;
        cb.onProgress = [&st, notifyHwnd](int permille, const std::wstring& status) {
            {
                std::lock_guard<std::mutex> lock(st.statusMutex);
                st.statusText = status;
            }
            PostMessageW(notifyHwnd, kMsgProgress, static_cast<WPARAM>(permille), 0);
        };

        std::wstring err;
        const bool ok = ExecuteInstallJob(job, cb, err);
        {
            std::lock_guard<std::mutex> lock(st.statusMutex);
            st.errorText = err;
            if (!ok && err != L"Cancelled") st.statusText = err;
        }
        PostMessageW(notifyHwnd, kMsgFinished, ok ? 1 : 0, 0);
    });
}

LRESULT CALLBACK WizardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* st = reinterpret_cast<WizardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        st = static_cast<WizardState*>(cs->lpCreateParams);
        st->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintWizardWindow(hwnd, *st);
        return 0;

    case WM_CTLCOLOREDIT:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(236, 240, 248));
        SetBkColor(dc, RGB(28, 31, 40));
        return reinterpret_cast<LRESULT>(st->editBrush);
    }

    case WM_DRAWITEM:
        DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), *st);
        return TRUE;

    case WM_DPICHANGED:
    {
        st->dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        st->RebuildFonts();
        st->LayoutControls();
        return 0;
    }

    case WM_SIZE:
        st->LayoutControls();
        return 0;

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (code == BN_CLICKED)
        {
            if (id == kIdChkDesktop && st->phase == WizardState::Phase::Ready)
            {
                st->optDesktop = !st->optDesktop;
                InvalidateRect(st->chkDesktop, nullptr, FALSE);
            }
            else if (id == kIdChkStartMenu && st->phase == WizardState::Phase::Ready)
            {
                st->optStartMenu = !st->optStartMenu;
                InvalidateRect(st->chkStartMenu, nullptr, FALSE);
            }
            else if (id == kIdChkLaunch)
            {
                st->optLaunch = !st->optLaunch;
                InvalidateRect(st->chkLaunch, nullptr, FALSE);
            }
            else if (id == kIdBrowseBtn && st->phase == WizardState::Phase::Ready)
            {
                int len = GetWindowTextLengthW(st->pathEdit);
                std::wstring current(static_cast<std::size_t>(std::max(0, len)), L'\0');
                if (len > 0) GetWindowTextW(st->pathEdit, current.data(), len + 1);
                const std::wstring dlgTitle = LookupText(st->strings, "Installer.SelectFolderTitle", L"Select Install Folder");
                if (PickFolderDialog(hwnd, dlgTitle, current))
                {
                    SetWindowTextW(st->pathEdit, current.c_str());
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            else if (id == kIdInstallBtn)
            {
                if (st->phase == WizardState::Phase::Ready)
                {
                    StartGuiInstallWorker(*st);
                }
                else if (st->phase == WizardState::Phase::Completed)
                {
                    if (st->optLaunch)
                    {
                        const fs::path editorExe = st->installedDir / L"FlareEditor.exe";
                        ShellExecuteW(nullptr, L"open", editorExe.c_str(), nullptr, st->installedDir.c_str(), SW_SHOWNORMAL);
                    }
                    DestroyWindow(hwnd);
                }
            }
            else if (id == kIdCancelBtn)
            {
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }

    case kMsgProgress:
        SendMessageW(st->progressBar, PBM_SETPOS, wParam, 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case kMsgFinished:
    {
        if (st->worker.joinable()) st->worker.join();
        if (wParam != 0)
        {
            st->phase = WizardState::Phase::Completed;
            SendMessageW(st->progressBar, PBM_SETPOS, 1000, 0);
            SetWindowTextW(st->installBtn, LookupText(st->strings, "Installer.FinishButton", L"Finish").c_str());
            EnableWindow(st->installBtn, TRUE);
            ShowWindow(st->cancelBtn, SW_HIDE);
            SetFocus(st->installBtn);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else
        {
            std::wstring err;
            {
                std::lock_guard<std::mutex> lock(st->statusMutex);
                err = st->errorText;
            }
            if (err == L"Cancelled")
            {
                DestroyWindow(hwnd);
                return 0;
            }
            st->phase = WizardState::Phase::Ready;
            EnableWindow(st->pathEdit, TRUE);
            EnableWindow(st->browseBtn, TRUE);
            EnableWindow(st->chkDesktop, TRUE);
            EnableWindow(st->chkStartMenu, TRUE);
            EnableWindow(st->installBtn, TRUE);
            SetWindowTextW(st->installBtn, LookupText(st->strings, "Installer.InstallButton", L"Install").c_str());
            InvalidateRect(hwnd, nullptr, FALSE);
            MessageBoxW(hwnd, err.c_str(), LookupText(st->strings, "Installer.WindowTitle", L"Flare Engine Setup").c_str(), MB_OK | MB_ICONERROR);
        }
        return 0;
    }

    case WM_CLOSE:
        if (st->phase == WizardState::Phase::Installing)
        {
            st->cancelRequested.store(true);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        st->cancelRequested.store(true);
        if (st->worker.joinable()) st->worker.join();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunWizardGui(HINSTANCE instance, const fs::path& selfPath, const ArchiveHeaderInfo& header, const fs::path& initialDirOverride, bool writeRegistry, std::string_view langOverride)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WizardState st;
    st.instance = instance;
    st.selfPath = selfPath;
    st.header = header;
    st.writeRegistry = writeRegistry;

    const bool useChinese = langOverride == "zh-CN" ||
        (langOverride.empty() && ((GetUserDefaultUILanguage() & 0x3FF) == LANG_CHINESE));
    st.strings = useChinese ? (!header.zhStrings.empty() ? header.zhStrings : header.enStrings)
                            : (!header.enStrings.empty() ? header.enStrings : header.zhStrings);
    st.statusText = LookupText(st.strings, "Installer.StatusReady", L"Ready to install Flare Engine.");
    st.editBrush = CreateSolidBrush(RGB(28, 31, 40));

    HDC screenDc = GetDC(nullptr);
    st.dpi = screenDc ? static_cast<UINT>(GetDeviceCaps(screenDc, LOGPIXELSX)) : 96u;
    if (screenDc) ReleaseDC(nullptr, screenDc);

    st.appIconLarge = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, st.Scale(48), st.Scale(48), LR_DEFAULTCOLOR));
    HICON iconSmall = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    const wchar_t* className = L"FlareEngineInstallerWizard";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WizardWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon = st.appIconLarge;
    wc.hIconSm = iconSmall;
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 5;

    const DWORD winStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT winRect{0, 0, st.Scale(580), st.Scale(400)};
    AdjustWindowRectExForDpi(&winRect, winStyle, FALSE, 0, st.dpi);
    const int winW = winRect.right - winRect.left;
    const int winH = winRect.bottom - winRect.top;

    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
    {
        posX = work.left + std::max(0L, ((work.right - work.left) - winW) / 2);
        posY = work.top + std::max(0L, ((work.bottom - work.top) - winH) / 2);
    }

    const std::wstring winTitle = LookupText(st.strings, "Installer.WindowTitle", L"Flare Engine Setup");
    HWND hwnd = CreateWindowExW(0, className, winTitle.c_str(), winStyle,
                                posX, posY, winW, winH, nullptr, nullptr, instance, &st);
    if (!hwnd) return 5;

    ApplyDarkTitleBar(hwnd);

    fs::path defaultDir = initialDirOverride;
    if (defaultDir.empty())
    {
        const fs::path localApp = KnownFolder(FOLDERID_LocalAppData);
        defaultDir = (!localApp.empty() ? localApp : fs::path(L"C:\\")) / L"Programs" / L"FlareEngine";
    }

    st.pathEdit = CreateWindowExW(0, L"EDIT", defaultDir.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPathEdit)), instance, nullptr);
    st.browseBtn = CreateWindowExW(0, L"BUTTON", LookupText(st.strings, "Installer.Browse", L"Browse...").c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBrowseBtn)), instance, nullptr);
    st.chkDesktop = CreateWindowExW(0, L"BUTTON", LookupText(st.strings, "Installer.DesktopShortcut", L"Create a desktop shortcut").c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkDesktop)), instance, nullptr);
    st.chkStartMenu = CreateWindowExW(0, L"BUTTON", LookupText(st.strings, "Installer.StartMenuShortcut", L"Create a Start Menu shortcut").c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkStartMenu)), instance, nullptr);
    st.chkLaunch = CreateWindowExW(0, L"BUTTON", LookupText(st.strings, "Installer.LaunchAfter", L"Launch Flare Engine when finished").c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkLaunch)), instance, nullptr);

    st.progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                     0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdProgress)), instance, nullptr);
    SetWindowTheme(st.progressBar, L"", L"");
    SendMessageW(st.progressBar, PBM_SETBKCOLOR, 0, RGB(28, 31, 40));
    SendMessageW(st.progressBar, PBM_SETBARCOLOR, 0, RGB(224, 166, 58));
    SendMessageW(st.progressBar, PBM_SETRANGE32, 0, 1000);

    st.installBtn = CreateWindowExW(0, L"BUTTON", LookupText(st.strings, "Installer.InstallButton", L"Install").c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdInstallBtn)), instance, nullptr);
    st.cancelBtn = CreateWindowExW(0, L"BUTTON", LookupText(st.strings, "Installer.CancelButton", L"Cancel").c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancelBtn)), instance, nullptr);

    st.RebuildFonts();
    st.LayoutControls();

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(st.installBtn);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (st.titleFont) DeleteObject(st.titleFont);
    if (st.bodyFont) DeleteObject(st.bodyFont);
    if (st.boldFont) DeleteObject(st.boldFont);
    if (st.editBrush) DeleteObject(st.editBrush);
    if (st.appIconLarge) DestroyIcon(st.appIconLarge);
    if (iconSmall) DestroyIcon(iconSmall);
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv)
    {
        args.assign(argv, argv + argc);
        LocalFree(argv);
    }

    bool modePack = false;
    bool modeUninstall = false;
    bool silent = false;
    bool noShortcuts = false;
    bool noRegistry = false;
    fs::path packInput;
    fs::path packOutput;
    fs::path targetDir;
    std::string version = "0.1.0";
    std::string langOverride;

    for (std::size_t i = 1; i < args.size(); ++i)
    {
        const std::wstring& a = args[i];
        if (a == L"--pack" && i + 2 < args.size())
        {
            modePack = true;
            packInput = fs::path(args[++i]);
            packOutput = fs::path(args[++i]);
        }
        else if (a == L"--version" && i + 1 < args.size())
        {
            version = ToUtf8(args[++i]);
        }
        else if (a == L"--dir" && i + 1 < args.size())
        {
            targetDir = fs::path(args[++i]);
        }
        else if (a == L"--language" && i + 1 < args.size())
        {
            langOverride = ToUtf8(args[++i]);
        }
        else if (a == L"--uninstall")
        {
            modeUninstall = true;
        }
        else if (a == L"--silent" || a == L"/S" || a == L"/quiet")
        {
            silent = true;
        }
        else if (a == L"--no-shortcuts")
        {
            noShortcuts = true;
        }
        else if (a == L"--no-registry")
        {
            noRegistry = true;
        }
    }

    if (modePack)
    {
        return RunPackMode(packInput, packOutput, version);
    }

    struct ComScope
    {
        HRESULT hr;
        ~ComScope() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
    } com{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};

    const fs::path selfPath = SelfExecutablePath();
    ArchiveHeaderInfo header;
    const bool hasPayload = LoadArchiveHeader(selfPath, header);

    if (modeUninstall || (!hasPayload && fs::exists(selfPath.parent_path() / L"uninstall.dat")))
    {
        return RunUninstallMode(selfPath, silent, noRegistry, langOverride);
    }

    if (!hasPayload)
    {
        WriteStderrLine("FlareInstaller: no embedded package payload found. Use --pack <packageDir> <outputSetupExe>.");
        return 1;
    }

    if (silent)
    {
        if (targetDir.empty())
        {
            const fs::path localApp = KnownFolder(FOLDERID_LocalAppData);
            targetDir = (!localApp.empty() ? localApp : fs::path(L"C:\\")) / L"Programs" / L"FlareEngine";
        }
        const bool useChinese = langOverride == "zh-CN" ||
            (langOverride.empty() && ((GetUserDefaultUILanguage() & 0x3FF) == LANG_CHINESE));
        InstallJobOptions job;
        job.selfPath = selfPath;
        job.targetDir = fs::absolute(targetDir);
        job.createDesktopShortcut = !noShortcuts;
        job.createStartMenuShortcut = !noShortcuts;
        job.writeRegistry = !noRegistry;
        job.activeStrings = useChinese ? (!header.zhStrings.empty() ? header.zhStrings : header.enStrings)
                                       : (!header.enStrings.empty() ? header.enStrings : header.zhStrings);
        job.header = std::move(header);

        std::wstring err;
        if (!ExecuteInstallJob(job, {}, err))
        {
            WriteStderrLine("Silent install failed: " + ToUtf8(err));
            return 2;
        }
        WriteStdoutLine("Installed Flare Engine to: " + ToUtf8(job.targetDir.wstring()));
        return 0;
    }

    return RunWizardGui(instance, selfPath, header, targetDir, !noRegistry, langOverride);
}
