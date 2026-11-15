#include <catch2/catch_test_macros.hpp>

#include "format/lst/Lst.h"
#include "resource/GameResources.h"
#include "support/Fixtures.h"
#include "ui/panels/ObjectPalettePanel.h"

#include <QComboBox>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>

// The object palette's Game list shows another game's pieces from a resource set of its own, and
// bringing one into the mod mounts an overlay whose lists replace the ones already read.

namespace {

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/// A loose-file "game": the test palette, and an items list naming the drug proto fixture.
std::filesystem::path makeGame(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "proto" / "items");
    std::filesystem::copy_file(geck::test::dataPath("color.pal"), root / "color.pal");
    std::filesystem::copy_file(geck::test::dataPath("test_item_drug_radx.pro"), root / "proto" / "items" / "00000001.pro");
    write(root / "proto" / "items" / "items.lst", "00000001.pro\r\n");
    return root;
}

} // namespace

TEST_CASE("forget makes a list read again and keeps the old copy alive", "[qt][othergames]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path base(tmp.path().toStdU16String());
    const auto game = base / "game";
    const auto overlay = base / "overlay";
    write(game / "proto" / "scenery" / "scenery.lst", "00000001.pro\r\n");
    write(overlay / "proto" / "scenery" / "scenery.lst", "00000001.pro\r\n00000002.pro\r\n");

    geck::resource::GameResources resources;
    resources.files().addDataPath(game);
    const auto* before = resources.repository().load<geck::Lst>("proto/scenery/scenery.lst");
    REQUIRE(before->list().size() == 1);

    resources.files().addDataPath(overlay);
    CHECK(resources.repository().load<geck::Lst>("proto/scenery/scenery.lst") == before); // still cached

    resources.repository().forget("/PROTO/Scenery/scenery.lst"); // any spelling of the key
    const auto* after = resources.repository().load<geck::Lst>("proto/scenery/scenery.lst");
    CHECK(after != before);
    CHECK(after->list().size() == 2);
    CHECK(before->list().size() == 1); // the old copy is still readable
}

TEST_CASE("The object palette shows another game's pieces and goes back to its own", "[qt][othergames]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path base(tmp.path().toStdU16String());
    const auto other = makeGame(base / "other");

    geck::resource::GameResources own; // the map's own game: nothing mounted
    geck::ObjectPalettePanel palette(own);
    int listed = 0;
    palette.setOtherGameProviders(
        [&listed](QString*) {
            ++listed;
            return QList<geck::ObjectPalettePanel::OtherGame>{ { "other", "Other Game" } };
        },
        [&other](const QString& id, QString* error) {
            if (id != "other") {
                *error = "unknown";
                return std::vector<std::filesystem::path>{};
            }
            return std::vector<std::filesystem::path>{ other };
        });
    palette.loadObjects();
    CHECK(palette.getObjectInfo(0, geck::ObjectCategory::ITEMS) == nullptr);

    auto* picker = palette.findChild<QComboBox*>("gamePicker");
    REQUIRE(picker != nullptr);
    REQUIRE(picker->count() == 1); // the other games are listed when the list first opens
    picker->showPopup();
    picker->hidePopup();
    REQUIRE(picker->count() == 2);
    picker->showPopup(); // asked once, not every time
    picker->hidePopup();
    CHECK(listed == 1);

    picker->setCurrentIndex(1);
    CHECK(palette.showingOtherGame());
    const geck::ObjectInfo* info = palette.getObjectInfo(0, geck::ObjectCategory::ITEMS);
    REQUIRE(info != nullptr);
    REQUIRE(info->pro != nullptr);
    CHECK(info->sourceGame == "other");
    CHECK(info->resources != nullptr);
    CHECK(info->resources != &own);

    picker->setCurrentIndex(0);
    CHECK_FALSE(palette.showingOtherGame());
    CHECK(palette.getObjectInfo(0, geck::ObjectCategory::ITEMS) == nullptr);

    picker->setCurrentIndex(1);
    REQUIRE(palette.showingOtherGame());
    palette.releaseOtherGames();
    CHECK_FALSE(palette.showingOtherGame());
    CHECK(picker->currentIndex() == 0);
    CHECK(palette.getObjectInfo(0, geck::ObjectCategory::ITEMS) == nullptr);
}
