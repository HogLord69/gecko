#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QDialog>
#include <QString>

#include "pattern/Pattern.h"

class QFileSystemModel;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QTreeView;

namespace geck {

class HexagonGrid;
namespace resource {
    class GameResources;
}

/// Browses the user's pattern library as a folder tree + thumbnail grid, with import. The stamps that
/// come with gecko (PatternLibrary::bundledDir) are a second tree under it, read where they are.
/// Picking a pattern (double-click or Open) accepts the dialog; the chosen pattern is
/// then available via selectedPattern().
class PatternBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit PatternBrowserDialog(resource::GameResources& resources, QWidget* parent = nullptr);
    ~PatternBrowserDialog() override;

    std::optional<pattern::Pattern> selectedPattern() const { return _selected; }
    /// The picked stamp's file, and the game it is in the numbers of when that isn't the map's own
    /// (a "Came with gecko" folder cut from another game): such a stamp is converted before it is used.
    QString selectedPath() const { return _selectedPath; }
    QString selectedGameId() const { return _selectedGame; }

    /// The game a "Came with gecko" folder was cut from, as the mod tools say (gecko_assets.py
    /// stamp-games). Folders in the map's own numbers are not listed.
    struct StampGame {
        QString id;
        QString label;
        bool available = false; ///< set up here, so it can be drawn and converted
        QString why;            ///< why not, when it isn't
    };
    using StampGamesProvider = std::function<std::map<QString, StampGame>()>;
    using GameResourcesProvider = std::function<resource::GameResources*(const QString& id, const QString& label)>;
    /// Without these, every folder is drawn with the map's own data, as before.
    void setOtherGames(StampGamesProvider games, GameResourcesProvider resources);

private slots:
    void onFolderSelected(const QModelIndex& index);
    void onBundledFolderSelected(const QModelIndex& index);
    void onPatternActivated(QListWidgetItem* item);
    void onImport();

private:
    /// `resources` draws the thumbnails; null draws none (a game that isn't set up here).
    void populateGrid(const QString& folder, resource::GameResources* resources);
    /// The game a bundled folder is in the numbers of, or nullopt for the map's own.
    std::optional<StampGame> bundledGame(const QString& folder);

    resource::GameResources& _resources;
    std::unique_ptr<HexagonGrid> _hexgrid;
    QFileSystemModel* _folderModel = nullptr;
    QTreeView* _folderTree = nullptr;
    QFileSystemModel* _bundledModel = nullptr; ///< null when gecko came with no stamps
    QTreeView* _bundledTree = nullptr;
    QLabel* _gameNote = nullptr;
    StampGamesProvider _stampGames;
    GameResourcesProvider _gameResources;
    std::optional<std::map<QString, StampGame>> _games; ///< asked for once, when a bundled folder opens
    std::optional<StampGame> _currentGame;
    QString _selectedPath;
    QString _selectedGame;
    QListWidget* _grid = nullptr;
    QString _currentFolder;
    std::optional<pattern::Pattern> _selected;
};

} // namespace geck
