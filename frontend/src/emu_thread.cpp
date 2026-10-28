#include "emu_thread.h"
#include "app.h"
#include "audio_output.h"
#include "camera_source.h"
#include "game_list.h"
#include "ioncore/ioncore.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// Composite-video stand-in: horizontal [1 2 1]/4 smear per channel; the art's dithered screen-door transparency was
// designed to blend on a TV. Same as ionview's.
void composite(const uint32_t *src, uint32_t *dst) {
    for (int y = 0; y < ion::SCREEN_H; y++) {
        const uint32_t *s = src + y * ion::SCREEN_W;
        uint32_t *d = dst + y * ion::SCREEN_W;
        for (int x = 0; x < ion::SCREEN_W; x++) {
            const uint32_t a = s[x > 0 ? x - 1 : 0], b = s[x], c = s[x < ion::SCREEN_W - 1 ? x + 1 : x];
            uint32_t out = 0;
            for (int sh = 0; sh <= 16; sh += 8)
                out |= ((((a >> sh) & 0xff) + 2 * ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + 2) >> 2) << sh;
            d[x] = out;
        }
    }
}

std::vector<uint8_t> readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.readAll();
    return {b.begin(), b.end()};
}

bool writeFileAtomic(const QString &path, const std::vector<uint8_t> &data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(reinterpret_cast<const char *>(data.data()), qint64(data.size()));
    return f.commit();
}

}  // namespace

EmuThread::EmuThread(QObject *parent) : QThread(parent) {}
EmuThread::~EmuThread() { stopAndWait(); }

void EmuThread::startDisc(const QString &path, int entry) {
    stopAndWait();
    m_path = path;
    m_entry = entry;
    m_quit = false; m_paused = false; m_reset = false;
    m_saveSlot = 0; m_loadSlot = 0;
    m_buttons = 0; m_fastForward = false; m_mouseDown = false;
    {
        QMutexLocker lock(&m_mutex);
        m_framePending = false;
        m_title.clear(); m_saveName.clear();
    }
    start();
}

void EmuThread::setPaused(bool paused) {
    m_paused = paused;
    m_cond.wakeAll();
}

void EmuThread::requestReset() { m_reset = true; m_cond.wakeAll(); }
void EmuThread::requestStop() { m_quit = true; m_cond.wakeAll(); }

void EmuThread::stopAndWait() {
    if (!isRunning()) return;
    requestStop();
    wait();
}

bool EmuThread::takeFrame(QImage &out) {
    QMutexLocker lock(&m_mutex);
    if (!m_framePending) return false;
    out = m_frame;
    m_framePending = false;
    return true;
}

void EmuThread::moveHand(int x, int y) {
    m_handX = x; m_handY = y;
    m_handSerial++;
}

bool EmuThread::overlayData(std::vector<float> &heat, std::vector<uint8_t> &cells, int &cols, int &rows) const {
    QMutexLocker lock(&m_mutex);
    if (!m_overlayValid) return false;
    heat = m_heat; cells = m_cells; cols = m_cols; rows = m_rows;
    return true;
}

void EmuThread::setPlayerLook(const ion::PlayerLook &look) {
    QMutexLocker lock(&m_mutex);
    m_playerLook = look;
    m_playerLookDirty = true;
}

QString EmuThread::title() const { QMutexLocker lock(&m_mutex); return m_title; }
QString EmuThread::saveName() const { QMutexLocker lock(&m_mutex); return m_saveName; }

void EmuThread::run() {
    ion::Options opt;
    ion::Console con(opt);
    if (!con.open(m_path.toStdString())) {
        emit failed(QString("Cannot open %1:\n%2").arg(m_path, QString::fromStdString(con.open_error())));
        return;
    }
    QString title = QString::fromStdString(con.title());
    if (title.isEmpty()) title = QFileInfo(m_path).completeBaseName();
    GameEntry ge;
    ge.path = m_path; ge.title = title; ge.sha1 = QString::fromStdString(con.disc_sha1());
    const QString saveName = ge.saveName();
    {
        QMutexLocker lock(&m_mutex);
        m_title = title;
        m_saveName = saveName;
    }
    // Save flash (in-game progress): one file per disc, written whenever the game changes it.
    const QString flashPath = app::savesDir() + "/" + saveName + ".flash";
    if (!con.load_flash(readFile(flashPath))) emit message("Ignoring an unreadable save file: " + flashPath);
    auto persistFlash = [&] {
        if (!con.flash_dirty()) return;
        if (writeFileAtomic(flashPath, con.flash_image())) con.clear_flash_dirty();
        else emit message("Could not write the save file " + flashPath);
    };
    if (!con.power_on(m_entry)) {
        emit failed(QString("Cannot boot entry %1 of %2").arg(m_entry).arg(m_path));
        return;
    }
    AudioOutput audio;
    audio.open(ion::AUDIO_RATE, m_bufferMs);
    emit started(title);

    std::vector<uint32_t> px(size_t(ion::SCREEN_W) * ion::SCREEN_H), shown(px.size());
    ion::Input &in = con.input();
    uint32_t lastSerial = m_handSerial;
    int64_t lastMove = -1000;
    QElapsedTimer clock, statClock;
    clock.start(); statClock.start();
    int loops = 0, moving = -1;
    qint64 nextFrameNs = clock.nsecsElapsed();
    quint64 camSerial = 0;
    QImage camFrame;
    std::vector<uint16_t> readWords;
    std::vector<float> heat(600, 0.f);
    std::vector<uint8_t> cells;
    int cols = 40, rows = 30;
    bool overlayWas = false;

    while (!m_quit) {
        QCoreApplication::processEvents();          // the audio sink's own events live on this thread
        if (m_reset.exchange(false)) {
            persistFlash();
            con.power_on(m_entry);
            lastMove = -1000;
            emit message("Reset");
        }
        if (const int slot = m_saveSlot.exchange(0)) {
            const auto st = con.save_state();
            const QString path = app::statesDir() + QString("/%1.slot%2.state").arg(saveName).arg(slot);
            emit message(writeFileAtomic(path, st) ? QString("Saved state %1").arg(slot) : "Could not write " + path);
        }
        if (const int slot = m_loadSlot.exchange(0)) {
            const QString path = app::statesDir() + QString("/%1.slot%2.state").arg(saveName).arg(slot);
            emit message(con.load_state(readFile(path)) ? QString("Loaded state %1").arg(slot) : QString("No usable save state in slot %1").arg(slot));
            lastMove = -1000;
        }
        if (m_paused) {
            audio.suspend();
            QMutexLocker lock(&m_mutex);
            while (m_paused && !m_quit && !m_reset) m_cond.wait(&m_mutex, 50);
            lock.unlock();
            audio.resume();
            nextFrameNs = clock.nsecsElapsed();
            continue;
        }
        if (con.stopped()) {                         // fw12: the game exited to the system (the Open button)
            emit message("The game exited to the system");
            break;
        }
        // live options
        ion::Options &o = con.options();
        const bool camOn = m_cameraOn && m_cameraSource;
        o.camera_mode = camOn ? 2 : 1;
        o.camera_zoom = m_zoomCenti / 100.f;
        o.motion_sensitivity = m_sensitivityCenti / 100.f;
        o.adpcm36_model = m_adpcm36;
        {
            QMutexLocker lock(&m_mutex);
            if (m_playerLookDirty) { o.player = m_playerLook; m_playerLookDirty = false; }
        }
        const bool overlay = m_overlay || m_overlayConsumers > 0;
        o.trace_motion_reads = overlay;
        if (overlay != overlayWas) { std::fill(heat.begin(), heat.end(), 0.f); overlayWas = overlay; }
        if (camOn && m_cameraSource->latestFrame(camFrame, camSerial))
            con.set_camera_frame(reinterpret_cast<const uint32_t *>(camFrame.constBits()), camFrame.width(), camFrame.height(), m_mirror);
        // input
        const uint32_t serial = m_handSerial;
        if (serial != lastSerial) { lastSerial = serial; lastMove = int64_t(con.frame()); }
        in.hand_x = std::clamp(int(m_handX), 0, ion::SCREEN_W - 1);
        in.hand_y = std::clamp(int(m_handY), 0, ion::SCREEN_H - 1);
        in.hand_active = m_mouseDown || int64_t(con.frame()) - lastMove < 8;
        in.buttons = m_buttons;
        const bool ff = m_fastForward || m_ffToggle;
        for (int k = 0; k < (ff ? 8 : 1); k++) con.step_frame();
        // audio: master gain (+12 dB default), then the sink paces us
        auto &samples = con.audio();
        if (audio.ok() && !ff) {
            if (m_mute) std::fill(samples.begin(), samples.end(), int16_t(0));
            else if (const int gq = int(std::lround(std::pow(10.0, m_gainCentiDb / 2000.0) * 4096.0)); gq != 4096)
                for (auto &v : samples) v = int16_t(std::clamp((int(v) * gq) >> 12, -32768, 32767));
            audio.write(samples.data(), samples.size() / 2);
        }
        samples.clear();
        // video
        con.render(px.data());
        const int filter = m_filter;
        std::vector<uint32_t> &out = filter == FilterComposite ? (composite(px.data(), shown.data()), shown) : px;
        for (uint32_t &v : out) v |= 0xff000000u;      // Format_RGB32 needs 0xff on top; the core writes 0x00RRGGBB
        {
            QImage img(reinterpret_cast<const uchar *>(out.data()), ion::SCREEN_W, ion::SCREEN_H, ion::SCREEN_W * 4, QImage::Format_RGB32);
            QMutexLocker lock(&m_mutex);
            m_frame = img.copy();
            const bool wasPending = m_framePending;
            m_framePending = true;
            lock.unlock();
            if (!wasPending) emit frameReady();
        }
        if (overlay) {                                 // what the game's detector sees and where the game polls
            moving = con.motion_cells(cells, cols, rows);
            if (moving < 0) cells.clear();                 // detector off: no red cells
            con.motion_reads(readWords);
            for (int i = 0; i < 600; i++) heat[i] = std::max(heat[i] * 0.96f, readWords[i] ? 1.f : 0.f);   // fades over ~1 s so bursts of polling stay visible
            QMutexLocker lock(&m_mutex);
            m_heat = heat; m_cells = cells; m_cols = cols; m_rows = rows; m_overlayValid = true;
        } else if (moving != -1 || overlayWas) {
            moving = -1;
            QMutexLocker lock(&m_mutex);
            m_overlayValid = false;
        }
        if (m_screenshot.exchange(false)) {
            const QString dir = app::dataDir() + "/screenshots";
            QDir().mkpath(dir);
            const QString path = dir + QString("/%1 %2.png").arg(saveName).arg(con.frame(), 6, 10, QChar('0'));
            for (uint32_t &v : px) v |= 0xff000000u;
            QImage shot(reinterpret_cast<const uchar *>(px.data()), ion::SCREEN_W, ion::SCREEN_H, ion::SCREEN_W * 4, QImage::Format_RGB32);
            emit message(shot.save(path) ? "Screenshot saved: " + path : "Could not save " + path);
        }
        if (m_logEvents) for (const std::string &e : con.events()) fprintf(stderr, "%s\n", e.c_str());
        con.events().clear();
        if (con.frame() % 300 == 0) persistFlash();
        // pacing without audio: 60 Hz wall clock
        if (!audio.ok() && !ff) {
            nextFrameNs += 16666667;
            const qint64 now = clock.nsecsElapsed();
            if (nextFrameNs > now) QThread::usleep(uint((nextFrameNs - now) / 1000));
            else if (now - nextFrameNs > 200000000) nextFrameNs = now;   // fell far behind: don't try to catch up
        }
        loops++;
        if (statClock.elapsed() >= 1000) {
            emit stats(loops * 1000.0 / statClock.elapsed(), "Audio: " + audio.description(), moving);
            loops = 0;
            statClock.restart();
        }
    }
    persistFlash();
    audio.close();
}
