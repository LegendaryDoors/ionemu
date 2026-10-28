// The emulation view: paints the console picture letterboxed at 4:3, turns the mouse into the virtual hand,
// maps keys to console buttons, and hosts the Esc pause menu (Resume / Reset / Stop / Fullscreen).
#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QKeySequence>
#include <QWidget>
#include <cstdint>
#include <vector>

class EmuThread;
class QPushButton;

class PauseMenu : public QWidget {
    Q_OBJECT
public:
    explicit PauseMenu(QWidget *parent);
    void setFullscreenLabel(bool fullscreen);
signals:
    void resumeClicked();
    void resetClicked();
    void stopClicked();
    void fullscreenClicked();
protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void showEvent(QShowEvent *) override;
private:
    QPushButton *m_resume, *m_reset, *m_stop, *m_fullscreen;
};

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(EmuThread *emu, QWidget *parent = nullptr);
    PauseMenu *pauseMenu() const { return m_menu; }
    void setMenuVisible(bool visible);
    bool menuVisible() const;
    void setShowHand(bool show);
    void setCameraOn(bool on) { m_cameraOn = on; updateCursor(); update(); }
    void setIntegerScaling(bool on) { m_integerScaling = on; update(); }
    void setOverlay(bool on) { m_overlay = on; update(); }
    void setBindings(const QHash<QString, QKeySequence> &keys);   // console buttons + fast-forward
    void releaseInput();
    QRect videoRect() const;   // where the 320x240 picture lands

signals:
    void escapePressed();
    void fullscreenRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    bool focusNextPrevChild(bool next) override;   // Tab is the fast-forward key, except inside the pause menu
    bool event(QEvent *e) override;                 // console bindings win over single-key QAction shortcuts

private:
    void onFrameReady();
    void handFromMouse(const QPointF &pos);
    uint16_t buttonForKey(int key) const;
    void updateCursor();

    EmuThread *m_emu;
    PauseMenu *m_menu;
    QImage m_frame;
    QPoint m_hand{160, 120};
    QElapsedTimer m_handMoved;
    bool m_mouseDown = false, m_showHand = true, m_cameraOn = false, m_integerScaling = false, m_overlay = false;
    uint16_t m_buttons = 0;
    QHash<int, uint16_t> m_keyButtons;
    int m_ffKey = Qt::Key_Tab;
    std::vector<float> m_heat;
    std::vector<uint8_t> m_cells;
    int m_cols = 40, m_rows = 30;
};
