# ioncore

The Hasbro iON emulation core is a C++20 static library with three command-line clients:
- **`ionrun`**: headless; event trace, snapshots, WAV, scripted input, save-state self-test.
- **`ionview`**: SDL3 window, mouse = hand, audio, save states.
- **`ionrepair`**: repairs a damaged dump from donor images (`ionrepair <image> <out_dir> [donors...]`; `ion::repair_disc`).

Its output is checked by `tests/golden.py` against recorded hashes (see below).

## Build

```sh
cmake -S ioncore -B ioncore/build -G Ninja      # Release by default; ionview needs SDL3 (find_package(SDL3 CONFIG))
cmake --build ioncore/build
(cd ioncore/build && ctest)                      # multi-instance + save-state test; skips unless -DIONCORE_TEST_EXTRACT points at your Blue's Room disc
python3 ioncore/tests/golden.py --runner ioncore/build/ionrun --compare --images   # golden regression over the discs you have (ION_DUMPS=folder), ~20 s
```

`add_subdirectory(ioncore)` from another project (the launcher in `frontend/`) builds only the library:
`IONCORE_BUILD_TOOLS` defaults to ON only when ioncore is the top-level project.

`src/cpu/unsp/` is generated from MAME's u'nSP interpreter by `python3 src/cpu/port_unsp.py`. It uses the cached upstream copy in `src/cpu/mame_orig/`; add `--fetch` to refresh from GitHub.

## Run

```sh
ioncore/build/ionview "path/to/disc.cue"                # .cue/.bin/.mdx
ioncore/build/ionview path/to/extracted-disc/        # a directory of extracted programs; optional 2nd arg = entry
ioncore/build/ionrun  "path/to/disc.cue" 1 9000 --trace-api --png-dir snaps --png-at 600,8500
```

**Viewer keys:**
- mouse = hand;
- Tab = fast-forward;
- S = screenshot;
- C = camera plane;
- M = mute;
- F = filter (composite/sharp/smooth);
- **F5 / F8 = save / load state** (`ionview.state`);
- **ADPCM36 (Blue's/Dora voices):** the default decoder is a per-index two-tap table fitted to real-console recordings; `--adpcm36-q6`, `--adpcm36-mame` and `--adpcm36-lin` select the earlier models for comparison;
- **Volume:** the core emits the raw SPU mix (peaks ≈ −16 dBFS on the discs; real-console captures sit 4-12 dB higher), so the viewer applies **+12 dB** by default with clamping; `--gain dB` sets it, `+`/`-` step it, `M` mutes. `ionrun --wav` stays raw;
- **D = motion overlay:** paints the cells the game is currently polling (blue, fading) and the cells its detector flags as moving (red), with an on-screen legend, status (polling / not reading the camera, camera fps, sensitivity, zoom, gain) and a positioning tip; `--overlay` starts with it on, `--shot-at N` writes the composited window as `ionview_shot.ppm` at frame N;
- **Camera zoom:** `--cam-zoom 0.7` (or `[`/`]` while playing) shrinks the webcam picture into the frame so a player close to the camera looks as far away as the console expects (the console wanted the child 3+ feet back with the head near the top of the picture); values above 1 crop in;
- **W = webcam** on/off, or start with `--webcam`. The camera picture becomes the camera layer and motion is detected from it; `--sensitivity X` tunes the threshold, and `--motion-frames` runs the detector on the synthetic player; the viewer asks the camera for its uncompressed mode nearest 640×480 (MJPEG modes can't be converted) and prints the measured frame rate: below 25 fps the games' dwell timing suffers, so add light or, on Linux, `v4l2-ctl -d /dev/video0 --set-ctrl=exposure_dynamic_framerate=0`;
- **in-game saves** (the console's save flash) persist automatically per disc in `$IONCORE_SAVE_DIR` or the per-user data directory (`~/.local/share/ioncore/ionview/<disc>.flash` on Linux; the launcher keeps its own under `ionemu/`);
- Esc = quit.

**Console buttons:**
- Space/Enter = Play/Pause;
- Backspace = Stop;
- →/← = Next/Previous;
- B = Brightness;
- O = Open, which reboots the disc.

## Layout

| Path | What |
| --- | --- |
| `include/ioncore/ioncore.h` | **public API**: `ion::Console` (content, power-on, `step_frame`, input, render, audio, events, save states, debug info), `ion::identify_disc` (SHA-1 + title without a Console), `ion::repair_disc` and `ion::MotionPreview` (the frame detector for camera setup screens), for launchers. No MAME headers leak. |
| `src/core.{h,cpp}` | `Core`: memory, CPU wiring (per-instance bus and hooks), the frame loop, save states, and the `Console` implementation |
| `src/soc.cpp` | `Soc`: I/O register file, GPIO, video IRQ, DMA, I²C stub, PRNG, the PPU + camera-plane compositor, and the SPU |
| `src/board.cpp` | `Board`: buttons and the virtual hand, the motion-bitmap model, the stand-in camera frame |
| `src/disc.{h,cpp}` | disc layer: `.cue`/raw image/extracted directory → directory entries → memory images (block rule); SHA-1 identification; `repair_disc` (checksum-matching donor blocks, < 512 bytes apart, written as `(repaired)` copies) |
| `src/spi_flash.{h,cpp}` | the 64 KB SPI save flash (SST25VF512A-class) on GPIO port A |
| `src/firmware_hle.cpp` | `FirmwareHle`: IRQ trampolines, the `fw12` halt stub, the directory loader, the 7 live API entries |
| `src/cpu/` | `port_unsp.py`, `mame_orig/` (MAME, GPL-2.0+), `shim/` (MAME framework stand-in, no globals), `unsp/` (generated) |
| `src/ppu/` | MAME `spg_renderer` (BSD-3) with one addition (`line_buffer()`) |
| `src/spu/` | `ion_spu`: 24-channel SPU derived from MAME `spg2xx_audio` (BSD-3); `mame_orig/` for reference |
| `apps/` | `ionrun.cpp`, `ionview.cpp`, `ionrepair.cpp` |
| `tests/` | `golden.py` + `golden/*.json` (hashes only), `instance_test.cpp` and `camera_test.cpp` (CTest) |

## Guarantees and tests

- **Deterministic:** the same content and inputs give the same frames, audio and RAM, and the IMA tables are literals.
- **No process-global state:** two consoles stepped alternately in one process stay identical (`instance_test`).
- **Save states** (`IONSTATE` v2): CPU core struct, I/O, SPU, board input, HLE and frame-loop state, the non-zero 4K-word memory pages, and the save flash (interface state + non-erased sectors); ~1-7.5 MB. States carry a content ID and are refused for other content. A truncated state is rejected and leaves the console intact. A round trip replays identically on all six discs (`ionrun --state-roundtrip N`).
- **Golden:** `tests/golden/<disc>.json` holds hashes of the event trace, 7 snapshots, WAV and iRAM for a 20,000-frame scripted run per disc. They are re-recorded only when a change is meant to alter the output (`--record`), and the change is noted in the commit.
