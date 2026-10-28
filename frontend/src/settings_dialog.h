// Settings dialog, PCSX2-style: pages on the left, the page on the right, OK / Cancel / Apply. Edits a copy of the
// Settings; the main window applies it. The Camera page shows the live preview and doubles as the pre-game setup.
#pragma once
#include "settings.h"
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPointF>
#include <QMouseEvent>
#include <QTimer>
#include <QHash>
#include <QPushButton>

class CameraPreview;
class CameraSource;
class EmuThread;
class KeyButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QRadioButton;
class QSlider;
class QSpinBox;
class QStackedWidget;

class AspectLabel : public QLabel {
    Q_OBJECT
public:
    explicit AspectLabel(QWidget *parent = nullptr) : QLabel(parent) {
        setMinimumSize(320, 240);
        setAlignment(Qt::AlignCenter);
        QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
        setMouseTracking(true);
    }
    void setSource(const QImage &img);
    QSize sizeHint() const override { return {480, 360}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * 3 / 4; }
signals:
    void handMoved(QPointF norm, bool pressed);   // the pointer over the picture, 0..1 in both axes
    void handLeft();
protected:
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    void rescale();
    void report(const QPointF &pos, bool pressed);
    QRect pictureRect() const;
    QImage m_source;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum Page { General = 0, Graphics, Audio, Camera, Player, Controls, Advanced };
    SettingsDialog(const Settings &current, EmuThread *emu, CameraSource *camera, QWidget *parent = nullptr);
    void showPage(int page);
    Settings settings() const { return m_edit; }
    void syncCameraPreview();                      // start the combo's device for the live preview if it is not running

signals:
    void applied(const Settings &s);
    void cameraChangeRequested(const QString &deviceId);   // live: start/stop the shared camera for the preview

private:
    QWidget *buildGeneral();
    QWidget *buildGraphics();
    QWidget *buildAudio();
    QWidget *buildCamera();
    QWidget *buildPlayer();
    void refreshPlayerPreview();
    QWidget *buildControls();
    QWidget *buildAdvanced();
    void load();
    void store();
    void refreshCameraInfo();

    Settings m_edit;
    EmuThread *m_emu;
    CameraSource *m_camera;
    QListWidget *m_pageList = nullptr;
    QStackedWidget *m_pages = nullptr;
    // general
    QListWidget *m_folders = nullptr;
    QCheckBox *m_startFullscreen = nullptr, *m_pauseOnFocusLoss = nullptr, *m_checkUpdates = nullptr;
    // graphics
    QRadioButton *m_filterButtons[3] = {};
    QCheckBox *m_integerScaling = nullptr, *m_showHand = nullptr;
    // audio
    QSlider *m_gain = nullptr;
    QLabel *m_gainLabel = nullptr;
    QCheckBox *m_mute = nullptr;
    QSpinBox *m_bufferMs = nullptr;
    // camera
    QComboBox *m_cameraCombo = nullptr;
    CameraPreview *m_preview = nullptr;
    QSlider *m_zoom = nullptr, *m_sensitivity = nullptr;
    QLabel *m_zoomLabel = nullptr, *m_sensitivityLabel = nullptr, *m_cameraInfo = nullptr;
    QCheckBox *m_mirror = nullptr;
    // player
    class ColourButton *m_skin = nullptr, *m_hair = nullptr, *m_shirt = nullptr, *m_wall = nullptr, *m_floor = nullptr;
    QComboBox *m_hairStyle = nullptr, *m_playerStyle = nullptr;
    QCheckBox *m_window = nullptr;
    AspectLabel *m_playerPreview = nullptr;
    int m_handX = 210, m_handY = 96;                       // the stand-in's raised hand (320x240 coordinates)
    bool m_handActive = true;                              // moving or held: lit; still: dimmed, as the games see it
    ion::MotionPreview m_playerMotion;                     // the console's detector on the preview frames
    QTimer *m_handSettle = nullptr;
    void renderPlayer(bool lookChanged);
public:
    void devHand(int x, int y);                            // development: move the hand as the mouse would
    // controls
    QHash<QString, KeyButton *> m_keyButtons;
    // advanced
    QCheckBox *m_overlay = nullptr;
};

// A colour swatch button: click to pick.
class ColourButton : public QPushButton {
    Q_OBJECT
public:
    explicit ColourButton(QWidget *parent = nullptr);
    void setColour(const QColor &c);
    QColor colour() const { return m_colour; }
signals:
    void colourChanged(const QColor &c);
private:
    QColor m_colour;
};

// A button that captures the next key press as its binding (Esc cancels, Delete/Backspace clear when empty).
class KeyButton : public QPushButton {
    Q_OBJECT
public:
    explicit KeyButton(QWidget *parent = nullptr);
    void setKey(const QKeySequence &k);
    QKeySequence key() const { return m_key; }
signals:
    void keyChanged(const QKeySequence &k);
protected:
    bool event(QEvent *e) override;                // Tab/Backtab reach us while capturing
    void keyPressEvent(QKeyEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
private:
    void refresh();
    QKeySequence m_key;
    bool m_capturing = false;
};
