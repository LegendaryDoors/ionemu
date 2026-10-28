// SST25VF0x0A-class SPI NOR flash (see spi_flash.h).
#include "spi_flash.h"
#include <algorithm>
#include <cstdio>

namespace ion {

void SpiFlash::reset() {
    m_cs_n = true; m_sck = true; m_miso = false;
    m_in = m_in_bits = m_out = m_out_idx = 0;
    m_cmd = 0; m_nbytes = m_addr = 0;
    m_aai = false; m_aai_addr = 0; m_wel = false; m_id_seq = 0;
}

void SpiFlash::note(const char *what) {
    if (log_cb && m_logged++ < 24) log_cb(what);
}

void SpiFlash::begin() {                                   // CS# fell: a new command
    m_in = m_in_bits = 0;
    m_out = m_out_idx = 0;
    m_cmd = 0; m_nbytes = 0; m_addr = 0; m_id_seq = 0;
}

void SpiFlash::pins(bool cs_n, bool sck, bool mosi) {
    if (cs_n != m_cs_n) {
        m_cs_n = cs_n;
        if (!cs_n) begin();
    }
    if (sck == m_sck) return;
    m_sck = sck;
    if (m_cs_n) return;
    if (sck) {                                             // rising edge: sample MOSI
        m_in = (uint8_t)((m_in << 1) | (mosi ? 1 : 0));
        if (++m_in_bits == 8) { on_byte(m_in, m_nbytes++); m_in_bits = 0; }
    } else {                                               // falling edge: present the next MISO bit
        if (m_in_bits == 0) { m_out = next_out(); m_out_idx = 0; }
        else m_out_idx = (uint8_t)std::min(7, m_out_idx + 1);
        m_miso = (m_out >> (7 - m_out_idx)) & 1;
    }
}

void SpiFlash::program(uint32_t addr, uint8_t v) {
    uint8_t &cell = data[addr & (SIZE - 1)];
    const uint8_t nv = cell & v;                           // NOR: programming only clears bits
    if (nv != cell) { cell = nv; dirty = true; }
}

void SpiFlash::erase(uint32_t addr, uint32_t len) {
    addr &= (SIZE - 1) & ~(len - 1);
    std::fill(data.begin() + addr, data.begin() + addr + len, 0xFF);
    dirty = true;
}

void SpiFlash::on_byte(uint8_t b, uint32_t n) {
    char msg[96];
    if (n == 0) {
        m_cmd = b;
        switch (b) {
        case 0x06: m_wel = true; return;                                   // WREN
        case 0x04: m_wel = false; if (m_aai) note("flash: AAI program ended (WRDI)"); m_aai = false; return;   // WRDI
        case 0x50: return;                                                 // EWSR (enable WRSR)
        case 0x60: case 0xC7: erase(0, SIZE); m_wel = false; note("flash: chip erase"); return;
        case 0xAF:
            if (m_aai) { m_nbytes = 4; }                                   // in AAI mode the address is implicit
            return;
        default: return;
        }
    }
    if (n <= 3 && m_cmd != 0x01 && m_cmd != 0x05 && m_cmd != 0x9F) {      // 24-bit address, MSB first
        m_addr = (m_addr << 8) | b;
        if (n < 3) return;
        switch (m_cmd) {
        case 0x03: snprintf(msg, sizeof msg, "flash: read from %04x", m_addr & (SIZE - 1)); note(msg); return;
        case 0x20: erase(m_addr, 0x1000); snprintf(msg, sizeof msg, "flash: erase 4K sector %04x", m_addr & (SIZE - 1) & ~0xFFFu); note(msg); return;
        case 0x52: erase(m_addr, 0x8000); note("flash: erase 32K block"); return;
        case 0xD8: erase(m_addr, 0x10000); note("flash: erase 64K block"); return;
        default: return;
        }
    }
    switch (m_cmd) {
    case 0x01: if (n == 1) m_status = b & 0xBC; return;                    // WRSR: BP3..0 + BPL
    case 0x02: if (n == 4) program(m_addr, b); return;                     // byte program
    case 0xAF:                                                             // AAI: first = address + byte, then byte
        if (!m_aai) {
            m_aai = true; m_aai_addr = m_addr;
            snprintf(msg, sizeof msg, "flash: AAI program from %04x", m_addr & (SIZE - 1)); note(msg);
        }
        if (n == 4) program(m_aai_addr++, b);
        return;
    default: return;                                                       // data phase of reads: MOSI ignored
    }
}

uint8_t SpiFlash::next_out() {
    switch (m_cmd) {
    case 0x03: return m_nbytes >= 4 ? data[m_addr++ & (SIZE - 1)] : 0;    // READ
    case 0x0B: return m_nbytes >= 5 ? data[m_addr++ & (SIZE - 1)] : 0;    // FAST READ (1 dummy byte)
    case 0x05: return m_nbytes >= 1 ? (uint8_t)(m_status | (m_wel ? 0x02 : 0) | (m_aai ? 0x40 : 0)) : 0;   // RDSR, never busy
    case 0x90: case 0xAB:                                                  // READ-ID: manufacturer 0xBF, device 0x48 (512A)
        if (m_nbytes < 4) return 0;
        return ((m_addr & 1) ^ (m_id_seq++ & 1)) ? 0x48 : 0xBF;
    default: return 0;
    }
}

}  // namespace ion
