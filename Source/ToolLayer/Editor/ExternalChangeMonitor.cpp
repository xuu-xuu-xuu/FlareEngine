#include "ExternalChangeMonitor.h"
#include "ResourceLayer/Assets/AssetIO.h"

#include <system_error>
#include <vector>

namespace flare
{
void RecordEditorFileStamp(EditorFileStamp& stamp, const std::filesystem::path& path)
{
    stamp = {};
    if (path.empty())
    {
        return;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
    {
        return;
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return;
    }
    stamp.size = size;
    stamp.modified = modified;
    stamp.valid = true;
    std::vector<std::uint8_t> bytes;
    if (asset_io::Read(path, bytes))
    {
        stamp.digest = asset_io::Hash(bytes);
    }
}

bool EditorFileStampChanged(const EditorFileStamp& stamp, const std::filesystem::path& path)
{
    if (!stamp.valid || path.empty())
    {
        return false;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
    {
        return true;
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error || size != stamp.size || modified != stamp.modified)
    {
        return true;
    }
    std::vector<std::uint8_t> bytes;
    return !asset_io::Read(path, bytes) || (!stamp.digest.empty() && asset_io::Hash(bytes) != stamp.digest);
}

void ExternalChangeMonitor::Reset(const std::filesystem::path& path)
{
    path_ = path;
    modified_ = {};
    size_ = 0;
    valid_ = false;
    if (path_.empty())
    {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error)
    {
        return;
    }
    modified_ = std::filesystem::last_write_time(path_, error);
    if (error)
    {
        return;
    }
    size_ = std::filesystem::file_size(path_, error);
    valid_ = !error;
}

bool ExternalChangeMonitor::Poll() const
{
    if (!valid_ || path_.empty())
    {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error)
    {
        return false;
    }
    const auto modified = std::filesystem::last_write_time(path_, error);
    if (error)
    {
        return false;
    }
    const auto size = std::filesystem::file_size(path_, error);
    if (error)
    {
        return false;
    }
    return modified != modified_ || size != size_;
}

void ExternalChangeMonitor::Consume()
{
    if (path_.empty())
    {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error)
    {
        valid_ = false;
        return;
    }
    modified_ = std::filesystem::last_write_time(path_, error);
    if (error)
    {
        valid_ = false;
        return;
    }
    size_ = std::filesystem::file_size(path_, error);
    valid_ = !error;
}
} // namespace flare
