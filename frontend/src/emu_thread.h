// The emulation thread: owns the ion::Console, steps it at 60 Hz paced by the audio sink, hands frames to the
// display through a mutex, and takes input from atomics written by the GUI thread.
#pragma once
#include <QImage>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include "ioncore/ioncore.h"
#include <cmath>
#include <atomic>
#include <cstdint>
#include <vector>

class CameraSource;

class EmuThread : public QThread {
    Q_OBJECT
public:
    enum Filter { FilterComposite = 0, FilterSharp = 1, FilterSmooth = 2 };

    explicit EmuThread(QObject *parent = nullptr);
    ~EmuThread() override;

    void startDisc(const QString &path, int entry);   // starts the thread; started() or failed() follows
    void setPaused(bool paused);
    void requestReset();
    void requestStop();
    void stopAndWait();

    bool isPaused() const { return m_paused; }
    bool takeFrame(QImage &out);                        // the newest frame, once

    // input and settings (GUI thread)
    void moveHand(int x, int y);
    void setMouseDown(bool down) { m_mouseDown = down; }
    void setButtons(uint16_t mask) { m_buttons = mask; }
    void setFastForward(bool on) { m_fastForward = on; }          // held key
    void setFastForwardToggle(bool on) { m_ffToggle = on; }       // toolbar / menu toggle
    void setGainDb(double db) { m_gainCentiDb = int(std::lround(db * 100)); }
    double gainDb() const { return m_gainCentiDb / 100.0; }
    void setMute(bool mute) { m_mute = mute; }
    bool mute() const { return m_mute; }
    void setFilter(int filter) { m_filter = filter; }
    int filter() const { return m_filter; }
    void requestSaveState(int slot) { m_saveSlot = slot; }
    void requestLoadState(int slot) { m_loadSlot = slot; }
    void requestScreenshot() { m_screenshot = true; }
    void setAudioBufferMs(int ms) { m_bufferMs = ms; }            // applies at the next start
    void setAdpcm36Model(int model) { m_adpcm36 = model; }
    void setLogEvents(bool on) { m_logEvents = on; }
    // camera (frames come from the CameraSource on the GUI thread; the loop takes the newest each iteration)
    void setCameraSource(CameraSource *src) { m_cameraSource = src; }
    void setCameraEnabled(bool on) { m_cameraOn = on; }
    bool cameraEnabled() const { return m_cameraOn; }
    void setMirror(bool on) { m_mirror = on; }
    void setZoom(double z) { m_zoomCenti = int(std::lround(z * 100)); }
    void setSensitivity(double s) { m_sensitivityCenti = int(std::lround(s * 100)); }
    // motion overlay: blue = map words the game polled recently (600 words, decaying), red = flagged cells
    void setOverlay(bool on) { m_overlay = on; }
    void setPlayerLook(const ion::PlayerLook &look);
    void addOverlayConsumer(int delta) { m_overlayConsumers += delta; }   // e.g. the camera page preview
    bool overlayData(std::vector<float> &heat, std::vector<uint8_t> &cells, int &cols, int &rows) const;

    QString title() const;
    QString saveName() const;

signals:
    void started(const QString &title);
    void failed(const QString &error);
    void frameReady();
    void stats(double fps, const QString &audio, int movingCells);   // movingCells: -1 = detector off
    void message(const QString &text);

protected:
    void run() override;

private:
    QString m_path;
    int m_entry = 1;
    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    QImage m_frame;
    bool m_framePending = false;
    QString m_title, m_saveName;

    std::atomic<bool> m_paused{false}, m_quit{false}, m_reset{false};
    std::atomic<int> m_handX{160}, m_handY{120};
    std::atomic<uint32_t> m_handSerial{0};
    std::atomic<bool> m_mouseDown{false}, m_fastForward{false}, m_ffToggle{false}, m_mute{false};
    std::atomic<uint16_t> m_buttons{0};
    std::atomic<int> m_gainCentiDb{1200}, m_filter{FilterComposite};
    std::atomic<int> m_saveSlot{0}, m_loadSlot{0};
    std::atomic<bool> m_screenshot{false}, m_logEvents{false}, m_cameraOn{false}, m_mirror{true}, m_overlay{false};
    std::atomic<int> m_bufferMs{80}, m_adpcm36{3}, m_zoomCenti{100}, m_sensitivityCenti{100}, m_overlayConsumers{0};
    CameraSource *m_cameraSource = nullptr;
    ion::PlayerLook m_playerLook;                 // under m_mutex
    bool m_playerLookDirty = true;
    // overlay data (under m_mutex)
    std::vector<float> m_heat;
    std::vector<uint8_t> m_cells;
    int m_cols = 40, m_rows = 30;
    bool m_overlayValid = false;
};
