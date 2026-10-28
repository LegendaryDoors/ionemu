#include "audio_output.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <algorithm>
#include <cmath>

AudioOutput::AudioOutput() = default;
AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open(double sourceRate, int bufferMs) {
    close();
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) { m_desc = "no audio device"; return false; }
    QAudioFormat fmt;
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);
    fmt.setSampleRate(int(std::lround(sourceRate)));
    m_resample = !dev.isFormatSupported(fmt);
    if (m_resample) {
        int rate = dev.preferredFormat().sampleRate();
        if (rate < 8000 || rate > 192000) rate = 48000;
        fmt.setSampleRate(rate);
        if (!dev.isFormatSupported(fmt)) { m_desc = "format unsupported"; return false; }
    }
    m_srcRate = sourceRate;
    m_rate = fmt.sampleRate();
    m_ratio = m_srcRate / m_rate;
    m_pos = 0;
    m_prev[0] = m_prev[1] = 0;
    m_sink = std::make_unique<QAudioSink>(dev, fmt);
    m_sink->setBufferSize(int(m_rate * 4 * bufferMs / 1000));
    m_dev = m_sink->start();
    if (!m_dev) { m_desc = "start failed"; m_sink.reset(); return false; }
    m_desc = QString("%1 Hz%2").arg(m_rate).arg(m_resample ? " (resampled)" : "");
    return true;
}

void AudioOutput::close() {
    if (m_sink) { m_sink->stop(); m_sink.reset(); }
    m_dev = nullptr;
}

void AudioOutput::suspend() { if (m_sink) m_sink->suspend(); }
void AudioOutput::resume() { if (m_sink) m_sink->resume(); }

void AudioOutput::write(const int16_t *stereo, size_t frames) {
    if (!m_dev || frames == 0) return;
    const int16_t *data = stereo;
    size_t n = frames;
    if (m_resample) {
        // Linear interpolation between consecutive source frames; m_pos is the next output's position in source
        // frames, where the previous call's last frame sits at 0 and stereo[i] at i + 1.
        m_buf.clear();
        while (m_pos < double(frames)) {
            const size_t i = size_t(m_pos);
            const double t = m_pos - double(i);
            const int16_t *a = i == 0 ? m_prev : stereo + 2 * (i - 1), *b = stereo + 2 * i;
            m_buf.push_back(int16_t(std::lround(a[0] + (b[0] - a[0]) * t)));
            m_buf.push_back(int16_t(std::lround(a[1] + (b[1] - a[1]) * t)));
            m_pos += m_ratio;
        }
        m_pos -= double(frames);
        m_prev[0] = stereo[2 * frames - 2];
        m_prev[1] = stereo[2 * frames - 1];
        data = m_buf.data();
        n = m_buf.size() / 2;
    }
    const char *p = reinterpret_cast<const char *>(data);
    qint64 remaining = qint64(n) * 4;
    int stalls = 0;
    while (remaining > 0) {
        const qint64 w = m_dev->write(p, remaining);
        if (w > 0) { p += w; remaining -= w; stalls = 0; continue; }
        if (w < 0 || m_sink->state() == QAudio::StoppedState || ++stalls > 2000) {   // device gone: stop pacing on it
            close();
            m_desc = "device lost";
            return;
        }
        QThread::usleep(1000);
    }
}
