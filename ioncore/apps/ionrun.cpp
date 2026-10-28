// ionrun: headless ioncore runner (event trace, PPM snapshots, WAV capture, scripted input, per-file checks).
// usage: ionrun <disc> [entry=1] [frames=600] [--no-data-walk] [--trace-api] [--hand-scan] [--no-camera]
//        [--png-dir DIR --png-at F1,F2,...] [--dump-ram FILE] [--wav FILE] [--adpcm36-q6] [--adpcm36-mame] [--adpcm36-lin c0,c1]
//        [--adpcm36-table a0,...,a29] [--adpcm36-table2 b0,...,b29] [--press BITS@FRAME[+N]]...
//        [--state-roundtrip FRAME]   save at FRAME, run to the end, reload, re-run, and compare RAM/audio/picture
//        [--flash FILE]              save-flash image: loaded before power-on if it exists, written back if changed
//        [--motion-frames]           motion from the (synthetic) camera frames via the SoC's frame detector
// <disc>: a .cue, a raw image with the data track (.bin/.img/.mdx), or a directory of extracted programs.
// ioncore/tests/golden.py drives it.
#include "ioncore/ioncore.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

namespace {
void write_ppm(const std::string &path, const uint32_t *px) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", ion::SCREEN_W, ion::SCREEN_H);
    for (int i = 0; i < ion::SCREEN_W * ion::SCREEN_H; i++) {
        const unsigned char c[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}
void wav_header(FILE *f, uint32_t frames) {                     // 16-bit stereo PCM at the SPU rate
    const uint32_t rate = (uint32_t)ion::AUDIO_RATE, bytes = frames * 4, riff = 36 + bytes, fmt = 16, brate = rate * 4;
    const uint16_t pcm = 1, chans = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&chans, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}
uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) { h ^= static_cast<const uint8_t *>(p)[i]; h *= 1099511628211ull; }
    return h;
}
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ionrun <disc> [entry=1] [frames=600] [options] (see source)\n"); return 1; }
    ion::Options opt;
    opt.debug_stats = true;
    const int entry = argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : 1;
    const uint64_t frames = argc > 3 && argv[3][0] != '-' ? strtoull(argv[3], nullptr, 10) : 600;
    const char *ramdump = nullptr, *wav_path = nullptr;
    std::string snap_dir = ".";
    std::set<uint64_t> snap_frames;
    std::vector<std::tuple<uint64_t, uint64_t, uint16_t>> presses;   // (from, to, bits)
    bool hand_scan = false; int hand_x = -1, hand_y = -1; const char *map_trace_path = nullptr;
    long long roundtrip = -1;
    const char *flash_path = nullptr;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-data-walk")) opt.data_walk = false;
        if (!strcmp(argv[i], "--trace-api")) opt.trace_events = true;
        if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc) ramdump = argv[++i];
        if (!strcmp(argv[i], "--hand-scan")) hand_scan = true;
        if (!strcmp(argv[i], "--hand") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &hand_x, &hand_y);   // fixed virtual-hand position
        if (!strcmp(argv[i], "--trace-map-reads") && i + 1 < argc) { map_trace_path = argv[++i]; opt.trace_motion_reads = true; }
        if (!strcmp(argv[i], "--no-camera")) opt.camera_mode = 0;
        if (!strcmp(argv[i], "--motion-frames")) opt.motion_from_frames = true;
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        if (!strcmp(argv[i], "--adpcm36-mame")) opt.adpcm36_model = 1;
        if (!strcmp(argv[i], "--adpcm36-q6")) opt.adpcm36_model = 0;       // the pre-2026-09-26d guess (a = 0.5 + idx/64)
        if (!strcmp(argv[i], "--spu-noint")) opt.spu_no_interpolation = true;
        if (!strcmp(argv[i], "--adpcm36-lin") && i + 1 < argc) { opt.adpcm36_model = 2; sscanf(argv[++i], "%f,%f", &opt.adpcm36_c0, &opt.adpcm36_c1); }
        if (!strcmp(argv[i], "--adpcm36-table") && i + 1 < argc) {           // a0,a1,...,a29 (missing entries keep the last)
            opt.adpcm36_model = 3;
            int n = 0; float last = 0.5f;
            for (char *tok = strtok(argv[++i], ","); tok && n < 32; tok = strtok(nullptr, ",")) opt.adpcm36_table[n++] = last = (float)atof(tok);
            for (; n < 32; n++) opt.adpcm36_table[n] = last;
        }
        if ((!strcmp(argv[i], "--adpcm36-table3") || !strcmp(argv[i], "--adpcm36-gain")) && i + 1 < argc) {   // experimental taps
            float *tab = argv[i][10] == 'g' ? opt.adpcm36_gain : opt.adpcm36_table3;
            opt.adpcm36_model = 3;
            int n = 0; float last = argv[i][10] == 'g' ? 1.f : 0.f;
            for (char *tok = strtok(argv[++i], ","); tok && n < 32; tok = strtok(nullptr, ",")) tab[n++] = last = (float)atof(tok);
            for (; n < 32; n++) tab[n] = last;
        }
        if (!strcmp(argv[i], "--adpcm36-dequant") && i + 1 < argc) {         // q0,...,q15: code -> residual, nibble units (model 3)
            opt.adpcm36_model = 3;
            int n = 0;
            for (char *tok = strtok(argv[++i], ","); tok && n < 16; tok = strtok(nullptr, ",")) opt.adpcm36_dequant[n++] = (float)atof(tok);
        }
        if (!strcmp(argv[i], "--adpcm36-table2") && i + 1 < argc) {          // second tap b0,...,b29 (model 3)
            opt.adpcm36_model = 3;
            int n = 0; float last = 0.f;
            for (char *tok = strtok(argv[++i], ","); tok && n < 32; tok = strtok(nullptr, ",")) opt.adpcm36_table2[n++] = last = (float)atof(tok);
            for (; n < 32; n++) opt.adpcm36_table2[n] = last;
        }
        if (!strcmp(argv[i], "--state-roundtrip") && i + 1 < argc) roundtrip = atoll(argv[++i]);
        if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_path = argv[++i];
        if (!strcmp(argv[i], "--press") && i + 1 < argc) {                   // BITS@FRAME[+FRAMES], e.g. 0x80@1500+10
            unsigned bits = 0; unsigned long long f = 0, dur = 10;
            sscanf(argv[++i], "%i@%llu+%llu", &bits, &f, &dur);
            presses.push_back({f, f + dur, (uint16_t)bits});
        }
        if (!strcmp(argv[i], "--png-dir") && i + 1 < argc) snap_dir = argv[++i];
        if (!strcmp(argv[i], "--png-at") && i + 1 < argc) {
            char *t = strtok(argv[++i], ",");
            while (t) { snap_frames.insert(strtoull(t, nullptr, 10)); t = strtok(nullptr, ","); }
        }
    }

    ion::Console con(opt);
    if (!con.open(argv[1])) { fprintf(stderr, "cannot open %s: %s\n", argv[1], con.open_error().c_str()); return 2; }
    std::vector<uint32_t> px(ion::SCREEN_W * ion::SCREEN_H);
    con.on_frame = [&] {
        if (!snap_frames.count(con.frame())) return;
        con.render(px.data());
        char path[512]; snprintf(path, sizeof path, "%s/frame_%05llu.ppm", snap_dir.c_str(), (unsigned long long)con.frame());
        write_ppm(path, px.data());
        con.log(std::string("snapshot ") + path);
    };
    if (flash_path) {
        std::vector<uint8_t> img;
        if (FILE *f = fopen(flash_path, "rb")) {
            uint8_t buf[65536]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) img.insert(img.end(), buf, buf + n);
            fclose(f);
        }
        if (!con.load_flash(img)) { fprintf(stderr, "bad flash image %s\n", flash_path); return 2; }
    }
    if (!con.power_on(entry)) return 2;
    con.input().hand_scan = hand_scan;
    if (hand_x >= 0) { con.input().hand_x = hand_x; con.input().hand_y = hand_y; }
    FILE *map_trace = map_trace_path ? fopen(map_trace_path, "w") : nullptr;   // one line per frame: frame, words read, 600 counts
    std::vector<uint16_t> map_words;
    auto prev_on_frame = con.on_frame;
    if (map_trace) con.on_frame = [&] {
        if (prev_on_frame) prev_on_frame();
        const int n = con.motion_reads(map_words);
        if (n) { fprintf(map_trace, "%llu %d", (unsigned long long)con.frame(), n); for (auto w : map_words) fprintf(map_trace, " %u", w); fputc('\n', map_trace); }
    };

    FILE *wav = wav_path ? fopen(wav_path, "wb") : nullptr;
    uint32_t wav_frames = 0;
    if (wav) wav_header(wav, 0);
    auto run_to = [&](uint64_t end, uint64_t *hash) {
        while (con.frame() < end && !con.stopped()) {
            if (!presses.empty()) {
                uint16_t b = 0;
                for (auto &[f0, f1, bits] : presses) if (con.frame() >= f0 && con.frame() < f1) b |= bits;
                con.input().buttons = b;
            }
            con.step_frame();
            auto &a = con.audio();
            if (hash) {
                *hash = fnv(a.data(), a.size() * 2, *hash);
                if (con.frame() % 97 == 0) { con.render(px.data()); *hash = fnv(px.data(), px.size() * 4, *hash); }
            }
            if (wav) { fwrite(a.data(), 2, a.size(), wav); wav_frames += a.size() / 2; }
            a.clear();
        }
    };

    if (roundtrip >= 0) {                                   // save-state determinism self-test
        run_to((uint64_t)roundtrip, nullptr);
        const std::vector<uint8_t> st = con.save_state();
        uint64_t h1 = 1469598103934665603ull, h2 = h1;
        run_to(frames, &h1);
        h1 = fnv(con.ram(), 0x400000 * 2, h1);
        if (!con.load_state(st)) { fprintf(stderr, "load_state failed\n"); return 3; }
        run_to(frames, &h2);
        h2 = fnv(con.ram(), 0x400000 * 2, h2);
        printf("== state round trip at frame %lld (%zu bytes): %s (%016llx vs %016llx)\n", roundtrip, st.size(),
               h1 == h2 ? "IDENTICAL" : "DIFFERENT", (unsigned long long)h1, (unsigned long long)h2);
        return h1 == h2 ? 0 : 4;
    }
    run_to(frames, nullptr);
    if (wav) { wav_header(wav, wav_frames); fclose(wav); }
    if (flash_path && con.flash_dirty()) {
        const std::vector<uint8_t> img = con.flash_image();
        if (FILE *f = fopen(flash_path, "wb")) { fwrite(img.data(), 1, img.size(), f); fclose(f); }
        fprintf(stderr, "flash: wrote %zu bytes to %s\n", img.size(), flash_path);
    }

    const ion::DebugInfo di = con.debug_info();
    printf("== disc: %s (%d entries, SHA-1 %s)\n", con.title().empty() ? "unknown title" : con.title().c_str(), con.entry_count(),
           con.disc_sha1().empty() ? "n/a" : con.disc_sha1().c_str());
    printf("== ioncore run: %s entry %d, frames=%llu insns=%llu data_walk=%d stopped=%d\n", argv[1], entry,
           (unsigned long long)con.frame(), (unsigned long long)di.instructions, opt.data_walk, con.stopped());
    printf("== final PC=%06x SP=%04x SR=%04x irq_en=%u\n", di.pc, di.sp, di.sr, di.irq_enabled);
    printf("== events (%zu):\n", con.events().size());
    for (size_t i = 0, shown = 0; i < con.events().size() && shown < 60; i++) {
        const std::string &e = con.events()[i];
        if (e.find("fw18") != std::string::npos || e.find("fw19") != std::string::npos || e.find("fw20") != std::string::npos) continue;
        printf("  %s\n", e.c_str()); shown++;
    }
    printf("== per-file checks:\n");
    for (auto &f : di.files) {
        std::string lines;
        for (auto &[l, n] : f.irq_lines) lines += (l ? " IRQ" + std::to_string(l - 1) : std::string(" FIQ")) + "x" + std::to_string(n);
        printf("  entry %2d %-28s min SP=%04x  motion map=%04x-%04x  IRQs=%llu (while [0010]==0: %llu)%s\n", f.entry, f.file.c_str(),
               f.min_sp, f.motion_map, f.motion_map ? f.motion_map + 599 : 0, (unsigned long long)f.irqs, (unsigned long long)f.irqs_gate0, lines.c_str());
    }
    printf("== SPU: channels used mask=%06x, peak simultaneous voices=%d, starts pcm8=%u pcm16=%u ima=%u adpcm36=%u\n",
           di.spu_channels_used, di.spu_peak_voices, di.spu_starts[0], di.spu_starts[1], di.spu_starts[2], di.spu_starts[3]);
    printf("== API calls:"); for (auto &[k, n] : di.api_calls) printf(" fw%02d x%llu", k, (unsigned long long)n); printf("\n");
    printf("== hottest PC buckets (256 words):");
    for (size_t i = 0; i < di.hot_pc.size() && i < 6; i++) printf(" %06x:%llu", di.hot_pc[i].first, (unsigned long long)di.hot_pc[i].second);
    printf("\n== I/O registers (excluding PPU/SPU RAM arrays 2900-33FF):");
    size_t arrays = 0;
    for (auto &[a, r, w] : di.io_access) {
        if (a >= 0x2900 && a < 0x3400) { arrays++; continue; }
        printf(" %04x(r%llu/w%llu)", a, (unsigned long long)r, (unsigned long long)w);
    }
    printf("\n== (+%zu array words touched)\n", arrays);
    if (ramdump) { FILE *f = fopen(ramdump, "wb"); if (f) { fwrite(con.ram(), 2, 0x2800, f); fclose(f); } }
    return 0;
}
