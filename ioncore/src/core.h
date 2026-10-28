// ioncore internals: the machine split into SoC / Board / FirmwareHle around a shared Core (memory, CPU, frame loop).
// Not part of the public API (include/ioncore/ioncore.h).
#pragma once
#include "emu.h"
#include "unsp.h"
#include "spg_renderer.h"
#include "ion_spu.h"
#include "spi_flash.h"
#include "disc.h"
#include "ioncore/ioncore.h"
#include <map>

namespace ion {

constexpr uint32_t IMG_BASE = 0x40000, FW_API = 0x3FF6C, BOOT_SP = 0x27F0, CYCLES_PER_FRAME = 27000000 / 60;
constexpr int SLICES_PER_FRAME = 16;                       // CPU/SPU interleave (~1 ms)
// Firmware-owned words: camera profile and the threshold/register-table {seg, off} descriptors.
constexpr uint32_t CAM_PROFILE = 0x27FE, THR_DESC = 0x27FC, REG_DESC = 0x27FA, HALT_STUB = 0xF030;

struct Core;

// SPG220 on-chip peripherals: register file 0x2800-0x3FFF, PPU renderer + camera-plane compositing, SPU, GPIO,
// video IRQ status, DMA, I2C master stub, PRNG.
struct Soc {
    Core &c;
    uint16_t io[0x1800] = {};
    spg_renderer_device ppu{machine_config{}, "ppu", nullptr, 0};
    ion_spu spu;
    uint32_t prng = 0x1234;
    std::map<uint32_t, uint64_t> reg_logs;                 // logged changes per register (bounded)

    explicit Soc(Core &core);
    uint16_t &IO(uint32_t a) { return io[a - 0x2800]; }
    void reset();
    uint16_t read(uint32_t a);                             // 0x2800-0x3FFF
    void write(uint32_t a, uint16_t d);
    uint16_t gpio_read(uint32_t data_reg, uint16_t pins);  // outputs from the buffer, inputs from the pins
    void update_video_irq();
    void update_spi();                                     // IOA outputs -> save-flash pins
    void end_of_frame();                                   // vblank + line-position status for enabled sources
    void render(bitmap_rgb32 &bm);                         // PPU layers with the camera plane at its priority slot
};

// Camera helpers shared by Board and the public MotionPreview (board.cpp).
void resample_camera(const uint32_t *xrgb, int w, int h, bool mirror, float zoom, uint32_t *out320x240);
uint8_t luma_of(uint32_t r, uint32_t g, uint32_t b);
// The SoC's frame detector: per cell, m = mean(Y); flagged when |m - m_prev| > t. Updates prev_mean/prev_valid;
// writes cells[row * cols + col] (1 = flagged) when cells != nullptr. Returns the flagged count.
int detect_motion_cells(const uint8_t *luma, int cell, float t, int16_t *prev_mean, bool &prev_valid, uint8_t *cells);

// Board: front-panel buttons and the virtual hand (Input), the SPI save flash, the stand-in camera frame source,
// and the SoC motion detector model that writes the 40x30 motion bitmap into iRAM.
struct Board {
    Core &c;
    Input in;
    SpiFlash flash;
    std::vector<uint16_t> cam = std::vector<uint16_t>(SCREEN_W * SCREEN_H);   // camera frame, RGB555 (opaque)
    std::vector<uint8_t> luma = std::vector<uint8_t>(SCREEN_W * SCREEN_H);    // its Y, for the frame detector
    std::vector<uint32_t> ext = std::vector<uint32_t>(SCREEN_W * SCREEN_H);   // latest external frame (displayed)
    bool ext_valid = false;
    uint32_t ext_seq = 0, det_seq = 0;                    // external frames delivered / consumed by the detector
    // frame detector state
    int16_t prev_mean[40 * 30] = {};
    bool prev_valid = false;
    uint16_t prev_base = 0, prev_cell = 0;

    explicit Board(Core &core) : c(core) {}
    bool frames_mode() const;                              // camera picture + motion come from per-frame camera frames
    void motion_frame();
    void synth_camera(int wiggle = 0);
    void set_external(const uint32_t *xrgb, int w, int h, bool mirror);
private:
    void scan_hand();
    void update_camera();
    void motion_from_frame();
};

// Firmware HLE: IRQ trampolines, the fw12 halt stub, the directory loader and the 7 live API entries.
struct FirmwareHle {
    Core &c;
    Disc disc;                                             // directory entry k (1-based) -> disc.entry_*(k)
    bool stopped = false;
    int cur_entry = 0;
    std::map<int, uint64_t> api_calls;

    explicit FirmwareHle(Core &core) : c(core) {}
    bool load_entry(int entry);
    void install_stubs();
    void init_hardware();                                  // firmware's power-on GPIO setup (save-flash port)
    void enter_image();
    void api(int k);
private:
    uint16_t arg(int i);
    uint32_t arg32(int i);
    void api_return(uint32_t r1, uint32_t r2 = 0);
    void data_walk_table(uint32_t tbl);
};

struct Core : unsp_bus {
    Options opt;
    std::vector<uint16_t> mem = std::vector<uint16_t>(0x400000, 0);
    unsp_device cpu{machine_config{}, "cpu", nullptr, 27000000};
    Soc soc{*this};
    Board board{*this};
    FirmwareHle hle{*this};

    uint64_t frame = 0;
    std::vector<int16_t> audio;
    double audio_due = 0;
    int cycle_carry = 0;                                   // instruction overshoot of the previous slice
    uint32_t last_pc = 0;                                  // previous instruction: the caller of a firmware trap
    std::vector<std::string> events;
    std::function<void()> on_frame;

    // debug statistics (Options::debug_stats)
    uint64_t insns = 0;
    std::vector<uint64_t> pc_hist = std::vector<uint64_t>(0x4000);
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> io_rw;
    uint32_t min_sp = 0xffff; uint64_t irq_gate0 = 0, irq_taken = 0;
    std::map<int, uint64_t> irq_lines;
    std::vector<FileStats> file_stats;
    uint32_t spu_seen = 0; int spu_max_voices = 0;
    std::vector<uint16_t> map_reads = std::vector<uint16_t>(600, 0);   // Options::trace_motion_reads

    Core();
    uint16_t read(uint32_t a) override;
    void write(uint32_t a, uint16_t d) override;
    void log(const std::string &s);
    bool power_on(int entry);
    void step_frame();
    void file_changed(int entry);                          // close the previous file's stats, start new ones
    FileStats current_file_stats() const;

    static void insn_hook(void *ctx, uint32_t pc);
    static void irq_hook(void *ctx, int line);
};

}  // namespace ion
