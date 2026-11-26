#pragma once

#include <QDialog>
#include <QImage>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace geck {

class ModToolsRunner;
struct ModToolResult;

/**
 * Tools > Make Tiles from Picture. A top-down picture of a surface (a photo or a texture) becomes
 * new floor or roof tiles, through the mod's own tools/gecko_assets.py (which uses image_tiles.py):
 * the picture laid flat on a block of squares at the game's angle, cut into one tile per square,
 * in the game's colours, each tile in a spare line of tiles.lst. A stamp in the patterns' Custom
 * folder lays a patch of the block; its tileRepeat snaps each patch to the block's grid.
 *
 * The left pane is the picture, turned as it is laid, with the square that is used; the right pane is a patch of the
 * tiles as the game draws them, beside the player, and the same with each tile's square outlined.
 */
class MakeTilesDialog : public QDialog {
    Q_OBJECT

public:
    explicit MakeTilesDialog(ModToolsRunner* runner, QWidget* parent = nullptr);
    /// Open a picture as Choose... does, without the file dialog (tests use it).
    void loadPicture(const QString& path);
    /// What the tools are asked for, without the command ("tiles-preview" / "tiles-create").
    [[nodiscard]] QStringList tileArguments() const;

    [[nodiscard]] int tilesMade() const { return _tilesMade; }

private:
    void setupUI();
    void choosePicture();
    void drawSourceView();
    void schedulePreview();
    void runPreview();
    void showPreview(const ModToolResult& result);
    void drawPreview();
    void create();
    void updateCreateButton();

    ModToolsRunner* _runner;

    QString _picturePath;
    QImage _picture;

    QString _previewPath;
    QImage _previewImage;
    bool _previewRunning = false;
    bool _previewDirty = false;
    bool _previewOk = false;
    bool _creating = false;
    int _tilesMade = 0;
    QTimer* _previewTimer = nullptr;

    QLineEdit* _pictureEdit = nullptr;
    QLabel* _sourceLabel = nullptr;
    QLabel* _previewLabel = nullptr;
    QComboBox* _zoomCombo = nullptr;
    QLineEdit* _prefixEdit = nullptr;
    QLineEdit* _stampEdit = nullptr;
    QLabel* _problemLabel = nullptr;
    QComboBox* _blockCombo = nullptr;
    QComboBox* _layerCombo = nullptr;
    QSpinBox* _turnSpin = nullptr; ///< degrees clockwise
    QCheckBox* _repeatCheck = nullptr;
    QCheckBox* _evenCheck = nullptr;
    QCheckBox* _falloutColoursCheck = nullptr;
    QLabel* _infoLabel = nullptr;
    QPushButton* _createButton = nullptr;
};

} // namespace geck
