// Core: memory, CPU wiring, the frame loop, and the public ion::Console (see core.h, include/ioncore/ioncore.h).
#include "core.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace ion {

// ---------------------------------------------------------------- Core
Core::Core() {
    cpu.set_bus(this);
    cpu.set_hooks(this, &Core::insn_hook, &Core::irq_hook);
    cpu.start();
    static_cast<device_t &>(soc.ppu).device_start();
    board.flash.log_cb = [this](const char *m) { log(m); };
}

uint16_t Core::read(uint32_t a) {
    a &= 0x3fffff;
    if (a >= 0x2800 && a < 0x4000) {
        if (opt.debug_stats) io_rw[a].first++;
        return soc.read(a);
    }
    if (opt.trace_motion_reads) {
        const uint32_t base = soc.IO(0x2895);
        if (a >= base && a < base + 600 && map_reads[a - base] < 0xffff) map_reads[a - base]++;
    }
    return mem[a];
}

void Core::write(uint32_t a, uint16_t d) {
    a &= 0x3fffff;
    if (a >= 0x2800 && a < 0x4000) {
        if (opt.debug_stats) io_rw[a].second++;
        soc.write(a, d);
        return;
    }
    mem[a] = d;
}

void Core::log(const std::string &s) {
    events.push_back("[f" + std::to_string(frame) + "] " + s);
    if (opt.trace_events) fprintf(stderr, "%s\n", events.back().c_str());
}

void Core::insn_hook(void *ctx, uint32_t pc) {
    Core &c = *static_cast<Core *>(ctx);
    if (c.opt.debug_stats) {
        c.insns++;
        c.pc_hist[pc >> 8]++;
        if (c.cpu.reg(0) < c.min_sp) c.min_sp = c.cpu.reg(0);
    }
    if (pc >= FW_API && pc <= FW_API + 40 && ((pc - FW_API) & 1) == 0) c.hle.api((pc - FW_API) / 2);
    c.last_pc = pc;
}

void Core::irq_hook(void *ctx, int line) {                // line 0 = FIQ, k+1 = IRQk
    Core &c = *static_cast<Core *>(ctx);
    c.irq_taken++; c.irq_lines[line]++;
    if (c.mem[0x0010] == 0 && c.irq_gate0++ < 3) c.log("IRQ" + std::to_string(line - 1) + " taken while [0010]==0");
    if (line == 0) c.log("FIQ taken");
}

FileStats Core::current_file_stats() const {
    FileStats s{hle.cur_entry, hle.cur_entry ? hle.disc.entry_label(hle.cur_entry) : std::string(), min_sp, soc.io[0x2895 - 0x2800],
                irq_taken, irq_gate0, {}};
    for (auto &[l, n] : irq_lines) s.irq_lines.push_back({l, n});
    return s;
}

void Core::file_changed(int) {
    if (hle.cur_entry) file_stats.push_back(current_file_stats());
    min_sp = 0xffff; irq_gate0 = irq_taken = 0; irq_lines.clear();
}

bool Core::power_on(int entry) {
    std::fill(mem.begin(), mem.end(), 0);
    soc.reset();
    board.flash.reset();                                  // interface only: flash contents survive power cycles
    cpu.reset();
    audio.clear(); audio_due = 0; cycle_carry = 0; last_pc = 0; frame = 0;
    hle.stopped = false; hle.api_calls.clear();
    hle.install_stubs();
    hle.init_hardware();
    if (!hle.load_entry(entry)) return false;
    hle.enter_image();
    return true;
}

// One video frame: CPU and SPU interleaved in slices (~1 ms, so an SPU beat IRQ lands on time), then the camera
// block, the frame hook, and vblank + line-position status for the enabled video IRQ sources.
void Core::step_frame() {
    soc.spu.adpcm36_model = opt.adpcm36_model;            // options may change between frames
    soc.spu.adpcm36_c0 = opt.adpcm36_c0;
    soc.spu.adpcm36_c1 = opt.adpcm36_c1;
    for (int i = 0; i < 32; i++) soc.spu.adpcm36_coef_q12[i] = (int)std::lround(opt.adpcm36_table[i] * 4096.0f);
    for (int i = 0; i < 32; i++) soc.spu.adpcm36_coef2_q12[i] = (int)std::lround(opt.adpcm36_table2[i] * 4096.0f);
    for (int i = 0; i < 16; i++) soc.spu.adpcm36_dq_q12[i] = (int)std::lround(opt.adpcm36_dequant[i] * 4096.0f);
    for (int i = 0; i < 32; i++) soc.spu.adpcm36_coef3_q12[i] = (int)std::lround(opt.adpcm36_table3[i] * 4096.0f);
    for (int i = 0; i < 32; i++) soc.spu.adpcm36_gain_q12[i] = (int)std::lround(opt.adpcm36_gain[i] * 4096.0f);
    soc.spu.force_noint = opt.spu_no_interpolation;
    for (int s = 0; s < SLICES_PER_FRAME && !hle.stopped; s++) {
        const int want = CYCLES_PER_FRAME / SLICES_PER_FRAME - cycle_carry;
        cycle_carry = cpu.run(want) - want;
        audio_due += AUDIO_RATE / 60.0 / SLICES_PER_FRAME;
        const int n = (int)audio_due;
        audio_due -= n;
        const size_t at = audio.size();
        audio.resize(at + 2 * n);
        soc.spu.generate(&audio[at], n);
        const uint32_t pm = soc.spu.playing_mask();
        spu_seen |= pm;
        spu_max_voices = std::max(spu_max_voices, std::popcount(pm));
    }
    board.motion_frame();
    if (on_frame) on_frame();
    soc.end_of_frame();
    frame++;
}

// ---------------------------------------------------------------- save states
namespace {
constexpr char STATE_MAGIC[8] = {'I', 'O', 'N', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t STATE_VERSION = 5;                     // bump when the layout or HLE behaviour changes (2: flash, 3: detector)
constexpr uint32_t PAGE_WORDS = 0x1000;

struct Writer {
    std::vector<uint8_t> &out;
    void operator()(const void *p, size_t n) { auto b = static_cast<const uint8_t *>(p); out.insert(out.end(), b, b + n); }
    template <class T> void pod(const T &v) { (*this)(&v, sizeof v); }
};
struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    void operator()(void *dst, size_t n) {
        if (!ok || (size_t)(end - p) < n) { ok = false; return; }
        memcpy(dst, p, n); p += n;
    }
    template <class T> T pod() { T v{}; (*this)(&v, sizeof v); return v; }
};

uint64_t content_id(const FirmwareHle &hle) {             // FNV-1a over the directory's entry labels
    uint64_t h = 1469598103934665603ull;
    for (int k = 1; k <= hle.disc.entry_count(); k++)
        for (char ch : hle.disc.entry_label(k) + '\n') { h ^= (uint8_t)ch; h *= 1099511628211ull; }
    return h;
}

template <class F> void serialize_core(Core &c, F &&f) {  // everything except memory (paged separately)
    f(c.cpu.core_state(), unsp_device::core_state_size());
    f(c.soc.io, sizeof c.soc.io);
    f(&c.soc.prng, sizeof c.soc.prng);
    c.soc.spu.serialize(f);
    f(&c.board.in, sizeof c.board.in);
    f(&c.hle.stopped, sizeof c.hle.stopped);
    f(&c.hle.cur_entry, sizeof c.hle.cur_entry);
    f(&c.frame, sizeof c.frame);
    f(&c.audio_due, sizeof c.audio_due);
    f(&c.cycle_carry, sizeof c.cycle_carry);
    f(&c.last_pc, sizeof c.last_pc);
    c.board.flash.serialize(f);
    f(c.board.prev_mean, sizeof c.board.prev_mean);
    f(&c.board.prev_valid, sizeof c.board.prev_valid);
    f(&c.board.prev_base, sizeof c.board.prev_base);
    f(&c.board.prev_cell, sizeof c.board.prev_cell);
}
}

// ---------------------------------------------------------------- Console
struct Console::Impl {
    Core core;
    bitmap_rgb32 bm;
};

Console::Console(const Options &opt) : d(std::make_unique<Impl>()) {
    d->core.opt = opt;
    d->core.on_frame = [this] { if (on_frame) on_frame(); };
}
Console::~Console() = default;

DiscInfo identify_disc(const std::string &path) {
    Disc disc;
    DiscInfo info;
    info.ok = disc.open(path);
    info.error = disc.error();
    info.sha1 = disc.sha1();
    info.title = disc.title();
    info.entries = disc.entry_count();
    info.sectors = disc.sectors();
    info.bad_checksums = disc.bad_checksums();
    info.missing_sectors = disc.missing_sectors();
    return info;
}

bool Console::open(const std::string &path) {
    Disc &disc = d->core.hle.disc;
    const bool ok = disc.open(path);
    if (ok && (disc.bad_checksums() || disc.missing_sectors()))
        d->core.log("disc: " + std::to_string(disc.bad_checksums()) + " bad-checksum and " + std::to_string(disc.missing_sectors()) +
                    " missing sectors (ionrepair or the launcher can repair it)");
    return ok;
}
bool Console::open_extracted(const std::string &dir) { return open(dir); }
const std::string &Console::open_error() const { return d->core.hle.disc.error(); }
const std::string &Console::title() const { return d->core.hle.disc.title(); }
const std::string &Console::disc_sha1() const { return d->core.hle.disc.sha1(); }
int Console::entry_count() const { return d->core.hle.disc.entry_count(); }
bool Console::power_on(int entry) { return d->core.power_on(entry); }
void Console::step_frame() { d->core.step_frame(); }
bool Console::stopped() const { return d->core.hle.stopped; }
uint64_t Console::frame() const { return d->core.frame; }
Input &Console::input() { return d->core.board.in; }
Options &Console::options() { return d->core.opt; }
std::vector<int16_t> &Console::audio() { return d->core.audio; }
std::vector<std::string> &Console::events() { return d->core.events; }
void Console::log(const std::string &line) { d->core.log(line); }
const uint16_t *Console::ram() const { return d->core.mem.data(); }
int Console::motion_reads(std::vector<uint16_t> &words) {
    words = d->core.map_reads;
    int n = 0;
    for (auto &w : d->core.map_reads) { n += w != 0; w = 0; }
    return n;
}
int Console::motion_cells(std::vector<uint8_t> &cells, int &cols, int &rows) const {
    const uint16_t md = d->core.soc.IO(0x289D), base = d->core.soc.IO(0x2895);
    if (!(md & 1)) return -1;
    const int cell = ((md >> 10) & 3) == 1 ? 8 : 16;
    cols = SCREEN_W / cell; rows = SCREEN_H / cell;
    if (base + rows * cols / 2 > 0x2800) return -1;
    cells.assign((size_t)cols * rows, 0);
    int n = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const uint16_t w = d->core.mem[base + r * (cols / 2) + (c >> 1)];
            const uint8_t on = (c & 1) ? (w & 1) : ((w >> 1) & 1);
            cells[(size_t)r * cols + c] = on; n += on;
        }
    return n;
}

void Console::set_camera_frame(const uint32_t *xrgb, int width, int height, bool mirror) {
    d->core.board.set_external(xrgb, width, height, mirror);
}

void Console::render(uint32_t *xrgb) {
    d->core.soc.render(d->bm);
    memcpy(xrgb, d->bm.px.data(), SCREEN_W * SCREEN_H * sizeof(uint32_t));
}

std::vector<uint8_t> Console::save_state() const {
    Core &c = d->core;
    std::vector<uint8_t> out;
    Writer w{out};
    w(STATE_MAGIC, sizeof STATE_MAGIC);
    w.pod(STATE_VERSION);
    w.pod((uint32_t)unsp_device::core_state_size());
    w.pod(content_id(c.hle));
    serialize_core(c, w);
    for (uint32_t pg = 0; pg < c.mem.size() / PAGE_WORDS; pg++) {        // non-zero 4K-word pages only
        const uint16_t *p = &c.mem[pg * PAGE_WORDS];
        if (std::any_of(p, p + PAGE_WORDS, [](uint16_t v) { return v != 0; })) { w.pod(pg); w(p, PAGE_WORDS * 2); }
    }
    w.pod(0xffffffffu);
    const auto &fl = c.board.flash.data;                                  // save flash: non-erased 4K sectors
    for (uint32_t s = 0; s < SpiFlash::SIZE / SpiFlash::SECTOR; s++) {
        const uint8_t *p = &fl[s * SpiFlash::SECTOR];
        if (std::any_of(p, p + SpiFlash::SECTOR, [](uint8_t v) { return v != 0xFF; })) { w.pod(s); w(p, SpiFlash::SECTOR); }
    }
    w.pod(0xffffffffu);
    return out;
}

bool Console::load_state(const std::vector<uint8_t> &state) {
    Core &c = d->core;
    Reader r{state.data(), state.data() + state.size()};
    char magic[8];
    r(magic, sizeof magic);
    if (!r.ok || memcmp(magic, STATE_MAGIC, 8) != 0) return false;
    if (r.pod<uint32_t>() != STATE_VERSION || r.pod<uint32_t>() != unsp_device::core_state_size()) return false;
    if (r.pod<uint64_t>() != content_id(c.hle)) return false;           // a state from different content
    std::vector<uint8_t> backup = save_state();                          // restore on a truncated state
    serialize_core(c, r);
    std::fill(c.mem.begin(), c.mem.end(), 0);
    for (;;) {
        const uint32_t pg = r.pod<uint32_t>();
        if (!r.ok || pg == 0xffffffffu) break;
        if (pg >= c.mem.size() / PAGE_WORDS) { r.ok = false; break; }
        r(&c.mem[pg * PAGE_WORDS], PAGE_WORDS * 2);
    }
    auto &fl = c.board.flash.data;
    std::fill(fl.begin(), fl.end(), 0xFF);
    for (; r.ok;) {
        const uint32_t s = r.pod<uint32_t>();
        if (!r.ok || s == 0xffffffffu) break;
        if (s >= SpiFlash::SIZE / SpiFlash::SECTOR) { r.ok = false; break; }
        r(&fl[s * SpiFlash::SECTOR], SpiFlash::SECTOR);
    }
    if (!r.ok) { load_state(backup); return false; }
    c.board.flash.dirty = true;                                           // the frontend should persist the restored flash
    c.audio.clear();
    c.soc.update_video_irq();                                            // re-derive the IRQ0 line from the registers
    return true;
}

bool Console::load_flash(const std::vector<uint8_t> &image) {
    auto &fl = d->core.board.flash;
    if (image.size() > SpiFlash::SIZE) return false;
    std::copy(image.begin(), image.end(), fl.data.begin());
    std::fill(fl.data.begin() + image.size(), fl.data.end(), 0xFF);
    fl.dirty = false;
    return true;
}

std::vector<uint8_t> Console::flash_image() const {
    const auto &fl = d->core.board.flash.data;
    size_t end = SpiFlash::SIZE;
    while (end > 0 && fl[end - 1] == 0xFF) end--;
    end = (end + SpiFlash::SECTOR - 1) / SpiFlash::SECTOR * SpiFlash::SECTOR;
    return std::vector<uint8_t>(fl.begin(), fl.begin() + end);
}
bool Console::flash_dirty() const { return d->core.board.flash.dirty; }
void Console::clear_flash_dirty() { d->core.board.flash.dirty = false; }

DebugInfo Console::debug_info() const {
    const Core &c = d->core;
    DebugInfo info;
    info.instructions = c.insns;
    info.pc = const_cast<unsp_device &>(c.cpu).pc();
    info.sp = const_cast<unsp_device &>(c.cpu).reg(0);
    info.sr = const_cast<unsp_device &>(c.cpu).reg(6);
    info.irq_enabled = const_cast<unsp_device &>(c.cpu).irq_enable();
    for (uint32_t i = 0; i < c.pc_hist.size(); i++) if (c.pc_hist[i]) info.hot_pc.push_back({i << 8, c.pc_hist[i]});
    std::sort(info.hot_pc.begin(), info.hot_pc.end(), [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &[k, n] : c.hle.api_calls) info.api_calls.push_back({k, n});
    for (auto &[a, rw] : c.io_rw) info.io_access.push_back({a, rw.first, rw.second});
    info.files = c.file_stats;
    if (c.hle.cur_entry) info.files.push_back(c.current_file_stats());
    info.spu_channels_used = c.spu_seen;
    info.spu_peak_voices = c.spu_max_voices;
    for (int i = 0; i < 4; i++) info.spu_starts[i] = const_cast<Core &>(c).soc.spu.starts[i];
    return info;
}

}  // namespace ion
