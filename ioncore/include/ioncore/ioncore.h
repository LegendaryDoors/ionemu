// ioncore: Hasbro iON (SPG220) emulation core. Public API; no MAME/shim headers leak through here.
// The SoC is emulated at register level (MAME-derived u'nSP CPU and SPG2xx renderer, a 24-channel SPU); the
// console firmware is reimplemented at a high level. Deterministic: identical inputs give identical
// frames, audio and state. No process-global state: any number of Consoles may coexist.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ion {

constexpr int SCREEN_W = 320, SCREEN_H = 240;
constexpr double AUDIO_RATE = 281250.0 / 4;            // stereo int16 samples per second produced by the SPU

// Console buttons as the games' button word.
enum Button : uint16_t {
    BTN_NEXT = 0x008, BTN_STOP = 0x080, BTN_OPEN = 0x100, BTN_PREVIOUS = 0x200, BTN_BRIGHTNESS = 0x020, BTN_PLAY_PAUSE = 0x040,
};

// How the stand-in player (camera_mode 1, no webcam) looks. Colours are 0-255; defaults reproduce the original
// stand-in exactly. style: 0 = simple (flat shapes, the original), 1 = detailed (shaded, with a face and hands).
// hair_style: 0 = short, 1 = long (to the shoulders), 2 = none.
struct PlayerLook {
    int style = 0;
    float skin[3] = {196, 156, 128};
    float hair[3] = {70, 48, 36};
    float shirt[3] = {52, 118, 128};
    float wall[3] = {92, 96, 112};
    float floor[3] = {70, 60, 54};
    int hair_style = 0;
    bool window = true;             // the lit window on the wall
};

struct Options {
    bool data_walk = true;          // crt0 .data walk inside fw01
    PlayerLook player;              // the stand-in player (camera_mode 1)
    int camera_mode = 1;            // camera source: 0 = off (black), 1 = synthetic stand-in player, 2 = external frames
                                    // (Console::set_camera_frame, e.g. a webcam)
    bool motion_from_frames = false;// motion bitmap from the camera frames (the SoC's block-mean detector) instead
                                    // of the virtual-hand blob; always on with camera_mode 2
    float motion_sensitivity = 1.0f;// frame detector: the game's threshold T is divided by this
    float camera_zoom = 1.0f;       // external frames: < 1 shrinks the picture into the frame (black around it) so a player
                                    // close to a webcam looks as far away as the console expects; > 1 crops in
    int adpcm36_model = 3;          // ADPCM36 predictor: 3 = per-index two-tap table fitted to real-console recordings
                                    // (default), 0 = the old q6 guess, 1 = MAME's, 2 = linear
    float adpcm36_c0 = 0.5f, adpcm36_c1 = 0.5f;  // model 2: a = c0 + c1 * idx / 32
    // model 3: v = e + table[idx]*prev + table2[idx]*prev2. Levenberg-Marquardt fit against a real-console Blue's Room
    // recording (16.8 dB waveform gain there, 9.3 dB on unseen Dora, vs 2.8/2.2 for q6). Indices 30-31 never occur in the data.
    float adpcm36_table[32] = {
        0.124770f, 0.908590f, 1.773546f, 1.515879f, 1.903060f, 1.945753f, 0.943754f, -0.654086f,
        -0.927738f, 0.970153f, 0.246979f, 0.827859f, -0.093716f, 1.118582f, -0.160373f, 0.978830f,
        0.404490f, 1.144480f, 0.283322f, 1.312575f, 1.001300f, 1.504808f, 0.010457f, 1.322861f,
        0.590453f, 1.216601f, 0.705420f, 1.507921f, 0.610455f, 1.727264f, 0.000000f, 0.000000f};
    float adpcm36_table2[32] = {
        0.001153f, 0.016806f, -0.791873f, -0.850585f, -0.936767f, -0.962217f, 0.051266f, 0.317745f,
        -0.323561f, -0.091784f, 0.202998f, -0.176782f, -0.428279f, -0.318081f, -0.175356f, -0.020635f,
        -0.107282f, -0.201005f, -0.442411f, -0.528337f, -0.503856f, -0.732215f, -0.200958f, -0.394469f,
        0.246721f, -0.725297f, -0.421397f, -0.579736f, -0.754407f, -0.810723f, 0.000000f, 0.000000f};
    float adpcm36_dequant[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};  // model 3: code -> residual (nibble units)
    float adpcm36_table3[32] = {};  // experimental (ionrun --adpcm36-table3): third tap; fitted values did not generalise
    float adpcm36_gain[32] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // experimental (--adpcm36-gain): per-index residual gain
    bool spu_no_interpolation = false;  // experiment: zero-order-hold playback regardless of SPU_CONTROL
    bool trace_events = false;      // echo every event-log line to stderr as it happens
    bool trace_motion_reads = false;// count CPU reads of the motion-map words (Console::motion_reads); a debugging aid
    bool debug_stats = false;       // per-instruction statistics (PC histogram, lowest SP, I/O access counts)
};

struct Input {
    uint16_t buttons = 0;           // Button bits held this frame
    int hand_x = 160, hand_y = 120; // virtual hand position in screen pixels (drives the motion bitmap)
    bool hand_active = true;        // report motion under the hand this frame
    bool hand_scan = false;         // test driver: sweep the hand over a 4x3 grid, 180 frames per position
};

// Per-file checks collected when Options::debug_stats is on.
struct FileStats { int entry; std::string file; uint32_t min_sp, motion_map; uint64_t irqs, irqs_gate0; std::vector<std::pair<int, uint64_t>> irq_lines; };

struct DebugInfo {
    uint64_t instructions = 0;
    uint32_t pc = 0, sp = 0, sr = 0, irq_enabled = 0;
    std::vector<std::pair<uint32_t, uint64_t>> hot_pc;              // (256-word bucket base, count), hottest first
    std::vector<std::pair<int, uint64_t>> api_calls;                // firmware entry -> calls
    std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> io_access; // (address, reads, writes)
    std::vector<FileStats> files;                                   // closed per-file stats plus the current file
    uint32_t spu_channels_used = 0; int spu_peak_voices = 0; uint32_t spu_starts[4] = {};   // PCM8, PCM16, IMA, ADPCM36
};

// Disc identification without a Console (launchers scanning a folder): the logical-image SHA-1 and the known title.
// Accepts the same paths as Console::open. Reads the whole data track (~0.3 s per disc).
struct DiscInfo {
    bool ok = false;
    std::string error, sha1, title;
    int entries = 0, sectors = 0, bad_checksums = 0, missing_sectors = 0;
};
DiscInfo identify_disc(const std::string &path);

class Console {
public:
    explicit Console(const Options &opt = {});
    ~Console();
    Console(const Console &) = delete;
    Console &operator=(const Console &) = delete;

    // Content: a .cue sheet, a raw image holding the data track (.bin/.img/.mdx), or a directory of extracted
    // programs (NN_name.mem.bin = directory entry NN+1). Damaged dumps play as they are; repair_disc() rebuilds them.
    bool open(const std::string &path);
    bool open_extracted(const std::string &dir);   // same as open()
    const std::string &open_error() const;
    const std::string &title() const;              // known title from the logical-image SHA-1, or ""
    const std::string &disc_sha1() const;          // logical-image SHA-1 ("" for an extracted directory without logical.bin)
    int entry_count() const;

    bool power_on(int entry = 1);   // power-on/Reset: clear all state and boot directory entry `entry`
    void step_frame();              // one 60 Hz video frame (CPU, SPU, camera block, vblank)
    bool stopped() const;           // the game called fw12 (exit to system, e.g. Open); the CPU is parked
    uint64_t frame() const;         // frames stepped since power-on

    Input &input();                 // read before each step_frame
    Options &options();

    void render(uint32_t *xrgb);                   // current picture, SCREEN_W x SCREEN_H, 0x00RRGGBB
    // External camera frame (camera_mode 2): any size, 0x00RRGGBB, camera view. It is centre-cropped to 4:3, scaled to
    // 320x240 and (mirror = true, as the console's sensor does) flipped horizontally into the displayed orientation.
    // The latest frame is used by every step_frame until the next one arrives.
    void set_camera_frame(const uint32_t *xrgb, int width, int height, bool mirror = true);
    std::vector<int16_t> &audio();                 // interleaved stereo samples produced since the caller last cleared it

    std::vector<std::string> &events();            // event log ("[fN] ..." lines); callers may drain it
    void log(const std::string &line);
    std::function<void()> on_frame;                // once per frame, after the camera block and before vblank

    // Save flash: the console's SPI NOR chip where games keep progress (e.g. Danny Phantom). It survives power_on;
    // frontends persist it per disc. flash_image() is trimmed to the last non-erased 4K sector (empty = fully erased).
    bool load_flash(const std::vector<uint8_t> &image);
    std::vector<uint8_t> flash_image() const;
    bool flash_dirty() const;                      // changed since load_flash / clear_flash_dirty
    void clear_flash_dirty();

    std::vector<uint8_t> save_state() const;       // versioned snapshot of all emulated state (includes the flash)
    bool load_state(const std::vector<uint8_t> &state);

    const uint16_t *ram() const;                   // CPU address space (0x400000 words); iRAM = [0, 0x2800)
    // The motion map the game is reading this frame: cells[row * cols + col] = 1 where the SoC detector flagged a
    // change (8x8 or 16x16 px cells). Returns the number of flagged cells, or -1 while the detector is off.
    int motion_cells(std::vector<uint8_t> &cells, int &cols, int &rows) const;
    // Options::trace_motion_reads: per-word CPU read counts of the motion map since the last call (600 words, row-major,
    // two cells per word). Returns the number of words read; the counters are cleared.
    int motion_reads(std::vector<uint16_t> &words);
    DebugInfo debug_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Repair a damaged dump (sectors whose stored checksum does not match their data) from donor blocks: this disc's own
// sectors plus the given images (other discs share the system files). A donor must sum to the stored checksum and
// differ in fewer than 512 bytes, which separates real copies (burst read errors: 110-204 bytes) from random 16-bit
// collisions (~2000 bytes). Writes copies named "<name> (repaired)" into out_dir and never touches the originals.
struct RepairReport {
    bool ok = false;
    std::string error;
    int damaged = 0, repaired = 0;                 // damaged sectors found / replaced
    std::vector<std::string> notes;                // one line per damaged sector, plus ignored donors
    std::string output;                            // the repaired .cue or image to open
    std::string sha1_after, title_after;           // identification of the result ("" if still unknown)
};
RepairReport repair_disc(const std::string &path, const std::vector<std::string> &donors, const std::string &out_dir);

// Preview of the stand-in player for a character-creation screen: the same picture camera_mode 1 shows the games
// (320x240, 0x00RRGGBB), with the hand at (hand_x, hand_y) and raised when `hand_active`.
void render_player(const PlayerLook &look, int hand_x, int hand_y, bool hand_active, uint32_t *xrgb320x240);

// Camera setup aid: the SoC's frame detector on its own, so a settings screen can show what a
// game would flag before any game runs. Feed camera frames as they arrive (any size, 0x00RRGGBB, camera view); each
// call compares with the previous frame using the game's threshold (12 or 18 in the discs) divided by sensitivity,
// exactly like Console::set_camera_frame followed by the game polling its map.
class MotionPreview {
public:
    MotionPreview();
    ~MotionPreview();
    MotionPreview(const MotionPreview &) = delete;
    MotionPreview &operator=(const MotionPreview &) = delete;
    void reset();                                        // forget the previous frame (e.g. after a device change)
    // Returns the number of flagged 8x8 cells; cells[row * cols + col] = 1 where flagged (cols = 40, rows = 30).
    int feed(const uint32_t *xrgb, int width, int height, bool mirror, float zoom, float sensitivity, int threshold,
             std::vector<uint8_t> &cells, int &cols, int &rows);
    const uint32_t *picture() const;                     // the 320x240 picture the game would see (after crop/mirror/zoom)

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace ion
