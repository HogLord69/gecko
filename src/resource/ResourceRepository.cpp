#include "ResourceRepository.h"

#include <algorithm>
#include <cctype>

namespace geck::resource {

ResourceRepository::ResourceRepository(DataFileSystem& files)
    : _files(files) {
}

void ResourceRepository::clear() {
    const std::scoped_lock lock(_mutex);
    _resources.clear();
    _forgotten.clear();
}

void ResourceRepository::forget(const std::filesystem::path& path) {
    // Callers spell keys their own way ("proto/scenery/scenery.lst", "/PROTO/SCENERY/..."), and the
    // VFS answers them all: match without case or a leading slash.
    const auto comparable = [](std::string key) {
        std::replace(key.begin(), key.end(), '\\', '/');
        key.erase(0, key.find_first_not_of('/'));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return key;
    };
    const std::string wanted = comparable(normalizeKey(path));
    const std::scoped_lock lock(_mutex);
    for (auto it = _resources.begin(); it != _resources.end();) {
        if (comparable(it->first) == wanted) {
            _forgotten.push_back(std::move(it->second));
            it = _resources.erase(it);
        } else {
            ++it;
        }
    }
}

std::string ResourceRepository::normalizeKey(const std::filesystem::path& path) {
    return path.generic_string();
}

} // namespace geck::resource
