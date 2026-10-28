// Push-mode QAudioSink for the SPU's stereo int16 stream. Opened at the SPU rate when the device accepts it,
// otherwise at the device's preferred rate with a linear resampler. write() blocks while the buffer is full, which
// is what paces the emulation thread (as ionview's SDL queue did).
#pragma once
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

class QAudioSink;
class QIODevice;

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    bool open(double sourceRate, int bufferMs);   // call on the thread that will write
    void close();
    bool ok() const { return m_dev != nullptr; }
    void write(const int16_t *stereo, size_t frames);
    void suspend();
    void resume();
    QString description() const { return m_desc; }

private:
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_dev = nullptr;
    double m_srcRate = 0, m_ratio = 1, m_pos = 0;
    int m_rate = 0;
    bool m_resample = false;
    int16_t m_prev[2] = {0, 0};
    std::vector<int16_t> m_buf;
    QString m_desc;
};
