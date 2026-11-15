#include "ui/dialogs/PatternBrowserDialog.h"

#include <QApplication>
#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QCollator>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QPushButton>
#include <QSplitter>
#include <QTreeView>
#include <QVBoxLayout>

#include "editor/HexagonGrid.h"
#include "pattern/PatternLibrary.h"
#include "pattern/PatternSerializer.h"
#include "pattern/PatternThumbnail.h"
#include "ui/QtDialogs.h"

namespace geck {

namespace {
    constexpr int THUMBNAIL_SIZE = 96;
    constexpr int PATH_ROLE = Qt::UserRole + 1;

    /// The stamp library's order: size folders smallest first (Small, Medium, Large, Huge, as
    /// tools/organize_stamps.py names them), everything else by name with numbers read as numbers
    /// ("#3" before "#13").
    int sizeRank(const QString& name) {
        static const QStringList sizes{ "Small", "Medium", "Large", "Huge" };
        return static_cast<int>(sizes.indexOf(name));
    }

    bool libraryLess(const QString& left, const QString& right) {
        const int l = sizeRank(left);
        const int r = sizeRank(right);
        if (l >= 0 && r >= 0) {
            return l < r;
        }
        static const QCollator collator = [] {
            QCollator c;
            c.setNumericMode(true);
            c.setCaseSensitivity(Qt::CaseInsensitive);
            return c;
        }();
        return collator.compare(left, right) < 0;
    }

    class LibraryOrder final : public QSortFilterProxyModel {
    public:
        using QSortFilterProxyModel::QSortFilterProxyModel;

    protected:
        bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
            return libraryLess(left.data().toString(), right.data().toString());
        }
    };

    /// The folder a converted stamp goes in, under Custom: "From <game>", without the characters a
    /// Windows folder name can't hold (gecko_assets.py stamp-convert names it the same way).
    QString convertedFolder(const QString& label) {
        return "From " + QString(label).remove(QRegularExpression(R"([<>:"/\\|?*])")).trimmed();
    }

    std::optional<pattern::Pattern> loadPattern(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        return pattern::PatternSerializer::deserialize(file.readAll());
    }
} // namespace

PatternBrowserDialog::PatternBrowserDialog(resource::GameResources& resources, QWidget* parent)
    : QDialog(parent)
    , _resources(resources)
    , _hexgrid(std::make_unique<HexagonGrid>()) {
    setWindowTitle("Pattern Library");
    resize(720, 480);

    const QString root = pattern::PatternLibrary::rootDir();

    _folderModel = new QFileSystemModel(this);
    _folderModel->setRootPath(root);
    _folderModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

    _folderOrder = new LibraryOrder(this);
    _folderOrder->setSourceModel(_folderModel);
    _folderOrder->sort(0);
    _folderTree = new QTreeView(this);
    _folderTree->setModel(_folderOrder);
    _folderTree->setRootIndex(_folderOrder->mapFromSource(_folderModel->index(root)));
    _folderTree->setHeaderHidden(true);
    for (int column = 1; column < _folderModel->columnCount(); ++column) {
        _folderTree->hideColumn(column); // show names only, not size/type/date
    }

    _grid = new QListWidget(this);
    _grid->setViewMode(QListView::IconMode);
    _grid->setIconSize(QSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE));
    _grid->setGridSize(QSize(THUMBNAIL_SIZE + 24, THUMBNAIL_SIZE + 36));
    _grid->setResizeMode(QListView::Adjust);
    _grid->setMovement(QListView::Static);
    _grid->setWordWrap(true);

    // Left: the user's own stamps, then the ones that came with gecko, each a folder tree.
    auto* folders = new QWidget(this);
    auto* foldersLayout = new QVBoxLayout(folders);
    foldersLayout->setContentsMargins(0, 0, 0, 0);
    foldersLayout->addWidget(new QLabel("Your stamps", folders));
    foldersLayout->addWidget(_folderTree, 1);
    const QString bundled = pattern::PatternLibrary::bundledDir();
    if (!bundled.isEmpty()) {
        _bundledModel = new QFileSystemModel(this);
        _bundledModel->setRootPath(bundled);
        _bundledModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
        _bundledModel->setReadOnly(true);
        _bundledOrder = new LibraryOrder(this);
        _bundledOrder->setSourceModel(_bundledModel);
        _bundledOrder->sort(0);
        _bundledTree = new QTreeView(folders);
        _bundledTree->setObjectName("bundledStampsTree");
        _bundledTree->setModel(_bundledOrder);
        _bundledTree->setRootIndex(_bundledOrder->mapFromSource(_bundledModel->index(bundled)));
        _bundledTree->setHeaderHidden(true);
        for (int column = 1; column < _bundledModel->columnCount(); ++column) {
            _bundledTree->hideColumn(column);
        }
        auto* heading = new QLabel("Came with gecko", folders);
        heading->setToolTip("Each folder is for the game it is named after: a stamp holds that game's "
                            "object numbers, not pictures.");
        foldersLayout->addWidget(heading);
        foldersLayout->addWidget(_bundledTree, 2);
        connect(_bundledTree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) { onBundledFolderSelected(current); });
    }

    auto* splitter = new QSplitter(this);
    splitter->addWidget(folders);
    splitter->addWidget(_grid);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    auto* importButton = new QPushButton("Import…", this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(importButton, QDialogButtonBox::ActionRole);

    _gameNote = new QLabel(this);
    _gameNote->setWordWrap(true);
    _gameNote->setVisible(false);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(_gameNote);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);

    connect(_folderTree->selectionModel(), &QItemSelectionModel::currentChanged,
        this, [this](const QModelIndex& current, const QModelIndex&) { onFolderSelected(current); });
    connect(_grid, &QListWidget::itemActivated, this, &PatternBrowserDialog::onPatternActivated);
    connect(importButton, &QPushButton::clicked, this, &PatternBrowserDialog::onImport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Open on the user's own patterns. The tree lists folders only, so the root itself can't be
    // picked; selecting Custom also stops the tree defaulting to its first (library) folder.
    const QModelIndex custom = _folderOrder->mapFromSource(_folderModel->index(pattern::PatternLibrary::customDir()));
    if (custom.isValid()) {
        _folderTree->setCurrentIndex(custom); // currentChanged fills the grid
        _folderTree->scrollTo(custom);
    } else {
        populateGrid(root, &_resources);
    }
}

void PatternBrowserDialog::setOtherGames(StampGamesProvider games, GameResourcesProvider resources) {
    _stampGames = std::move(games);
    _gameResources = std::move(resources);
    _games.reset();
}

std::optional<PatternBrowserDialog::StampGame> PatternBrowserDialog::bundledGame(const QString& folder) {
    const QString bundled = pattern::PatternLibrary::bundledDir();
    if (!_stampGames || bundled.isEmpty()) {
        return std::nullopt;
    }
    const QString below = QDir(bundled).relativeFilePath(QFileInfo(folder).canonicalFilePath());
    const QString top = below.section('/', 0, 0);
    if (!_games) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        _games = _stampGames();
        QApplication::restoreOverrideCursor();
    }
    if (const auto found = _games->find(top); found != _games->end()) {
        return found->second;
    }
    return std::nullopt;
}

PatternBrowserDialog::~PatternBrowserDialog() = default;

void PatternBrowserDialog::onFolderSelected(const QModelIndex& index) {
    if (index.isValid()) {
        if (_bundledTree) {
            const QSignalBlocker block(_bundledTree->selectionModel());
            _bundledTree->clearSelection();
            _bundledTree->setCurrentIndex({});
        }
        _currentGame.reset();
        _gameNote->setVisible(false);
        populateGrid(_folderModel->filePath(_folderOrder->mapToSource(index)), &_resources);
    }
}

void PatternBrowserDialog::onBundledFolderSelected(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const QSignalBlocker block(_folderTree->selectionModel());
    _folderTree->clearSelection();
    _folderTree->setCurrentIndex({});
    const QString folder = _bundledModel->filePath(_bundledOrder->mapToSource(index));
    _currentGame = bundledGame(folder);
    resource::GameResources* resources = &_resources;
    if (_currentGame) {
        // Another game's numbers: drawn with that game's own data, or not at all.
        resources = _currentGame->available && _gameResources ? _gameResources(_currentGame->id, _currentGame->label) : nullptr;
        _gameNote->setText(resources
                ? QString("Stamps from %1, drawn with its own data. Picking one copies it into your stamps (Custom > "
                          "%2) with its pieces brought into your mod, so it stamps the same in your game.")
                      .arg(_currentGame->label, convertedFolder(_currentGame->label))
                : QString("Stamps from %1, which is in another game's numbers: %2.")
                      .arg(_currentGame->label, _currentGame->available ? QString("it couldn't be read") : _currentGame->why));
        _gameNote->setVisible(true);
    } else {
        _gameNote->setVisible(false);
    }
    populateGrid(folder, resources);
}

void PatternBrowserDialog::populateGrid(const QString& folder, resource::GameResources* resources) {
    _currentFolder = folder;
    _grid->clear();

    const QDir dir(folder);
    QStringList files = dir.entryList({ QStringLiteral("*.json") }, QDir::Files);
    std::sort(files.begin(), files.end(), libraryLess);
    for (const QString& file : files) {
        const QString path = dir.filePath(file);
        const auto pattern = loadPattern(path);
        if (!pattern.has_value()) {
            continue; // skip files that are not valid patterns
        }

        auto* item = new QListWidgetItem(QFileInfo(file).completeBaseName(), _grid);
        item->setData(PATH_ROLE, path);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (resources == nullptr) {
            continue; // listed by name only: its numbers would draw the wrong pieces here
        }
        // The thumbnail cache is keyed by file: another game's data draws another picture.
        const QPixmap thumbnail = pattern::PatternThumbnail::forPattern(
            *pattern, path, *resources, *_hexgrid, THUMBNAIL_SIZE);
        if (!thumbnail.isNull()) {
            item->setIcon(QIcon(thumbnail));
        }
    }
}

void PatternBrowserDialog::onPatternActivated(QListWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(PATH_ROLE).toString();
    auto pattern = loadPattern(path);
    if (!pattern.has_value()) {
        return;
    }
    if (_currentGame && !_currentGame->available) {
        QtDialogs::showWarning(this, "Pattern Library",
            QString("This stamp is in %1's numbers, and %1 isn't set up here (%2), so it would stamp the wrong pieces.")
                .arg(_currentGame->label, _currentGame->why));
        return;
    }
    _selected = std::move(pattern);
    _selectedPath = path;
    _selectedGame = _currentGame ? _currentGame->id : QString();
    accept();
}

void PatternBrowserDialog::onImport() {
    const QString source = QFileDialog::getOpenFileName(
        this, "Import Pattern", QString(), "Gecko Pattern (*.json)");
    if (source.isEmpty()) {
        return;
    }
    // Reject files that don't parse as patterns before copying them in.
    if (!loadPattern(source).has_value()) {
        return;
    }

    // The stamps that came with gecko are read where they are, never written to: an import while
    // one of their folders is open goes to the user's own Custom folder.
    const QString bundled = pattern::PatternLibrary::bundledDir();
    if (!bundled.isEmpty() && QFileInfo(_currentFolder).canonicalFilePath().startsWith(bundled, Qt::CaseInsensitive)) {
        _folderTree->setCurrentIndex(_folderOrder->mapFromSource(_folderModel->index(pattern::PatternLibrary::customDir())));
    }
    const QString target = QDir(_currentFolder).filePath(QFileInfo(source).fileName());
    if (QFileInfo(source) != QFileInfo(target)) {
        QFile::remove(target);
        QFile::copy(source, target);
    }
    populateGrid(_currentFolder, &_resources);
}

} // namespace geck
