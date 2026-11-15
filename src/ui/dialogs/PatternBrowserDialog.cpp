#include "ui/dialogs/PatternBrowserDialog.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
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
    constexpr int FOLDER_ROLE = Qt::UserRole + 2; ///< a subfolder's tile: the folder it opens

    /// The stamp library's own layout (resources/stamps/README.md): these top folders are in Fallout
    /// 2's numbers, the map's own; every other one was cut from another game.
    const QStringList& ownNumbersFolders() {
        static const QStringList folders{ QStringLiteral("Fallout 2"), QStringLiteral("HogLord Props") };
        return folders;
    }

    bool holdsStamps(const QString& folder) {
        return QDirIterator(folder, { QStringLiteral("*.json") }, QDir::Files, QDirIterator::Subdirectories).hasNext();
    }

    /// The stamp library's order: by name, with numbers read as numbers ("#3" before "#13").
    bool libraryLess(const QString& left, const QString& right) {
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

    // Someone with no stamps of their own yet opens on Fallout 2's that came with gecko, its folders
    // showing, rather than on an empty Custom folder.
    if (_bundledTree && !holdsStamps(root)) {
        const QModelIndex start = _bundledOrder->mapFromSource(
            _bundledModel->index(QDir(pattern::PatternLibrary::bundledDir()).filePath(ownNumbersFolders().front())));
        if (start.isValid()) {
            _bundledTree->expand(start);
            _bundledTree->setCurrentIndex(start); // currentChanged fills the grid with its folders
        }
    }
}

void PatternBrowserDialog::setOtherGames(StampGamesProvider games, GameResourcesProvider resources) {
    _stampGames = std::move(games);
    _gameResources = std::move(resources);
    _games.reset();
}

std::optional<PatternBrowserDialog::StampGame> PatternBrowserDialog::bundledGame(const QString& folder) {
    const QString bundled = pattern::PatternLibrary::bundledDir();
    if (bundled.isEmpty()) {
        return std::nullopt;
    }
    const QString below = QDir(bundled).relativeFilePath(QFileInfo(folder).canonicalFilePath());
    const QString top = below.section('/', 0, 0);
    if (top.isEmpty() || top == "." || top == ".." || ownNumbersFolders().contains(top, Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    if (_stampGames && !_games) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        _games = _stampGames();
        QApplication::restoreOverrideCursor();
    }
    if (_games && !_games->empty()) {
        if (const auto found = _games->find(top); found != _games->end()) {
            return found->second;
        }
        return std::nullopt;
    }
    // No answer from the mod tools (there are none, or Python isn't there to run them). Drawn with
    // this game's data these stamps would show, and stamp, the wrong pieces.
    return StampGame{ QString(), top, false,
        "the map workflow, which converts them, isn't set up here (it needs Python 3.10 or newer: run "
        "python setup.py in the workflow folder beside gecko.exe)" };
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
    // Subfolders first, as tiles: the stamps of a game's folder are a level or two further down
    // (Fallout 2 > Buildings > Adobe), and a folder with none of its own must not look empty.
    QStringList subfolders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(subfolders.begin(), subfolders.end(), libraryLess);
    const QIcon folderIcon = QFileIconProvider().icon(QFileIconProvider::Folder);
    for (const QString& name : subfolders) {
        auto* item = new QListWidgetItem(folderIcon, name, _grid);
        item->setData(FOLDER_ROLE, dir.filePath(name));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip("Double-click to open this folder");
    }

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
    if (const QString folder = item->data(FOLDER_ROLE).toString(); !folder.isEmpty()) {
        openFolder(folder);
        return;
    }
    const QString path = item->data(PATH_ROLE).toString();
    auto pattern = loadPattern(path);
    if (!pattern.has_value()) {
        return;
    }
    QString game = _currentGame ? _currentGame->id : QString();
    if (_currentGame && !_currentGame->available) {
        // Right as it is only for someone editing that game itself; in any other it can't be converted.
        if (!QtDialogs::showQuestion(this, "Pattern Library",
                QString("This stamp is in %1's numbers, and %1 isn't set up here: %2.\n\n"
                        "Stamp it as it is? That is only right while the game you are editing is %1 itself; "
                        "in any other game it places the wrong pieces.")
                    .arg(_currentGame->label, _currentGame->why))) {
            return;
        }
        game.clear(); // as it is: nothing to convert
    }
    _selected = std::move(pattern);
    _selectedPath = path;
    _selectedGame = game;
    accept();
}

void PatternBrowserDialog::openFolder(const QString& path) {
    const QString bundled = pattern::PatternLibrary::bundledDir();
    const bool came = _bundledTree && !bundled.isEmpty()
        && QFileInfo(path).canonicalFilePath().startsWith(bundled + '/', Qt::CaseInsensitive);
    QTreeView* tree = came ? _bundledTree : _folderTree;
    QFileSystemModel* files = came ? _bundledModel : _folderModel;
    QSortFilterProxyModel* order = came ? _bundledOrder : _folderOrder;
    const QModelIndex index = order->mapFromSource(files->index(path));
    if (!index.isValid()) {
        populateGrid(path, came ? nullptr : &_resources);
        return;
    }
    tree->expand(index.parent());
    tree->setCurrentIndex(index); // currentChanged fills the grid
    tree->scrollTo(index);
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
