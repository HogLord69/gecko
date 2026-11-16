#include <catch2/catch_test_macros.hpp>

#include "format/lst/Lst.h"
#include "resource/GameResources.h"
#include "support/Fixtures.h"
#include "ui/panels/ObjectPalettePanel.h"
#include "pattern/PatternLibrary.h"
#include "ui/dialogs/MapBrowserDialog.h"
#include "ui/dialogs/PatternBrowserDialog.h"

#include <QDialogButtonBox>
#include <QPushButton>

#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QSortFilterProxyModel>
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


// The Pattern Library's trees show their folder model through a sort layer (numbers as numbers):
// the index of a folder, and the folder of an index, go through it.
QModelIndex folderIndex(QTreeView* tree, const QString& path) {
    auto* order = qobject_cast<QSortFilterProxyModel*>(tree->model());
    REQUIRE(order != nullptr);
    auto* files = qobject_cast<QFileSystemModel*>(order->sourceModel());
    REQUIRE(files != nullptr);
    return order->mapFromSource(files->index(path));
}

QFileSystemModel* folderModel(QTreeView* tree) {
    auto* order = qobject_cast<QSortFilterProxyModel*>(tree->model());
    return order ? qobject_cast<QFileSystemModel*>(order->sourceModel()) : nullptr;
}

/// Open the tree down to `path` (each folder above it expanded) and select it.
void revealFolder(QTreeView* tree, const QString& root, const QString& below) {
    QString at = root;
    for (const QString& part : below.split('/')) {
        at = QDir(at).filePath(part);
        tree->expand(folderIndex(tree, QFileInfo(at).path()));
    }
    tree->setCurrentIndex(folderIndex(tree, at));
}

QStringList gridNames(const QListWidget* grid) {
    QStringList names;
    for (int i = 0; i < grid->count(); ++i) {
        names << grid->item(i)->text();
    }
    return names;
}

/// Double-click (activate) the grid's tile named `name`; false when there is none.
bool activateTile(QListWidget* grid, const QString& name) {
    for (int i = 0; i < grid->count(); ++i) {
        if (grid->item(i)->text() == name) {
            Q_EMIT grid->itemActivated(grid->item(i));
            return true;
        }
    }
    return false;
}

/// The note above the Pattern Library's trees about another game's stamps, or empty when hidden.
QString gameNote(const QDialog& dialog) {
    for (auto* label : dialog.findChildren<QLabel*>()) {
        if (!label->isHidden() && label->text().startsWith("Stamps from")) {
            return label->text();
        }
    }
    return {};
}

QString currentFolderName(QTreeView* tree) {
    auto* order = qobject_cast<QSortFilterProxyModel*>(tree->model());
    return QFileInfo(folderModel(tree)->filePath(order->mapToSource(tree->currentIndex()))).fileName();
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
    auto* model = folderModel(tree);
    REQUIRE(model != nullptr);
    auto* order = qobject_cast<QSortFilterProxyModel*>(tree->model());
    CHECK(QFileInfo(model->filePath(order->mapToSource(tree->rootIndex()))).canonicalFilePath() == bundled);
    CHECK(model->isReadOnly());
    CHECK(QDir(bundled).exists("Fallout 2"));
}

TEST_CASE("The Pattern Library draws another game's stamps with its data and says it converts them", "[qt][othergames]") {
    const QString bundled = geck::pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty() || !QDir(bundled).exists("Sonora/Buildings/Adobe")) {
        SKIP("no stamps beside this test program");
    }
    geck::resource::GameResources own;
    geck::resource::GameResources sonora;
    QStringList asked;
    geck::PatternBrowserDialog dialog(own);
    dialog.setOtherGames(
        []() {
            return std::map<QString, geck::PatternBrowserDialog::StampGame>{
                { "Sonora", { "sonora", "Fallout: Sonora", true, {} } },
                { "Olympus", { "olympus", "Olympus 2207", false, "left out here" } },
            };
        },
        [&asked, &sonora](const QString& id, const QString&) -> geck::resource::GameResources* {
            asked << id;
            return &sonora;
        });
    dialog.show();
    auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
    REQUIRE(tree != nullptr);
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

    revealFolder(tree, bundled, "Sonora/Buildings/Adobe");
    CHECK(asked == QStringList{ "sonora" });
    CHECK(note().contains("Fallout: Sonora"));
    CHECK(note().contains("Custom > From Fallout Sonora")); // the folder stamp-convert writes
    REQUIRE(grid->count() > 0);
    Q_EMIT grid->itemActivated(grid->item(0));
    CHECK(dialog.selectedGameId() == "sonora");
    CHECK(dialog.selectedPath().endsWith(".json"));
    CHECK(dialog.selectedPattern().has_value());

    revealFolder(tree, bundled, "Olympus");
    CHECK(asked == QStringList{ "sonora" }); // a game that isn't set up is never loaded
    CHECK(note().contains("left out here"));

    revealFolder(tree, bundled, "Fallout 2/Buildings/Adobe");
    CHECK(note().isEmpty()); // the map's own numbers: nothing to convert
    REQUIRE(grid->count() > 0);
    Q_EMIT grid->itemActivated(grid->item(0));
    CHECK(dialog.selectedGameId().isEmpty());
}

TEST_CASE("A Pattern Library folder shows its folders as tiles that open them", "[qt][othergames]") {
    const QString bundled = geck::pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty() || !QDir(bundled).exists("Fallout 2/Buildings/Adobe")) {
        SKIP("no stamps beside this test program");
    }
    geck::resource::GameResources resources;
    geck::PatternBrowserDialog dialog(resources);
    dialog.show();
    auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
    REQUIRE(tree != nullptr);
    auto* grid = dialog.findChild<QListWidget*>();
    REQUIRE(grid != nullptr);

    // A game's folder holds folders, not stamps: they are its tiles, not an empty grid.
    revealFolder(tree, bundled, "Fallout 2");
    CHECK(gridNames(grid).contains("Buildings"));
    CHECK(gridNames(grid).contains("Ruins"));
    REQUIRE(activateTile(grid, "Buildings"));
    CHECK(currentFolderName(tree) == "Buildings"); // the tree follows
    CHECK_FALSE(dialog.selectedPattern().has_value()); // a folder opens; nothing is picked
    REQUIRE(activateTile(grid, "Adobe"));
    CHECK(currentFolderName(tree) == "Adobe");
    REQUIRE(grid->count() > 0);
    Q_EMIT grid->itemActivated(grid->item(0)); // a leaf folder: its first tile is a stamp
    CHECK(dialog.selectedPattern().has_value());
    CHECK(dialog.selectedGameId().isEmpty());
}

TEST_CASE("Without the mod tools another game's stamps are not passed off as the map's own", "[qt][othergames]") {
    const QString bundled = geck::pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty() || !QDir(bundled).exists("Sonora/Buildings/Adobe")) {
        SKIP("no stamps beside this test program");
    }
    geck::resource::GameResources resources;
    geck::PatternBrowserDialog dialog(resources);
    SECTION("no mod tools at all") {
    }
    SECTION("mod tools that give no answer (Python isn't there to run them)") {
        dialog.setOtherGames([]() { return std::map<QString, geck::PatternBrowserDialog::StampGame>{}; },
            [](const QString&, const QString&) -> geck::resource::GameResources* { return nullptr; });
    }
    dialog.show();
    auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
    REQUIRE(tree != nullptr);
    auto* grid = dialog.findChild<QListWidget*>();
    REQUIRE(grid != nullptr);

    revealFolder(tree, bundled, "Sonora/Buildings/Adobe");
    CHECK(gameNote(dialog).contains("Sonora"));
    CHECK(gameNote(dialog).contains("map workflow"));
    REQUIRE(grid->count() > 0);
    CHECK(grid->item(0)->icon().isNull()); // listed by name: no picture made of the wrong pieces

    revealFolder(tree, bundled, "Fallout 2/Buildings/Adobe");
    CHECK(gameNote(dialog).isEmpty()); // the map's own numbers

    revealFolder(tree, bundled, "HogLord Props");
    CHECK(gameNote(dialog).isEmpty());
}

TEST_CASE("Browse Maps offers the other games, and only looks at their maps", "[qt][othergames]") {
    geck::resource::GameResources resources;
    const QList<geck::MapBrowserDialog::Game> others{ { "sonora", "Fallout: Sonora" }, { "et_tu", "Fallout Et Tu" } };

    geck::MapBrowserDialog own(resources);
    own.setGames(others, QString());
    auto* combo = own.findChild<QComboBox*>("mapBrowserGame");
    REQUIRE(combo != nullptr);
    CHECK(combo->count() == 3); // the game being edited, then the others
    CHECK(combo->currentIndex() == 0);
    CHECK_FALSE(combo->parentWidget()->isHidden());
    combo->setCurrentIndex(2);
    CHECK(own.result() == geck::MapBrowserDialog::SwitchGame); // the browser is opened again on it
    CHECK(own.requestedGame() == "et_tu");

    geck::MapBrowserDialog sonora(resources);
    sonora.setGames(others, "sonora");
    CHECK(sonora.windowTitle().contains("Fallout: Sonora"));
    auto* buttons = sonora.findChild<QDialogButtonBox*>();
    REQUIRE(buttons != nullptr);
    CHECK_FALSE(buttons->button(QDialogButtonBox::Open)->isEnabled()); // its numbers aren't this game's
    bool said = false;
    for (auto* label : sonora.findChildren<QLabel*>()) {
        said = said || (!label->isHidden() && label->text().contains("can't be opened here"));
    }
    CHECK(said);

    geck::MapBrowserDialog alone(resources);
    alone.setGames({}, QString());
    auto* none = alone.findChild<QComboBox*>("mapBrowserGame");
    REQUIRE(none != nullptr);
    CHECK(none->parentWidget()->isHidden()); // no other games: no Game list
}

TEST_CASE("Someone with no stamps of their own opens the Pattern Library on Fallout 2's", "[qt][othergames]") {
    const QString bundled = geck::pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty() || !QDir(bundled).exists("Fallout 2/Buildings")) {
        SKIP("no stamps beside this test program");
    }
    const QString own = geck::pattern::PatternLibrary::rootDir(); // the test location's, in test mode
    if (QDirIterator(own, { "*.json" }, QDir::Files, QDirIterator::Subdirectories).hasNext()) {
        SKIP("the test location already holds stamps");
    }
    geck::resource::GameResources resources;
    geck::PatternBrowserDialog dialog(resources);
    auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
    REQUIRE(tree != nullptr);
    auto* grid = dialog.findChild<QListWidget*>();
    REQUIRE(grid != nullptr);
    CHECK(currentFolderName(tree) == "Fallout 2");
    CHECK(tree->isExpanded(tree->currentIndex()));
    CHECK(gridNames(grid).contains("Buildings"));
}
