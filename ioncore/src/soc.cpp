// SPG220 on-chip peripherals (see core.h): the registers the games use, GPIO, the camera block and its plane.
#include "core.h"
#include <cstring>

namespace ion {

Soc::Soc(Core &core) : c(core) {
    spu.read_mem = [this](uint32_t a) { return c.mem[a & 0x3fffff]; };
    spu.irq_cb = [this](int st) { c.cpu.set_input(UNSP_IRQ4_LINE, st); };
    spu.log_cb = [this](const char *m) { c.log(m); };
}

void Soc::reset() {
    memset(io, 0, sizeof io);
    spu.reset();
    static_cast<device_t &>(ppu).device_reset();
    prng = 0x1234;
    reg_logs.clear();
}

uint16_t Soc::gpio_read(uint32_t data_reg, uint16_t pins) {
    const uint16_t dir = IO(data_reg + 2);
    return (IO(data_reg + 1) & dir) | (pins & ~dir);
}

void Soc::update_video_irq() { c.cpu.set_input(UNSP_IRQ0_LINE + 0, (IO(0x2862) & IO(0x2863)) ? 1 : 0); }

// Save flash on port A: CS# = A14, SCK = A10, MOSI = A11, driven only once they are outputs.
void Soc::update_spi() {
    const uint16_t buf = IO(0x3D02), dir = IO(0x3D03);
    if ((dir & 0x4C00) != 0x4C00) return;
    c.board.flash.pins(buf & 0x4000, buf & 0x0400, buf & 0x0800);
}

void Soc::end_of_frame() {
    IO(0x2863) |= IO(0x2862) & 3;                          // vblank (bit 0) + line-position (bit 1, line 240)
    update_video_irq();
}

uint16_t Soc::read(uint32_t a) {
    if (a >= 0x3000 && a < 0x3800) return spu.read(a);
    const uint16_t buttons = c.board.in.buttons;
    switch (a) {
    case 0x3D2F: return (c.cpu.reg(6) >> 10) & 0x3f;                    // DS lives in SR bits 15:10
    case 0x3D01: return gpio_read(a, (buttons & 0x0388) | (c.board.flash.miso() ? 0x1000 : 0));   // IOA: buttons, flash MISO
    case 0x3D06: return gpio_read(a, 0);                                 // IOB: dev keypad, not fitted
    case 0x3D0B: return gpio_read(a, (buttons & 0x0060) << 4);          // IOC: C9 Brightness, C10 Play/Pause
    case 0x3D59: return IO(a);                                           // I2C status (set after a command)
    case 0x3E02: return 0;                                               // DMA done
    case 0x3D2C: case 0x3D2D: prng = prng * 1103515245 + 12345; return (prng >> 16) & 0x7fff;
    default: return IO(a);
    }
}

void Soc::write(uint32_t a, uint16_t d) {
    if (a >= 0x3000 && a < 0x3800) { spu.write(a, d); return; }
    if ((a == 0x3D21 || a == 0x3D2E) && IO(a) != d && reg_logs[a]++ < 20) {           // INT_CTRL, FIQ_SEL
        char b[96]; snprintf(b, sizeof b, "io %04x (%s): %04x -> %04x", a, a == 0x3D21 ? "INT_CTRL" : "FIQ_SEL", IO(a), d); c.log(b);
    }
    if ((a == 0x2899 || a == 0x283D || a == 0x289A) && IO(a) != d && reg_logs[a]++ < 40) {   // camera plane, video mode
        char b[96]; snprintf(b, sizeof b, "io %04x: %04x -> %04x", a, IO(a), d); c.log(b);
    }
    switch (a) {
    case 0x3D2F: c.cpu.reg(6) = (c.cpu.reg(6) & 0x03ff) | ((d & 0x3f) << 10); return;
    case 0x3D01: IO(a) = d; IO(a + 1) = d; update_spi(); return;                   // data writes set the buffer
    case 0x3D02: case 0x3D03: IO(a) = d; update_spi(); return;                      // IOA buffer / direction
    case 0x3D06: case 0x3D0B: IO(a) = d; IO(a + 1) = d; return;
    case 0x2863: IO(a) &= ~d; update_video_irq(); return;               // write-1-to-clear
    case 0x2862: IO(a) = d; update_video_irq(); return;
    case 0x3D22: IO(0x3D22) &= ~d; return;                               // INT_CLEAR
    case 0x3D58: IO(a) = d; IO(0x3D59) = 1; return;                      // I2C command -> done
    case 0x2872: {                                                       // PPU sprite DMA (MAME do_sprite_dma)
        const uint32_t src = IO(0x2870) & 0x3fff, dst = IO(0x2871);
        for (uint32_t j = 0; j < d; j++) IO(0x2c00 + ((dst + j) & 0x3ff)) = c.read(src + j);
        IO(a) = 0; return; }
    case 0x3D59: IO(a) &= ~d; return;                                    // W1C
    case 0x3E02: {                                                       // system DMA: src (3E00/01) -> dst (3E03)
        const uint32_t src = IO(0x3E00) | ((IO(0x3E01) & 0x3f) << 16), dst = IO(0x3E03);
        for (uint32_t i = 0; i < d; i++) c.write(dst + i, c.read(src + i));
        IO(a) = 0; return; }
    default: IO(a) = d; return;
    }
}

// MAME spg2xx_video::screen_update equivalent, plus the camera plane: on when 0x2899 bit 6; bits 2:0 = v = 2d+1-b
// place it just below depth d (b = 1) or just above it (b = 0), i.e. before PPU depth level (v+1)/2 (4 = above all).
void Soc::render(bitmap_rgb32 &bm) {
    address_space sp{&c};
    rectangle clip(0, SCREEN_W - 1, 0, SCREEN_H - 1);
    ppu.set_video_reg_1c(IO(0x281c)); ppu.set_video_reg_1d(IO(0x281d)); ppu.set_video_reg_1e(IO(0x281e));
    ppu.set_video_reg_2a(IO(0x282a)); ppu.set_video_reg_30(IO(0x2830)); ppu.set_video_reg_3c(IO(0x283c));
    ppu.set_video_reg_42(IO(0x2842));
    uint16_t *regs = &IO(0x2800);
    const uint32_t p1 = regs[0x20], p2 = regs[0x21], spr = 0x40 * regs[0x22];
    const int cam_slot = (c.opt.camera_mode && (IO(0x2899) & 0x40)) ? ((IO(0x2899) & 7) + 1) / 2 : -1;
    if (cam_slot >= 0 && !c.board.frames_mode()) c.board.synth_camera();   // frames mode: already this frame's picture
    for (uint32_t y = 0; y < SCREEN_H; y++) {
        ppu.new_line(clip);
        for (int pri = 0; pri <= 4; pri++) {
            if (pri == cam_slot) memcpy(ppu.line_buffer(), &c.board.cam[y * SCREEN_W], SCREEN_W * sizeof(uint16_t));
            if (pri == 4) break;
            ppu.draw_page(clip, y, pri, p1, regs + 0x10, regs + 0x12, sp, &IO(0x2b00), &IO(0x2900), 0);
            ppu.draw_page(clip, y, pri, p2, regs + 0x16, regs + 0x18, sp, &IO(0x2b00), &IO(0x2900), 1);
            ppu.draw_sprites(clip, y, pri, spr, sp, &IO(0x2b00), &IO(0x2c00), 256);
        }
        ppu.apply_saturation_and_fade(bm, clip, y);
    }
}

}  // namespace ion
