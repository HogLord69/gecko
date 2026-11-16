#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <QDialog>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QString>

class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QObject;
class QImage;
class QPushButton;
class QShowEvent;
class QThread;
class QTimer;

namespace geck {

class HexagonGrid;
class MapRenderWorker;
namespace resource {
    class GameResources;
}

/// Browses the maps available in the mounted game data as a thumbnail grid, with a
/// filter box and a larger preview of the highlighted map. Thumbnails are rendered
/// lazily — only for the cells currently in view, one per event-loop turn — because
/// each one parses and renders a whole map, which is far too heavy to do up front for
/// the hundreds of maps a DAT can hold. Picking a map (double-click or Open) accepts
/// the dialog; its VFS path is then available via selectedMapPath().
class MapBrowserDialog : public QDialog {
    Q_OBJECT

public:
    /// `dataPaths` (the app's mounted data paths) enable the background render worker for
    /// thumbnail cache misses; with an empty list the dialog falls back to rendering on the
    /// UI thread.
    explicit MapBrowserDialog(resource::GameResources& resources,
        std::vector<std::filesystem::path> dataPaths = {},
        QWidget* parent = nullptr);
    ~MapBrowserDialog() override;

    /// VFS path of the chosen map (e.g. "maps/sfshutl2.map"); empty unless accepted.
    QString selectedMapPath() const { return _selectedPath; }

    /// exec()'s answer when another game was picked in the Game list: open the browser again on
    /// requestedGame()'s maps ("" = the map's own game).
    static constexpr int SwitchGame = 2;
    struct Game {
        QString id;    ///< "" for the game being edited
        QString label;
    };
    /// Offer a Game list above the maps: the game being edited, then `others`. `current` is the
    /// one shown; another game's maps are drawn with its own data and can't be opened here.
    void setGames(const QList<Game>& others, const QString& current);
    QString requestedGame() const { return _requestedGame; }

protected:
    void showEvent(QShowEvent* event) override;
    // Rescales the preview to fit when the preview label is resized (e.g. by the splitter).
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onFilterChanged(const QString& text);
    void onCurrentItemChanged(const QListWidgetItem* current);
    void onItemActivated(const QListWidgetItem* item);
    void renderNextVisibleThumbnail();
    void onWorkerRendered(const QString& vfsPath, int size, const QImage& image);

private:
    void populate();
    void acceptCurrent();
    void updatePreview(const QListWidgetItem* item);
    void rescalePreview();
    QListWidgetItem* nextUnrenderedVisibleItem() const;

    resource::GameResources& _resources;
    std::unique_ptr<HexagonGrid> _hexgrid;
    QLineEdit* _search = nullptr;
    QListWidget* _grid = nullptr;
    QLabel* _previewImage = nullptr;
    QLabel* _previewName = nullptr;
    QPushButton* _openButton = nullptr;
    QTimer* _thumbnailTimer = nullptr;
    QPixmap _previewSource; ///< Native-resolution preview, rescaled to fit on resize.
    QString _selectedPath;
    QComboBox* _gameCombo = nullptr;
    QLabel* _gameNote = nullptr;
    QString _currentGame;   ///< "" = the game being edited
    QString _requestedGame;

    // Background renderer for cache misses (null when no data paths were provided). Requests
    // are queued to the worker's thread; results arrive through onWorkerRendered.
    QThread* _renderThread = nullptr;
    MapRenderWorker* _renderWorker = nullptr;
    QSet<QString> _requestedThumbnails;
    QString _pendingPreviewPath;
};

} // namespace geck
