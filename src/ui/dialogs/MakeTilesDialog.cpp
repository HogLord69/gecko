#include "MakeTilesDialog.h"

#include "ui/QtDialogs.h"
#include "ui/assets/ModToolsRunner.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QPainterPath>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace geck {

using namespace ui::constants;

namespace {

    constexpr int SOURCE_W = 320;
    constexpr int SOURCE_H = 320;
    constexpr int PREVIEW_DELAY_MS = 500;

    const QColor PANE_BACKGROUND(40, 36, 30);
    const QColor USED(240, 150, 40);

    /// image_tiles.turned_side: the largest upright square inside a square of `side` turned by `degrees`.
    double turnedSide(double side, int degrees) {
        const double r = degrees * std::numbers::pi / 180.0;
        return side / (std::abs(std::cos(r)) + std::abs(std::sin(r)));
    }

} // namespace

MakeTilesDialog::MakeTilesDialog(ModToolsRunner* runner, QWidget* parent)
    : QDialog(parent)
    , _runner(runner)
    , _previewPath(QDir(QDir::tempPath()).filePath(
          QString("gecko-make-tiles-%1.png").arg(QCoreApplication::applicationPid()))) {
    setWindowTitle("Make Tiles from Picture");
    setModal(true);
    setupUI();

    _previewTimer = new QTimer(this);
    _previewTimer->setSingleShot(true);
    _previewTimer->setInterval(PREVIEW_DELAY_MS);
    connect(_previewTimer, &QTimer::timeout, this, &MakeTilesDialog::runPreview);

    drawSourceView();
    updateCreateButton();
}

void MakeTilesDialog::setupUI() {
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(SPACING_LOOSE, SPACING_LOOSE, SPACING_LOOSE, SPACING_LOOSE);
    main->setSpacing(SPACING_LOOSE);

    auto* pictureRow = new QHBoxLayout();
    pictureRow->addWidget(new QLabel("Picture:"));
    _pictureEdit = new QLineEdit();
    _pictureEdit->setReadOnly(true);
    _pictureEdit->setPlaceholderText("A surface seen from straight above: dirt, floorboards, a tiled floor, shingles, a rug");
    pictureRow->addWidget(_pictureEdit, 1);
    auto* chooseButton = new QPushButton("Choose...");
    connect(chooseButton, &QPushButton::clicked, this, &MakeTilesDialog::choosePicture);
    pictureRow->addWidget(chooseButton);
    main->addLayout(pictureRow);

    auto* panes = new QHBoxLayout();
    panes->setSpacing(SPACING_LOOSE);

    auto* sourceBox = new QGroupBox("The picture");
    auto* sourceLayout = new QVBoxLayout(sourceBox);
    _sourceLabel = new QLabel();
    _sourceLabel->setFixedSize(SOURCE_W, SOURCE_H);
    sourceLayout->addWidget(_sourceLabel);
    auto* hint = new QLabel("The picture as it is turned. The square in the middle (outlined) is used, laid flat on "
                            "the ground: its top edge runs up-right on the screen and its left edge down-right.");
    hint->setWordWrap(true);
    hint->setMaximumWidth(SOURCE_W);
    sourceLayout->addWidget(hint);
    sourceLayout->addStretch();
    panes->addWidget(sourceBox);

    auto* previewBox = new QGroupBox("In the game, beside the player, and each tile's square");
    auto* previewLayout = new QVBoxLayout(previewBox);
    auto* scroll = new QScrollArea();
    scroll->setMinimumSize(440, SOURCE_H);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setStyleSheet(QString("QScrollArea { background: %1; }").arg(PANE_BACKGROUND.name()));
    _previewLabel = new QLabel("Choose a picture to see it here.");
    _previewLabel->setAlignment(Qt::AlignCenter);
    _previewLabel->setWordWrap(true);
    scroll->setWidget(_previewLabel);
    scroll->setWidgetResizable(true);
    previewLayout->addWidget(scroll, 1);
    auto* zoomRow = new QHBoxLayout();
    zoomRow->addStretch();
    zoomRow->addWidget(new QLabel("Zoom:"));
    _zoomCombo = new QComboBox();
    _zoomCombo->addItems({ "1x", "2x", "3x" });
    connect(_zoomCombo, &QComboBox::currentIndexChanged, this, &MakeTilesDialog::drawPreview);
    zoomRow->addWidget(_zoomCombo);
    previewLayout->addLayout(zoomRow);
    panes->addWidget(previewBox, 1);
    main->addLayout(panes, 1);

    auto* form = new QFormLayout();
    form->setSpacing(SPACING_NORMAL);

    auto* namesRow = new QHBoxLayout();
    _prefixEdit = new QLineEdit();
    _prefixEdit->setObjectName("prefixEdit");
    _prefixEdit->setMaxLength(6);
    _prefixEdit->setValidator(new QRegularExpressionValidator(QRegularExpression("[a-z0-9_]{0,6}"), _prefixEdit));
    _prefixEdit->setPlaceholderText("up to 6 letters");
    _prefixEdit->setToolTip("The tiles are named this plus 01, 02, ...: search for it in the tile palette");
    _prefixEdit->setMaximumWidth(130);
    namesRow->addWidget(_prefixEdit);
    namesRow->addWidget(new QLabel("Stamp name:"));
    _stampEdit = new QLineEdit();
    _stampEdit->setObjectName("stampEdit");
    _stampEdit->setPlaceholderText("as it shows in Edit > Stamp Pattern > Custom (empty: no stamp)");
    namesRow->addWidget(_stampEdit, 1);
    form->addRow("Tile names:", namesRow);

    _problemLabel = new QLabel();
    _problemLabel->setStyleSheet(ui::theme::styles::statusError());
    _problemLabel->setWordWrap(true);
    form->addRow(QString(), _problemLabel);

    auto* shapeRow = new QHBoxLayout();
    _blockCombo = new QComboBox();
    _blockCombo->setObjectName("blockCombo");
    _blockCombo->addItem("1 x 1 square (1 tile)", 1);
    _blockCombo->addItem("2 x 2 squares (4 tiles)", 2);
    _blockCombo->addItem("3 x 3 squares (9 tiles)", 3);
    _blockCombo->addItem("4 x 4 squares (16 tiles)", 4);
    _blockCombo->setCurrentIndex(1);
    _blockCombo->setToolTip("How much ground the picture covers. Its tiles fit together, and copies of the block "
                            "meet without a seam where the picture repeats. Each tile takes one spare tile slot.");
    shapeRow->addWidget(_blockCombo);
    shapeRow->addWidget(new QLabel("Layer:"));
    _layerCombo = new QComboBox();
    _layerCombo->setObjectName("layerCombo");
    _layerCombo->addItem("Floor", "floor");
    _layerCombo->addItem("Roof", "roof");
    _layerCombo->setToolTip("A tile works on either layer. This sets the colours it is held to, the preview (a roof is "
                            "drawn 96 pixels up, over everything) and the layer the stamp fills.");
    shapeRow->addWidget(_layerCombo);
    shapeRow->addWidget(new QLabel("Turn:"));
    _turnSpin = new QSpinBox();
    _turnSpin->setObjectName("turnSpin");
    _turnSpin->setRange(-360, 360);
    _turnSpin->setSingleStep(15);
    _turnSpin->setSuffix(QString::fromUtf8("°"));
    _turnSpin->setToolTip("Degrees clockwise: type any number (a minus number turns it left), or step by 15 with the "
                          "arrows. 90, 180 and 270 keep the whole square; any other turn uses the largest square "
                          "inside the turned picture, and the picture no longer repeats by itself.");
    shapeRow->addWidget(_turnSpin);
    shapeRow->addStretch();
    form->addRow("The picture covers:", shapeRow);

    auto* optionsRow = new QHBoxLayout();
    _repeatCheck = new QCheckBox("Make the edges repeat");
    _repeatCheck->setObjectName("repeatCheck");
    _repeatCheck->setToolTip("For a picture that doesn't repeat by itself: its edges are blended so the block meets "
                             "its next copy without a seam.");
    optionsRow->addWidget(_repeatCheck);
    _evenCheck = new QCheckBox("Even out the light");
    _evenCheck->setObjectName("evenCheck");
    _evenCheck->setToolTip("For a photo lit more on one side, which would show as bands where the block repeats.");
    optionsRow->addWidget(_evenCheck);
    _falloutColoursCheck = new QCheckBox("Fallout's brightness and colour");
    _falloutColoursCheck->setObjectName("falloutColoursCheck");
    _falloutColoursCheck->setChecked(true);
    _falloutColoursCheck->setToolTip("Never brighter or more colourful than the game's own floor (or roof) tiles. "
                                     "Off: the picture's own brightness and colour.");
    optionsRow->addWidget(_falloutColoursCheck);
    optionsRow->addStretch();
    form->addRow("Options:", optionsRow);
    main->addLayout(form);

    _infoLabel = new QLabel();
    _infoLabel->setWordWrap(true);
    _infoLabel->setStyleSheet(ui::theme::styles::statusInfo());
    main->addWidget(_infoLabel);

    auto* note = new QLabel("The tiles go into the game with Tools > Apply New Art. Then paint them from the tile "
                            "palette (search for their name), or lay a patch of the block with Edit > Stamp Pattern > "
                            "Custom: each patch snaps to the block's grid, so patches meet.");
    note->setWordWrap(true);
    main->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    _createButton = buttons->addButton("Make Tiles", QDialogButtonBox::ActionRole);
    _createButton->setDefault(true);
    connect(_createButton, &QPushButton::clicked, this, &MakeTilesDialog::create);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    main->addWidget(buttons);

    for (QComboBox* combo : { _blockCombo, _layerCombo }) {
        connect(combo, &QComboBox::currentIndexChanged, this, &MakeTilesDialog::schedulePreview);
    }
    connect(_turnSpin, &QSpinBox::valueChanged, this, [this]() {
        drawSourceView();
        schedulePreview();
    });
    for (QCheckBox* check : { _repeatCheck, _evenCheck, _falloutColoursCheck }) {
        connect(check, &QCheckBox::toggled, this, &MakeTilesDialog::schedulePreview);
    }
    for (QLineEdit* edit : { _prefixEdit, _stampEdit }) {
        connect(edit, &QLineEdit::textChanged, this, [this]() {
            _problemLabel->clear();
            updateCreateButton();
            schedulePreview(); // the preview checks the names
        });
    }

    resize(940, 760);
}

void MakeTilesDialog::choosePicture() {
    const QString path = QtDialogs::openFile(this, "Choose a Picture of a Surface",
        "Pictures (*.png *.webp *.jpg *.jpeg *.bmp *.tga *.tif *.tiff);;All Files (*.*)");
    if (!path.isEmpty()) {
        loadPicture(path);
    }
}

void MakeTilesDialog::loadPicture(const QString& path) {
    QImage image(path);
    if (image.isNull()) {
        QtDialogs::showWarning(this, "Make Tiles", QString("Couldn't read this picture:\n%1").arg(path));
        return;
    }
    _picturePath = path;
    _picture = image;
    _previewOk = false;
    _pictureEdit->setText(QDir::toNativeSeparators(path));
    if (_stampEdit->text().isEmpty()) {
        _stampEdit->setText(QFileInfo(path).completeBaseName());
    }
    drawSourceView();
    updateCreateButton();
    schedulePreview();
}

void MakeTilesDialog::drawSourceView() {
    QPixmap canvas(SOURCE_W, SOURCE_H);
    canvas.fill(PANE_BACKGROUND);
    if (_picture.isNull()) {
        _sourceLabel->setPixmap(canvas);
        return;
    }
    // The picture turned as image_tiles.py turns it, scaled so all of it fits the pane.
    const int degrees = _turnSpin->value();
    const double r = degrees * std::numbers::pi / 180.0;
    const double c = std::abs(std::cos(r));
    const double s = std::abs(std::sin(r));
    const double w = _picture.width();
    const double h = _picture.height();
    const double scale = std::min({ SOURCE_W / (w * c + h * s), SOURCE_H / (w * s + h * c), 4.0 });
    const QPointF centre(SOURCE_W / 2.0, SOURCE_H / 2.0);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.save();
    painter.translate(centre);
    painter.rotate(degrees); // clockwise on the screen, as --rotate
    painter.scale(scale, scale);
    painter.drawImage(QRectF(-w / 2, -h / 2, w, h), _picture);
    painter.restore();

    // What image_tiles.py uses: the middle square, or the largest upright square inside it once
    // turned by an angle that isn't a quarter. The rest is dimmed.
    const double side = turnedSide(std::min(w, h) * scale, degrees % 90 == 0 ? 0 : degrees);
    const QRectF used(centre.x() - side / 2, centre.y() - side / 2, side, side);
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(QRectF(canvas.rect()));
    outside.addRect(used);
    painter.fillPath(outside, QColor(0, 0, 0, 150));
    painter.setPen(QPen(USED, 1.5, Qt::DashLine));
    painter.drawRect(used.adjusted(0.5, 0.5, -0.5, -0.5));
    painter.end();
    _sourceLabel->setPixmap(canvas);
}

QStringList MakeTilesDialog::tileArguments() const {
    QStringList args{ _picturePath, "--block", QString::number(_blockCombo->currentData().toInt()), "--rotate",
        QString::number(_turnSpin->value()) };
    if (_layerCombo->currentData().toString() == "roof") {
        args << "--roof";
    }
    if (_repeatCheck->isChecked()) {
        args << "--repeat-edges";
    }
    if (_evenCheck->isChecked()) {
        args << "--even-light";
    }
    if (!_falloutColoursCheck->isChecked()) {
        args << "--own-colours";
    }
    if (!_prefixEdit->text().isEmpty()) {
        args << "--prefix" << _prefixEdit->text();
    }
    if (!_stampEdit->text().trimmed().isEmpty()) {
        args << "--stamp" << _stampEdit->text().trimmed();
    }
    return args;
}

void MakeTilesDialog::schedulePreview() {
    if (!_picture.isNull()) {
        _previewTimer->start();
    }
}

void MakeTilesDialog::runPreview() {
    if (_picture.isNull()) {
        return;
    }
    if (_previewRunning) {
        _previewDirty = true;
        return;
    }
    _previewRunning = true;
    _previewDirty = false;
    _infoLabel->setStyleSheet(ui::theme::styles::statusInfo());
    _infoLabel->setText("Making the preview...");
    _runner->run(QStringList{ "tiles-preview" } + tileArguments() + QStringList{ "--out", _previewPath },
        [this](const ModToolResult& result) {
            _previewRunning = false;
            if (_previewDirty) {
                runPreview();
                return;
            }
            showPreview(result);
        });
}

void MakeTilesDialog::showPreview(const ModToolResult& result) {
    _previewOk = result.ok;
    if (!result.ok) {
        _previewImage = QImage();
        _previewLabel->setPixmap(QPixmap());
        _previewLabel->setText("No preview.");
        _infoLabel->setStyleSheet(ui::theme::styles::statusError());
        _infoLabel->setText(result.error);
        updateCreateButton();
        return;
    }
    _previewImage = QImage(result.json.value("preview").toString()); // QImage: no pixmap cache to go stale
    drawPreview();

    const int tiles = result.json.value("tiles").toInt();
    _infoLabel->setStyleSheet(ui::theme::styles::statusInfo());
    QString info = QString("%1 %2 tile%3; %4 spare tile slots are left (each new tile takes one).")
                       .arg(tiles)
                       .arg(_layerCombo->currentData().toString())
                       .arg(tiles == 1 ? "" : "s")
                       .arg(result.json.value("spare_tile_slots").toInt());
    if (_turnSpin->value() % 90 != 0 && !_repeatCheck->isChecked()) {
        info += " Turned by an angle, the picture no longer repeats by itself: Make the edges repeat joins the "
                "blocks without a seam.";
    }
    _infoLabel->setText(info);

    QStringList problems;
    for (const char* key : { "name_problem", "slots_problem", "stamp_problem" }) {
        const QString problem = result.json.value(key).toString();
        if (!problem.isEmpty()) {
            problems << problem;
        }
    }
    _problemLabel->setText(problems.join(' '));
    updateCreateButton();
}

void MakeTilesDialog::drawPreview() {
    if (_previewImage.isNull()) {
        return;
    }
    const int zoom = _zoomCombo->currentIndex() + 1;
    const QImage shown = _previewImage.scaled(_previewImage.size() * zoom, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    _previewLabel->setText(QString());
    _previewLabel->setPixmap(QPixmap::fromImage(shown));
}

void MakeTilesDialog::updateCreateButton() {
    const bool ready = !_creating && !_picture.isNull() && _previewOk && !_prefixEdit->text().isEmpty()
        && _problemLabel->text().isEmpty();
    _createButton->setEnabled(ready);
    _createButton->setToolTip(!_picture.isNull() && !_creating && _prefixEdit->text().isEmpty()
            ? QString("Still needed: the tile names")
            : QString());
}

void MakeTilesDialog::create() {
    if (_creating) {
        return;
    }
    _creating = true;
    updateCreateButton();
    _infoLabel->setStyleSheet(ui::theme::styles::statusInfo());
    _infoLabel->setText("Making the tiles...");
    _runner->run(QStringList{ "tiles-create" } + tileArguments(), [this](const ModToolResult& result) {
        _creating = false;
        if (!result.ok) {
            _infoLabel->setStyleSheet(ui::theme::styles::statusError());
            _infoLabel->setText("Not made.");
            updateCreateButton();
            QtDialogs::showWarning(this, "Make Tiles", result.error);
            return;
        }
        const QJsonArray made = result.json.value("made").toArray();
        _tilesMade += static_cast<int>(made.size());
        const QString first = made.isEmpty() ? QString() : made.first().toObject().value("name").toString();
        const QString last = made.isEmpty() ? QString() : made.last().toObject().value("name").toString();
        const QString names = first == last ? first : QString("%1 to %2").arg(first, last);
        const QString stamp = QFileInfo(result.json.value("stamp").toString()).completeBaseName();
        const QString stampText = stamp.isEmpty()
            ? QString()
            : QString(", and the stamp \"%1\" (Edit > Stamp Pattern > Custom)").arg(stamp);
        _infoLabel->setStyleSheet(ui::theme::styles::statusSuccess());
        _infoLabel->setText(QString("Made %1 tile%2 (%3)%4. Tools > Apply New Art puts them in the game.")
                                .arg(made.size())
                                .arg(made.size() == 1 ? "" : "s")
                                .arg(names, stampText));
        _prefixEdit->clear();
        _stampEdit->clear();
        updateCreateButton();
    });
}

} // namespace geck
