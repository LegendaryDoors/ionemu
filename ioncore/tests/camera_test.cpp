// External-camera test: feed ion::Console 640x480 "webcam" frames (camera view, unmirrored) of a dark room with a
// waving bright hand that sweeps the 4x3 grid used by --hand-scan. Motion must come only from the frames
// (camera_mode 2 -> the SoC's frame detector), and Blue's Room's menu (entry 3) must start a mini-game (fw03).
// usage: ioncore_camera_test <blues extract dir or image> [frames=1500]   (skips with exit 0 if the content is absent)
#include "ioncore/ioncore.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ioncore_camera_test <disc> [frames]\n"); return 2; }
    const int frames = argc > 2 ? atoi(argv[2]) : 1500;
    ion::Options opt;
    opt.camera_mode = 2;
    ion::Console con(opt);
    if (!con.open(argv[1])) { printf("SKIP: %s\n", con.open_error().c_str()); return 0; }
    if (!con.power_on(3)) { printf("SKIP: no entry 3\n"); return 0; }
    const int W = 640, H = 480;
    std::vector<uint32_t> cam(W * H);
    static const int xs[4] = {40, 120, 200, 280}, ys[3] = {60, 120, 180};   // screen-space hand positions
    for (int f = 0; f < frames; f++) {
        const int slot = (f / 180) % 12, sx = xs[slot % 4] + (((f >> 1) & 1) ? 6 : -6), sy = ys[slot / 4];
        const int cx = (ion::SCREEN_W - 1 - sx) * W / ion::SCREEN_W, cy = sy * H / ion::SCREEN_H;   // camera view is unmirrored
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int dx = x - cx, dy = y - cy;
                cam[y * W + x] = dx * dx + dy * dy < 30 * 30 ? 0xE0B090 : 0x303848;   // skin-toned hand on a dark wall
            }
        con.set_camera_frame(cam.data(), W, H, true);
        con.step_frame();
        con.audio().clear();
        for (auto &e : con.events())
            if (e.find("fw03(") != std::string::npos) { printf("OK: %s\n", e.c_str()); return 0; }
        con.events().clear();
    }
    printf("FAIL: no mini-game started from camera motion in %d frames\n", frames);
    return 1;
}
