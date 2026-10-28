// license:BSD-3-Clause
// copyright-holders:Ryan Holtz,Jonathan Gevaryahu (MAME spg2xx_audio, the source of this logic)
// See ion_spu.h. Differences from MAME's spg2xx_audio: 24 channels with control bank 1 at 0x3500; channel
// registers decoded as (offset >> 4, offset & 0xF), because MAME's 0xF0F mask sends channels 16+ to the default
// case; envelope-clock updates index the right channel; the ADPCM36 predictor is selectable; the mix saturates.
#include "ion_spu.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace {
const int k_ima_shift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
const uint32_t k_rampdown_frames[8] = {13 * 4, 13 * 16, 13 * 64, 13 * 256, 13 * 1024, 13 * 4096, 13 * 8192, 13 * 8192};
const uint32_t k_envclk_frames[16] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 8192, 8192, 8192, 8192};
const int k_ima_steps[89] = {                           // canonical IMA step table (= MAME's computed table)
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

const int *ima_diff() {                                  // per-(step, nibble) deltas, as MAME ima_adpcm_state
    static const auto table = [] {
        std::array<int, 89 * 16> t{};
        for (int step = 0; step < 89; step++) {
            const int sv = k_ima_steps[step];
            for (int nib = 0; nib < 16; nib++) {
                int d = sv / 8;
                if (nib & 4) d += sv;
                if (nib & 2) d += sv / 2;
                if (nib & 1) d += sv / 4;
                t[step * 16 + nib] = (nib & 8) ? -d : d;
            }
        }
        return t;
    }();
    return table.data();
}
}

ion_spu::ion_spu() { reset(); }

void ion_spu::reset() {
    memset(m_regs, 0, sizeof m_regs); memset(m_phase, 0, sizeof m_phase); memset(m_ctrl, 0, sizeof m_ctrl);
    for (int ch = 0; ch < NCH; ch++) {
        m_sample_shift[ch] = 0; m_rate[ch] = m_rate_accum[ch] = 0; m_rampdown_frame[ch] = 0;
        m_envclk_frame[ch] = 4; m_envelope_addr[ch] = 0; m_ima[ch] = {0, 0}; m_a36[ch] = {0, 0, 0, 0, 0};
    }
    m_beat_base = 0;
    m_ctrl[CHANNEL_REPEAT] = 0x3f;
    m_ctrl[CHANNEL_ENV_MODE] = 0x3f;
}

uint32_t ion_spu::envclk_frames(int ch) {
    const int bank = ch >> 4, c = ch & 15;
    const uint16_t r = m_ctrl[bank * 0x100 + ENVCLK0 + (c >> 2)];     // ENVCLK0, 0_HIGH, 1, 1_HIGH: 4 channels each
    return k_envclk_frames[(r >> ((c & 3) * 4)) & 15];
}
uint32_t ion_spu::rampdown_frames(int ch) { return k_rampdown_frames[phr(ch, RAMP_DOWN_CLOCK) & 7]; }
void ion_spu::update_rate(int ch) {
    const uint32_t phase = (uint32_t)(phr(ch, PHASE_HIGH) & 7) << 16 | phr(ch, PHASE);
    m_rate[ch] = phase * 140625.0 * 2.0 / (double)(1 << 19);
    m_rate_accum[ch] = 0.0;
}

// ---------------------------------------------------------------- register interface
uint16_t ion_spu::read(uint32_t addr) {
    if (addr >= 0x3000 && addr < 0x3200) return m_regs[addr - 0x3000];
    if (addr >= 0x3200 && addr < 0x3400) return m_phase[addr - 0x3200];
    if (addr >= 0x3400 && addr < 0x3800) return m_ctrl[addr - 0x3400];
    return 0;
}

void ion_spu::write(uint32_t addr, uint16_t data) {
    if (addr >= 0x3000 && addr < 0x3200) {
        const int off = addr - 0x3000, r = off & 15;
        switch (r) {
        case PAN_VOL: data &= 0x7f7f; break;
        case ENVELOPE_DATA: data &= 0xff7f; break;
        case ADPCM_SEL: data &= 0xfe00; break;
        }
        m_regs[off] = data;
    } else if (addr >= 0x3200 && addr < 0x3400) {
        const int off = addr - 0x3200, ch = off >> 4, r = off & 15;
        switch (r) {
        case PHASE_HIGH: case PHASE_ACCUM_HIGH: case TARGET_PHASE_HIGH: case RAMP_DOWN_CLOCK: data &= 7; break;
        }
        m_phase[off] = data;
        if ((r == PHASE_HIGH || r == PHASE) && ch < NCH) update_rate(ch);
    } else if (addr >= 0x3400 && addr < 0x3800) {
        ctrl_write(addr - 0x3400, data);
    }
}

void ion_spu::ctrl_write(int off, uint16_t data) {
    const int bank = off >> 8, r = off & 0xff, base = bank * 16, nch = bank ? NCH - 16 : 16;
    uint16_t &R = m_ctrl[off];
    const bool per_channel = r == CHANNEL_ENABLE || r == CHANNEL_FIQ_ENABLE || r == CHANNEL_FIQ_STATUS ||
                             (r >= ENVCLK0 && r <= ENVCLK1_HIGH) || (r >= ENV_RAMP_DOWN && r <= CHANNEL_ZERO_CROSS) ||
                             r == CHANNEL_STATUS || (r >= CHANNEL_REPEAT && r <= CHANNEL_ENV_IRQ);
    if (bank > 1 || (bank == 1 && !per_channel)) { R = data; return; }     // bank 1 carries only per-channel registers
    switch (r) {
    case CHANNEL_ENABLE: {
        const uint16_t changed = R ^ data;
        R = data;
        for (int b = 0; b < nch; b++) {
            if (!((changed >> b) & 1)) continue;
            const int ch = base + b;
            if ((data >> b) & 1) { if (!cbit(CHANNEL_STOP, ch) && !cbit(CHANNEL_STATUS, ch)) start_channel(ch); }
            else if (cbit(CHANNEL_STATUS, ch)) stop_channel(ch);
        }
        return;
    }
    case MAIN_VOLUME: R = data & 0x7f; return;
    case CHANNEL_FIQ_ENABLE:
        if (data && log_cb) log_cb("SPU: channel FIQ enable requested (not modelled)");
        R = data; return;
    case CHANNEL_FIQ_STATUS: R &= ~data; return;
    case BEAT_BASE_COUNT: R = data & 0x7ff; m_beat_base = R; return;
    case BEAT_COUNT: {
        const uint16_t old = R;
        R &= ~(data & BIS); R &= BIS; R |= data & ~BIS;
        if ((old ^ R) & (BIS | BIE) && irq_cb) irq_cb((R & (BIS | BIE)) == (BIS | BIE));
        return;
    }
    case ENVCLK0: case ENVCLK0_HIGH: case ENVCLK1: case ENVCLK1_HIGH: {
        const uint16_t changed = R ^ data;
        R = data;
        const int first = base + (r - ENVCLK0) * 4;
        for (int i = 0; i < 4; i++)
            if (((changed >> (4 * i)) & 15) && first + i < NCH) m_envclk_frame[first + i] = envclk_frames(first + i);
        return;
    }
    case ENV_RAMP_DOWN: {
        const uint16_t old = R;
        R = data & m_ctrl[bank * 0x100 + CHANNEL_STATUS];
        const uint16_t changed = old ^ R;
        for (int b = 0; b < nch; b++)
            if (((changed & data) >> b) & 1) m_rampdown_frame[base + b] = rampdown_frames(base + b);
        return;
    }
    case CHANNEL_STOP: {
        const uint16_t old = R;
        R &= ~data;
        const uint16_t changed = old ^ R;
        for (int b = 0; b < nch; b++) {
            const int ch = base + b;
            if (((changed >> b) & 1) && cbit(CHANNEL_ENABLE, ch) && !cbit(CHANNEL_STATUS, ch)) start_channel(ch);
        }
        return;
    }
    case CONTROL: R = data & 0x9fe8; return;
    case CHANNEL_STATUS: return;                                           // read-only
    case CHANNEL_ENV_IRQ: R &= ~data; return;
    default: R = data; return;
    }
}

// ---------------------------------------------------------------- channels
void ion_spu::start_channel(int ch) {
    if (cbit(CHANNEL_FIQ_ENABLE, ch) && log_cb) log_cb("SPU: channel started with FIQ enabled (not modelled)");
    setbit(CHANNEL_STATUS, ch);
    starts[adpcm36(ch) ? 3 : adpcm(ch) ? 2 : pcm16(ch) ? 1 : 0]++;
    m_envelope_addr[ch] = envelope_addr(ch);
    set_env_count(ch, env_load(ch));
    m_ima[ch] = {0, 0};
    m_sample_shift[ch] = 0;
    if (adpcm36(ch)) m_a36[ch] = {0, 0, 0, 0, 0};
}

void ion_spu::stop_channel(int ch) {
    clrbit(CHANNEL_STATUS, ch);
    reg(ch, MODE) &= ~0x8000;
    clrbit(CHANNEL_TONE_RELEASE, ch);
    clrbit(ENV_RAMP_DOWN, ch);
}

bool ion_spu::advance_channel(int ch) {
    m_rate_accum[ch] += m_rate[ch];
    uint32_t n = 0;
    while (m_rate_accum[ch] >= RATE) { m_rate_accum[ch] -= RATE; n++; }
    for (uint32_t i = 0; i < n; i++) {
        if (!fetch_sample(ch)) return false;
        if (adpcm(ch) || adpcm36(ch)) {
            m_sample_shift[ch] += 4;
            if (m_sample_shift[ch] >= 16) {
                m_sample_shift[ch] = 0;
                set_wave_addr(ch, wave_addr(ch) + 1);
                if (adpcm36(ch)) m_a36[ch].remaining--;
            }
        } else if (pcm16(ch)) {
            set_wave_addr(ch, wave_addr(ch) + 1);
        } else {
            m_sample_shift[ch] += 8;
            if (m_sample_shift[ch] >= 16) { m_sample_shift[ch] = 0; set_wave_addr(ch, wave_addr(ch) + 1); }
        }
    }
    return true;
}

int16_t ion_spu::ima_clock(int ch, uint8_t nib) {
    auto &s = m_ima[ch];
    s.sig = std::clamp(s.sig + ima_diff()[s.step * 16 + nib], -32768, 32767);
    s.step = std::clamp(s.step + k_ima_shift[nib & 7], 0, 88);
    return (int16_t)s.sig;
}

int16_t ion_spu::adpcm36_clock(int ch, uint8_t nib) {
    auto &s = m_a36[ch];
    const int shift = s.header & 15, k = (s.header >> 4) & 0x3f;
    int32_t v = adpcm36_model == 3 ? adpcm36_dq_q12[nib] >> shift : (int16_t)(nib << 12) >> shift;   // model 3: dequantiser table
    // q6: a = k/64 with k = header bits 9:4. Bit 9 is always set in the data (it may belong to the ADPCM_SEL-style
    // point number), so this is a = 0.5 + idx/64 for the 5-bit index idx = bits 8:4 (0-29).
    if (adpcm36_model == 0) v += (s.prev * k) >> 6;
    else if (adpcm36_model == 2) {                                           // tunable linear predictor (Q12)
        const int coef = (int)((adpcm36_c0 + adpcm36_c1 * (k & 0x1f) / 32.0f) * 4096.0f + 0.5f);
        v += (s.prev * coef) >> 12;
    }
    else if (adpcm36_model == 3) {                                          // per-index two-tap table
        const int i = k & 0x1f;
        v = (v * adpcm36_gain_q12[i]) >> 12;
        v += (s.prev * adpcm36_coef_q12[i] + s.prev2 * adpcm36_coef2_q12[i] + s.prev3 * adpcm36_coef3_q12[i]) >> 12;
    }
    else v += (s.prev * (k & 0x20 ? k - 64 : k) + 32) >> 12;                // MAME's approximation
    v = std::clamp(v, -32768, 32767);
    s.prev3 = s.prev2;
    s.prev2 = s.prev;
    s.prev = v;
    return (int16_t)v;
}

bool ion_spu::fetch_sample(int ch) {
    reg(ch, WAVE_DATA_PREV) = reg(ch, WAVE_DATA);
    const int tm = tone_mode(ch);
    auto end_of_wave = [&](bool set_stop) {
        if (tm == 1) {                                                     // hardware one-shot
            if (set_stop) setbit(CHANNEL_STOP, ch);
            stop_channel(ch);
            return false;
        }
        set_wave_addr(ch, loop_addr(ch));                                  // hardware loop
        m_sample_shift[ch] = 0;
        m_a36[ch].remaining = 0;                                           // the loop point starts with a block header
        return true;
    };
    if (adpcm36(ch) && tm != 0 && m_a36[ch].remaining == 0) {                // 9-word block: header + 8 data words
        uint16_t h = read_mem(wave_addr(ch));
        if (h == 0xffff) {                     // the 0xFFFF stop mark sits where the next header would be (the discs pad
            if (!end_of_wave(true)) return false;   // 1-3 words; a single one used to be eaten as a header -> garbage)
            h = read_mem(wave_addr(ch));
            if (h == 0xffff) { stop_channel(ch); return false; }
        }
        m_a36[ch].header = h;
        m_a36[ch].remaining = 8;
        set_wave_addr(ch, wave_addr(ch) + 1);
    }
    uint16_t raw = tm ? read_mem(wave_addr(ch)) : reg(ch, WAVE_DATA);
    if (adpcm(ch) || adpcm36(ch)) {
        if (tm != 0 && raw == 0xffff) {
            if (!end_of_wave(true)) return false;
            reg(ch, MODE) &= ~0x8000;                                      // MAME: ADPCM loops continue as PCM
        } else {
            const uint8_t nib = (raw >> m_sample_shift[ch]) & 15;
            reg(ch, WAVE_DATA) = (uint16_t)(adpcm36(ch) ? adpcm36_clock(ch, nib) : ima_clock(ch, nib)) ^ 0x8000;
        }
    } else if (pcm16(ch)) {
        if (tm != 0 && raw == 0xffff) { if (!end_of_wave(false)) return false; }
        else reg(ch, WAVE_DATA) = raw;
    } else if (tm != 0) {
        raw = m_sample_shift[ch] ? (raw & 0xff00) : (uint16_t)(raw << 8);
        raw |= raw >> 8;
        if (raw == 0xffff) { if (!end_of_wave(true)) return false; }
        else reg(ch, WAVE_DATA) = raw;
    }
    return true;
}

void ion_spu::rampdown_tick(int ch) {
    const uint8_t old = edd(ch);
    uint8_t v = old - (reg(ch, ENVELOPE_LOOP_CTRL) >> 9);
    if (v > old) v = 0;
    if (v) { set_edd(ch, v); m_rampdown_frame[ch] = rampdown_frames(ch); }
    else { setbit(CHANNEL_STOP, ch); stop_channel(ch); }
}

void ion_spu::envelope_tick(int ch) {
    uint16_t count = env_count(ch);
    const uint16_t cur = edd(ch);
    if (count > 0) set_env_count(ch, --count);
    if (count != 0) return;
    const uint16_t target = (reg(ch, ENVELOPE0) >> 8) & 0x7f, inc = reg(ch, ENVELOPE0) & 0x7f;
    uint16_t v = cur;
    if (v != target) {
        if (reg(ch, ENVELOPE0) & 0x80) {                                   // decrement
            v -= inc;
            if (v > cur) v = 0;
            else if (v < target) v = target;
            if (v == 0) { setbit(CHANNEL_STOP, ch); stop_channel(ch); return; }
        } else {
            v += inc;
            if (v >= target) v = target;
        }
    }
    if (v == target) {
        if (reg(ch, ENVELOPE1) & 0x100) {                                  // repeat
            const uint16_t rep = ((reg(ch, ENVELOPE1) >> 9) & 0x7f) - 1;
            if (rep == 0) {
                reg(ch, ENVELOPE0) = read_mem(m_envelope_addr[ch]);
                reg(ch, ENVELOPE1) = read_mem(m_envelope_addr[ch] + 1);
                reg(ch, ENVELOPE_LOOP_CTRL) = read_mem(m_envelope_addr[ch] + 2);
                m_envelope_addr[ch] = envelope_addr(ch) + (reg(ch, ENVELOPE_LOOP_CTRL) & 0x1ff);
            } else {
                reg(ch, ENVELOPE1) = (reg(ch, ENVELOPE1) & 0x1ff) | ((rep << 9) & 0xfe00);
            }
        } else {
            reg(ch, ENVELOPE0) = read_mem(m_envelope_addr[ch]);
            reg(ch, ENVELOPE1) = read_mem(m_envelope_addr[ch] + 1);
            m_envelope_addr[ch] += 2;
        }
    }
    set_env_count(ch, env_load(ch));
    set_edd(ch, v);
}

void ion_spu::beat_tick() {
    if (m_beat_base > 0) m_beat_base--;
    if (m_beat_base != 0) return;
    m_beat_base = m_ctrl[BEAT_BASE_COUNT];
    uint16_t count = m_ctrl[BEAT_COUNT] & BEAT_COUNT_MASK;
    if (count > 0) { count--; m_ctrl[BEAT_COUNT] = (m_ctrl[BEAT_COUNT] & ~BEAT_COUNT_MASK) | count; }
    if (count == 0 && (m_ctrl[BEAT_COUNT] & BIE)) {
        const bool was = m_ctrl[BEAT_COUNT] & BIS;
        m_ctrl[BEAT_COUNT] |= BIS;
        if (!was && irq_cb) irq_cb(1);
    }
}

// ---------------------------------------------------------------- mixer
void ion_spu::generate(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        int32_t left = 0, right = 0;
        for (int ch = 0; ch < NCH; ch++) {
            if (!cbit(CHANNEL_STATUS, ch) || !advance_channel(ch)) continue;
            int32_t s = (int16_t)(reg(ch, WAVE_DATA) ^ 0x8000);
            if (!(m_ctrl[CONTROL] & CONTROL_NOINT) && !force_noint) {        // linear interpolation
                const int32_t prev = (int16_t)(reg(ch, WAVE_DATA_PREV) ^ 0x8000);
                const int32_t f = (int32_t)(m_rate_accum[ch] / RATE * 256.0);
                s = ((prev * (0x100 - f)) >> 8) + ((s * f) >> 8);
            }
            s = (s * edd(ch)) >> 7;
            const int32_t vol = reg(ch, PAN_VOL) & 0x7f, pan = (reg(ch, PAN_VOL) >> 8) & 0x7f;
            const int32_t pl = pan < 0x40 ? 0x7f * vol : (0x7f - pan) * 2 * vol;
            const int32_t pr = pan < 0x40 ? pan * 2 * vol : 0x7f * vol;
            left += (s * pl) >> 14;
            right += (s * pr) >> 14;
            if (cbit(ENV_RAMP_DOWN, ch)) {
                if (m_rampdown_frame[ch] > 0) m_rampdown_frame[ch]--;
                if (m_rampdown_frame[ch] == 0) rampdown_tick(ch);
            } else if (!cbit(CHANNEL_ENV_MODE, ch)) {                         // automatic envelope
                if (m_envclk_frame[ch] > 0) m_envclk_frame[ch]--;
                if (m_envclk_frame[ch] == 0) { envelope_tick(ch); m_envclk_frame[ch] = envclk_frames(ch); }
            }
        }
        if (m_ctrl[WAVE_IN_L]) left += (int32_t)m_ctrl[WAVE_IN_L] - 0x8000;
        if (m_ctrl[WAVE_IN_R]) right += (int32_t)m_ctrl[WAVE_IN_R] - 0x8000;
        const int volsel = (m_ctrl[CONTROL] >> 6) & 3;                       // MAME: 1/16, else 1/4 (x1/x2 clip)
        left >>= volsel == 0 ? 4 : 2;
        right >>= volsel == 0 ? 4 : 2;
        out[2 * i] = (int16_t)std::clamp((left * (int32_t)m_ctrl[MAIN_VOLUME]) >> 7, -32768, 32767);
        out[2 * i + 1] = (int16_t)std::clamp((right * (int32_t)m_ctrl[MAIN_VOLUME]) >> 7, -32768, 32767);
        beat_tick();
    }
}
