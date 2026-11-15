#include <catch2/catch_test_macros.hpp>

#include "pattern/PatternLibrary.h"
#include "resource/GameResources.h"
#include "ui/Settings.h"
#include "ui/assets/ModToolsRunner.h"
#include "ui/dialogs/AddSceneryDialog.h"
#include "ui/dialogs/CutBuildingDialog.h"
#include "ui/dialogs/PatternBrowserDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/panels/ObjectPalettePanel.h"
#include "ui/widgets/WelcomeWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileSystemModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QFileInfo>
#include <QTabWidget>
#include <QTreeView>

#include <filesystem>
#include <memory>

// The pictures in the README, drawn by gecko's own widgets off screen. Hidden: run it by name,
//
//   set GECKO_SHOTS_DIR=<folder for the PNGs>
//   set GECKO_SHOTS_DATA=<the game's data paths, lowest first, ;-separated>
//   set GECKO_SHOTS_MODTOOLS=<a mod folder with tools/gecko_assets.py>
//   set GECKO_SHOTS_SCENERY=<a picture for Add Scenery>   set GECKO_SHOTS_BUILDING=<a building render>
//   qt_tests "[.readme]"

namespace {

QString env(const char* name) {
    return qEnvironmentVariable(name);
}

void wait(int ms, const std::function<bool()>& done = {}) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms && !(done && done())) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void save(QWidget& widget, const QString& name) {
    widget.show();
    wait(300);
    const QString path = QDir(env("GECKO_SHOTS_DIR")).filePath(name);
    REQUIRE(widget.grab().save(path));
    widget.hide();
}

std::shared_ptr<geck::resource::GameResources> gameData() {
    auto resources = std::make_shared<geck::resource::GameResources>();
    for (const QString& path : env("GECKO_SHOTS_DATA").split(';', Qt::SkipEmptyParts)) {
        resources->files().addDataPath(std::filesystem::path(path.toStdU16String()));
    }
    return resources;
}

std::unique_ptr<geck::ModToolsRunner> runner() {
    return std::make_unique<geck::ModToolsRunner>(std::filesystem::path(env("GECKO_SHOTS_MODTOOLS").toStdU16String()), "python");
}

// The picture field shows where the picture is on this computer; the README shows its name only.
void showFileNameOnly(QWidget& dialog, const QString& path) {
    for (auto* edit : dialog.findChildren<QLineEdit*>()) {
        if (edit->isReadOnly() && QFileInfo(edit->text()) == QFileInfo(path)) {
            edit->setText(QFileInfo(path).fileName());
        }
    }
}

int labelsWithPictures(const QWidget& widget) {
    int n = 0;
    for (const auto* label : widget.findChildren<QLabel*>()) {
        n += label->pixmap().isNull() ? 0 : 1;
    }
    return n;
}

} // namespace

TEST_CASE("README pictures", "[.readme]") {
    if (env("GECKO_SHOTS_DIR").isEmpty() || env("GECKO_SHOTS_DATA").isEmpty() || env("GECKO_SHOTS_MODTOOLS").isEmpty()) {
        SKIP("set GECKO_SHOTS_DIR, GECKO_SHOTS_DATA and GECKO_SHOTS_MODTOOLS");
    }
    QDir().mkpath(env("GECKO_SHOTS_DIR"));
    auto resources = gameData();
    REQUIRE(resources->files().exists("color.pal"));

    SECTION("welcome") {
        geck::WelcomeWidget welcome;
        welcome.resize(720, 640);
        save(welcome, "welcome.png");
    }

    SECTION("game list") {
        geck::ObjectPalettePanel palette(*resources);
        auto tools = runner();
        palette.setOtherGameProviders(
            [&tools](QString*) {
                QList<geck::ObjectPalettePanel::OtherGame> games;
                for (const auto& value : tools->runAndWait({ "sources" }).json.value("games").toArray()) {
                    games.push_back({ value.toObject().value("id").toString(), value.toObject().value("label").toString() });
                }
                return games;
            },
            [&tools](const QString& id, QString* error) -> std::optional<geck::ObjectPalettePanel::OtherGameData> {
                const auto result = tools->runAndWait({ "source", id, "--new" });
                if (!result.ok) {
                    *error = result.error;
                    return std::nullopt;
                }
                geck::ObjectPalettePanel::OtherGameData data;
                for (const auto& value : result.json.value("paths").toArray()) {
                    data.layers.emplace_back(value.toString().toStdU16String());
                }
                const QJsonObject pieces = result.json.value("new").toObject();
                for (auto it = pieces.constBegin(); it != pieces.constEnd(); ++it) {
                    for (const auto& line : it.value().toArray()) {
                        data.newPieces[it.key()].insert(line.toInt());
                    }
                }
                return data;
            });
        palette.loadObjects();
        auto* picker = palette.findChild<QComboBox*>("gamePicker");
        REQUIRE(picker != nullptr);
        picker->showPopup();
        picker->hidePopup();
        const int sonora = picker->findData("sonora");
        REQUIRE(sonora > 0);
        picker->setCurrentIndex(sonora);
        palette.findChild<QTabWidget*>()->setCurrentIndex(1); // Scenery
        palette.resize(430, 860);
        save(palette, "game-list.png");
    }

    SECTION("pattern library") {
        const QString bundled = geck::pattern::PatternLibrary::bundledDir();
        REQUIRE_FALSE(bundled.isEmpty());
        geck::PatternBrowserDialog dialog(*resources);
        dialog.resize(980, 600);
        auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
        REQUIRE(tree != nullptr);
        auto* model = qobject_cast<QFileSystemModel*>(tree->model());
        const QString folder = QDir(bundled).filePath("Fallout 2 Stamps/Adobe");
        dialog.show();
        wait(1500);
        tree->expand(model->index(QDir(bundled).filePath("Fallout 2 Stamps")));
        tree->setCurrentIndex(model->index(folder));
        save(dialog, "pattern-library.png");
    }

    SECTION("first run") {
        auto settings = std::make_shared<geck::Settings>();
        settings->setDataPaths({});
        settings->setGameDataDirectory("D:/SteamLibrary/steamapps/common/Fallout 2");
        geck::SettingsDialog dialog(settings);
        dialog.resize(820, 640);
        save(dialog, "first-run.png");
    }

    SECTION("add scenery") {
        if (env("GECKO_SHOTS_SCENERY").isEmpty()) {
            SKIP("set GECKO_SHOTS_SCENERY");
        }
        auto tools = runner();
        geck::AddSceneryDialog dialog(tools.get());
        dialog.resize(1100, 720);
        dialog.show();
        dialog.loadPicture(env("GECKO_SHOTS_SCENERY"));
        wait(60000, [&dialog] { return labelsWithPictures(dialog) >= 2; });
        wait(500);
        showFileNameOnly(dialog, env("GECKO_SHOTS_SCENERY"));
        save(dialog, "add-scenery.png");
    }

    SECTION("cut building") {
        if (env("GECKO_SHOTS_BUILDING").isEmpty()) {
            SKIP("set GECKO_SHOTS_BUILDING");
        }
        auto tools = runner();
        geck::CutBuildingDialog dialog(tools.get());
        dialog.resize(1180, 760);
        dialog.show();
        dialog.loadPicture(env("GECKO_SHOTS_BUILDING"));
        wait(90000, [&dialog] { return labelsWithPictures(dialog) >= 2; });
        wait(500);
        showFileNameOnly(dialog, env("GECKO_SHOTS_BUILDING"));
        save(dialog, "cut-building.png");
    }
}
