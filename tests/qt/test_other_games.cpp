#include <catch2/catch_test_macros.hpp>

#include "format/lst/Lst.h"
#include "resource/GameResources.h"
#include "support/Fixtures.h"
#include "ui/panels/ObjectPalettePanel.h"
#include "pattern/PatternLibrary.h"
#include "ui/dialogs/PatternBrowserDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QLabel>
#include <QListWidget>
#include <QTreeView>
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
        [&other](const QString& id, QString* error) -> std::optional<geck::ObjectPalettePanel::OtherGameData> {
            if (id != "other") {
                *error = "unknown";
                return std::nullopt;
            }
            geck::ObjectPalettePanel::OtherGameData data;
            data.layers = { other };
            data.newPieces["items"] = { 1 }; // its one item is new to the map's game
            return data;
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
}

TEST_CASE("The object palette shows only another game's new pieces until told otherwise", "[qt][othergames]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path base(tmp.path().toStdU16String());
    const auto other = makeGame(base / "other");

    geck::resource::GameResources own;
    geck::ObjectPalettePanel palette(own);
    palette.setOtherGameProviders(
        [](QString*) { return QList<geck::ObjectPalettePanel::OtherGame>{ { "other", "Other Game" } }; },
        [&other](const QString&, QString*) -> std::optional<geck::ObjectPalettePanel::OtherGameData> {
            geck::ObjectPalettePanel::OtherGameData data;
            data.layers = { other };
            data.newPieces["items"] = {}; // its one item is a picture the map's game already has
            return data;
        });
    palette.loadObjects();
    auto* picker = palette.findChild<QComboBox*>("gamePicker");
    auto* onlyNew = palette.findChild<QCheckBox*>("onlyNewPieces");
    REQUIRE(picker != nullptr);
    REQUIRE(onlyNew != nullptr);
    picker->showPopup();
    picker->hidePopup();
    picker->setCurrentIndex(1);

    CHECK(onlyNew->isChecked());
    CHECK(palette.getObjectInfo(0, geck::ObjectCategory::ITEMS) == nullptr); // left out
    onlyNew->setChecked(false);
    const geck::ObjectInfo* info = palette.getObjectInfo(0, geck::ObjectCategory::ITEMS);
    REQUIRE(info != nullptr);
    CHECK(info->listIndex == 0);
    onlyNew->setChecked(true);
    CHECK(palette.getObjectInfo(0, geck::ObjectCategory::ITEMS) == nullptr);
}

TEST_CASE("Releasing the other games puts the palette back on its own game", "[qt][othergames]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path base(tmp.path().toStdU16String());
    const auto other = makeGame(base / "other");

    geck::resource::GameResources own;
    geck::ObjectPalettePanel palette(own);
    palette.setOtherGameProviders(
        [](QString*) { return QList<geck::ObjectPalettePanel::OtherGame>{ { "other", "Other Game" } }; },
        [&other](const QString&, QString*) -> std::optional<geck::ObjectPalettePanel::OtherGameData> {
            geck::ObjectPalettePanel::OtherGameData data;
            data.layers = { other };
            return data;
        });
    palette.loadObjects();
    auto* picker = palette.findChild<QComboBox*>("gamePicker");
    REQUIRE(picker != nullptr);
    picker->showPopup();
    picker->hidePopup();

    picker->setCurrentIndex(1);
    REQUIRE(palette.showingOtherGame());
    palette.releaseOtherGames();
    CHECK_FALSE(palette.showingOtherGame());
    CHECK(picker->currentIndex() == 0);
    CHECK(palette.getObjectInfo(0, geck::ObjectCategory::ITEMS) == nullptr);
}

TEST_CASE("The Pattern Library lists the stamps that came with gecko under the user's own", "[qt][othergames]") {
    const QString bundled = geck::pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty()) {
        SKIP("no stamps beside this test program");
    }
    geck::resource::GameResources resources;
    geck::PatternBrowserDialog dialog(resources);
    auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
    REQUIRE(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    REQUIRE(model != nullptr);
    CHECK(QFileInfo(model->filePath(tree->rootIndex())).canonicalFilePath() == bundled);
    CHECK(model->isReadOnly());
    CHECK(QDir(bundled).exists("Fallout 2 Stamps"));
}

TEST_CASE("The Pattern Library draws another game's stamps with its data and says it converts them", "[qt][othergames]") {
    const QString bundled = geck::pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty() || !QDir(bundled).exists("Sonora Stamps/Adobe")) {
        SKIP("no stamps beside this test program");
    }
    geck::resource::GameResources own;
    geck::resource::GameResources sonora;
    QStringList asked;
    geck::PatternBrowserDialog dialog(own);
    dialog.setOtherGames(
        []() {
            return std::map<QString, geck::PatternBrowserDialog::StampGame>{
                { "Sonora Stamps", { "sonora", "Fallout: Sonora", true, {} } },
                { "Olympus Stamps", { "olympus", "Olympus 2207", false, "left out here" } },
            };
        },
        [&asked, &sonora](const QString& id, const QString&) -> geck::resource::GameResources* {
            asked << id;
            return &sonora;
        });
    dialog.show();
    auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
    REQUIRE(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    auto* grid = dialog.findChild<QListWidget*>();
    REQUIRE(grid != nullptr);
    const auto note = [&dialog]() {
        for (auto* label : dialog.findChildren<QLabel*>()) {
            if (!label->isHidden() && label->text().startsWith("Stamps from")) { // shown, even once accepted
                return label->text();
            }
        }
        return QString();
    };

    tree->setCurrentIndex(model->index(QDir(bundled).filePath("Sonora Stamps/Adobe")));
    CHECK(asked == QStringList{ "sonora" });
    CHECK(note().contains("Fallout: Sonora"));
    CHECK(note().contains("Custom > From Fallout Sonora")); // the folder stamp-convert writes
    REQUIRE(grid->count() > 0);
    Q_EMIT grid->itemActivated(grid->item(0));
    CHECK(dialog.selectedGameId() == "sonora");
    CHECK(dialog.selectedPath().endsWith(".json"));
    CHECK(dialog.selectedPattern().has_value());

    tree->setCurrentIndex(model->index(QDir(bundled).filePath("Olympus Stamps")));
    CHECK(asked == QStringList{ "sonora" }); // a game that isn't set up is never loaded
    CHECK(note().contains("left out here"));

    tree->setCurrentIndex(model->index(QDir(bundled).filePath("Fallout 2 Stamps/Adobe")));
    CHECK(note().isEmpty()); // the map's own numbers: nothing to convert
    REQUIRE(grid->count() > 0);
    Q_EMIT grid->itemActivated(grid->item(0));
    CHECK(dialog.selectedGameId().isEmpty());
}
