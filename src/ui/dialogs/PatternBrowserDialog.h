#pragma once

#include <memory>
#include <optional>

#include <QDialog>
#include <QString>

#include "pattern/Pattern.h"

class QFileSystemModel;
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

private slots:
    void onFolderSelected(const QModelIndex& index);
    void onBundledFolderSelected(const QModelIndex& index);
    void onPatternActivated(QListWidgetItem* item);
    void onImport();

private:
    void populateGrid(const QString& folder);

    resource::GameResources& _resources;
    std::unique_ptr<HexagonGrid> _hexgrid;
    QFileSystemModel* _folderModel = nullptr;
    QTreeView* _folderTree = nullptr;
    QFileSystemModel* _bundledModel = nullptr; ///< null when gecko came with no stamps
    QTreeView* _bundledTree = nullptr;
    QListWidget* _grid = nullptr;
    QString _currentFolder;
    std::optional<pattern::Pattern> _selected;
};

} // namespace geck
