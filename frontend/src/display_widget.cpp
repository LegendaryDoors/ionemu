#include "display_widget.h"
#include "camera_preview.h"
#include "emu_thread.h"
#include "settings.h"
#include "ioncore/ioncore.h"
#include <QGridLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

// ---- PauseMenu ------------------------------------------------------------------------------------------------

PauseMenu::PauseMenu(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setFocusPolicy(Qt::StrongFocus);
    auto *panel = new QWidget(this);
    panel->setObjectName("pausePanel");
    panel->setStyleSheet("#pausePanel { background: rgba(28, 28, 32, 235); border: 1px solid rgba(255,255,255,40); border-radius: 12px; }"
                         "QPushButton { min-width: 220px; min-height: 40px; font-size: 15px; }");
    auto *v = new QVBoxLayout(panel);
    v->setContentsMargins(28, 24, 28, 24);
    v->setSpacing(10);
    m_resume = new QPushButton("Resume", panel);
    m_reset = new QPushButton("Reset", panel);
    m_fullscreen = new QPushButton("Fullscreen", panel);
    m_stop = new QPushButton("Stop", panel);
    for (QPushButton *b : {m_resume, m_reset, m_fullscreen, m_stop}) v->addWidget(b);
    m_resume->setDefault(true);
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(panel, 1, 1, Qt::AlignCenter);
    grid->setRowStretch(0, 1); grid->setRowStretch(2, 1);
    grid->setColumnStretch(0, 1); grid->setColumnStretch(2, 1);
    connect(m_resume, &QPushButton::clicked, this, &PauseMenu::resumeClicked);
    connect(m_reset, &QPushButton::clicked, this, &PauseMenu::resetClicked);
    connect(m_stop, &QPushButton::clicked, this, &PauseMenu::stopClicked);
    connect(m_fullscreen, &QPushButton::clicked, this, &PauseMenu::fullscreenClicked);
}

void PauseMenu::setFullscreenLabel(bool fullscreen) { m_fullscreen->setText(fullscreen ? "Exit Fullscreen" : "Fullscreen"); }

void PauseMenu::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 140));
}

void PauseMenu::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) { emit resumeClicked(); return; }
    e->accept();                                   // nothing leaks into the console input while the menu is up
}

void PauseMenu::showEvent(QShowEvent *) { m_resume->setFocus(); }

// ---- DisplayWidget --------------------------------------------------------------------------------------------

DisplayWidget::DisplayWidget(EmuThread *emu, QWidget *parent) : QWidget(parent), m_emu(emu) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_menu = new PauseMenu(this);
    m_menu->hide();
    m_handMoved.start();
    connect(m_emu, &EmuThread::frameReady, this, &DisplayWidget::onFrameReady);
    setBindings(Settings::defaultKeys());
    updateCursor();
}

void DisplayWidget::setMenuVisible(bool visible) {
    m_menu->setGeometry(rect());
    if (visible) releaseInput();
    m_menu->setVisible(visible);                   // showEvent focuses the Resume button
    if (!visible) setFocus();
    updateCursor();
}

bool DisplayWidget::menuVisible() const { return m_menu->isVisible(); }

void DisplayWidget::setShowHand(bool show) { m_showHand = show; updateCursor(); }

void DisplayWidget::updateCursor() { setCursor(m_showHand && !m_cameraOn && !menuVisible() ? Qt::BlankCursor : Qt::ArrowCursor); }

void DisplayWidget::setBindings(const QHash<QString, QKeySequence> &keys) {
    static const struct { const char *id; uint16_t button; } map[] = {
        {"play_pause", ion::BTN_PLAY_PAUSE}, {"stop", ion::BTN_STOP}, {"next", ion::BTN_NEXT},
        {"previous", ion::BTN_PREVIOUS}, {"brightness", ion::BTN_BRIGHTNESS}, {"open", ion::BTN_OPEN}};
    m_keyButtons.clear();
    auto norm = [](int key) { return key == Qt::Key_Enter ? int(Qt::Key_Return) : key; };   // keypad Enter = Return
    for (const auto &m : map) {
        const QKeySequence k = keys.value(m.id);
        if (!k.isEmpty()) m_keyButtons.insert(norm(k[0].key()), m.button);
    }
    const QKeySequence ff = keys.value("fast_forward");
    m_ffKey = ff.isEmpty() ? -1 : norm(ff[0].key());
    releaseInput();
}

bool DisplayWidget::focusNextPrevChild(bool next) { return menuVisible() ? QWidget::focusNextPrevChild(next) : false; }

bool DisplayWidget::event(QEvent *e) {
    if (e->type() == QEvent::ShortcutOverride && !menuVisible()) {   // a console binding beats a QAction on the same key
        const int key = static_cast<QKeyEvent *>(e)->key();
        const int k = key == Qt::Key_Enter ? int(Qt::Key_Return) : key;
        if (k == m_ffKey || m_keyButtons.contains(k)) { e->accept(); return true; }
    }
    return QWidget::event(e);
}

void DisplayWidget::releaseInput() {
    m_buttons = 0;
    m_mouseDown = false;
    m_emu->setButtons(0);
    m_emu->setMouseDown(false);
    m_emu->setFastForward(false);
}

QRect DisplayWidget::videoRect() const {
    const int w = width(), h = height();
    int tw = w, th = w * 3 / 4;
    if (th > h) { th = h; tw = h * 4 / 3; }
    if (m_integerScaling) {
        const int n = std::max(1, std::min(w / ion::SCREEN_W, h / ion::SCREEN_H));
        tw = n * ion::SCREEN_W; th = n * ion::SCREEN_H;
    }
    return {(w - tw) / 2, (h - th) / 2, tw, th};
}

void DisplayWidget::onFrameReady() {
    if (m_emu->takeFrame(m_frame)) update();
}

void DisplayWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    const QRect target = videoRect();
    if (!m_frame.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_emu->filter() != EmuThread::FilterSharp);
        p.drawImage(target, m_frame);
    }
    if (m_overlay && m_emu->overlayData(m_heat, m_cells, m_cols, m_rows)) {   // blue = polled, red = flagged; no text
        const double hw = target.width() / 20.0, hh = target.height() / 30.0;   // the map is always 600 words: 20 x 30
        for (int i = 0; i < 600 && i < int(m_heat.size()); i++)
            if (m_heat[i] > 0.1f) p.fillRect(QRectF(target.x() + (i % 20) * hw, target.y() + (i / 20) * hh, hw, hh), QColor(40, 120, 255, int(90 * m_heat[i])));
        const double cw = target.width() / double(m_cols), ch = target.height() / double(m_rows);   // red: 8x8 or 16x16 cells
        if (m_cells.size() >= size_t(m_cols) * m_rows)                       // empty while the game's detector is off
            for (int r = 0; r < m_rows; r++)
                for (int c = 0; c < m_cols; c++)
                    if (m_cells[size_t(r) * m_cols + c]) p.fillRect(QRectF(target.x() + c * cw, target.y() + r * ch, cw, ch), QColor(255, 40, 40, 120));
        if (m_cameraOn) CameraPreview::paintOutline(p, target, 0.5);
    }
    if (m_showHand && !m_cameraOn && !menuVisible() && underMouse()) {   // the virtual hand: game layers may hide the player
        const double sx = target.width() / double(ion::SCREEN_W), sy = target.height() / double(ion::SCREEN_H);
        const QPointF c(target.x() + (m_hand.x() + 0.5) * sx, target.y() + (m_hand.y() + 0.5) * sy);
        const bool active = m_mouseDown || m_handMoved.elapsed() < 130;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(active ? QColor(255, 210, 0) : QColor(150, 150, 150), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 10 * sx, 10 * sy);
        p.setBrush(active ? QColor(255, 210, 0) : QColor(150, 150, 150));
        p.drawEllipse(c, 1.5 * sx, 1.5 * sy);
    }
}

void DisplayWidget::resizeEvent(QResizeEvent *) { m_menu->setGeometry(rect()); }

void DisplayWidget::handFromMouse(const QPointF &pos) {
    const QRect t = videoRect();
    const int x = std::clamp(int((pos.x() - t.x()) * ion::SCREEN_W / std::max(1, t.width())), 0, ion::SCREEN_W - 1);
    const int y = std::clamp(int((pos.y() - t.y()) * ion::SCREEN_H / std::max(1, t.height())), 0, ion::SCREEN_H - 1);
    if (x == m_hand.x() && y == m_hand.y()) return;
    m_hand = {x, y};
    m_handMoved.restart();
    m_emu->moveHand(x, y);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent *e) { handFromMouse(e->position()); }

void DisplayWidget::mousePressEvent(QMouseEvent *e) {
    setFocus();
    if (e->button() == Qt::LeftButton) { m_mouseDown = true; m_emu->setMouseDown(true); handFromMouse(e->position()); }
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) { m_mouseDown = false; m_emu->setMouseDown(false); }
}

// Console buttons from the Controls settings; Enter doubles for a Return binding.
uint16_t DisplayWidget::buttonForKey(int key) const {
    if (key == Qt::Key_Enter) key = Qt::Key_Return;
    return m_keyButtons.value(key, 0);
}

void DisplayWidget::keyPressEvent(QKeyEvent *e) {
    if (e->isAutoRepeat() || menuVisible()) return;
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::AltModifier)) { emit fullscreenRequested(); return; }
    switch (e->key()) {
    case Qt::Key_Escape: emit escapePressed(); return;
    default: break;
    }
    if (e->key() == m_ffKey) { m_emu->setFastForward(true); return; }
    if (const uint16_t b = buttonForKey(e->key())) { m_buttons |= b; m_emu->setButtons(m_buttons); return; }
    QWidget::keyPressEvent(e);
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *e) {
    if (e->isAutoRepeat() || menuVisible()) return;
    if (e->key() == m_ffKey) { m_emu->setFastForward(false); return; }
    if (const uint16_t b = buttonForKey(e->key())) { m_buttons &= uint16_t(~b); m_emu->setButtons(m_buttons); return; }
    QWidget::keyReleaseEvent(e);
}

void DisplayWidget::focusOutEvent(QFocusEvent *) { releaseInput(); }
