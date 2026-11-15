#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

#include "resource/DataFileSystem.h"
#include "util/GameDataPathResolver.h"

namespace {

/// RAII helper for a temporary directory tree.
struct TempDir {
    std::filesystem::path root;

    TempDir() {
        root = std::filesystem::temp_directory_path() / ("geck_test_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(root);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void mkdirs(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
}

void touchFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path).put('\0');
}

void createFallout2DataLayout(const std::filesystem::path& base) {
    mkdirs(base / "data");
    touchFile(base / "master.dat");
    touchFile(base / "critter.dat");
}

} // namespace

// =============================================================================
// hasFallout2DataLayout
// =============================================================================

TEST_CASE("hasFallout2DataLayout returns false for non-existent path", "[paths]") {
    REQUIRE_FALSE(geck::util::hasFallout2DataLayout("/nonexistent/path"));
}

TEST_CASE("hasFallout2DataLayout returns false for empty directory", "[paths]") {
    TempDir tmp;
    REQUIRE_FALSE(geck::util::hasFallout2DataLayout(tmp.root));
}

TEST_CASE("hasFallout2DataLayout detects data/ subdirectory", "[paths]") {
    TempDir tmp;
    mkdirs(tmp.root / "data");
    REQUIRE(geck::util::hasFallout2DataLayout(tmp.root));
}

TEST_CASE("hasFallout2DataLayout detects master.dat", "[paths]") {
    TempDir tmp;
    touchFile(tmp.root / "master.dat");
    REQUIRE(geck::util::hasFallout2DataLayout(tmp.root));
}

TEST_CASE("hasFallout2DataLayout detects critter.dat", "[paths]") {
    TempDir tmp;
    touchFile(tmp.root / "critter.dat");
    REQUIRE(geck::util::hasFallout2DataLayout(tmp.root));
}

TEST_CASE("hasFallout2DataLayout detects patch000.dat", "[paths]") {
    TempDir tmp;
    touchFile(tmp.root / "patch000.dat");
    REQUIRE(geck::util::hasFallout2DataLayout(tmp.root));
}

// =============================================================================
// looseDataDirectory
// =============================================================================

TEST_CASE("looseDataDirectory is the install root's data folder", "[paths]") {
    TempDir tmp;
    createFallout2DataLayout(tmp.root);

    REQUIRE(geck::util::looseDataDirectory(tmp.root) == tmp.root / "data");
}

TEST_CASE("looseDataDirectory falls back to the root of a DAT-only install", "[paths]") {
    TempDir tmp;
    touchFile(tmp.root / "master.dat");

    REQUIRE(geck::util::looseDataDirectory(tmp.root) == tmp.root);
}

// Appending `data` is only safe because a resolved game root is never a data folder itself, and a
// data folder is only recognisable by its company: it holds a `data` folder of its own (ai.txt,
// city.txt, ...) and so satisfies hasFallout2DataLayout exactly like an install root does.
TEST_CASE("a data folder resolves to its install root even though it looks like one", "[paths]") {
    TempDir tmp;
    createFallout2DataLayout(tmp.root);
    mkdirs(tmp.root / "data/data");
    mkdirs(tmp.root / "data/proto");

    const auto root = geck::util::resolveGameDataRoot(tmp.root / "data");
    REQUIRE(root.has_value());
    REQUIRE(*root == tmp.root);
    REQUIRE(geck::util::looseDataDirectory(*root) == tmp.root / "data"); // and back, not data/data
}

// The counterpart to the append: a folder that ships loose data WITHOUT the engine's archives is the
// data itself (a standalone data tree, or a mod overlay adding files under data/). Descending on its
// `data` folder would bury art/, proto/ and the rest.
TEST_CASE("looseDataDirectory leaves a data tree that is not an install alone", "[paths]") {
    TempDir tmp;
    mkdirs(tmp.root / "art/scenery");
    mkdirs(tmp.root / "data"); // its own data/ (ai.txt, city.txt, ...), not an install's

    REQUIRE(geck::util::looseDataDirectory(tmp.root) == tmp.root);
}

TEST_CASE("DataFileSystem mounts a data tree as given", "[paths][vfs]") {
    TempDir tmp;
    touchFile(tmp.root / "data/maps.txt"); // the data tree's own data/maps.txt
    touchFile(tmp.root / "proto/scenery/scenery.lst");

    geck::resource::DataFileSystem dfs;
    dfs.addDataPath(tmp.root);

    CHECK(dfs.exists("data/maps.txt"));
    CHECK(dfs.exists("proto/scenery/scenery.lst"));
}

// Regression: choosing the install folder as a data path mounted the folder itself, so its loose
// `data` files landed at "/data/proto/..." - a path nothing looks up - and master.dat answered
// instead. A Restoration Project map then hit PIDs past the end of vanilla's proto lists and failed
// to load at all ("PID index out of range").
TEST_CASE("DataFileSystem mounts an install folder's loose data files", "[paths][vfs]") {
    TempDir tmp;
    touchFile(tmp.root / "master.dat");
    touchFile(tmp.root / "data/proto/scenery/scenery.lst");

    geck::resource::DataFileSystem dfs;
    dfs.addDataPath(tmp.root);

    REQUIRE(dfs.exists("proto/scenery/scenery.lst"));
}

// =============================================================================
// resolveGameDataRoot
// =============================================================================

TEST_CASE("resolveGameDataRoot returns nullopt for empty path", "[paths]") {
    REQUIRE_FALSE(geck::util::resolveGameDataRoot(std::filesystem::path{}).has_value());
}

TEST_CASE("resolveGameDataRoot returns .dat files as-is", "[paths]") {
    TempDir tmp;
    touchFile(tmp.root / "master.dat");
    const auto datPath = tmp.root / "master.dat";

    auto result = geck::util::resolveGameDataRoot(datPath);
    REQUIRE(result.has_value());
    REQUIRE(*result == datPath);
}

TEST_CASE("resolveGameDataRoot returns standard Fallout 2 directory unchanged", "[paths]") {
    TempDir tmp;
    createFallout2DataLayout(tmp.root);

    auto result = geck::util::resolveGameDataRoot(tmp.root);
    REQUIRE(result.has_value());
    REQUIRE(*result == tmp.root);
}

TEST_CASE("resolveGameDataRoot strips trailing 'data' directory component", "[paths]") {
    TempDir tmp;
    createFallout2DataLayout(tmp.root);

    auto result = geck::util::resolveGameDataRoot(tmp.root / "data");
    REQUIRE(result.has_value());
    REQUIRE(*result == tmp.root);
}

TEST_CASE("resolveGameDataRoot returns nullopt for unrecognized directory", "[paths]") {
    TempDir tmp;
    // Empty directory — no data layout
    REQUIRE_FALSE(geck::util::resolveGameDataRoot(tmp.root).has_value());
}

TEST_CASE("resolveGameDataRoot handles non-existent paths", "[paths]") {
    const std::filesystem::path bogus = "/nonexistent/path/to/nowhere";
    REQUIRE_FALSE(geck::util::resolveGameDataRoot(bogus).has_value());
}

TEST_CASE("resolveGameDataRoot: directory named 'Resources' with valid layout resolves to itself", "[paths]") {
    TempDir tmp;
    const auto resourcesDir = tmp.root / "Resources";
    createFallout2DataLayout(resourcesDir);

    auto result = geck::util::resolveGameDataRoot(resourcesDir);
    REQUIRE(result.has_value());
    // Should NOT be treated as a macOS bundle — it resolves to itself
    REQUIRE(*result == resourcesDir);
}

// =============================================================================
// pathsEquivalent
// =============================================================================

TEST_CASE("pathsEquivalent detects identical paths", "[paths]") {
    TempDir tmp;
    createFallout2DataLayout(tmp.root);
    REQUIRE(geck::util::pathsEquivalent(tmp.root, tmp.root));
}

TEST_CASE("pathsEquivalent detects equivalent paths via resolution", "[paths]") {
    TempDir tmp;
    createFallout2DataLayout(tmp.root);
    // tmp.root/data should resolve to tmp.root
    REQUIRE(geck::util::pathsEquivalent(tmp.root, tmp.root / "data"));
}

TEST_CASE("pathsEquivalent returns false for unrelated paths", "[paths]") {
    TempDir tmp;
    mkdirs(tmp.root / "a");
    mkdirs(tmp.root / "b");
    REQUIRE_FALSE(geck::util::pathsEquivalent(tmp.root / "a", tmp.root / "b"));
}

// =============================================================================
// expandDataPaths
// =============================================================================

TEST_CASE("expandDataPaths makes a folder's DATs explicit, ordered entries", "[paths]") {
    TempDir tmp;
    std::ofstream(tmp.root / "master.dat") << "x";
    std::ofstream(tmp.root / "critter.dat") << "x";

    const auto out = geck::util::expandDataPaths({ tmp.root });
    REQUIRE(out.size() == 3);
    CHECK(out[0] == tmp.root / "master.dat"); // DATs first so the folder's loose files (last) override them
    CHECK(out[1] == tmp.root / "critter.dat");
    CHECK(out[2] == tmp.root);
}

TEST_CASE("expandDataPaths passes a .dat entry through unchanged", "[paths]") {
    TempDir tmp;
    std::ofstream(tmp.root / "master.dat") << "x";
    const auto datEntry = tmp.root / "master.dat";

    const auto out = geck::util::expandDataPaths({ datEntry });
    REQUIRE(out.size() == 1);
    CHECK(out[0] == datEntry);
}

TEST_CASE("expandDataPaths leaves a DAT-less folder alone", "[paths]") {
    TempDir tmp;
    const auto out = geck::util::expandDataPaths({ tmp.root });
    REQUIRE(out.size() == 1);
    CHECK(out[0] == tmp.root);
}

TEST_CASE("expandDataPaths does not duplicate an already-listed DAT", "[paths]") {
    TempDir tmp;
    std::ofstream(tmp.root / "master.dat") << "x";
    std::ofstream(tmp.root / "critter.dat") << "x";

    const auto out = geck::util::expandDataPaths({ tmp.root, tmp.root / "master.dat" });
    REQUIRE(out.size() == 3);
    CHECK(out[0] == tmp.root / "master.dat"); // the folder's DATs come first; the explicit dup is dropped
    CHECK(out[1] == tmp.root / "critter.dat");
    CHECK(out[2] == tmp.root);
}

// =============================================================================
// macOS bundle tests
// =============================================================================

#ifdef __APPLE__

TEST_CASE("resolveGameDataRoot resolves macOS .app GOG bundle to wrapped game root", "[paths][macos]") {
    TempDir tmp;
    const std::filesystem::path gogPath = "Fallout 2.app/Contents/Resources/game/Fallout 2.app/Contents/Resources/drive_c/Program Files/GOG.com/Fallout 2";
    createFallout2DataLayout(tmp.root / gogPath);

    auto result = geck::util::resolveGameDataRoot(tmp.root / "Fallout 2.app");
    REQUIRE(result.has_value());
    REQUIRE(*result == tmp.root / gogPath);
}

TEST_CASE("resolveGameDataRoot rejects macOS .app bundle without GOG wrapper", "[paths][macos]") {
    TempDir tmp;
    mkdirs(tmp.root / "Fallout 2.app/data");
    mkdirs(tmp.root / "Fallout 2.app/Contents/Resources");

    auto result = geck::util::resolveGameDataRoot(tmp.root / "Fallout 2.app");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("resolveGameDataRoot resolves Contents/Resources path to GOG wrapper", "[paths][macos]") {
    TempDir tmp;
    const std::filesystem::path gogPath = "MyApp.app/Contents/Resources/game/Fallout 2.app/Contents/Resources/drive_c/Program Files/GOG.com/Fallout 2";
    createFallout2DataLayout(tmp.root / gogPath);

    auto result = geck::util::resolveGameDataRoot(tmp.root / "MyApp.app/Contents/Resources");
    REQUIRE(result.has_value());
    REQUIRE(*result == tmp.root / gogPath);
}

TEST_CASE("resolveGameDataRoot rejects Contents/Resources path without GOG wrapper", "[paths][macos]") {
    TempDir tmp;
    mkdirs(tmp.root / "MyApp.app/Contents/Resources");

    auto result = geck::util::resolveGameDataRoot(tmp.root / "MyApp.app/Contents/Resources");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("resolveGameDataRoot: 'Resources' not inside .app/Contents is not treated as bundle", "[paths][macos]") {
    TempDir tmp;
    // SomeDir/Contents/Resources — but SomeDir doesn't have .app extension
    const auto resourcesDir = tmp.root / "SomeDir/Contents/Resources";
    createFallout2DataLayout(resourcesDir);

    auto result = geck::util::resolveGameDataRoot(resourcesDir);
    REQUIRE(result.has_value());
    // Should resolve to itself (valid data layout), not be treated as a bundle
    REQUIRE(*result == resourcesDir);
}

#endif // __APPLE__

TEST_CASE("ensureFallbackDataPath inserts a missing fallback at the lowest priority", "[paths]") {
    TempDir tmp;
    const auto gameDir = tmp.root / "game";
    const auto resourcesDir = tmp.root / "resources";
    mkdirs(gameDir);
    mkdirs(resourcesDir);

    std::vector<std::filesystem::path> paths = { gameDir, gameDir / "master.dat" };
    geck::util::ensureFallbackDataPath(paths, resourcesDir);

    REQUIRE(paths.size() == 3);
    // Stored order is lowest-priority-first (the VFS resolves last-mounted-wins), so the
    // gap-filling fallback must land at the front — user data keeps overriding it.
    REQUIRE(paths.front() == resourcesDir);
    REQUIRE(paths.back() == gameDir / "master.dat");
}

TEST_CASE("ensureFallbackDataPath does not duplicate an already-listed fallback", "[paths]") {
    TempDir tmp;
    const auto resourcesDir = tmp.root / "resources";
    mkdirs(resourcesDir);

    SECTION("exact same path") {
        std::vector<std::filesystem::path> paths = { resourcesDir };
        geck::util::ensureFallbackDataPath(paths, resourcesDir);
        REQUIRE(paths.size() == 1);
    }

    SECTION("non-normal form of the same directory") {
        std::vector<std::filesystem::path> paths = { tmp.root / "." / "resources" };
        geck::util::ensureFallbackDataPath(paths, resourcesDir);
        REQUIRE(paths.size() == 1);
    }
}

TEST_CASE("ensureFallbackDataPath skips a fallback directory that does not exist", "[paths]") {
    TempDir tmp;
    std::vector<std::filesystem::path> paths = { tmp.root };
    geck::util::ensureFallbackDataPath(paths, tmp.root / "no_such_dir");
    REQUIRE(paths.size() == 1);
}

// =============================================================================
// First-run setup: the Game Location as game data, and Steam libraries on other drives
// =============================================================================

TEST_CASE("providesGameData: a master.dat entry or a folder of loose game data, not a folder holding a DAT", "[paths]") {
    TempDir tmp;
    const auto game = tmp.root / "Fallout 2";
    createFallout2DataLayout(game);
    const auto resources = tmp.root / "gecko" / "resources";
    touchFile(resources / "art" / "tiles" / "blank.frm");

    REQUIRE_FALSE(geck::util::providesGameData({}));
    REQUIRE_FALSE(geck::util::providesGameData({ resources }));
    // DataFileSystem mounts a folder's loose files only; its master.dat is an entry of its own.
    REQUIRE_FALSE(geck::util::providesGameData({ resources, game }));
    REQUIRE(geck::util::providesGameData({ resources, game / "master.dat", game }));
    REQUIRE_FALSE(geck::util::providesGameData({ tmp.root / "gone" / "master.dat" }));

    const auto extracted = tmp.root / "extracted";
    touchFile(extracted / "color.pal");
    touchFile(extracted / "art" / "tiles" / "tiles.lst");
    REQUIRE(geck::util::providesGameData({ resources, extracted }));
}

TEST_CASE("gameLocationDataPaths lists an install the way Add Path does, from any of its spellings", "[paths]") {
    TempDir tmp;
    const auto game = tmp.root / "Fallout 2";
    createFallout2DataLayout(game);
    touchFile(game / "fallout2.exe");
    const std::vector<std::filesystem::path> expected { game / "master.dat", game / "critter.dat", game };

    REQUIRE(geck::util::gameLocationDataPaths(game) == expected);
    REQUIRE(geck::util::gameLocationDataPaths(game / "data") == expected);
    REQUIRE(geck::util::gameLocationDataPaths(game / "fallout2.exe") == expected);

    REQUIRE(geck::util::gameLocationDataPaths({}).empty());
    REQUIRE(geck::util::gameLocationDataPaths(tmp.root / "nowhere").empty());
    const auto modOnly = tmp.root / "mod";
    mkdirs(modOnly / "data");
    REQUIRE(geck::util::gameLocationDataPaths(modOnly).empty()); // no master.dat: not a game
}

TEST_CASE("withBaseGameDataPaths puts the game above the bundled resources and below everything else", "[paths]") {
    const std::filesystem::path resources = "E:/gecko/resources";
    const std::filesystem::path mod = "D:/mods/foc";
    const std::vector<std::filesystem::path> game { "D:/Fallout 2/master.dat", "D:/Fallout 2/critter.dat", "D:/Fallout 2" };

    // Spelled the way the friend's settings had it: mixed separators.
    const std::vector<std::filesystem::path> start { "E:/gecko\\resources", mod };
    const auto out = geck::util::withBaseGameDataPaths(start, game, resources);
    REQUIRE(out == std::vector<std::filesystem::path> { "E:/gecko\\resources", game[0], game[1], game[2], mod });

    // Nothing listed twice when part of the game is there already.
    const auto again = geck::util::withBaseGameDataPaths({ resources, game[2] }, game, resources);
    REQUIRE(again == std::vector<std::filesystem::path> { resources, game[0], game[1], game[2] });

    // A folder whose name only starts like the resources folder is not inside it.
    const auto lookalike = geck::util::withBaseGameDataPaths({ "E:/gecko/resources-old" }, game, resources);
    REQUIRE(lookalike.front() == game[0]);
}

TEST_CASE("parseSteamLibraryFolders reads the current libraryfolders.vdf", "[paths][steam]") {
    const std::string vdf = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps"
		{
			"228980"		"1"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"apps"
		{
			"38410"		"6"
		}
	}
}
)";
    const auto folders = geck::util::parseSteamLibraryFolders(vdf);
    REQUIRE(folders == std::vector<std::filesystem::path> { "C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary" });
}

TEST_CASE("parseSteamLibraryFolders reads the pre-2021 libraryfolders.vdf", "[paths][steam]") {
    const std::string vdf = R"("LibraryFolders"
{
	"TimeNextStatsReport"		"1612345678"
	"ContentStatsID"		"-1234567890"
	"1"		"D:\\SteamLibrary"
	"2"		"E:\\Games\\Steam \"Two\""
}
)";
    const auto folders = geck::util::parseSteamLibraryFolders(vdf);
    REQUIRE(folders == std::vector<std::filesystem::path> { "D:\\SteamLibrary", "E:\\Games\\Steam \"Two\"" });
    REQUIRE(geck::util::parseSteamLibraryFolders("").empty());
    REQUIRE(geck::util::parseSteamLibraryFolders("\"libraryfolders\" { \"0\" {").empty());
}

TEST_CASE("steamLibraryFolders finds a Fallout 2 in a library on another drive", "[paths][steam]") {
    TempDir tmp;
    const auto steam = tmp.root / "Steam";
    const auto library = tmp.root / "SteamLibrary";
    const auto game = library / "steamapps" / "common" / "Fallout 2";
    createFallout2DataLayout(game);
    mkdirs(steam / "steamapps");
    {
        std::string escaped;
        for (const char ch : library.u8string()) {
            if (ch == '\\') {
                escaped += "\\\\";
            } else {
                escaped += static_cast<char>(ch);
            }
        }
        std::ofstream vdf(steam / "steamapps" / "libraryfolders.vdf", std::ios::binary);
        vdf << "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" << escaped << "\"\n\t}\n}\n";
    }

    const auto folders = geck::util::steamLibraryFolders(steam);
    REQUIRE(folders.size() == 2);
    REQUIRE(folders[0] == steam);
    REQUIRE(std::filesystem::equivalent(folders[1], library));
    REQUIRE(geck::util::hasFallout2DataLayout(folders[1] / "steamapps" / "common" / "Fallout 2"));

    // A Steam install with no library file is still its own library.
    REQUIRE(geck::util::steamLibraryFolders(library) == std::vector<std::filesystem::path> { library });
}
