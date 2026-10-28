// The iON's save chip: an SST25VF512A-class SPI NOR flash (64 KB) that the SDK bit-bangs over GPIO port A:
// IOA14 = CS#, IOA10 = SCK, IOA11 = MOSI, IOA12 = MISO (SDK FUN_040538/04057d/040a3b/040707).
// SPI mode 0 as the SDK drives it: MOSI is sampled on SCK rising edges and MISO changes on falling edges. The SDK polls
// RDSR only twice after a program, so writes and erases complete instantly. Programming can only clear bits (NOR).
// Write enable and block protection are tracked but not enforced (the SDK always sends WREN first).
// Size: the SDK's sector erase is called with the address low word only (FUN_04027c -> FUN_040c99(0xF000)), so the
// high address byte it sends is stack garbage (0x55 in Danny Phantom). That works only if A23-A16 are don't-care,
// i.e. a 64 KB part; the saves live in its top 4K sector, 0xF000. Addresses wrap at 64 KB.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace ion {

class SpiFlash {
public:
    static constexpr uint32_t SIZE = 0x10000;              // 64 KB (see above)
    static constexpr uint32_t SECTOR = 0x1000;

    std::vector<uint8_t> data = std::vector<uint8_t>(SIZE, 0xFF);   // erased = factory state
    bool dirty = false;                                    // contents changed since the frontend last saved them
    std::function<void(const char *)> log_cb;

    void reset();                                          // interface state only; contents persist
    void pins(bool cs_n, bool sck, bool mosi);             // new output levels (one GPIO write)
    bool miso() const { return m_miso; }

    template <class F> void serialize(F &&f) {             // interface state (contents are saved separately)
        f(&m_cs_n, sizeof m_cs_n); f(&m_sck, sizeof m_sck); f(&m_miso, sizeof m_miso);
        f(&m_in, sizeof m_in); f(&m_in_bits, sizeof m_in_bits); f(&m_out, sizeof m_out); f(&m_out_idx, sizeof m_out_idx);
        f(&m_cmd, sizeof m_cmd); f(&m_nbytes, sizeof m_nbytes); f(&m_addr, sizeof m_addr); f(&m_aai, sizeof m_aai);
        f(&m_aai_addr, sizeof m_aai_addr); f(&m_wel, sizeof m_wel); f(&m_status, sizeof m_status); f(&m_id_seq, sizeof m_id_seq);
    }

private:
    bool m_cs_n = true, m_sck = true, m_miso = false;
    uint8_t m_in = 0, m_in_bits = 0, m_out = 0, m_out_idx = 0;
    uint8_t m_cmd = 0;
    uint32_t m_nbytes = 0, m_addr = 0;
    bool m_aai = false;                                    // auto-address-increment program mode (0xAF)
    uint32_t m_aai_addr = 0;
    bool m_wel = false;
    uint8_t m_status = 0;                                  // BP3..0 (bits 5:2) and BPL (bit 7) as written by WRSR
    uint32_t m_id_seq = 0;
    uint32_t m_logged = 0;

    void begin();
    void on_byte(uint8_t b, uint32_t n);
    uint8_t next_out();
    void program(uint32_t addr, uint8_t v);
    void erase(uint32_t addr, uint32_t len);
    void note(const char *what);
};

}  // namespace ion
