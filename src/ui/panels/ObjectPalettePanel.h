#pragma once

#include "ui/common/BasePanel.h"
#include "ui/palette/PaletteModel.h"
#include "ui/theme/ThemeManager.h"
#include "ui/palette/PaletteView.h"
#include <QCheckBox>
#include <QComboBox>
#include <QList>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QDrag>
#include <QMimeData>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <memory>
#include <optional>
#include <utility>
#include <unordered_map>

namespace geck {

class Map;
class Lst;
class Pro;
namespace resource {
    class GameResources;
}

class ObjectPalettePanel;

// Defined outside the class so it can be used as an unordered_map key.
enum class ObjectCategory {
    ITEMS,    // Weapons, armor, consumables, etc.
    SCENERY,  // Furniture, decorations, interactive objects
    CRITTERS, // NPCs, monsters, characters
    WALLS,    // Wall segments and structural elements
    MISC      // Miscellaneous objects
};

} // namespace geck

// Hash function for ObjectCategory to use in unordered_map
template <>
struct std::hash<geck::ObjectCategory> {
    std::size_t operator()(geck::ObjectCategory c) const noexcept {
        return static_cast<std::size_t>(c);
    }
};

namespace geck {

/**
 * @brief Information about a loaded object for the palette
 */
struct ObjectInfo {
    QString proFileName; // Original .pro filename from LST
    QString displayName; // Human-readable name for display
    const Pro* pro;      // Raw pointer to PRO file (managed by ResourceRepository cache)
    QString frmPath;     // Path to FRM file for thumbnail
    int listIndex;       // Index in the category list
    // The game this entry was read from: the map's own, or another one picked in the palette's Game
    // list. pro and frmPath belong to that game's lists, so another game's entry is never placed as
    // it is: it is brought into the mod first (MainWindow::bringInFromOtherGame).
    resource::GameResources* resources = nullptr;
    QString sourceGame; // empty for the map's own game, else the mod tools' id for the other game

    ObjectInfo(const QString& fileName, int index)
        : proFileName(fileName)
        , pro(nullptr)
        , listIndex(index) { }
};

/**
 * @brief Panel showing all available objects organized by category
 *
 * Features:
 * - Tabbed display by object type (Items, Scenery, Critters, Walls, Misc)
 * - Grid display of objects with Qt pixmaps
 * - Object selection for placement
 * - Search functionality within categories
 * - Single object placement mode
 */
class ObjectPalettePanel : public BasePanel {
    Q_OBJECT

public:
    explicit ObjectPalettePanel(resource::GameResources& resources, QWidget* parent = nullptr);
    ~ObjectPalettePanel();

    /// Another game the palette can show, as the mod tools list it (gecko_assets.py sources).
    struct OtherGame {
        QString id;
        QString label;
    };
    /// The other games, or none with `note` saying why. Called when the Game list first opens.
    using GameListProvider = std::function<QList<OtherGame>(QString* note)>;
    /// What the palette needs to show another game.
    struct OtherGameData {
        std::vector<std::filesystem::path> layers; ///< its data paths, lowest priority first
        /// Per proto folder ("scenery", "walls", ...), the 1-based .lst lines of the pieces whose
        /// picture the map's own game doesn't have. A folder that is missing is not filtered.
        std::map<QString, std::set<int>> newPieces;
    };
    /// A game's data, or nothing with `error` saying why.
    using GameLayersProvider = std::function<std::optional<OtherGameData>(const QString& id, QString* error)>;
    void setOtherGameProviders(GameListProvider list, GameLayersProvider layers);
    /// Drop every other game's data and textures; the palette goes back to the map's own game.
    void releaseOtherGames();
    [[nodiscard]] bool showingOtherGame() const { return !_activeGame.isEmpty(); }

    // Initialization
    void loadObjects();
    void setMap(Map* map) { _map = map; }

    // Object selection
    int getSelectedObjectIndex() const { return _selectedObjectIndex; }
    ObjectCategory getCurrentCategory() const { return _currentCategory; }
    bool hasSelectedObject() const { return _selectedObjectIndex >= 0; }

    // Eyedropper: switch to the proto's category tab and filter to it so it is revealed and selected.
    // Returns the proto's {list index, category} — the caller uses it to arm click-to-place — or
    // std::nullopt if the PID's type is not shown here (e.g. a tile) or has no palette entry.
    std::optional<std::pair<int, ObjectCategory>> revealProto(uint32_t pid);

    // Access to object info for drag and drop
    const ObjectInfo* getObjectInfo(int objectIndex, ObjectCategory category) const;

signals:
    void objectSelected(int objectIndex, ObjectCategory category);
    void placeObjectRequested(int objectIndex, ObjectCategory category);

public slots:
    void onObjectClicked(int objectIndex);
    void onCategoryChanged(int tabIndex);
    void onSearchTextChanged(const QString& text) override;

public:
    static constexpr int OBJECT_SIZE = 64; // icon size in the grid

private:
    void setupUI() override;
    void setupGamePicker();
    void listOtherGames();
    void onGameChanged(int index);
    /// Mount `id`'s layers in a resource set of its own; false (after saying why) when it can't be read.
    bool loadOtherGame(const QString& id, const QString& label);
    [[nodiscard]] resource::GameResources& activeResources() const;
    void setupCategoryTabs();
    void setupSearchControls();
    void setupObjectView();

    /// The engine's own name for a proto, or the .pro file name when it has none.
    [[nodiscard]] QString protoDisplayName(const Pro& pro, const QString& proFileName) const;
    /// Feed the model the current category's objects.
    void rebuildItems();
    void updateStatusLabel();
    void selectRowForObject(int objectIndex);

    void loadCategoryObjects(ObjectCategory category);
    QPixmap createObjectThumbnail(const ObjectInfo* objectInfo, ObjectCategory category);
    QString getCategoryPath(ObjectCategory category) const;
    QString getCategoryDisplayName(ObjectCategory category) const;

    std::vector<std::unique_ptr<ObjectInfo>>& getObjectList(ObjectCategory category);
    const std::vector<std::unique_ptr<ObjectInfo>>& getObjectList(ObjectCategory category) const;

    void clearObjectSelection();

    // UI Components
    QVBoxLayout* _mainLayout = nullptr;

    // Game picker: the map's own game, then the other games the mod tools list
    QComboBox* _gameCombo = nullptr;
    QLabel* _gameNote = nullptr;
    QCheckBox* _onlyNewCheck = nullptr; ///< another game: only the pieces the map's game doesn't have
    std::map<QString, std::map<QString, std::set<int>>> _newPieces; ///< per other game, see OtherGameData
    GameListProvider _gameListProvider;
    GameLayersProvider _gameLayersProvider;
    bool _gamesListed = false;
    QString _activeGame; // empty: the map's own game
    std::map<QString, std::unique_ptr<resource::GameResources>> _otherGames;

    // Category tabs
    QTabWidget* _categoryTabs = nullptr;

    // Search controls
    QGroupBox* _searchGroup = nullptr;

    ui::PaletteView* _objectView = nullptr;
    ui::PaletteModel* _model = nullptr;
    QSortFilterProxyModel* _filter = nullptr;

    QLabel* _statusLabel = nullptr;

    // Data
    resource::GameResources& _resources;
    Map* _map = nullptr;

    // State
    int _selectedObjectIndex = -1;
    ObjectCategory _currentCategory = ObjectCategory::ITEMS;

    // Object lists by category
    std::unordered_map<ObjectCategory, std::vector<std::unique_ptr<ObjectInfo>>> _objectsByCategory;
};

} // namespace geck
