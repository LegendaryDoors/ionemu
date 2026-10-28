// Board-level inputs (see core.h): the virtual hand -> SoC motion bitmap, and the stand-in camera frame source.
#include "core.h"
#include <algorithm>
#include <cmath>

namespace ion {

bool Board::frames_mode() const { return c.opt.camera_mode == 2 || c.opt.motion_from_frames; }

void Board::scan_hand() {                                 // test driver: sweep 4x3 positions, 180 frames each
    static const int xs[4] = {40, 120, 200, 280}, ys[3] = {60, 120, 180};
    const int slot = (c.frame / 180) % 12;
    in.hand_x = xs[slot % 4]; in.hand_y = ys[slot / 4];
    if (c.frame % 180 == 0) { char b[64]; snprintf(b, sizeof b, "hand -> (%d,%d)", in.hand_x, in.hand_y); c.log(b); }
}

// Motion bitmap for the SoC motion block, written to iRAM at [0x2895] while 0x289D bit 0 (enable) is set.
// Default: a synthetic "hand" (8x8 mode), cells under a wiggling blob reported as changed.
// frames_mode(): the SoC's frame detector on the camera frames (synthetic player or external, e.g. a webcam).
void Board::motion_frame() {
    Soc &s = c.soc;
    const bool enabled = s.IO(0x289D) & 1;
    if (frames_mode()) {
        if (enabled && in.hand_scan) scan_hand();
        update_camera();
        if (!enabled) { prev_valid = false; return; }
        motion_from_frame();
        return;
    }
    if (!enabled) return;
    const uint32_t base = s.IO(0x2895);
    if (base + 600 > 0x2800) return;
    if (in.hand_scan) scan_hand();
    const int wx = in.hand_x + ((c.frame & 1) ? 4 : -4), r = 20;
    for (int i = 0; i < 600; i++) c.mem[base + i] = 0;
    if (!in.hand_active) return;
    for (int cy = std::max(0, (in.hand_y - r) >> 3); cy <= std::min(29, (in.hand_y + r) >> 3); cy++)
        for (int cx = std::max(0, (wx - r) >> 3); cx <= std::min(39, (wx + r) >> 3); cx++)
            c.mem[base + cy * 20 + (cx >> 1)] |= (cx & 1) ? 1 : 2;
}

uint8_t luma_of(uint32_t r, uint32_t g, uint32_t b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }

int detect_motion_cells(const uint8_t *luma, int cell, float t, int16_t *prev_mean, bool &prev_valid, uint8_t *cells) {
    const int cols = SCREEN_W / cell, rows = SCREEN_H / cell;
    int flagged = 0;
    for (int r = 0; r < rows; r++)
        for (int col = 0; col < cols; col++) {
            int sum = 0;
            for (int y = r * cell; y < (r + 1) * cell; y++)
                for (int x = col * cell; x < (col + 1) * cell; x++) sum += luma[y * SCREEN_W + x];
            const int16_t m = (int16_t)(sum / (cell * cell));
            int16_t &prev = prev_mean[r * cols + col];
            const bool hit = prev_valid && std::abs(m - prev) > t;
            if (cells) cells[r * cols + col] = hit ? 1 : 0;
            flagged += hit;
            prev = m;
        }
    prev_valid = true;
    return flagged;
}

// This frame's camera picture (RGB555 for the plane) and its luma (for the detector).
void Board::update_camera() {
    auto y_of = luma_of;
    if (c.opt.camera_mode == 2) {
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
            const uint32_t p = ext_valid ? ext[i] : 0, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            cam[i] = (uint16_t)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            luma[i] = y_of(r, g, b);
        }
        return;
    }
    // a waving hand: alternate per camera frame (two fields), which is what the detector samples
    if (c.opt.camera_mode == 1) synth_camera(in.hand_active ? (((c.frame >> 1) & 1) ? 4 : -4) : 0);
    else std::fill(cam.begin(), cam.end(), 0);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        const uint16_t p = cam[i];
        luma[i] = y_of(((p >> 10) & 31) << 3, ((p >> 5) & 31) << 3, (p & 31) << 3);
    }
}

// The SoC's frame detector: per cell, m = mean(Y); active = |m - m_prev| > T (T = 0x289D bits 7:1, / sensitivity); pack two
// cells per word (bit 1 = even/left, bit 0 = odd/right); one camera frame per two fields.
void Board::motion_from_frame() {
    Soc &s = c.soc;
    const uint16_t md = s.IO(0x289D);
    const uint32_t base = s.IO(0x2895);
    const int cell = ((md >> 10) & 3) == 1 ? 8 : 16, cols = SCREEN_W / cell, rows = SCREEN_H / cell, words = rows * cols / 2;
    if (base + words > 0x2800) return;
    if (base != prev_base || cell != prev_cell) { prev_valid = false; prev_base = (uint16_t)base; prev_cell = (uint16_t)cell; }
    if (c.frame & 1) return;                              // one camera frame per two fields; the map holds between them
    if (c.opt.camera_mode == 2) {                         // external frames: only when a new frame has arrived, so a
        if (ext_seq == det_seq) return;                   // 15 fps webcam holds its map instead of alternating with zeros
        det_seq = ext_seq;
    }
    const float t = ((md >> 1) & 0x7f) / std::max(0.01f, c.opt.motion_sensitivity);
    for (int i = 0; i < words; i++) c.mem[base + i] = 0;
    uint8_t cells[40 * 30];
    detect_motion_cells(luma.data(), cell, t, prev_mean, prev_valid, cells);
    for (int r = 0; r < rows; r++)
        for (int col = 0; col < cols; col++)
            if (cells[r * cols + col]) c.mem[base + r * (cols / 2) + (col >> 1)] |= (col & 1) ? 1 : 2;
}

void resample_camera(const uint32_t *xrgb, int w, int h, bool mirror, float zoom, uint32_t *out) {
    int cw = w, ch = h, x0 = 0, y0 = 0;                   // centre crop to 4:3
    if (w * 3 > h * 4) { cw = h * 4 / 3; x0 = (w - cw) / 2; } else { ch = w * 3 / 4; y0 = (h - ch) / 2; }
    const float z = zoom > 0.05f ? zoom : 1.f;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            // zoom about the centre: u,v = position in the un-zoomed 320x240 picture
            const float u = (x - SCREEN_W / 2) / z + SCREEN_W / 2, v = (y - SCREEN_H / 2) / z + SCREEN_H / 2;
            if (u < 0 || v < 0 || u >= SCREEN_W || v >= SCREEN_H) { out[y * SCREEN_W + x] = 0; continue; }
            const int ux = (int)u, vy = (int)v;
            const int sx = x0 + (mirror ? (SCREEN_W - 1 - ux) : ux) * cw / SCREEN_W, sy = y0 + vy * ch / SCREEN_H;
            out[y * SCREEN_W + x] = xrgb[sy * w + sx] & 0xffffff;
        }
}

void Board::set_external(const uint32_t *xrgb, int w, int h, bool mirror) {
    if (!xrgb || w <= 0 || h <= 0) return;
    resample_camera(xrgb, w, h, mirror, c.opt.camera_zoom, ext.data());
    ext_valid = true;
    ext_seq++;
}

// ---- MotionPreview (public, ioncore.h) --------------------------------------------------------------------------

struct MotionPreview::Impl {
    std::vector<uint32_t> picture = std::vector<uint32_t>(SCREEN_W * SCREEN_H);
    std::vector<uint8_t> luma = std::vector<uint8_t>(SCREEN_W * SCREEN_H);
    int16_t prev_mean[40 * 30] = {};
    bool prev_valid = false;
};

MotionPreview::MotionPreview() : d(new Impl) {}
MotionPreview::~MotionPreview() = default;
void MotionPreview::reset() { d->prev_valid = false; }

int MotionPreview::feed(const uint32_t *xrgb, int width, int height, bool mirror, float zoom, float sensitivity, int threshold,
                        std::vector<uint8_t> &cells, int &cols, int &rows) {
    cols = SCREEN_W / 8; rows = SCREEN_H / 8;
    cells.assign((size_t)cols * rows, 0);
    if (!xrgb || width <= 0 || height <= 0) return 0;
    resample_camera(xrgb, width, height, mirror, zoom, d->picture.data());
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        const uint32_t p = d->picture[i];
        d->luma[i] = luma_of((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
    }
    const float t = (float)(threshold & 0x7f) / std::max(0.01f, sensitivity);
    return detect_motion_cells(d->luma.data(), 8, t, d->prev_mean, d->prev_valid, cells.data());
}

const uint32_t *MotionPreview::picture() const { return d->picture.data(); }

namespace {
uint16_t rgb555(float r, float g, float b) {
    auto q = [](float v) { return (uint16_t)(std::clamp((int)v, 0, 255) >> 3); };
    return (q(r) << 10) | (q(g) << 5) | q(b);
}
float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay, t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy + 1e-6f), 0.f, 1.f);
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}
}

// Stand-in camera frame: a mirror-image player in a dim room whose raised hand follows the virtual hand.
// The look is Options::player. Drawn from signed distance fields so edges are anti-aliased,
// with a cartoon rim and soft shading; parts are composited back to front. Calls put(x, y, r, g, b) per pixel so
// the SoC path (RGB555) and the preview (XRGB) share the drawing.
namespace {
struct V2 { float x, y; };
float sd_circle(float px, float py, float cx, float cy, float r) { return std::hypot(px - cx, py - cy) - r; }
float sd_ellipse(float px, float py, float cx, float cy, float rx, float ry) {      // approximate (scaled circle)
    const float k = ry / rx;
    return (std::hypot((px - cx) * k, py - cy) - ry) / k;
}
float sd_capsule(float px, float py, float ax, float ay, float bx, float by, float r) { return seg_dist(px, py, ax, ay, bx, by) - r; }
float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float rad) {
    const float qx = std::abs(px - cx) - hw + rad, qy = std::abs(py - cy) - hh + rad;
    return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - rad;
}
float smin(float a, float b, float k) {                                            // smooth union of two distance fields
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.f, 1.f);
    return b + (a - b) * h - k * h * (1 - h);
}
// Two-bone arm: shoulder S to hand H with upper/forearm lengths, the elbow bent away from the body (sign).
V2 elbow_of(V2 s, V2 h, float l1, float l2, float sign) {
    const float dx = h.x - s.x, dy = h.y - s.y, d = std::max(1e-3f, std::hypot(dx, dy));
    const float ux = dx / d, uy = dy / d;
    if (d >= l1 + l2 - 0.5f) return {s.x + ux * l1, s.y + uy * l1};
    const float a = (l1 * l1 - l2 * l2 + d * d) / (2 * d), hh = std::sqrt(std::max(0.f, l1 * l1 - a * a));
    return {s.x + ux * a + sign * (-uy) * hh, s.y + uy * a + sign * ux * hh};
}
}  // namespace

// Simple style: the original flat stand-in, colours from the look.
template <class Put>
static void draw_player_simple(const PlayerLook &lk, int hand_x, int hand_y, bool hand_active, int wiggle, Put put) {
    const float cx = 160, hy = 92, sy = 138;                                 // body centre, head y, shoulder y
    const bool left = hand_x < cx;
    const float sax = cx + (left ? -36 : 36), sbx = cx + (left ? 36 : -36);  // raised / resting shoulder
    const float hx = hand_x + wiggle, hyy = hand_y, lit = hand_active ? 1.f : 0.8f;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            float r, g, b;
            if (y < 172) { const float t = y / 172.f; r = lk.wall[0] - 20 * t; g = lk.wall[1] - 18 * t; b = lk.wall[2] - 16 * t; }   // wall
            else { const float t = (y - 172) / 68.f; r = lk.floor[0] - 14 * t; g = lk.floor[1] - 12 * t; b = lk.floor[2] - 10 * t; }  // floor
            if (lk.window && x >= 236 && x < 300 && y >= 26 && y < 104) { r += 40; g += 44; b += 52; }          // window
            const float head = std::hypot(x - cx, (y - hy) * 0.92f), torso_x = std::abs(x - cx);
            const bool torso = y >= 124 && torso_x < 44 - std::max(0.f, 140.f - y) * 0.6f;
            const bool arm_up = seg_dist(x, y, sax, sy, hx, hyy) < 9, arm_down = seg_dist(x, y, sbx, sy, sbx + (left ? 10 : -10), 212) < 9;
            const bool hand = std::hypot(x - hx, y - hyy) < 12;
            if (torso || arm_down || arm_up) { r = lk.shirt[0]; g = lk.shirt[1]; b = lk.shirt[2]; }             // shirt
            if (head < 25 || hand || (torso_x < 9 && y >= 112 && y < 124)) { r = lk.skin[0] * lit; g = lk.skin[1] * lit; b = lk.skin[2] * lit; }   // skin
            bool hair = head < 25 && y < hy - 8 && head > 12;                                                   // short: a cap
            if (lk.hair_style == 1) hair = hair || (y >= hy - 8 && y < sy && torso_x >= 20 && torso_x < 30 && !hand);   // long: down the sides
            if (lk.hair_style == 2) hair = false;
            if (hair) { r = lk.hair[0]; g = lk.hair[1]; b = lk.hair[2]; }
            put(x, y, r, g, b);
        }
}

// Detailed style: signed-distance shapes drawn to the conventions of flat emoji-style figures (reference: the open
// Twemoji/OpenMoji waving-hand and person glyphs): a rounded palm with four separate finger columns of slightly
// different lengths and a thumb angled from the palm's lower corner, no drawn lines; hair as one mass sitting just
// outside the head outline with sideburns and an asymmetric fringe, or a bell-shaped mass to the shoulders; ears; limbs
// as sausages of one thickness; one rim weight; light from the window on the left.
template <class Put>
static void draw_player_detailed(const PlayerLook &lk, int hand_x, int hand_y, bool hand_active, int wiggle, Put put) {
    const float cx = 160;
    const float head_y = 86, head_rx = 23, head_ry = 26;
    const float neck_top = 106, shoulder_y = 128, torso_hw = 46;              // a short neck: the chin sits near the shoulders
    const float upper_r = 11, fore_r = 9.5f;                                   // an upper arm is about half a head wide
    const bool left = hand_x < cx;                                             // which side waves (mirror view)
    const float side = left ? -1.f : 1.f;
    const V2 sh_up{cx + side * 35, shoulder_y + 10}, sh_down{cx - side * 35, shoulder_y + 10};   // shoulder joints
    const V2 hand{float(hand_x + wiggle), float(hand_y)};
    const V2 elbow = elbow_of(sh_up, hand, 40, 38, side);
    const V2 rest_elbow{sh_down.x - side * 7, shoulder_y + 50}, rest_wrist{sh_down.x - side * 6, 208};
    const float lit = hand_active ? 1.f : 0.85f;
    const float sk[3] = {lk.skin[0] * lit, lk.skin[1] * lit, lk.skin[2] * lit};
    const float hair_rgb[3] = {lk.hair[0], lk.hair[1], lk.hair[2]}, shirt_rgb[3] = {lk.shirt[0], lk.shirt[1], lk.shirt[2]};
    const float eye_rgb[3] = {40, 30, 30}, mouth_rgb[3] = {150 * lit * 0.7f, 70 * lit * 0.7f, 70 * lit * 0.7f};

    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float r, g, b;
            // room: wall with a soft gradient and a vignette, a floor, a lit window with a glow
            if (py < 172) { const float t = py / 172.f; r = lk.wall[0] - 20 * t; g = lk.wall[1] - 18 * t; b = lk.wall[2] - 16 * t; }
            else { const float t = (py - 172) / 68.f; r = lk.floor[0] - 14 * t; g = lk.floor[1] - 12 * t; b = lk.floor[2] - 10 * t; }
            const float vig = 1.f - 0.18f * std::pow(std::hypot((px - 160) / 200.f, (py - 120) / 160.f), 2.f);
            r *= vig; g *= vig; b *= vig;
            if (lk.window) {
                const float wd = sd_rrect(px, py, 268, 65, 32, 39, 3);
                const float glow = std::clamp(1.f - std::max(wd, 0.f) / 30.f, 0.f, 1.f) * 0.35f;
                r += 40 * glow; g += 44 * glow; b += 52 * glow;
                if (wd < 0) { r += 46; g += 50; b += 58; if (std::abs(px - 268) < 1.5f || std::abs(py - 65) < 1.5f) { r -= 30; g -= 30; b -= 30; } }
            }
            // one painter for every part: anti-aliased edge, subtle cylindrical shading, a rim of one weight
            auto paint = [&](float d, const float *col, float shade_cx, float shade_hw, float rim = 1.6f) {
                const float a = std::clamp(0.5f - d, 0.f, 1.f);
                if (a <= 0) return;
                const float side_t = std::clamp(std::abs(px - shade_cx) / std::max(1.f, shade_hw), 0.f, 1.f);
                float k = 1.06f - 0.16f * side_t * side_t;
                k *= 1.f + 0.05f * (1.f - px / SCREEN_W);
                if (d > -rim) k *= 0.74f;
                r = r + (col[0] * k - r) * a; g = g + (col[1] * k - g) * a; b = b + (col[2] * k - b) * a;
            };
            // a hand at wrist w pointing along (ux, uy), the thumb on thumb_side. A real open hand is an oblong about
            // 1.7x taller than wide with nearly straight sides, the fingertip edge sloping down towards the pinky, and
            // the thumb angled off the lower third; at 320x240 (~20 px) that oblong is all that can be drawn.
            auto draw_hand = [&](V2 w, float ux, float uy, float thumb_side) {
                const float nx = -uy, ny = ux;
                const float u = (px - w.x) * ux + (py - w.y) * uy, v = (px - w.x) * nx + (py - w.y) * ny;   // local: along, across
                const float len = 22, hw = 6.8f;
                float hand = sd_rrect(u, v, len / 2, 0, len / 2, hw, 4.5f);                          // the oblong
                const float t = std::clamp((-thumb_side * v + hw) / (2 * hw), 0.f, 1.f);                // 0 = thumb side, 1 = pinky
                hand = std::max(hand, u - (len - 3.5f * t));                                          // sloped fingertips
                hand = std::max(hand, -(u + 1.5f + 0.35f * std::abs(v)));                             // slight taper at the wrist
                // the thumb is part of the same silhouette: a short bulge blended into the palm's side (smooth union)
                const float thumb = sd_capsule(u, v, 6, thumb_side * 3, 10.5f, thumb_side * 8.f, 3.2f);   // tip ~30 % past the palm edge, as on the emoji hands
                const float k = 3.5f, h = std::clamp(0.5f + 0.5f * (thumb - hand) / k, 0.f, 1.f);
                const float merged = thumb + (hand - thumb) * h - k * h * (1 - h);
                paint(merged, sk, w.x, 10, 1.4f);
            };
            // torso: sloping rounded shoulders blending into the trunk, with the shoulder joints part of the silhouette
            {
                float torso = sd_capsule(px, py, cx - 26, shoulder_y + 8, cx + 26, shoulder_y + 8, 15);   // shoulder line
                torso = smin(torso, sd_rrect(px, py, cx, (shoulder_y + 24 + 262) / 2, torso_hw - 4, (262 - shoulder_y - 24) / 2, 14), 10);   // trunk
                torso = smin(torso, sd_circle(px, py, sh_up.x, sh_up.y, 13), 6);                           // deltoids
                torso = smin(torso, sd_circle(px, py, sh_down.x, sh_down.y, 13), 6);
                paint(torso, shirt_rgb, cx, torso_hw);
            }
            paint(sd_capsule(px, py, cx, neck_top, cx, shoulder_y - 2, 9), sk, cx, 9);
            // resting arm: a thick upper arm in the sleeve, a forearm, an open hand pointing down (thumb towards the body)
            paint(sd_capsule(px, py, sh_down.x, sh_down.y, rest_elbow.x, rest_elbow.y, upper_r), shirt_rgb, sh_down.x, upper_r);
            paint(sd_capsule(px, py, rest_elbow.x, rest_elbow.y, rest_wrist.x, rest_wrist.y, fore_r), sk, rest_elbow.x, fore_r);
            draw_hand(rest_wrist, 0, 1, side);
            // long hair: two lobes framing the face down to the shoulders (the neck stays visible), plus the crown
            if (lk.hair_style == 1) {
                float mass = sd_ellipse(px, py, cx, head_y - 6, head_rx + 5, head_ry);          // crown
                for (int i = 0; i < 4; i++) {                                                   // side lobes, flaring a little
                    const float yy = head_y + 2 + i * 14, dx = head_rx - 4 + i * 2.2f, rx = 10 + i * 1.2f, ry = 15;
                    mass = std::min(mass, sd_ellipse(px, py, cx - dx, yy, rx, ry));
                    mass = std::min(mass, sd_ellipse(px, py, cx + dx, yy, rx, ry));
                }
                mass = std::max(mass, py - (shoulder_y + 6));                                   // ends at the shoulders
                paint(mass, hair_rgb, cx, head_rx + 12);
            }
            // ears, head
            paint(sd_circle(px, py, cx - head_rx + 1, head_y + 3, 5), sk, cx - head_rx, 5);
            paint(sd_circle(px, py, cx + head_rx - 1, head_y + 3, 5), sk, cx + head_rx, 5);
            paint(sd_ellipse(px, py, cx, head_y, head_rx, head_ry), sk, cx, head_rx);
            // short hair: a cap just outside the head outline with sideburns, and an asymmetric fringe; long hair: the fringe only
            if (lk.hair_style != 2) {
                float cap = sd_ellipse(px, py, cx, head_y - 6, head_rx + 3.5f, head_ry - 1);
                float fringe_edge;                                                     // the fringe's lower edge, a swoop
                {
                    const float t = (px - (cx - head_rx)) / (2 * head_rx);              // 0..1 across the face
                    fringe_edge = head_y - 16 + 9 * std::sin(t * 3.14159f) - 6 * (t - 0.5f);   // dips in the middle, lower on the left
                }
                const float sideburn = std::abs(px - cx) > head_rx - 6 ? head_y + 4 : fringe_edge;   // sides run down to ear level
                if (lk.hair_style == 1) cap = std::max(cap, py - fringe_edge);
                else cap = std::max(cap, py - sideburn);
                paint(cap, hair_rgb, cx, head_rx + 3);
            }
            // face: tall oval eyes, a small nose, a smile
            paint(sd_ellipse(px, py, cx - 8.5f, head_y + 3, 2.2f, 3.6f), eye_rgb, cx - 8, 3, 0.8f);
            paint(sd_ellipse(px, py, cx + 8.5f, head_y + 3, 2.2f, 3.6f), eye_rgb, cx + 8, 3, 0.8f);
            {
                const float ring = std::abs(sd_circle(px, py, cx, head_y + 7, 9)) - 1.1f;   // smile: the lower arc of a ring
                paint(std::max(ring, (head_y + 13) - py), mouth_rgb, cx, 9, 0.8f);
            }
            // raised arm: the same sausages; the hand faces the camera, fingers up along the forearm, thumb inward
            {
                const float fdx = hand.x - elbow.x, fdy = hand.y - elbow.y, fl = std::max(1e-3f, std::hypot(fdx, fdy));
                const float ux = fdx / fl, uy = fdy / fl;
                const V2 wrist{hand.x - ux * 14, hand.y - uy * 14};
                paint(sd_capsule(px, py, sh_up.x, sh_up.y, elbow.x, elbow.y, upper_r), shirt_rgb, (sh_up.x + elbow.x) / 2, upper_r);
                paint(sd_capsule(px, py, elbow.x, elbow.y, wrist.x, wrist.y, fore_r), sk, (elbow.x + wrist.x) / 2, fore_r);
                draw_hand(wrist, ux, uy, -side);
            }
            put(x, y, r, g, b);
        }
}

template <class Put>
static void draw_player(const PlayerLook &lk, int hand_x, int hand_y, bool hand_active, int wiggle, Put put) {
    if (lk.style == 1) draw_player_detailed(lk, hand_x, hand_y, hand_active, wiggle, put);
    else draw_player_simple(lk, hand_x, hand_y, hand_active, wiggle, put);
}

void Board::synth_camera(int wiggle) {
    draw_player(c.opt.player, in.hand_x, in.hand_y, in.hand_active, wiggle,
                [&](int x, int y, float r, float g, float b) { cam[y * SCREEN_W + x] = rgb555(r, g, b); });
}

void render_player(const PlayerLook &look, int hand_x, int hand_y, bool hand_active, uint32_t *out) {
    draw_player(look, hand_x, hand_y, hand_active, 0, [&](int x, int y, float r, float g, float b) {
        auto q = [](float v) { return (uint32_t)(std::clamp((int)v, 0, 255) >> 3 << 3); };   // the RGB555 the games see
        out[y * SCREEN_W + x] = q(r) << 16 | q(g) << 8 | q(b);
    });
}

}  // namespace ion
