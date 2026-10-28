// license:BSD-3-Clause
// copyright-holders:Ryan Holtz,Jonathan Gevaryahu (MAME spg2xx_audio, the source of this logic)
//
// Standalone SPG2xx SPU for the iON's SPG220: MAME's spg2xx_audio behaviour (mame_orig/, BSD-3) without the
// MAME framework, extended from 16 to 24 channels. Channel registers 0x3000+16*ch, phase registers
// 0x3200+16*ch, control bank 0 at 0x3400 (channels 0-15) and bank 1 at 0x3500 (channels 16-23, bit n =
// channel 16+n). The ADPCM36 predictor is chosen by ion::Options::adpcm36_model.
#pragma once
#include <cstdint>
#include <functional>

class ion_spu {
public:
    static constexpr int NCH = 24;
    static constexpr double RATE = 281250.0 / 4;       // output sample rate = beat tick rate (70312.5 Hz)

    std::function<uint16_t(uint32_t)> read_mem;         // SPU bus-master reads (22-bit word address)
    std::function<void(int)> irq_cb;                    // beat IRQ line (IRQ4)
    std::function<void(const char *)> log_cb;           // unexpected events (FIQ requests, ...)
    int adpcm36_model = 0;                              // 0 = q6 (a = k/64, default), 1 = MAME's approximation,
                                                        // 2 = linear a = c0 + c1 * idx / 32 (idx = header bits 8:4; tuning)
    float adpcm36_c0 = 0.5f, adpcm36_c1 = 0.5f;         // model 2 (0.5/0.5 reproduces q6)
    int adpcm36_coef_q12[32] = {};                      // model 3: per-index one-tap coefficient (Q12)
    int adpcm36_coef2_q12[32] = {};                     // model 3: per-index second-tap coefficient (Q12)
    int adpcm36_coef3_q12[32] = {};                     // model 3, experimental: third tap (Q12)
    int adpcm36_gain_q12[32] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};   // model 3, experimental: residual gain (Q12)
    int adpcm36_dq_q12[16] = {0, 4096, 8192, 12288, 16384, 20480, 24576, 28672, -32768, -28672, -24576, -20480, -16384, -12288, -8192, -4096};   // model 3: code -> residual (Q12 nibble units)
    bool force_noint = false;                           // experiment: ignore SPU_CONTROL and never interpolate

    ion_spu();
    void reset();
    uint16_t read(uint32_t addr);                       // 0x3000-0x37FF
    void write(uint32_t addr, uint16_t data);
    void generate(int16_t *out, int frames);            // interleaved stereo at RATE; also runs the beat counter
    uint32_t playing_mask() const { return status(0) | (uint32_t)status(1) << 16; }
    uint32_t starts[4] = {};                            // channel starts by format: PCM8, PCM16, IMA ADPCM, ADPCM36

    // Save states: visit every piece of mutable state as (pointer, bytes). Callbacks, options and stats are excluded.
    template <class F> void serialize(F &&f) {
        f(m_regs, sizeof m_regs); f(m_phase, sizeof m_phase); f(m_ctrl, sizeof m_ctrl);
        f(m_sample_shift, sizeof m_sample_shift); f(m_rate, sizeof m_rate); f(m_rate_accum, sizeof m_rate_accum);
        f(m_rampdown_frame, sizeof m_rampdown_frame); f(m_envclk_frame, sizeof m_envclk_frame);
        f(m_envelope_addr, sizeof m_envelope_addr); f(&m_beat_base, sizeof m_beat_base);
        f(m_ima, sizeof m_ima); f(m_a36, sizeof m_a36);
    }

private:
    // per-channel register offsets (0x3000 + 16*ch)
    enum { WAVE_ADDR = 0, MODE = 1, LOOP_ADDR = 2, PAN_VOL = 3, ENVELOPE0 = 4, ENVELOPE_DATA = 5, ENVELOPE1 = 6,
           ENVELOPE_ADDR_HIGH = 7, ENVELOPE_ADDR = 8, WAVE_DATA_PREV = 9, ENVELOPE_LOOP_CTRL = 10, WAVE_DATA = 11,
           ADPCM_SEL = 13 };
    // per-channel phase register offsets (0x3200 + 16*ch)
    enum { PHASE_HIGH = 0, PHASE_ACCUM_HIGH = 1, TARGET_PHASE_HIGH = 2, RAMP_DOWN_CLOCK = 3, PHASE = 4,
           PHASE_ACCUM = 5, TARGET_PHASE = 6, PHASE_CTRL = 7 };
    // control registers (0x3400 bank 0; the per-channel-bit ones repeat at 0x3500 for channels 16-23)
    enum { CHANNEL_ENABLE = 0x00, MAIN_VOLUME = 0x01, CHANNEL_FIQ_ENABLE = 0x02, CHANNEL_FIQ_STATUS = 0x03,
           BEAT_BASE_COUNT = 0x04, BEAT_COUNT = 0x05, ENVCLK0 = 0x06, ENVCLK0_HIGH = 0x07, ENVCLK1 = 0x08,
           ENVCLK1_HIGH = 0x09, ENV_RAMP_DOWN = 0x0a, CHANNEL_STOP = 0x0b, CHANNEL_ZERO_CROSS = 0x0c,
           CONTROL = 0x0d, COMPRESS_CTRL = 0x0e, CHANNEL_STATUS = 0x0f, WAVE_IN_L = 0x10, WAVE_IN_R = 0x11,
           CHANNEL_REPEAT = 0x14, CHANNEL_ENV_MODE = 0x15, CHANNEL_TONE_RELEASE = 0x16, CHANNEL_ENV_IRQ = 0x17 };
    enum { BIS = 0x4000, BIE = 0x8000, BEAT_COUNT_MASK = 0x3fff, CONTROL_NOINT = 0x0200 };

    uint16_t m_regs[0x200] = {}, m_phase[0x200] = {}, m_ctrl[0x400] = {};
    uint8_t m_sample_shift[NCH] = {};
    double m_rate[NCH] = {}, m_rate_accum[NCH] = {};
    uint32_t m_rampdown_frame[NCH] = {}, m_envclk_frame[NCH] = {}, m_envelope_addr[NCH] = {};
    uint16_t m_beat_base = 0;
    struct ima_state { int32_t sig, step; } m_ima[NCH] = {};
    struct a36_state { uint16_t remaining, header; int32_t prev, prev2, prev3; } m_a36[NCH] = {};

    // channel-bit helpers: bank b = ch >> 4, bit = ch & 15
    uint16_t &creg(int reg, int ch) { return m_ctrl[(ch >> 4) * 0x100 + reg]; }
    bool cbit(int reg, int ch) const { return (m_ctrl[(ch >> 4) * 0x100 + reg] >> (ch & 15)) & 1; }
    void setbit(int reg, int ch) { creg(reg, ch) |= 1 << (ch & 15); }
    void clrbit(int reg, int ch) { creg(reg, ch) &= ~(1 << (ch & 15)); }
    uint16_t status(int bank) const { return m_ctrl[bank * 0x100 + CHANNEL_STATUS]; }
    uint16_t &reg(int ch, int r) { return m_regs[(ch << 4) | r]; }
    uint16_t &phr(int ch, int r) { return m_phase[(ch << 4) | r]; }

    uint32_t wave_addr(int ch) { return (uint32_t)(reg(ch, MODE) & 0x3f) << 16 | reg(ch, WAVE_ADDR); }
    uint32_t loop_addr(int ch) { return (uint32_t)((reg(ch, MODE) >> 6) & 0x3f) << 16 | reg(ch, LOOP_ADDR); }
    void set_wave_addr(int ch, uint32_t a) { reg(ch, MODE) = (reg(ch, MODE) & ~0x3f) | ((a >> 16) & 0x3f); reg(ch, WAVE_ADDR) = a & 0xffff; }
    int tone_mode(int ch) { return (reg(ch, MODE) >> 12) & 3; }
    bool adpcm(int ch) { return reg(ch, MODE) & 0x8000; }
    bool pcm16(int ch) { return reg(ch, MODE) & 0x4000; }
    bool adpcm36(int ch) { return reg(ch, ADPCM_SEL) & 0x8000; }
    int edd(int ch) { return reg(ch, ENVELOPE_DATA) & 0x7f; }
    void set_edd(int ch, int v) { reg(ch, ENVELOPE_DATA) = (reg(ch, ENVELOPE_DATA) & ~0x7f) | (v & 0x7f); }
    int env_count(int ch) { return reg(ch, ENVELOPE_DATA) >> 8; }
    void set_env_count(int ch, int c) { reg(ch, ENVELOPE_DATA) = (reg(ch, ENVELOPE_DATA) & 0x7f) | (c << 8); }
    int env_load(int ch) { return reg(ch, ENVELOPE1) & 0xff; }
    uint32_t envelope_addr(int ch) { return (uint32_t)(reg(ch, ENVELOPE_ADDR_HIGH) & 0x3f) << 16 | reg(ch, ENVELOPE_ADDR); }
    uint32_t envclk_frames(int ch);
    uint32_t rampdown_frames(int ch);
    void update_rate(int ch);

    void start_channel(int ch);
    void stop_channel(int ch);
    bool advance_channel(int ch);
    bool fetch_sample(int ch);
    void envelope_tick(int ch);
    void rampdown_tick(int ch);
    void beat_tick();
    void ctrl_write(int off, uint16_t data);
    int16_t ima_clock(int ch, uint8_t nib);
    int16_t adpcm36_clock(int ch, uint8_t nib);
};
