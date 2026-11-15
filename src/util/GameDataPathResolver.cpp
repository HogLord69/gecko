#include "GameDataPathResolver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace geck::util {

namespace {

    bool isDirectory(const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::is_directory(path, ec);
    }

    bool isRegularFile(const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }

    // The archives the engine opens by name from the folder it runs in (gameDbInit). Their presence
    // is what separates an INSTALL from the loose data it ships: a data tree and a mod overlay both
    // hold a `data` folder of their own (ai.txt, city.txt, ...), so that folder alone proves nothing.
    bool hasEngineArchives(const std::filesystem::path& directory) {
        return isRegularFile(directory / "master.dat")
            || isRegularFile(directory / "critter.dat")
            || isRegularFile(directory / "patch000.dat");
    }

} // namespace

bool hasDatExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".dat";
}

bool hasFallout2DataLayout(const std::filesystem::path& path) {
    if (!isDirectory(path)) {
        return false;
    }

    return isDirectory(path / "data") || hasEngineArchives(path);
}

std::filesystem::path looseDataDirectory(const std::filesystem::path& gameRoot) {
    const std::filesystem::path data = gameRoot / "data";
    if (!hasEngineArchives(gameRoot) || !isDirectory(data)) {
        return gameRoot; // a data tree or mod folder IS the loose data; a DAT-only install has none
    }
    return data;
}

std::optional<std::filesystem::path> resolveLooseDataDirectory(const std::filesystem::path& path) {
    const auto gameRoot = resolveGameDataRoot(path);
    if (!gameRoot || gameRoot->empty()) {
        return std::nullopt;
    }
    return looseDataDirectory(*gameRoot);
}

// Append a directory's bundled master.dat/critter.dat (present and not already listed) to `out`.
// Extracted so expandDataPaths stays within the statement-nesting limit.
static void appendBundledDats(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
    for (const char* dat : { "master.dat", "critter.dat" }) {
        const std::filesystem::path datPath = dir / dat;
        if (isRegularFile(datPath) && std::find(out.begin(), out.end(), datPath) == out.end()) {
            out.push_back(datPath);
        }
    }
}

std::vector<std::filesystem::path> expandDataPaths(const std::vector<std::filesystem::path>& dataPaths) {
    std::vector<std::filesystem::path> out;
    const auto already = [&out](const std::filesystem::path& p) {
        return std::find(out.begin(), out.end(), p) != out.end();
    };
    for (const std::filesystem::path& path : dataPaths) {
        // Mount a directory's bundled archives BEFORE the directory itself so the directory's own loose
        // files take precedence over master.dat/critter.dat. Mounts are resolved last-wins, and this
        // matches the engine (fallout2-ce xfileOpen searches the loose data directory before the DATs) —
        // so an edited loose file (e.g. a saved .gam) overrides the packaged copy instead of being
        // shadowed by it. A non-directory entry (a .dat listed directly) just stands alone.
        if (isDirectory(path)) {
            appendBundledDats(path, out);
        }
        if (!already(path)) {
            out.push_back(path); // the directory last, so its loose files win
        }
    }
    return out;
}

std::optional<std::filesystem::path> resolveGameDataRoot(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }

    if (hasDatExtension(path)) {
        return path;
    }

#ifdef __APPLE__
    // Determine whether the input is a macOS bundle path.
    // Strict rule: the path must end in ".app" OR be a
    // "Contents/Resources" directory inside an ".app" bundle.
    std::filesystem::path bundleRoot;
    if (path.extension() == ".app") {
        bundleRoot = path;
    } else if (path.filename() == "Resources"
        && path.parent_path().filename() == "Contents"
        && path.parent_path().parent_path().extension() == ".app") {
        bundleRoot = path.parent_path().parent_path();
    }

    if (!bundleRoot.empty()) {
        const std::filesystem::path gogWrappedGameRoot = bundleRoot / "Contents/Resources/game/Fallout 2.app/Contents/Resources/drive_c/Program Files/GOG.com/Fallout 2";
        if (hasFallout2DataLayout(gogWrappedGameRoot)) {
            return gogWrappedGameRoot;
        }
        return std::nullopt;
    }
#endif

    // Before the layout test, not after: a data tree keeps a `data` folder of its own (ai.txt,
    // city.txt, ...), so an install's own "data" would otherwise satisfy hasFallout2DataLayout and
    // come back as the root — leaving looseDataDirectory() to append onto it and reach data/data.
    // The parent must be an install, not merely a folder with a `data` child (which every parent of
    // a path named "data" trivially is).
    if (path.filename() == "data" && hasEngineArchives(path.parent_path())) {
        return path.parent_path();
    }

    if (hasFallout2DataLayout(path)) {
        return path;
    }

    return std::nullopt;
}

bool pathsEquivalent(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code ec;
    if (std::filesystem::equivalent(left, right, ec)) {
        return true;
    }

    auto resolveOrIdentity = [](const std::filesystem::path& p) -> std::filesystem::path {
        if (hasDatExtension(p)) {
            return p;
        }
        if (auto resolved = resolveGameDataRoot(p)) {
            return *resolved;
        }
        return p;
    };

    return resolveOrIdentity(left).lexically_normal() == resolveOrIdentity(right).lexically_normal();
}

void ensureFallbackDataPath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& fallbackDir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(fallbackDir, ec)) {
        return;
    }

    // Plain path equivalence on purpose: pathsEquivalent() resolves entries to a game-data root,
    // which could alias two different directories; the fallback only needs literal dedup.
    const auto alreadyListed = std::any_of(paths.begin(), paths.end(),
        [&fallbackDir](const std::filesystem::path& existing) {
            std::error_code eqEc;
            return std::filesystem::equivalent(existing, fallbackDir, eqEc)
                || existing.lexically_normal() == fallbackDir.lexically_normal();
        });

    if (!alreadyListed) {
        // The stored order is lowest-priority-first (the VFS resolves last-mounted-wins, and the
        // Data Paths table displays the list reversed) — so lowest priority means the FRONT.
        paths.insert(paths.begin(), fallbackDir);
    }
}

namespace {

    std::string lowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return text;
    }

    // A path as UTF-8 with forward slashes, lower case and no trailing slash. Case and separators
    // differ between spellings of one folder ("E:/gecko\resources", "e:\gecko\resources/"), and
    // path::string() would go through the ANSI code page and throw on a name it cannot hold.
    std::string comparisonKey(const std::filesystem::path& path) {
        const std::u8string utf8 = path.lexically_normal().generic_u8string();
        std::string key = lowered(std::string(utf8.begin(), utf8.end()));
        while (key.size() > 1 && key.back() == '/') {
            key.pop_back();
        }
        return key;
    }

    bool isMasterDat(const std::filesystem::path& path) {
        const std::u8string name = path.filename().u8string();
        return lowered(std::string(name.begin(), name.end())) == "master.dat" && isRegularFile(path);
    }

    bool hasLooseGameData(const std::filesystem::path& directory) {
        const std::filesystem::path loose = looseDataDirectory(directory);
        return isRegularFile(loose / "color.pal") && isRegularFile(loose / "art" / "tiles" / "tiles.lst");
    }

    bool isInside(const std::filesystem::path& path, const std::filesystem::path& directory) {
        if (directory.empty()) {
            return false;
        }
        const std::string p = comparisonKey(path);
        const std::string d = comparisonKey(directory);
        return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
    }

    bool listed(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
        const std::string key = comparisonKey(candidate);
        return std::any_of(paths.begin(), paths.end(), [&key](const std::filesystem::path& existing) {
            return comparisonKey(existing) == key;
        });
    }

    // One "quoted string" token of a VDF file, or a brace. Steam escapes a backslash and a quote
    // with a backslash; anything else after a backslash is kept as written.
    struct VdfToken {
        char kind; // 's' string, '{' or '}'
        std::string text;
    };

    std::vector<VdfToken> tokenizeVdf(const std::string& text) {
        std::vector<VdfToken> tokens;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char ch = text[i];
            if (ch == '{' || ch == '}') {
                tokens.push_back({ ch, {} });
            } else if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                while (i < text.size() && text[i] != '\n') {
                    ++i;
                }
            } else if (ch == '"') {
                std::string value;
                for (++i; i < text.size() && text[i] != '"'; ++i) {
                    if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\\' || text[i + 1] == '"')) {
                        ++i;
                    }
                    value.push_back(text[i]);
                }
                tokens.push_back({ 's', std::move(value) });
            }
        }
        return tokens;
    }

    bool isAllDigits(const std::string& text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
    }

    std::filesystem::path fromUtf8(const std::string& text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

} // namespace

bool providesGameData(const std::vector<std::filesystem::path>& dataPaths) {
    return std::any_of(dataPaths.begin(), dataPaths.end(), [](const std::filesystem::path& path) {
        return isMasterDat(path) || (isDirectory(path) && hasLooseGameData(path));
    });
}

std::vector<std::filesystem::path> gameLocationDataPaths(const std::filesystem::path& location) {
    if (location.empty()) {
        return {};
    }
    // The Game Location's executable field names fallout2.exe; its install is the folder it sits in.
    const std::filesystem::path folder = isRegularFile(location) ? location.parent_path() : location;
    const auto root = resolveGameDataRoot(folder);
    if (!root || hasDatExtension(*root) || !isRegularFile(*root / "master.dat")) {
        return {};
    }
    return expandDataPaths({ *root });
}

std::vector<std::filesystem::path> withBaseGameDataPaths(const std::vector<std::filesystem::path>& dataPaths,
    const std::vector<std::filesystem::path>& gamePaths, const std::filesystem::path& resourcesDir) {
    std::vector<std::filesystem::path> out;
    std::size_t next = 0;
    while (next < dataPaths.size() && isInside(dataPaths[next], resourcesDir)) {
        out.push_back(dataPaths[next++]);
    }
    for (const auto& gamePath : gamePaths) {
        if (!listed(dataPaths, gamePath) && !listed(out, gamePath)) {
            out.push_back(gamePath);
        }
    }
    out.insert(out.end(), dataPaths.begin() + static_cast<std::ptrdiff_t>(next), dataPaths.end());
    return out;
}

std::vector<std::filesystem::path> parseSteamLibraryFolders(const std::string& vdfText) {
    const std::vector<VdfToken> tokens = tokenizeVdf(vdfText);
    std::vector<std::filesystem::path> folders;
    int depth = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == '{') {
            ++depth;
            continue;
        }
        if (tokens[i].kind == '}') {
            --depth;
            continue;
        }
        if (i + 1 >= tokens.size() || tokens[i + 1].kind != 's') {
            continue; // the name of a block: its '{' comes next
        }
        const std::string& key = tokens[i].text;
        const std::string& value = tokens[i + 1].text;
        ++i;
        // Since 2021: "libraryfolders" { "0" { "path" "D:\\SteamLibrary" ... } }.
        // Before: "LibraryFolders" { "TimeNextStatsReport" "..." "1" "D:\\SteamLibrary" }.
        const bool current = depth == 2 && lowered(key) == "path";
        const bool older = depth == 1 && isAllDigits(key);
        if ((current || older) && !value.empty() && !listed(folders, fromUtf8(value))) {
            folders.push_back(fromUtf8(value));
        }
    }
    return folders;
}

std::vector<std::filesystem::path> steamLibraryFolders(const std::filesystem::path& steamInstall) {
    std::vector<std::filesystem::path> folders;
    if (steamInstall.empty()) {
        return folders;
    }
    folders.push_back(steamInstall);
    std::ifstream file(steamInstall / "steamapps" / "libraryfolders.vdf", std::ios::binary);
    if (!file) {
        return folders;
    }
    std::ostringstream text;
    text << file.rdbuf();
    for (const auto& folder : parseSteamLibraryFolders(text.str())) {
        if (!listed(folders, folder)) {
            folders.push_back(folder);
        }
    }
    return folders;
}

} // namespace geck::util
