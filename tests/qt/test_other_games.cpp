#include <catch2/catch_test_macros.hpp>

#include "format/lst/Lst.h"
#include "resource/GameResources.h"
#include "support/Fixtures.h"
#include "ui/panels/ObjectPalettePanel.h"
#include "pattern/PatternLibrary.h"
#include "ui/assets/ModToolsRunner.h"
#include "ui/Settings.h"
#include "ui/dialogs/MakeTilesDialog.h"
#include "ui/dialogs/MapBrowserDialog.h"
#include "ui/dialogs/PatternBrowserDialog.h"

#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>

#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QSortFilterProxyModel>
#include <QLabel>
#include <QListWidget>
#include <QTreeView>
#include <QComboBox>
#include <QImage>
#include <QLineEdit>
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

TEST_CASE("The mod tools' missing Python packages are read from their answer", "[qt][othergames]") {
    geck::ModToolResult listed;
    listed.json = QJsonObject{ { "ok", false }, { "missing", QJsonArray{ "numpy", "scipy" } }, { "python_path", "C:/Py/python.exe" } };
    CHECK(geck::ModToolsRunner::missingPackages(listed) == QStringList{ "numpy", "scipy" });

    // Python stopped on an import before the tools could answer: the package is in the error text.
    const geck::ModToolResult bare = geck::ModToolsRunner::parseOutput(1, QByteArray(),
        "Traceback (most recent call last):\n  File \"x.py\", line 1\nModuleNotFoundError: No module named 'PIL'\n");
    CHECK(geck::ModToolsRunner::missingPackages(bare) == QStringList{ "pillow" });

    const geck::ModToolResult opencv = geck::ModToolsRunner::parseOutput(1, QByteArray(),
        "ModuleNotFoundError: No module named 'cv2'\n");
    CHECK(geck::ModToolsRunner::missingPackages(opencv) == QStringList{ "opencv-python-headless" });

    const geck::ModToolResult other = geck::ModToolsRunner::parseOutput(1, QByteArray(),
        "ModuleNotFoundError: No module named 'yaml'\n");
    CHECK(geck::ModToolsRunner::missingPackages(other).isEmpty()); // not one the tools need: not offered

    geck::ModToolResult fine;
    fine.ok = true;
    fine.json = QJsonObject{ { "ok", true } };
    CHECK(geck::ModToolsRunner::missingPackages(fine).isEmpty());
}

TEST_CASE("A tools answer of missing packages comes back as it is when no one can be asked", "[qt][othergames]") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const std::filesystem::path tools = std::filesystem::path(root.path().toStdU16String()) / "tools";
    write(tools / "gecko_assets.py",
        "import json, sys\n"
        "print(json.dumps({'ok': False, 'missing': ['numpy'], 'python_path': sys.executable,"
        " 'error': 'no numpy'}))\n");
    geck::ModToolsRunner runner(std::filesystem::path(root.path().toStdU16String()), "python");
    const geck::ModToolResult result = runner.runAndWait({ "info" }, 60000);
    if (result.json.isEmpty()) {
        SKIP("no python on this machine's PATH: " + result.error.toStdString());
    }
    CHECK_FALSE(result.ok); // off screen nobody can answer the offer, so nothing is installed or re-run
    CHECK(geck::ModToolsRunner::missingPackages(result) == QStringList{ "numpy" });
}

TEST_CASE("A tools answer of a missing working copy comes back as it is when no one can be asked", "[qt][othergames]") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const std::filesystem::path kit(root.path().toStdU16String());
    write(kit / "setup.py", "raise SystemExit('not to be run off screen')\n");
    write(kit / "tools" / "gecko_assets.py",
        "import json, os\n"
        "kit = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))\n"
        "print(json.dumps({'ok': False, 'needs_base': True, 'base': os.path.join(kit, 'base'),"
        " 'fallout2': '', 'setup': os.path.join(kit, 'setup.py'), 'error': 'no working copy'}))\n");
    geck::ModToolsRunner runner(kit, "python");
    const geck::ModToolResult result = runner.runAndWait({ "port", "sonora", "0x02000001" }, 60000);
    if (result.json.isEmpty()) {
        SKIP("no python on this machine's PATH: " + result.error.toStdString());
    }
    CHECK_FALSE(result.ok);
    CHECK(result.json.value("needs_base").toBool());
    CHECK_FALSE(std::filesystem::exists(kit / "base")); // nothing was made without anyone saying so
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

// What Apply New Art deployed is mounted on top of the game Preferences names, so a building's walls and
// the roof tiles in spare tile lines draw instead of whatever those lines held before.
TEST_CASE("gecko mounts the mod the tools deployed on top of the game", "[qt][othergames]") {
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const std::filesystem::path dir(temp.path().toStdU16String());
    const std::filesystem::path kit = dir / "workflow";
    const std::filesystem::path game = dir / "Fallout 2";
    std::filesystem::create_directories(game);
    write(kit / "tools" / "gecko_assets.py", "\n");
    const std::vector<std::filesystem::path> paths{ game };

    // nothing deployed yet: nothing to mount
    CHECK(geck::ModToolsRunner::deployedModFolder(kit).empty());
    CHECK(geck::ModToolsRunner::withDeployedMod(paths, kit, false) == paths);

    const std::filesystem::path deployed = kit / "base" / "mods" / "foc";
    std::filesystem::create_directories(deployed);
    CHECK(geck::ModToolsRunner::deployedModFolder(kit) == deployed.lexically_normal());
    const auto mounted = geck::ModToolsRunner::withDeployedMod(paths, kit, false);
    REQUIRE(mounted.size() == 2);
    CHECK(mounted.front() == game);
    CHECK(mounted.back() == deployed.lexically_normal()); // last: it wins over the game

    // a profile that already mounts it keeps its own order, however the path is spelled
    const std::vector<std::filesystem::path> profile{ game, kit / "base" / "mods" / ".." / "mods" / "foc", dir / "maps" };
    CHECK(geck::ModToolsRunner::withDeployedMod(profile, kit, false) == profile);

    // another game being browsed: the mod's lists would replace that game's
    CHECK(geck::ModToolsRunner::withDeployedMod(paths, kit, true) == paths);
    CHECK(geck::ModToolsRunner::withDeployedMod(paths, {}, false) == paths);
}

TEST_CASE("The deployed mod is found where config.json puts the working copy", "[qt][othergames]") {
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const std::filesystem::path dir(temp.path().toStdU16String());
    const std::filesystem::path kit = dir / "workflow";
    write(kit / "tools" / "gecko_assets.py", "\n");
    std::filesystem::create_directories(kit / "base" / "mods" / "foc"); // the default, not used once config names another

    write(kit / "config.json", R"({"fallout2": "", "base": "copies/fo2"})");
    CHECK(geck::ModToolsRunner::deployedModFolder(kit).empty()); // nothing deployed in the one config names
    std::filesystem::create_directories(kit / "copies" / "fo2" / "mods" / "foc");
    CHECK(geck::ModToolsRunner::deployedModFolder(kit) == (kit / "copies" / "fo2" / "mods" / "foc").lexically_normal());

    const std::filesystem::path elsewhere = dir / "Working Copy";
    std::filesystem::create_directories(elsewhere / "mods" / "foc");
    qputenv("GECKO_TEST_WORKING_COPY", QString::fromStdU16String(elsewhere.u16string()).toUtf8());
    write(kit / "config.json", R"({"base": "%GECKO_TEST_WORKING_COPY%"})");
    CHECK(geck::ModToolsRunner::deployedModFolder(kit) == (elsewhere / "mods" / "foc").lexically_normal());
    qunsetenv("GECKO_TEST_WORKING_COPY");

    write(kit / "config.json", R"({"base": ""})"); // empty: the working copy in the kit
    CHECK(geck::ModToolsRunner::deployedModFolder(kit) == (kit / "base" / "mods" / "foc").lexically_normal());
}

TEST_CASE("The tools' overlay goes on top of the deployed mod, and neither over another game", "[qt][othergames]") {
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const std::filesystem::path dir(temp.path().toStdU16String());
    const std::filesystem::path kit = dir / "workflow";
    const std::filesystem::path game = dir / "Fallout 2";
    std::filesystem::create_directories(game);
    write(kit / "tools" / "gecko_assets.py", "\n");
    std::filesystem::create_directories(kit / "base" / "mods" / "foc");
    std::filesystem::create_directories(kit / "build" / "gecko_assets" / "overlay");
    const std::vector<std::filesystem::path> paths{ game };

    const auto all = geck::ModToolsRunner::withToolsData(paths, kit, false);
    REQUIRE(all.size() == 3);
    CHECK(all[1] == (kit / "base" / "mods" / "foc").lexically_normal());
    CHECK(all[2] == kit / "build" / "gecko_assets" / "overlay");
    CHECK(geck::ModToolsRunner::withToolsData(paths, kit, true) == paths);
    CHECK(geck::ModToolsRunner::withToolsData(paths, {}, false) == paths);
}

TEST_CASE("Settings know when a launcher has gecko open on another game", "[qt][othergames]") {
    // the launcher keeps the mod's own settings beside gecko's as settings.json.foc-live (test-mode paths here)
    const QString marker = QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
                               .filePath(QStringLiteral("gecko/settings.json.foc-live"));
    QFile::remove(marker);
    geck::Settings settings;
    CHECK_FALSE(settings.isBrowsingAnotherGame());
    QDir().mkpath(QFileInfo(marker).absolutePath());
    {
        QFile file(marker);
        REQUIRE(file.open(QIODevice::WriteOnly));
    }
    CHECK(settings.isBrowsingAnotherGame());
    QFile::remove(marker);
    CHECK_FALSE(settings.isBrowsingAnotherGame());
}

TEST_CASE("The tools run with the Python that comes with gecko unless Preferences names another", "[qt][othergames]") {
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const std::filesystem::path dir(temp.path().toStdU16String());
    const QString release = QString::fromStdU16String((dir / "gecko").u16string());
    const QString build = QString::fromStdU16String((dir / "build").u16string());
    std::filesystem::create_directories(dir / "build");
    write(dir / "gecko" / "python" / "python.exe", "");
    const QString bundled = QDir::toNativeSeparators(QDir(release).filePath("python/python.exe"));

    CHECK(geck::ModToolsRunner::bundledPython(release) == bundled);
    CHECK(geck::ModToolsRunner::bundledPython(build).isEmpty());
    // an empty setting or a bare python: whatever PATH finds first is not wanted when gecko has its own
    CHECK(geck::ModToolsRunner::effectivePython("", release) == bundled);
    CHECK(geck::ModToolsRunner::effectivePython(" python ", release) == bundled);
    CHECK(geck::ModToolsRunner::effectivePython("Python.exe", release) == bundled);
    CHECK(geck::ModToolsRunner::effectivePython("C:\\Python313\\python.exe", release) == "C:\\Python313\\python.exe");
    CHECK(geck::ModToolsRunner::effectivePython("py", release) == "py");
    // a gecko with no Python of its own keeps PATH's
    CHECK(geck::ModToolsRunner::effectivePython("", build) == "python");
    CHECK(geck::ModToolsRunner::effectivePython("python.exe", build) == "python.exe");
}

TEST_CASE("The working copy's data paths follow the game's load order, as foc.py edit sets them", "[qt][othergames]") {
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const std::filesystem::path dir(temp.path().toStdU16String());
    const std::filesystem::path kit = dir / "workflow";
    const std::filesystem::path base = kit / "base";
    write(kit / "tools" / "gecko_assets.py", "\n");
    CHECK(geck::ModToolsRunner::workingCopyDataPaths(kit).empty()); // no working copy yet
    CHECK(geck::ModToolsRunner::workingCopyDataPaths({}).empty());

    write(base / "master.dat", "");
    write(base / "critter.dat", "");
    write(base / "PATCH001.DAT", "");
    write(base / "patch000.dat", "");
    write(base / "patch_old.dat", ""); // not one the engine loads (patch%03d.dat)
    write(base / "mods" / "rpu.dat", "");
    write(base / "mods" / "npc_armor.dat", "");
    write(base / "mods" / "mods_order.txt", "rpu.dat\r\n; a comment\r\nnot_there.dat\r\nfoc\r\n\r\nnpc_armor.dat\r\n");
    std::filesystem::create_directories(base / "mods" / "foc");

    const std::vector<std::filesystem::path> expected{
        base / "master.dat",
        base / "critter.dat",
        base / "patch000.dat",
        base / "PATCH001.DAT",
        base / "mods" / "rpu.dat",
        base / "mods" / "npc_armor.dat",
        base,
        base / "mods" / "foc",
    };
    std::vector<std::filesystem::path> normal;
    for (const auto& path : expected) {
        normal.push_back(path.lexically_normal());
    }
    CHECK(geck::ModToolsRunner::workingCopyDataPaths(kit) == normal);

    // the working copy config.json names, relative to the kit
    const std::filesystem::path other = kit / "copies" / "fo2";
    write(other / "master.dat", "");
    write(kit / "config.json", R"({"base": "copies/fo2"})");
    const std::vector<std::filesystem::path> named{ (other / "master.dat").lexically_normal(), other.lexically_normal() };
    CHECK(geck::ModToolsRunner::workingCopyDataPaths(kit) == named);
}

TEST_CASE("The Game list's Custom entry shows only the mod's own pieces of the map's game", "[qt][othergames]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const std::filesystem::path base(tmp.path().toStdU16String());
    // the map's own game: a base piece on line 1 and the mod's own on line 2
    const auto game = makeGame(base / "game");
    std::filesystem::copy_file(geck::test::dataPath("test_item_drug_radx.pro"), game / "proto" / "items" / "00000002.pro");
    write(game / "proto" / "items" / "items.lst", "00000001.pro\r\n00000002.pro\r\n");
    const auto other = makeGame(base / "other");

    geck::resource::GameResources own;
    own.files().addDataPath(game);
    geck::ObjectPalettePanel palette(own);
    palette.setOtherGameProviders(
        [](QString*) { return QList<geck::ObjectPalettePanel::OtherGame>{ { "other", "Other Game" } }; },
        [&other](const QString&, QString*) -> std::optional<geck::ObjectPalettePanel::OtherGameData> {
            geck::ObjectPalettePanel::OtherGameData data;
            data.layers = { other };
            return data;
        });
    int asked = 0;
    palette.setOwnPiecesProvider([&asked](QString*) -> std::optional<std::map<QString, std::set<int>>> {
        ++asked;
        return std::map<QString, std::set<int>>{ { "items", { 2, 9 } }, { "scenery", {} } }; // 9: not put in yet
    });
    palette.loadObjects();
    REQUIRE(palette.getObjectInfo(1, geck::ObjectCategory::ITEMS) != nullptr); // this game: both

    auto* picker = palette.findChild<QComboBox*>("gamePicker");
    REQUIRE(picker != nullptr);
    REQUIRE(picker->count() == 2);
    CHECK(picker->itemData(1).toString() == geck::ObjectPalettePanel::ownPiecesId());
    picker->showPopup(); // the other games go after it
    picker->hidePopup();
    REQUIRE(picker->count() == 3);
    CHECK(picker->itemData(1).toString() == geck::ObjectPalettePanel::ownPiecesId());
    CHECK(picker->itemData(2).toString() == "other");

    picker->setCurrentIndex(1);
    CHECK(palette.showingOwnPieces());
    CHECK_FALSE(palette.showingOtherGame()); // placed as they are, not brought in
    const geck::ObjectInfo* info = palette.getObjectInfo(0, geck::ObjectCategory::ITEMS);
    REQUIRE(info != nullptr);
    CHECK(info->proFileName == "00000002.pro");
    CHECK(info->resources == &own);
    CHECK(info->sourceGame.isEmpty());
    CHECK(palette.getObjectInfo(1, geck::ObjectCategory::ITEMS) == nullptr);

    // another game from Custom, then back: asked again, so a piece just put in would show
    picker->setCurrentIndex(2);
    CHECK_FALSE(palette.showingOwnPieces());
    CHECK(palette.showingOtherGame());
    picker->setCurrentIndex(1);
    CHECK(palette.showingOwnPieces());
    CHECK(asked == 2);

    // the eyedropper looks a map object up in all of the map's game
    const auto revealed = palette.revealProto(0x00000001);
    REQUIRE(revealed.has_value());
    CHECK(picker->currentIndex() == 0);
    CHECK_FALSE(palette.showingOwnPieces());

    palette.setOwnPiecesProvider({}); // no tools: no entry
    CHECK(picker->findData(geck::ObjectPalettePanel::ownPiecesId()) < 0);
}

TEST_CASE("Make Tiles from Picture asks the tools for what its settings say", "[qt][othergames]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString picture = QDir(tmp.path()).filePath("dirt.png");
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(QColor(150, 120, 80));
    REQUIRE(image.save(picture));

    geck::ModToolsRunner runner(std::filesystem::path(tmp.path().toStdU16String()), "python");
    geck::MakeTilesDialog dialog(&runner);
    dialog.loadPicture(picture);
    CHECK(dialog.tileArguments() == QStringList{ picture, "--block", "2", "--turn", "0", "--stamp", "dirt" });

    dialog.findChild<QComboBox*>("blockCombo")->setCurrentIndex(0);
    dialog.findChild<QComboBox*>("layerCombo")->setCurrentIndex(1);
    dialog.findChild<QComboBox*>("turnCombo")->setCurrentIndex(3);
    dialog.findChild<QCheckBox*>("repeatCheck")->setChecked(true);
    dialog.findChild<QCheckBox*>("evenCheck")->setChecked(true);
    dialog.findChild<QCheckBox*>("falloutColoursCheck")->setChecked(false);
    dialog.findChild<QLineEdit*>("prefixEdit")->setText("dirt");
    dialog.findChild<QLineEdit*>("stampEdit")->clear();
    CHECK(dialog.tileArguments()
        == QStringList{ picture, "--block", "1", "--turn", "3", "--roof", "--repeat-edges", "--even-light",
            "--own-colours", "--prefix", "dirt" });
    CHECK(dialog.tilesMade() == 0);
}
