#include "settings_dialog.h"
#include <algorithm>
#include <QPainter>
#include "camera_preview.h"
#include "camera_source.h"
#include "emu_thread.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

// ---- ColourButton ---------------------------------------------------------------------------------------------

ColourButton::ColourButton(QWidget *parent) : QPushButton(parent) {
    setMinimumWidth(110);
    connect(this, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_colour, this, "Pick a colour");
        if (c.isValid()) { setColour(c); emit colourChanged(c); }
    });
}

void ColourButton::setColour(const QColor &c) {
    m_colour = c;
    setText(c.name());
    setStyleSheet(QString("QPushButton { background: %1; color: %2; }").arg(c.name(), c.lightness() > 128 ? "#000" : "#fff"));
}

// ---- KeyButton ------------------------------------------------------------------------------------------------

KeyButton::KeyButton(QWidget *parent) : QPushButton(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(140);
    connect(this, &QPushButton::clicked, this, [this] { m_capturing = true; refresh(); setFocus(); });
    refresh();
}

void KeyButton::setKey(const QKeySequence &k) { m_key = k; m_capturing = false; refresh(); }

void KeyButton::refresh() {
    setText(m_capturing ? "Press a key..." : m_key.isEmpty() ? "(none)" : m_key.toString(QKeySequence::NativeText));
    setDown(m_capturing);
}

bool KeyButton::event(QEvent *e) {
    if (m_capturing && e->type() == QEvent::KeyPress) {
        const int k = static_cast<QKeyEvent *>(e)->key();
        if (k == Qt::Key_Tab || k == Qt::Key_Backtab) { keyPressEvent(static_cast<QKeyEvent *>(e)); return true; }
    }
    return QPushButton::event(e);
}

void KeyButton::keyPressEvent(QKeyEvent *e) {
    if (!m_capturing) { QPushButton::keyPressEvent(e); return; }
    const int k = e->key();
    if (k == Qt::Key_Escape) { m_capturing = false; refresh(); return; }
    if (k == Qt::Key_Control || k == Qt::Key_Shift || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_unknown) return;
    m_key = QKeySequence(k == Qt::Key_Backtab ? int(Qt::Key_Tab) : k);   // plain keys only: the console keys are held
    m_capturing = false;
    refresh();
    emit keyChanged(m_key);
}

void KeyButton::focusOutEvent(QFocusEvent *e) {
    if (m_capturing) { m_capturing = false; refresh(); }
    QPushButton::focusOutEvent(e);
}

// ---- SettingsDialog -------------------------------------------------------------------------------------------

SettingsDialog::SettingsDialog(const Settings &current, EmuThread *emu, CameraSource *camera, QWidget *parent)
    : QDialog(parent), m_edit(current), m_emu(emu), m_camera(camera) {
    setWindowTitle("Settings");
    resize(900, 620);
    auto *h = new QHBoxLayout;
    m_pageList = new QListWidget(this);
    m_pageList->setFixedWidth(150);
    m_pageList->setSpacing(2);
    m_pages = new QStackedWidget(this);
    struct { const char *name; QWidget *(SettingsDialog::*build)(); } pages[] = {
        {"General", &SettingsDialog::buildGeneral}, {"Graphics", &SettingsDialog::buildGraphics}, {"Audio", &SettingsDialog::buildAudio},
        {"Camera", &SettingsDialog::buildCamera}, {"Player", &SettingsDialog::buildPlayer}, {"Controls", &SettingsDialog::buildControls},
        {"Advanced", &SettingsDialog::buildAdvanced}};
    for (auto &pg : pages) {
        auto *item = new QListWidgetItem(pg.name, m_pageList);
        item->setSizeHint({0, 34});
        m_pages->addWidget((this->*pg.build)());
    }
    connect(m_pageList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    h->addWidget(m_pageList);
    h->addWidget(m_pages, 1);
    auto *v = new QVBoxLayout(this);
    v->addLayout(h, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    v->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this] { store(); emit applied(m_edit); accept(); });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(box->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { store(); emit applied(m_edit); });
    load();
    m_pageList->setCurrentRow(0);
    auto *tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &SettingsDialog::refreshCameraInfo);
    tick->start(500);
}

void SettingsDialog::showPage(int page) { m_pageList->setCurrentRow(page); }

void SettingsDialog::syncCameraPreview() {
    const QString want = m_cameraCombo->currentData().toString();
    if (want != m_camera->deviceId() || (!want.isEmpty() && !m_camera->active())) emit cameraChangeRequested(want);
}

static QLabel *note(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet("color: palette(placeholder-text);");
    return l;
}

QWidget *SettingsDialog::buildGeneral() {
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    auto *folders = new QGroupBox("Disc folders", w);
    auto *fv = new QVBoxLayout(folders);
    fv->addWidget(note("Folders with your disc images (.cue with .bin tracks, or .mdx), subfolders included. Extract .zip and .7z archives first.", folders));
    auto *fh = new QHBoxLayout;
    m_folders = new QListWidget(folders);
    fh->addWidget(m_folders, 1);
    auto *fb = new QVBoxLayout;
    auto *add = new QPushButton("Add...", folders);
    auto *remove = new QPushButton("Remove", folders);
    fb->addWidget(add); fb->addWidget(remove); fb->addStretch(1);
    fh->addLayout(fb);
    fv->addLayout(fh);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, "Add Disc Folder");
        if (!dir.isEmpty() && m_folders->findItems(dir, Qt::MatchExactly).isEmpty()) m_folders->addItem(dir);
    });
    connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(m_folders->selectedItems()); });
    v->addWidget(folders, 1);
    auto *behaviour = new QGroupBox("Behaviour", w);
    auto *bv = new QVBoxLayout(behaviour);
    m_startFullscreen = new QCheckBox("Start games in fullscreen", behaviour);
    m_pauseOnFocusLoss = new QCheckBox("Pause when the window loses focus", behaviour);
    m_checkUpdates = new QCheckBox("Check for updates once a day", behaviour);
    bv->addWidget(m_startFullscreen); bv->addWidget(m_pauseOnFocusLoss); bv->addWidget(m_checkUpdates);
    v->addWidget(behaviour);
    return w;
}

QWidget *SettingsDialog::buildGraphics() {
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    auto *filter = new QGroupBox("Video filter", w);
    auto *fv = new QVBoxLayout(filter);
    const char *names[3] = {"Composite (default)", "Sharp", "Smooth"};
    for (int i = 0; i < 3; i++) { m_filterButtons[i] = new QRadioButton(names[i], filter); fv->addWidget(m_filterButtons[i]); }
    fv->addWidget(note("Composite looks like the console on a TV, dithered translucency included.", filter));
    v->addWidget(filter);
    auto *scaling = new QGroupBox("Picture", w);
    auto *sv = new QVBoxLayout(scaling);
    m_integerScaling = new QCheckBox("Integer scaling", scaling);
    m_showHand = new QCheckBox("Show the hand marker", scaling);
    sv->addWidget(m_integerScaling); sv->addWidget(m_showHand);
    v->addWidget(scaling);
    v->addStretch(1);
    return w;
}

QWidget *SettingsDialog::buildAudio() {
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    auto *g = new QGroupBox("Volume", w);
    auto *f = new QFormLayout(g);
    auto *gh = new QHBoxLayout;
    m_gain = new QSlider(Qt::Horizontal, g);
    m_gain->setRange(-30, 30);
    m_gainLabel = new QLabel(g);
    m_gainLabel->setMinimumWidth(60);
    gh->addWidget(m_gain, 1); gh->addWidget(m_gainLabel);
    f->addRow("Master gain", gh);
    connect(m_gain, &QSlider::valueChanged, this, [this](int v) { m_gainLabel->setText(QString("%1%2 dB").arg(v >= 0 ? "+" : "").arg(v)); });
    m_mute = new QCheckBox("Mute", g);
    f->addRow("", m_mute);
    f->addRow("", note("+12 dB matches the real console's loudness.", g));
    v->addWidget(g);
    auto *lat = new QGroupBox("Latency", w);
    auto *lf = new QFormLayout(lat);
    m_bufferMs = new QSpinBox(lat);
    m_bufferMs->setRange(40, 250);
    m_bufferMs->setSuffix(" ms");
    lf->addRow("Audio buffer", m_bufferMs);
    lf->addRow("", note("Lower is more responsive; raise it if the sound crackles. Applies to the next game.", lat));
    v->addWidget(lat);
    v->addStretch(1);
    return w;
}

QWidget *SettingsDialog::buildCamera() {
    auto *w = new QWidget(this);
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;
    m_preview = new CameraPreview(m_camera, m_emu, w);
    left->addWidget(m_preview);              // 4:3 from the column width, at the top
    m_cameraInfo = new QLabel(w);
    left->addWidget(m_cameraInfo);
    left->addStretch(1);
    h->addLayout(left, 3);
    auto *right = new QVBoxLayout;
    auto *dev = new QGroupBox("Camera", w);
    auto *dv = new QVBoxLayout(dev);
    m_cameraCombo = new QComboBox(dev);
    dv->addWidget(m_cameraCombo);
    dv->addWidget(note("Off: the mouse is the hand.", dev));
    right->addWidget(dev);
    auto *pos = new QGroupBox("Position", w);
    auto *pf = new QFormLayout(pos);
    auto *zh = new QHBoxLayout;
    m_zoom = new QSlider(Qt::Horizontal, pos);
    m_zoom->setRange(30, 200);
    m_zoomLabel = new QLabel(pos);
    m_zoomLabel->setMinimumWidth(44);
    zh->addWidget(m_zoom, 1); zh->addWidget(m_zoomLabel);
    pf->addRow("Zoom", zh);
    auto *sh = new QHBoxLayout;
    m_sensitivity = new QSlider(Qt::Horizontal, pos);
    m_sensitivity->setRange(50, 300);
    m_sensitivityLabel = new QLabel(pos);
    m_sensitivityLabel->setMinimumWidth(44);
    sh->addWidget(m_sensitivity, 1); sh->addWidget(m_sensitivityLabel);
    pf->addRow("Sensitivity", sh);
    m_mirror = new QCheckBox("Mirror", pos);
    pf->addRow("", m_mirror);
    right->addWidget(pos);
    auto *guide = new QGroupBox("How to stand", w);
    auto *gv = new QVBoxLayout(guide);
    gv->addWidget(note("Stand back until your head fills the white outline. If you cannot get far enough away, lower the zoom.", guide));
    gv->addWidget(note("Red squares are the movement the console sees: they should follow your hand and vanish when you stand still. "
                       "Raise the sensitivity if small moves are missed, lower it if noise shows red.", guide));
    gv->addWidget(note("Blue squares show where a running game is looking.", guide));
    right->addWidget(guide);
    right->addStretch(1);
    h->addLayout(right, 2);
    connect(m_zoom, &QSlider::valueChanged, this, [this](int v) { m_zoomLabel->setText(QString("%1x").arg(v / 100.0, 0, 'f', 2)); m_preview->setZoom(v / 100.0); });
    connect(m_sensitivity, &QSlider::valueChanged, this, [this](int v) { m_sensitivityLabel->setText(QString("%1x").arg(v / 100.0, 0, 'f', 1)); m_preview->setSensitivity(v / 100.0); });
    connect(m_mirror, &QCheckBox::toggled, this, [this](bool on) { m_preview->setMirror(on); });
    connect(m_cameraCombo, &QComboBox::currentIndexChanged, this, [this](int i) { if (i >= 0) emit cameraChangeRequested(m_cameraCombo->itemData(i).toString()); });
    connect(m_preview, &CameraPreview::motionChanged, this, &SettingsDialog::refreshCameraInfo);
    return w;
}

// A 4:3 picture that follows its column width and rescales its source on resize (the Player page preview).
void AspectLabel::setSource(const QImage &img) { m_source = img; rescale(); }
void AspectLabel::resizeEvent(QResizeEvent *) { rescale(); }
void AspectLabel::rescale() {
    if (m_source.isNull()) return;
    setPixmap(QPixmap::fromImage(m_source).scaled(size().boundedTo(QSize(640, 480)), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
QRect AspectLabel::pictureRect() const {
    const QPixmap pm = pixmap();
    if (pm.isNull()) return rect();
    return {(width() - pm.width()) / 2, (height() - pm.height()) / 2, pm.width(), pm.height()};
}
void AspectLabel::report(const QPointF &pos, bool pressed) {
    const QRect r = pictureRect();
    if (r.width() <= 0 || r.height() <= 0) return;
    emit handMoved({(pos.x() - r.x()) / r.width(), (pos.y() - r.y()) / r.height()}, pressed);
}
void AspectLabel::mouseMoveEvent(QMouseEvent *e) { report(e->position(), e->buttons() & Qt::LeftButton); }
void AspectLabel::mousePressEvent(QMouseEvent *e) { if (e->button() == Qt::LeftButton) report(e->position(), true); }
void AspectLabel::mouseReleaseEvent(QMouseEvent *e) { if (e->button() == Qt::LeftButton) report(e->position(), false); }
void AspectLabel::leaveEvent(QEvent *) { emit handLeft(); }

// The stand-in player shown to the games when no camera is used: a small character creator.
QWidget *SettingsDialog::buildPlayer() {
    auto *w = new QWidget(this);
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;
    m_playerPreview = new AspectLabel(w);    // 4:3 from the column width, at the top, like the camera preview
    left->addWidget(m_playerPreview);
    left->addWidget(note("This is you when no camera is used. Move the mouse over the picture: the hand follows it and the red squares are "
                         "the movement the console sees. A still hand goes dim because the games cannot see it; holding the left button counts as waving.", w));
    left->addStretch(1);
    h->addLayout(left, 3);
    m_handSettle = new QTimer(this);
    m_handSettle->setSingleShot(true);
    m_handSettle->setInterval(250);
    connect(m_handSettle, &QTimer::timeout, this, [this] { m_handActive = false; renderPlayer(false); });
    connect(m_playerPreview, &AspectLabel::handMoved, this, [this](QPointF n, bool pressed) {
        m_handX = std::clamp(int(n.x() * ion::SCREEN_W), 0, ion::SCREEN_W - 1);
        m_handY = std::clamp(int(n.y() * ion::SCREEN_H), 0, ion::SCREEN_H - 1);
        m_handActive = true;
        renderPlayer(false);
        if (pressed) m_handSettle->stop(); else m_handSettle->start();
    });
    connect(m_playerPreview, &AspectLabel::handLeft, this, [this] { m_handSettle->stop(); m_handActive = false; renderPlayer(false); });
    auto *right = new QVBoxLayout;
    auto *look = new QGroupBox("Look", w);
    auto *f = new QFormLayout(look);
    m_skin = new ColourButton(look); m_hair = new ColourButton(look); m_shirt = new ColourButton(look);
    m_wall = new ColourButton(look); m_floor = new ColourButton(look);
    m_hairStyle = new QComboBox(look);
    m_hairStyle->addItems({"Short", "Long", "None"});
    m_playerStyle = new QComboBox(look);
    m_playerStyle->addItems({"Simple", "Detailed"});
    m_window = new QCheckBox("Window", look);
    f->addRow("Style", m_playerStyle);
    f->addRow("Skin", m_skin);
    f->addRow("Hair", m_hair);
    f->addRow("Hair style", m_hairStyle);
    f->addRow("Shirt", m_shirt);
    f->addRow("Wall", m_wall);
    f->addRow("Floor", m_floor);
    f->addRow("", m_window);
    right->addWidget(look);
    auto *reset = new QPushButton("Reset to default", w);
    right->addWidget(reset, 0, Qt::AlignLeft);
    right->addStretch(1);
    h->addLayout(right, 2);
    for (ColourButton *b : {m_skin, m_hair, m_shirt, m_wall, m_floor}) connect(b, &ColourButton::colourChanged, this, &SettingsDialog::refreshPlayerPreview);
    connect(m_hairStyle, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshPlayerPreview);
    connect(m_playerStyle, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshPlayerPreview);
    connect(m_window, &QCheckBox::toggled, this, &SettingsDialog::refreshPlayerPreview);
    connect(reset, &QPushButton::clicked, this, [this] {
        const ion::PlayerLook d;
        auto set = [](ColourButton *b, const float *c) { b->setColour(QColor(int(c[0]), int(c[1]), int(c[2]))); };
        set(m_skin, d.skin); set(m_hair, d.hair); set(m_shirt, d.shirt); set(m_wall, d.wall); set(m_floor, d.floor);
        m_hairStyle->setCurrentIndex(d.hair_style);
        m_playerStyle->setCurrentIndex(d.style);
        m_window->setChecked(d.window);
        refreshPlayerPreview();
    });
    return w;
}

void SettingsDialog::refreshPlayerPreview() { renderPlayer(true); }

void SettingsDialog::devHand(int x, int y) {
    m_handX = std::clamp(x, 0, ion::SCREEN_W - 1); m_handY = std::clamp(y, 0, ion::SCREEN_H - 1); m_handActive = true;
    renderPlayer(false);
}

// lookChanged: a new colour or style changes the whole picture, so the detector forgets the previous frame instead
// of flagging everything red; a hand move keeps it, and the squares show what moved.
void SettingsDialog::renderPlayer(bool lookChanged) {
    ion::PlayerLook lk;
    auto get = [](ColourButton *b, float *c) { const QColor q = b->colour(); c[0] = q.red(); c[1] = q.green(); c[2] = q.blue(); };
    get(m_skin, lk.skin); get(m_hair, lk.hair); get(m_shirt, lk.shirt); get(m_wall, lk.wall); get(m_floor, lk.floor);
    lk.hair_style = m_hairStyle->currentIndex();
    lk.style = m_playerStyle->currentIndex();
    lk.window = m_window->isChecked();
    std::vector<uint32_t> px(size_t(ion::SCREEN_W) * ion::SCREEN_H);
    ion::render_player(lk, m_handX, m_handY, m_handActive, px.data());
    std::vector<uint8_t> cells; int cols = 0, rows = 0;
    if (lookChanged) m_playerMotion.reset();
    m_playerMotion.feed(px.data(), ion::SCREEN_W, ion::SCREEN_H, false, 1.0f, 1.0f, 12, cells, cols, rows);
    for (uint32_t &v : px) v |= 0xff000000u;
    QImage img = QImage(reinterpret_cast<const uchar *>(px.data()), ion::SCREEN_W, ion::SCREEN_H, ion::SCREEN_W * 4, QImage::Format_RGB32).copy();
    if (!lookChanged && cols > 0 && rows > 0 && cells.size() >= size_t(cols) * rows) {   // the detector's squares, as in the Camera page
        QPainter p(&img);
        const double cw = double(ion::SCREEN_W) / cols, ch = double(ion::SCREEN_H) / rows;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                if (cells[size_t(r) * cols + c]) p.fillRect(QRectF(c * cw, r * ch, cw, ch), QColor(255, 40, 40, 120));
    }
    m_playerPreview->setSource(img);
}

QWidget *SettingsDialog::buildControls() {
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    v->addWidget(note("Click a binding and press a key. Plain keys only; Esc always opens the pause menu.", w));
    QString group;
    QFormLayout *form = nullptr;
    for (const Settings::Action &a : Settings::actions()) {
        if (a.group != group) {
            group = a.group;
            auto *g = new QGroupBox(group, w);
            form = new QFormLayout(g);
            v->addWidget(g);
        }
        auto *b = new KeyButton(w);
        m_keyButtons.insert(a.id, b);
        form->addRow(a.label, b);
    }
    auto *defaults = new QPushButton("Reset to defaults", w);
    connect(defaults, &QPushButton::clicked, this, [this] {
        const auto d = Settings::defaultKeys();
        for (auto it = m_keyButtons.begin(); it != m_keyButtons.end(); ++it) it.value()->setKey(d.value(it.key()));
    });
    v->addWidget(defaults, 0, Qt::AlignLeft);
    v->addStretch(1);
    return w;
}

QWidget *SettingsDialog::buildAdvanced() {
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    auto *dbg = new QGroupBox("Overlay", w);
    auto *bv = new QVBoxLayout(dbg);
    m_overlay = new QCheckBox("Motion overlay in the game view (blue: where the game looks, red: movement)", dbg);
    bv->addWidget(m_overlay);   // the core event log (advanced/log_events in the ini) is a developer aid, not a setting
    v->addWidget(dbg);
    v->addStretch(1);
    return w;
}

void SettingsDialog::load() {
    const Settings &s = m_edit;
    m_folders->clear();
    m_folders->addItems(s.discFolders);
    m_startFullscreen->setChecked(s.startFullscreen);
    m_pauseOnFocusLoss->setChecked(s.pauseOnFocusLoss);
    m_checkUpdates->setChecked(s.checkUpdates);
    m_filterButtons[qBound(0, s.filter, 2)]->setChecked(true);
    m_integerScaling->setChecked(s.integerScaling);
    m_showHand->setChecked(s.showHand);
    m_gain->setValue(int(s.gainDb));
    m_gainLabel->setText(QString("%1%2 dB").arg(s.gainDb >= 0 ? "+" : "").arg(int(s.gainDb)));
    m_mute->setChecked(s.mute);
    m_bufferMs->setValue(s.bufferMs);
    {
        QSignalBlocker block(m_cameraCombo);
        m_cameraCombo->clear();
        m_cameraCombo->addItem("Off", QString());
        int sel = 0;
        for (const CameraSource::DeviceInfo &d : CameraSource::devices()) {
            m_cameraCombo->addItem(d.description + (d.isDefault ? " (default)" : ""), d.id);
            if (d.id == s.cameraId) sel = m_cameraCombo->count() - 1;
        }
        if (!s.cameraId.isEmpty() && sel == 0) { m_cameraCombo->addItem("Not connected: " + s.cameraId, s.cameraId); sel = m_cameraCombo->count() - 1; }
        m_cameraCombo->setCurrentIndex(sel);
    }
    m_zoom->setValue(int(s.zoom * 100));
    m_zoomLabel->setText(QString("%1x").arg(s.zoom, 0, 'f', 2));
    m_sensitivity->setValue(int(s.sensitivity * 100));
    m_sensitivityLabel->setText(QString("%1x").arg(s.sensitivity, 0, 'f', 1));
    m_mirror->setChecked(s.mirror);
    m_preview->setZoom(s.zoom); m_preview->setSensitivity(s.sensitivity); m_preview->setMirror(s.mirror);
    for (auto it = m_keyButtons.begin(); it != m_keyButtons.end(); ++it) it.value()->setKey(s.keys.value(it.key()));
    {
        auto set = [](ColourButton *b, const float *c) { b->setColour(QColor(int(c[0]), int(c[1]), int(c[2]))); };
        set(m_skin, s.player.skin); set(m_hair, s.player.hair); set(m_shirt, s.player.shirt); set(m_wall, s.player.wall); set(m_floor, s.player.floor);
        m_hairStyle->setCurrentIndex(qBound(0, s.player.hair_style, 2));
        m_playerStyle->setCurrentIndex(qBound(0, s.player.style, 1));
        m_window->setChecked(s.player.window);
        refreshPlayerPreview();
    }
    m_overlay->setChecked(s.motionOverlay);
    refreshCameraInfo();
}

void SettingsDialog::store() {
    Settings &s = m_edit;
    s.discFolders.clear();
    for (int i = 0; i < m_folders->count(); i++) s.discFolders << m_folders->item(i)->text();
    s.startFullscreen = m_startFullscreen->isChecked();
    s.pauseOnFocusLoss = m_pauseOnFocusLoss->isChecked();
    s.checkUpdates = m_checkUpdates->isChecked();
    for (int i = 0; i < 3; i++) if (m_filterButtons[i]->isChecked()) s.filter = i;
    s.integerScaling = m_integerScaling->isChecked();
    s.showHand = m_showHand->isChecked();
    s.gainDb = m_gain->value();
    s.mute = m_mute->isChecked();
    s.bufferMs = m_bufferMs->value();
    s.cameraId = m_cameraCombo->currentData().toString();
    s.zoom = m_zoom->value() / 100.0;
    s.sensitivity = m_sensitivity->value() / 100.0;
    s.mirror = m_mirror->isChecked();
    for (auto it = m_keyButtons.begin(); it != m_keyButtons.end(); ++it) s.keys.insert(it.key(), it.value()->key());
    {
        auto get = [](ColourButton *b, float *c) { const QColor q = b->colour(); c[0] = q.red(); c[1] = q.green(); c[2] = q.blue(); };
        get(m_skin, s.player.skin); get(m_hair, s.player.hair); get(m_shirt, s.player.shirt); get(m_wall, s.player.wall); get(m_floor, s.player.floor);
        s.player.hair_style = m_hairStyle->currentIndex();
        s.player.style = m_playerStyle->currentIndex();
        s.player.window = m_window->isChecked();
    }
    s.motionOverlay = m_overlay->isChecked();
}

void SettingsDialog::refreshCameraInfo() {
    if (!m_camera->active()) {
        m_cameraInfo->setText(m_cameraCombo->currentData().toString().isEmpty() ? "Camera off." : "Starting the camera... " + m_camera->lastError());
        return;
    }
    QString t = m_camera->formatDescription();
    if (t.isEmpty()) t = "Waiting for the first frame...";
    else {
        const double fps = m_camera->fps();
        t += QString(", %1 fps").arg(fps, 0, 'f', 0);
        if (fps > 0 && fps < 25) t += "  (low: add light; the console expects 30 frames per second)";
        t += QString("  |  %1 squares moving").arg(m_preview->flaggedCells());
    }
    m_cameraInfo->setText(t);
}
