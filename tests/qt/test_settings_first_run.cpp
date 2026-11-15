#include <catch2/catch_test_macros.hpp>

#include "ui/Settings.h"
#include "ui/dialogs/SettingsDialog.h"
#include "util/GameDataPathResolver.h"

#include <QLabel>
#include <QMetaObject>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

// First-run setup: someone fills in the Game Location (it sits right under the data paths) and
// leaves gecko's own resources folder as the only data path. gecko then had no game to read and
// said only that the data paths had "reset to its own bundled placeholder data".

namespace {

std::filesystem::path makeInstall(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder / "data");
    std::ofstream(folder / "master.dat", std::ios::binary) << "dat";
    std::ofstream(folder / "critter.dat", std::ios::binary) << "dat";
    return folder;
}

bool lists(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& wanted) {
    return std::any_of(paths.begin(), paths.end(), [&wanted](const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::equivalent(path, wanted, ec);
    });
}

bool statusSays(const geck::SettingsDialog& dialog, const QString& words) {
    const auto labels = dialog.findChildren<QLabel*>();
    return std::any_of(labels.begin(), labels.end(), [&words](const QLabel* label) {
        return !label->isHidden() && label->text().contains(words);
    });
}

} // namespace

TEST_CASE("Preferences reads the game from the Game Location when no data path holds one", "[qt][settings]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path root(tmp.path().toStdU16String());
    const auto game = makeInstall(root / "SteamLibrary" / "steamapps" / "common" / "Fallout 2");
    const auto resources = root / "gecko" / "resources";
    std::filesystem::create_directories(resources / "art" / "tiles");

    auto settings = std::make_shared<geck::Settings>();
    settings->setDataPaths({ resources });
    settings->setGameDataDirectory(game);

    geck::SettingsDialog dialog(settings);
    CHECK(statusSays(dialog, "No data path holds a game yet"));

    REQUIRE(QMetaObject::invokeMethod(&dialog, "onApply"));

    const auto saved = settings->getDataPaths();
    CHECK(lists(saved, resources));
    CHECK(lists(saved, game / "master.dat"));
    CHECK(lists(saved, game / "critter.dat"));
    CHECK(lists(saved, game));
    CHECK(geck::util::providesGameData(saved));
    CHECK(statusSays(dialog, "Game Location"));
}

TEST_CASE("Preferences leaves the data paths alone when one already holds a game", "[qt][settings]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path root(tmp.path().toStdU16String());
    const auto listedGame = makeInstall(root / "Fallout 2");
    const auto otherGame = makeInstall(root / "Other Fallout 2");

    auto settings = std::make_shared<geck::Settings>();
    settings->setDataPaths(geck::util::expandDataPaths({ listedGame }));
    settings->setGameDataDirectory(otherGame);

    geck::SettingsDialog dialog(settings);
    CHECK_FALSE(statusSays(dialog, "No data path holds a game yet"));

    REQUIRE(QMetaObject::invokeMethod(&dialog, "onApply"));

    const auto saved = settings->getDataPaths();
    CHECK(saved.size() == 3);
    CHECK(lists(saved, listedGame / "master.dat"));
    CHECK_FALSE(lists(saved, otherGame / "master.dat"));
    CHECK_FALSE(lists(saved, otherGame));
}
