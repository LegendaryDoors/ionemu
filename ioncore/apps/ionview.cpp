// ionview: real-time SDL3 front end for ioncore. The mouse is the hand (motion while moving or left button held).
// usage: ionview <disc: .cue / .bin / .mdx / extracted dir> [entry=1] [--frames N] [--no-camera] [--adpcm36-q6] [--adpcm36-mame]
//        [--webcam] [--motion-frames] [--sensitivity X] [--gain dB] [--cam-zoom Z] [--adpcm36-lin c0,c1] [--adpcm36-table a0,..,a29] [--adpcm36-table2 b0,..,b29]
// keys: Esc quit, S screenshot (PPM in cwd), hold Tab = fast-forward x8, C = stand-in camera plane on/off, M = mute, +/- gain,
//       D = motion overlay (the cells the game's detector flags), [ / ] = camera zoom out / in,
//       F = video filter (composite -> sharp -> smooth), F5 = save state (ionview.state in cwd), F8 = load it,
//       W = webcam on/off (the camera picture becomes the camera plane and motion is detected from it).
// console buttons: Space/Enter = Play/Pause, Backspace = Stop, Right = Next,
//       Left = Previous, B = Brightness, O = Open (the game exits to the system via fw12; the viewer reboots the disc).
// Audio: an SDL3 stream at the SPU rate (SDL resamples); emulation is paced by the audio queue when a device is open,
// otherwise by a 60 Hz timer.
// Save flash (in-game progress) persists per disc in <IONCORE_SAVE_DIR or SDL pref path>/<disc dir name>.flash.
#include "ioncore/ioncore.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void write_ppm(const char *path, const uint32_t *px) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", ion::SCREEN_W, ion::SCREEN_H);
    for (int i = 0; i < ion::SCREEN_W * ion::SCREEN_H; i++) {
        const unsigned char c[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}
bool write_file(const char *path, const std::vector<uint8_t> &data) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}
std::vector<uint8_t> read_file(const char *path) {
    std::vector<uint8_t> data;
    if (FILE *f = fopen(path, "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    return data;
}
// Composite-video stand-in: horizontal [1 2 1]/4 smear per channel. The art's dithered screen-door transparency
// was designed to blend on a TV.
void composite(const uint32_t *src, uint32_t *dst) {
    for (int y = 0; y < ion::SCREEN_H; y++) {
        const uint32_t *s = src + y * ion::SCREEN_W;
        uint32_t *d = dst + y * ion::SCREEN_W;
        for (int x = 0; x < ion::SCREEN_W; x++) {
            const uint32_t a = s[x > 0 ? x - 1 : 0], b = s[x], c = s[x < ion::SCREEN_W - 1 ? x + 1 : x];
            uint32_t out = 0;
            for (int sh = 0; sh <= 16; sh += 8)
                out |= ((((a >> sh) & 0xff) + 2 * ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + 2) >> 2) << sh;
            d[x] = out;
        }
    }
}
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ionview <disc> [entry=1] [--frames N] [--no-camera] [--adpcm36-mame]\n"); return 1; }
    ion::Options opt;
    double gain_db = 12.0; bool start_overlay = false; long shot_at = -1;
    const int entry = argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : 1;
    long max_frames = -1;
    bool want_webcam = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        if (!strcmp(argv[i], "--no-camera")) opt.camera_mode = 0;
        if (!strcmp(argv[i], "--adpcm36-mame")) opt.adpcm36_model = 1;
        if (!strcmp(argv[i], "--adpcm36-q6")) opt.adpcm36_model = 0;       // the pre-2026-09-26d guess (a = 0.5 + idx/64)
        if (!strcmp(argv[i], "--spu-noint")) opt.spu_no_interpolation = true;
        if (!strcmp(argv[i], "--adpcm36-lin") && i + 1 < argc) { opt.adpcm36_model = 2; sscanf(argv[++i], "%f,%f", &opt.adpcm36_c0, &opt.adpcm36_c1); }
        if ((!strcmp(argv[i], "--adpcm36-table") || !strcmp(argv[i], "--adpcm36-table2")) && i + 1 < argc) {   // per-index predictor taps
            float *tab = argv[i][15] == '2' ? opt.adpcm36_table2 : opt.adpcm36_table;
            opt.adpcm36_model = 3;
            int n = 0; float last = 0.f;
            for (char *tok = strtok(argv[++i], ","); tok && n < 32; tok = strtok(nullptr, ",")) tab[n++] = last = (float)atof(tok);
            for (; n < 32; n++) tab[n] = last;
        }
        if (!strcmp(argv[i], "--webcam")) want_webcam = true;
        if (!strcmp(argv[i], "--motion-frames")) opt.motion_from_frames = true;
        if (!strcmp(argv[i], "--sensitivity") && i + 1 < argc) opt.motion_sensitivity = (float)atof(argv[++i]);
        if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain_db = atof(argv[++i]);
        if (!strcmp(argv[i], "--cam-zoom") && i + 1 < argc) opt.camera_zoom = (float)atof(argv[++i]);
        if (!strcmp(argv[i], "--overlay")) start_overlay = true;                                  // start with the D overlay on
        if (!strcmp(argv[i], "--shot-at") && i + 1 < argc) shot_at = atol(argv[++i]);            // write the composited window (PPM) at frame N
    }
    ion::Console con(opt);
    if (!con.open(argv[1])) { fprintf(stderr, "cannot open %s: %s\n", argv[1], con.open_error().c_str()); return 2; }
    fprintf(stderr, "disc: %s\n", con.title().empty() ? "(unknown title)" : con.title().c_str());

    // Save flash: <save dir>/<disc directory name>.flash, written atomically whenever the game changes it.
    std::string save_dir;
    if (const char *env = getenv("IONCORE_SAVE_DIR")) save_dir = std::string(env) + "/";
    else if (char *pref = SDL_GetPrefPath("ioncore", "ionview")) { save_dir = pref; SDL_free(pref); }
    std::string disc = argv[1];
    while (disc.size() > 1 && (disc.back() == '/' || disc.back() == '\\')) disc.pop_back();
    disc = disc.substr(disc.find_last_of("/\\") + 1);
    const std::string flash_path = save_dir + disc + ".flash";
    if (!con.load_flash(read_file(flash_path.c_str()))) fprintf(stderr, "ignoring bad save file %s\n", flash_path.c_str());
    auto persist_flash = [&] {
        if (!con.flash_dirty()) return;
        const std::string tmp = flash_path + ".tmp";
        if (write_file(tmp.c_str(), con.flash_image()) && std::rename(tmp.c_str(), flash_path.c_str()) == 0) {
            con.clear_flash_dirty();
            fprintf(stderr, "saved %s\n", flash_path.c_str());
        } else fprintf(stderr, "FAILED to write %s\n", flash_path.c_str());
    };
    if (!con.power_on(entry)) { fprintf(stderr, "cannot boot entry %d from %s\n", entry, argv[1]); return 2; }

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 3; }
    SDL_AudioStream *as = nullptr;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, (int)ion::AUDIO_RATE};
        as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (as) SDL_ResumeAudioStreamDevice(as);
    }
    if (!as) fprintf(stderr, "no audio device (%s): running silent\n", SDL_GetError());
    const int audio_target = (int)(ion::AUDIO_RATE * 0.05) * 4;              // keep ~50 ms queued (bytes)
    SDL_Window *win = SDL_CreateWindow("ioncore viewer", 960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    SDL_SetRenderLogicalPresentation(ren, ion::SCREEN_W, ion::SCREEN_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, ion::SCREEN_W, ion::SCREEN_H);
    enum { FILTER_COMPOSITE, FILTER_SHARP, FILTER_SMOOTH } filter = FILTER_COMPOSITE;
    static const char *filter_names[] = {"composite", "sharp", "smooth"};
    auto apply_scale_mode = [&] { SDL_SetTextureScaleMode(tex, filter == FILTER_SHARP ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR); };
    apply_scale_mode();

    std::vector<uint32_t> px(ion::SCREEN_W * ion::SCREEN_H), shown(px.size());

    // Webcam (SDL3 camera API): camera_mode 2 = external frames; motion then comes from the frame detector.
    SDL_Camera *webcam = nullptr;
    std::vector<uint32_t> campx;
    uint64_t webcam_frames = 0, webcam_t0 = 0; bool webcam_reported = false;
    double webcam_fps = 0; int hud_cells = -1;
    const int synth_mode = con.options().camera_mode;
    auto set_webcam = [&](bool on) {
        if (!on) {
            if (webcam) SDL_CloseCamera(webcam);
            webcam = nullptr;
            con.options().camera_mode = synth_mode;
            fprintf(stderr, "webcam off\n");
            return;
        }
        if (!SDL_WasInit(SDL_INIT_CAMERA) && !SDL_InitSubSystem(SDL_INIT_CAMERA)) { fprintf(stderr, "no camera support: %s\n", SDL_GetError()); return; }
        int n = 0;
        SDL_CameraID *ids = SDL_GetCameras(&n);
        // Ask for the device's native uncompressed mode nearest 640x480 at >= 25 fps and convert the frames ourselves.
        // With a null spec SDL 3.4 on Linux picked a 1280x720 MJPEG mode whose frames SDL_ConvertSurface cannot decode,
        // and SDL's own MJPEG/XRGB delivery produced garbage on a Logitech C270; native YUY2 converts correctly.
        SDL_CameraSpec want{}; bool have = false;
        if (ids && n > 0) {
            int m = 0; SDL_CameraSpec **specs = SDL_GetCameraSupportedFormats(ids[0], &m);
            double best = 1e18;
            for (int k = 0; specs && k < m; k++) {
                const SDL_CameraSpec &s = *specs[k];
                if (s.format == SDL_PIXELFORMAT_MJPG || s.format == SDL_PIXELFORMAT_UNKNOWN) continue;
                const double fps = s.framerate_denominator ? (double)s.framerate_numerator / s.framerate_denominator : 0;
                const double cost = std::abs(s.width * s.height - 640 * 480) / 1000.0 + (fps < 25 ? 1e6 : 0) + (60 - std::min(fps, 60.0)) * 10;
                if (cost < best) { best = cost; want = s; have = true; }
            }
            SDL_free(specs);
            if (have) webcam = SDL_OpenCamera(ids[0], &want);
            if (!webcam) {
                want = SDL_CameraSpec{}; want.format = SDL_PIXELFORMAT_XRGB8888; want.colorspace = SDL_COLORSPACE_SRGB;
                want.width = 640; want.height = 480; want.framerate_numerator = 30; want.framerate_denominator = 1;
                webcam = SDL_OpenCamera(ids[0], &want);
            }
            if (!webcam) webcam = SDL_OpenCamera(ids[0], nullptr);
        }
        if (webcam) fprintf(stderr, "webcam: %s (waiting for permission if the OS asks)\n", SDL_GetCameraName(ids[0]));
        else fprintf(stderr, "no webcam found (%s)\n", n ? SDL_GetError() : "0 devices");
        SDL_free(ids);
        if (webcam) con.options().camera_mode = 2;
        webcam_frames = 0; webcam_t0 = SDL_GetTicksNS(); webcam_reported = false;
    };
    auto poll_webcam = [&] {
        if (!webcam || SDL_GetCameraPermissionState(webcam) != SDL_CAMERA_PERMISSION_STATE_APPROVED) return;
        Uint64 ts = 0;
        SDL_Surface *frame = SDL_AcquireCameraFrame(webcam, &ts);
        if (!frame) return;
        for (;;) {                                                           // drain: keep only the newest pending frame
            Uint64 ts2 = 0; SDL_Surface *next = SDL_AcquireCameraFrame(webcam, &ts2);
            if (!next) break;
            SDL_ReleaseCameraFrame(webcam, frame); frame = next; webcam_frames++;
        }
        if (webcam_frames++ == 0) {
            SDL_CameraSpec got{}; SDL_GetCameraFormat(webcam, &got);
            fprintf(stderr, "webcam format: %s %dx%d @ %d/%d fps\n", SDL_GetPixelFormatName(got.format), got.width, got.height, got.framerate_numerator, got.framerate_denominator);
            if (frame->format == SDL_PIXELFORMAT_MJPG) fprintf(stderr, "webcam: compressed frames cannot be converted; the camera plane will be garbage\n");
            webcam_t0 = SDL_GetTicksNS();
        } else if (!webcam_reported && SDL_GetTicksNS() - webcam_t0 > 3000000000ull) {
            const double fps = (webcam_frames - 1) * 1e9 / (double)(SDL_GetTicksNS() - webcam_t0);
            fprintf(stderr, "webcam: %.1f frames/s%s\n", fps, fps < 25 ? " (low: add light, or on Linux `v4l2-ctl --set-ctrl=exposure_dynamic_framerate=0`)" : "");
            webcam_reported = true; webcam_fps = fps;
        }
        if (SDL_Surface *conv = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_XRGB8888)) {
            campx.resize((size_t)conv->w * conv->h);
            for (int y = 0; y < conv->h; y++)
                memcpy(&campx[(size_t)y * conv->w], static_cast<uint8_t *>(conv->pixels) + (size_t)y * conv->pitch, (size_t)conv->w * 4);
            con.set_camera_frame(campx.data(), conv->w, conv->h, true);
            SDL_DestroySurface(conv);
        }
        SDL_ReleaseCameraFrame(webcam, frame);
    };
    if (want_webcam) set_webcam(true);
    int64_t last_move = -1000;
    bool quit = false, button = false, mute = false, overlay = start_overlay;
    if (overlay) con.options().trace_motion_reads = true;
    std::vector<uint8_t> cells; int mcols = 0, mrows = 0;
    std::vector<uint16_t> read_words; std::vector<float> read_heat(600, 0.f);   // D overlay: cells the game polled recently
    // Master gain: the SPU output peaks around -16 dBFS on the discs; real-console captures sit 4-12 dB above the raw
    // mix, so the viewer applies +12 dB by default with clamping (+/- keys, --gain dB).
    auto gain_q12 = [&] { return (int)std::lround(std::pow(10.0, gain_db / 20.0) * 4096.0); };
    ion::Input &in = con.input();
    while (!quit && (max_frames < 0 || (long)con.frame() < max_frames)) {
        if (con.stopped()) {                                                 // fw12 (Open): power-cycle, boot the disc
            fprintf(stderr, "fw12: exit to system -> rebooting disc (entry 1)\n");
            persist_flash();
            con.power_on(1);
            last_move = -1000;
        }
        const uint64_t t0 = SDL_GetTicksNS();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: quit = true; break;
                case SDLK_S: {
                    char path[64]; snprintf(path, sizeof path, "frame_%05llu.ppm", (unsigned long long)con.frame());
                    con.render(px.data()); write_ppm(path, px.data()); fprintf(stderr, "screenshot %s\n", path); break; }
                case SDLK_C: con.options().camera_mode ^= 1; break;
                case SDLK_M: mute = !mute; break;
                case SDLK_D:
                    overlay = !overlay; con.options().trace_motion_reads = overlay; std::fill(read_heat.begin(), read_heat.end(), 0.f);
                    fprintf(stderr, "motion overlay %s\n", overlay ? "on: blue = cells the game is reading, red = cells flagged as moving" : "off"); break;
                case SDLK_LEFTBRACKET: con.options().camera_zoom = std::max(0.3f, con.options().camera_zoom - 0.05f); fprintf(stderr, "camera zoom %.2f\n", con.options().camera_zoom); break;
                case SDLK_RIGHTBRACKET: con.options().camera_zoom = std::min(2.0f, con.options().camera_zoom + 0.05f); fprintf(stderr, "camera zoom %.2f\n", con.options().camera_zoom); break;
                case SDLK_EQUALS: case SDLK_KP_PLUS: gain_db = std::min(30.0, gain_db + 1); fprintf(stderr, "gain %+.0f dB\n", gain_db); break;
                case SDLK_MINUS: case SDLK_KP_MINUS: gain_db = std::max(-30.0, gain_db - 1); fprintf(stderr, "gain %+.0f dB\n", gain_db); break;
                case SDLK_F:
                    filter = (decltype(filter))((filter + 1) % 3); apply_scale_mode();
                    fprintf(stderr, "video filter: %s\n", filter_names[filter]); break;
                case SDLK_F5: {
                    const auto st = con.save_state();
                    fprintf(stderr, "%s ionview.state (%zu KB, frame %llu)\n", write_file("ionview.state", st) ? "saved" : "FAILED to save",
                            st.size() / 1024, (unsigned long long)con.frame());
                    break; }
                case SDLK_W: set_webcam(webcam == nullptr); break;
                case SDLK_F8:
                    fprintf(stderr, con.load_state(read_file("ionview.state")) ? "loaded ionview.state\n" : "no usable ionview.state\n");
                    last_move = -1000;
                    break;
                default: break;
                }
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) button = true;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP) button = false;
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                float lx, ly; SDL_RenderCoordinatesFromWindow(ren, ev.motion.x, ev.motion.y, &lx, &ly);
                in.hand_x = std::clamp((int)lx, 0, ion::SCREEN_W - 1); in.hand_y = std::clamp((int)ly, 0, ion::SCREEN_H - 1);
                last_move = (int64_t)con.frame();
            }
        }
        in.hand_active = button || (int64_t)con.frame() - last_move < 8;
        const bool *keys = SDL_GetKeyboardState(nullptr);
        const bool ff = keys[SDL_SCANCODE_TAB];
        in.buttons = (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN] ? ion::BTN_PLAY_PAUSE : 0) |
                     (keys[SDL_SCANCODE_BACKSPACE] ? ion::BTN_STOP : 0) | (keys[SDL_SCANCODE_RIGHT] ? ion::BTN_NEXT : 0) |
                     (keys[SDL_SCANCODE_LEFT] ? ion::BTN_PREVIOUS : 0) | (keys[SDL_SCANCODE_B] ? ion::BTN_BRIGHTNESS : 0) |
                     (keys[SDL_SCANCODE_O] ? ion::BTN_OPEN : 0);
        poll_webcam();
        for (int k = 0; k < (ff ? 8 : 1); k++) con.step_frame();
        auto &audio = con.audio();
        if (as && !ff) {
            if (mute) std::fill(audio.begin(), audio.end(), 0);
            else if (const int gq = gain_q12(); gq != 4096) for (auto &v : audio) v = (int16_t)std::clamp(((int)v * gq) >> 12, -32768, 32767);
            SDL_PutAudioStreamData(as, audio.data(), (int)(audio.size() * sizeof(int16_t)));
        }
        audio.clear();
        con.render(px.data());
        if (filter == FILTER_COMPOSITE) composite(px.data(), shown.data());
        SDL_UpdateTexture(tex, nullptr, (filter == FILTER_COMPOSITE ? shown : px).data(), ion::SCREEN_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, nullptr, nullptr);
        if (overlay) {                                                       // what the game's motion detector sees
            const int n = con.motion_cells(cells, mcols, mrows);
            con.motion_reads(read_words);                                    // which map words the game polled this frame
            for (int i = 0; i < 600; i++) read_heat[i] = std::max(read_heat[i] * 0.93f, read_words[i] ? 1.f : 0.f);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            for (int i = 0; i < 600; i++)
                if (read_heat[i] > 0.1f) {                                   // word i = row i/20, cells 2*(i%20), 2*(i%20)+1 (8x8 grid)
                    SDL_SetRenderDrawColor(ren, 40, 120, 255, (Uint8)(90 * read_heat[i]));
                    const SDL_FRect q{(float)((i % 20) * 16), (float)((i / 20) * 8), 16, 8}; SDL_RenderFillRect(ren, &q);
                }
            if (n >= 0) {
                const float cw = (float)ion::SCREEN_W / mcols, ch = (float)ion::SCREEN_H / mrows;
                SDL_SetRenderDrawColor(ren, 255, 40, 40, 120);
                for (int r = 0; r < mrows; r++)
                    for (int c = 0; c < mcols; c++)
                        if (cells[(size_t)r * mcols + c]) { const SDL_FRect q{c * cw, r * ch, cw, ch}; SDL_RenderFillRect(ren, &q); }
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            }
            if (con.frame() % 60 == 0) fprintf(stderr, n >= 0 ? "motion: %d of %d cells flagged\n" : "motion detector off (the game is not reading the camera right now)\n", n, mcols * mrows);
            hud_cells = n;
            // legend + status (SDL's 8x8 debug font, in the 320x240 logical space)
            bool polling = false; for (float v : read_heat) if (v > 0.5f) { polling = true; break; }
            char l1[48], l2[48], l3[48], l4[48];                            // 40 columns of 8-px font fit the 320-px screen
            snprintf(l1, sizeof l1, "BLUE=game looks here RED=movement seen");
            snprintf(l2, sizeof l2, "%s | cam %.0ffps", n < 0 ? "game not reading camera" : polling ? "game polling blue boxes" : "on, game not polling", webcam_fps);
            snprintf(l3, sizeof l3, "sens %.1f zoom %.2f gain %+.0fdB", con.options().motion_sensitivity, con.options().camera_zoom, gain_db);
            snprintf(l4, sizeof l4, "%s", n < 0 ? "Head near the top of the picture" : polling ? "Move only in blue boxes; still=hide" : "Red = what you are doing now");
            const char *lines[4] = {l1, l2, l3, l4};
            for (int i = 0; i < 4; i++) {
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 200); SDL_RenderDebugText(ren, 3, 3 + 10 * i, lines[i]);
                SDL_SetRenderDrawColor(ren, 255, 255, 120, 255); SDL_RenderDebugText(ren, 2, 2 + 10 * i, lines[i]);
            }
            if (n >= 0) {
                char l5[48]; snprintf(l5, sizeof l5, "%d cells moving", n);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 200); SDL_RenderDebugText(ren, 3, ion::SCREEN_H - 9, l5);
                SDL_SetRenderDrawColor(ren, 255, 120, 120, 255); SDL_RenderDebugText(ren, 2, ion::SCREEN_H - 10, l5);
            }
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        }
        if (!webcam) {                                                       // mouse cursor: game layers may hide the plane
            const SDL_FRect hand{(float)in.hand_x - 10, (float)in.hand_y - 10, 20, 20};
            SDL_SetRenderDrawColor(ren, in.hand_active ? 255 : 120, in.hand_active ? 220 : 120, 0, 255);
            SDL_RenderRect(ren, &hand);
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        if (shot_at >= 0 && (long)con.frame() == shot_at) {                                       // composited screenshot
            if (SDL_Surface *sf = SDL_RenderReadPixels(ren, nullptr)) {
                if (SDL_Surface *cv = SDL_ConvertSurface(sf, SDL_PIXELFORMAT_XRGB8888)) {
                    std::vector<uint32_t> spx((size_t)cv->w * cv->h);
                    for (int y = 0; y < cv->h; y++) memcpy(&spx[(size_t)y * cv->w], (uint8_t *)cv->pixels + (size_t)y * cv->pitch, (size_t)cv->w * 4);
                    FILE *f = fopen("ionview_shot.ppm", "wb");
                    if (f) { fprintf(f, "P6\n%d %d\n255\n", cv->w, cv->h); for (auto p : spx) { unsigned char c[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p}; fwrite(c, 1, 3, f); } fclose(f); }
                    SDL_DestroySurface(cv);
                }
                SDL_DestroySurface(sf);
                fprintf(stderr, "wrote ionview_shot.ppm\n");
            }
        }
        SDL_RenderPresent(ren);
        for (auto &e : con.events()) fprintf(stderr, "%s\n", e.c_str());
        con.events().clear();
        if (con.frame() % 300 == 0) persist_flash();
        if (as) { while (!ff && SDL_GetAudioStreamQueued(as) > audio_target) SDL_DelayNS(1000000); }
        else { const uint64_t dt = SDL_GetTicksNS() - t0, budget = 16666667; if (!ff && dt < budget) SDL_DelayNS(budget - dt); }
    }
    persist_flash();
    if (webcam) SDL_CloseCamera(webcam);
    if (as) SDL_DestroyAudioStream(as);
    SDL_Quit();
    return 0;
}
