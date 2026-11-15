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
#include "Application.h"
#include "format/map/Map.h"
#include "state/loader/MapLoader.h"
#include "resource/ResourceInitializer.h"
#include "ui/core/EditorWidget.h"
#include "ui/core/MainWindow.h"
#include "ui/widgets/LoadingWidget.h"
#include "ui/widgets/SFMLWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

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

#include <cmath>
#include <filesystem>
#include <memory>

// The pictures in the README, drawn by gecko's own widgets off screen. Hidden: run it by name,
//
//   set GECKO_SHOTS_DIR=<folder for the PNGs>
//   set GECKO_SHOTS_DATA=<the game's data paths, lowest first, ;-separated>
//   set GECKO_SHOTS_MODTOOLS=<a mod folder with tools/gecko_assets.py>
//   set GECKO_SHOTS_SCENERY=<a picture for Add Scenery>   set GECKO_SHOTS_BUILDING=<a building render>
//   set GECKO_SHOTS_VANILLA=<Fallout 2's own data paths, lowest first, ;-separated: the editing pictures
//                            open one of its maps, so nothing of a mod shows>
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

// A mouse event as the map view gets it from a real mouse: `buttons` is what is held down.
void mouse(QWidget* view, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, pos, view->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(view, &event);
    QApplication::processEvents();
}

void click(QWidget* view, QPointF pos, Qt::MouseButton button = Qt::LeftButton) {
    mouse(view, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonPress, pos, button, button);
    mouse(view, QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
}

void drag(QWidget* view, QPointF from, QPointF to) {
    mouse(view, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
    mouse(view, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 12; ++step) {
        mouse(view, QEvent::MouseMove, from + (to - from) * (step / 12.0), Qt::NoButton, Qt::LeftButton);
    }
    mouse(view, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

void key(QWidget* view, Qt::Key code) {
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(view, &press);
    QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
    QApplication::sendEvent(view, &release);
    QApplication::processEvents();
}

void saveImage(const QImage& image, const QString& name) {
    REQUIRE(image.save(QDir(env("GECKO_SHOTS_DIR")).filePath(name)));
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
        const QString pick = env("GECKO_SHOTS_STAMP_FOLDER").isEmpty() ? QString("Fallout 2 Stamps/Adobe")
                                                                       : env("GECKO_SHOTS_STAMP_FOLDER");
        const QString folder = QDir(bundled).filePath(pick);
        dialog.show();
        wait(1500);
        tree->expand(model->index(QFileInfo(folder).path()));
        tree->setCurrentIndex(model->index(folder));
        save(dialog, env("GECKO_SHOTS_STAMP_FOLDER").isEmpty() ? QString("pattern-library.png")
                                                                : "stamps-" + QString(pick).replace('/', '-').replace(' ', '_') + ".png");
    }

    SECTION("other game's stamps") {
        // Sonora's folder as the Pattern Library shows it with the mod tools: drawn with Sonora's data.
        auto tools = runner();
        std::map<QString, std::unique_ptr<geck::resource::GameResources>> loadedGames;
        geck::PatternBrowserDialog dialog(*resources);
        dialog.setOtherGames(
            [&tools]() {
                std::map<QString, geck::PatternBrowserDialog::StampGame> games;
                const QJsonObject folders = tools->runAndWait({ "stamp-games" }).json.value("folders").toObject();
                for (auto it = folders.constBegin(); it != folders.constEnd(); ++it) {
                    const QJsonObject game = it.value().toObject();
                    games[it.key()] = { game.value("id").toString(), game.value("label").toString(),
                        game.value("available").toBool(), game.value("why").toString() };
                }
                return games;
            },
            [&tools, &loadedGames](const QString& id, const QString&) -> geck::resource::GameResources* {
                auto game = std::make_unique<geck::resource::GameResources>();
                for (const auto& value : tools->runAndWait({ "source", id }).json.value("paths").toArray()) {
                    game->files().addDataPath(std::filesystem::path(value.toString().toStdU16String()));
                }
                return (loadedGames[id] = std::move(game)).get();
            });
        dialog.resize(980, 640);
        auto* tree = dialog.findChild<QTreeView*>("bundledStampsTree");
        REQUIRE(tree != nullptr);
        auto* model = qobject_cast<QFileSystemModel*>(tree->model());
        const QString bundled = geck::pattern::PatternLibrary::bundledDir();
        dialog.show();
        wait(1500);
        tree->expand(model->index(QDir(bundled).filePath("Sonora Stamps")));
        tree->setCurrentIndex(model->index(QDir(bundled).filePath("Sonora Stamps/Adobe")));
        save(dialog, "stamps-other-game.png");
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

// The editing pictures: gecko's own window on one of Fallout 2's maps (Arroyo's village), driven the
// way a mouse and keyboard drive it. Nothing is saved.
TEST_CASE("README pictures of editing", "[.readme]") {
    if (env("GECKO_SHOTS_DIR").isEmpty() || env("GECKO_SHOTS_VANILLA").isEmpty()) {
        SKIP("set GECKO_SHOTS_DIR and GECKO_SHOTS_VANILLA");
    }
    QDir().mkpath(env("GECKO_SHOTS_DIR"));
    auto resources = std::make_shared<geck::resource::GameResources>();
    resources->files().addDataPath(geck::Application::getResourcesPath()); // gecko's own art, lowest, as the app mounts it
    for (const QString& path : env("GECKO_SHOTS_VANILLA").split(';', Qt::SkipEmptyParts)) {
        resources->files().addDataPath(std::filesystem::path(path.toStdU16String()));
    }
    REQUIRE(resources->files().exists("maps/arvillag.map"));
    geck::ResourceInitializer::loadEssentialLstFiles(*resources); // as loading the data paths does

    geck::MainWindow window(resources, std::make_shared<geck::Settings>());
    window.resize(1400, 900);
    window.show();
    std::unique_ptr<geck::Map> loaded;
    {
        geck::LoadingWidget loading(&window);
        loading.addLoader(std::make_unique<geck::MapLoader>(resources, "maps/arvillag.map", -1, false,
            [&loaded](std::unique_ptr<geck::Map> map) { loaded = std::move(map); }));
        loading.exec();
    }
    REQUIRE(loaded);
    window.setEditorWidget(std::make_unique<geck::EditorWidget>(*resources, std::move(loaded)));
    auto* editor = window.findChild<geck::EditorWidget*>();
    REQUIRE(editor != nullptr);
    auto* view = editor->findChild<geck::SFMLWidget*>();
    REQUIRE(view != nullptr);
    window.startGameLoop();
    editor->centerViewOnPlayerPosition();
    wait(1500);
    const QPointF middle(view->width() / 2.0, view->height() / 2.0);

    SECTION("right-click menu") {
        // Right-click outward from the middle until it lands on an object: with nothing selected or
        // copied, gecko only opens the menu over one.
        QImage shot;
        for (int ring = 0; ring < 8 && shot.isNull(); ++ring) {
            for (int step = 0; step < 8 && shot.isNull(); ++step) {
                const double angle = step * 3.14159265 / 4.0;
                const QPointF at = middle + QPointF(std::cos(angle), std::sin(angle)) * (ring * 45.0);
                QTimer::singleShot(600, [&window, &shot]() {
                    if (QWidget* menu = QApplication::activePopupWidget()) {
                        QImage image = window.grab().toImage();
                        QPainter painter(&image);
                        painter.drawPixmap(menu->mapToGlobal(QPoint()) - window.mapToGlobal(QPoint()), menu->grab());
                        painter.end();
                        shot = image;
                        menu->close();
                    }
                });
                mouse(view, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
                wait(200);
                mouse(view, QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
                mouse(view, QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::NoButton);
                wait(800);
            }
        }
        REQUIRE_FALSE(shot.isNull());
        saveImage(shot, "right-click-menu.png");
    }

    SECTION("copy and paste") {
        drag(view, middle + QPointF(-170, -110), middle + QPointF(60, 70));
        wait(400);
        editor->copySelection();
        editor->beginPaste();
        for (int step = 0; step <= 10; ++step) {
            mouse(view, QEvent::MouseMove, middle + QPointF(-60 + step * 22, 60 + step * 8), Qt::NoButton, Qt::NoButton);
        }
        wait(1200);
        saveImage(window.grab().toImage(), "copy-paste.png");
        key(view, Qt::Key_Escape);
    }

    SECTION("black edge") {
        REQUIRE(editor->activateBlackEdgeTool());
        click(view, middle + QPointF(-260, 40));
        click(view, middle + QPointF(-60, -110));
        click(view, middle + QPointF(180, 10));
        mouse(view, QEvent::MouseMove, middle + QPointF(60, 170), Qt::NoButton, Qt::NoButton);
        wait(1200);
        saveImage(window.grab().toImage(), "black-edge.png");
        key(view, Qt::Key_Escape);
    }

    SECTION("blockers") {
        editor->setShowWallBlockers(true);
        REQUIRE(editor->activateAddWallBlockerTool());
        drag(view, middle + QPointF(-200, 120), middle + QPointF(140, 120));
        mouse(view, QEvent::MouseMove, middle + QPointF(170, 150), Qt::NoButton, Qt::NoButton);
        wait(1200);
        saveImage(window.grab().toImage(), "blockers.png");
        key(view, Qt::Key_Escape);
    }

    window.stopGameLoop();
}
