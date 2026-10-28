// Multi-instance / save-state test: two Consoles stepped alternately in one process must stay bit-identical (no
// shared global state), and a third Console restored from the first one's save state must follow in lockstep.
// usage: ioncore_instance_test <extract_dir> [frames=3000]    (skips with exit 0 when the directory has no content)
#include "ioncore/ioncore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) { h ^= static_cast<const uint8_t *>(p)[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t digest(ion::Console &c) {                          // iRAM + this frame's audio; whole memory every 100 frames
    uint64_t h = fnv(c.ram(), c.frame() % 100 == 0 ? 0x400000 * 2 : 0x2800 * 2);
    h = fnv(c.audio().data(), c.audio().size() * 2, h);
    c.audio().clear();
    return h;
}
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ioncore_instance_test <extract_dir> [frames]\n"); return 2; }
    const int frames = argc > 2 ? atoi(argv[2]) : 3000;
    ion::Console a, b;
    if (!a.open_extracted(argv[1]) || !b.open_extracted(argv[1])) { printf("SKIP: no content in %s\n", argv[1]); return 0; }
    a.power_on(1); b.power_on(1);
    a.input().hand_scan = b.input().hand_scan = true;
    std::unique_ptr<ion::Console> c;
    for (int f = 0; f < frames; f++) {
        a.step_frame(); b.step_frame();
        if (c) c->step_frame();
        const uint64_t ha = digest(a), hb = digest(b);
        if (ha != hb) { printf("FAIL: instances diverged at frame %d\n", f); return 1; }
        if (c && digest(*c) != ha) { printf("FAIL: restored instance diverged at frame %d\n", f); return 1; }
        if (f == frames / 2) {                                // clone a into c through a save state
            c = std::make_unique<ion::Console>();
            c->open_extracted(argv[1]);
            if (!c->load_state(a.save_state())) { printf("FAIL: load_state rejected a fresh state\n"); return 1; }
            c->audio().clear();
        }
    }
    std::vector<uint8_t> bogus = a.save_state();
    bogus.resize(bogus.size() / 2);                            // truncated state must be rejected and leave a intact
    const uint64_t before = fnv(a.ram(), 0x400000 * 2);
    if (a.load_state(bogus) || fnv(a.ram(), 0x400000 * 2) != before) { printf("FAIL: truncated state not rejected cleanly\n"); return 1; }
    printf("OK: 2 interleaved instances + 1 restored instance identical over %d frames\n", frames);
    return 0;
}
