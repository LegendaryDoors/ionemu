// High-level firmware (see core.h): the console firmware is not dumped, so its observable contract is reimplemented
// from the games' side.
#include "core.h"
#include <algorithm>
#include <cstdio>

namespace ion {

bool FirmwareHle::load_entry(int entry) {
    if (entry < 1 || entry > disc.entry_count()) { c.log("load_entry: bad entry " + std::to_string(entry)); return false; }
    std::vector<uint16_t> words;
    if (!disc.entry_image(entry, words)) return false;
    std::fill(c.mem.begin() + IMG_BASE, c.mem.end(), 0);
    const size_t n = std::min(words.size(), c.mem.size() - IMG_BASE);
    std::copy(words.begin(), words.begin() + n, c.mem.begin() + IMG_BASE);
    c.log("loaded entry " + std::to_string(entry) + " " + disc.entry_label(entry) + " words=" + std::to_string(n));
    c.file_changed(entry);
    cur_entry = entry;
    return true;
}

void FirmwareHle::install_stubs() {
    for (int k = 0; k < 8; k++) {                                        // IRQk vector -> trampoline -> image vector
        c.mem[0xFFF8 + k] = 0xF000 + 2 * k;
        c.mem[0xF000 + 2 * k] = 0xFE84;                                  // goto far 4:(2k)
        c.mem[0xF001 + 2 * k] = 2 * k;
    }
    c.mem[0xFFF6] = 0xF020; c.mem[0xF020] = 0x9A98;                      // FIQ -> reti
    c.mem[HALT_STUB] = 0xFE80; c.mem[HALT_STUB + 1] = HALT_STUB;         // fw12 parks the CPU here: goto 0:F030
    c.mem[CAM_PROFILE] = 2;                                              // camera profile at power-on (Brightness 2)
}

// Games access the save flash without configuring port A first (Danny Phantom 02_MG0 reads it at startup), so the
// firmware must leave the SPI pins set up at boot (H). Use the SDK's own flash-port init values (FUN_040d87):
// A10 (SCK), A11 (MOSI), A14 (CS#) outputs; CS# and SCK high.
void FirmwareHle::init_hardware() {
    Soc &s = c.soc;
    s.IO(0x3D03) = (s.IO(0x3D03) & ~0x1000) | 0x4C00;        // IOA_DIR
    s.IO(0x3D04) = (s.IO(0x3D04) & ~0x1000) | 0x4C00;        // IOA_ATTRIB
    s.IO(0x3D02) = (s.IO(0x3D02) & ~0x0800) | 0x5400;        // IOA_BUFFER
    s.update_spi();
}

void FirmwareHle::enter_image() {
    c.cpu.reg(0) = BOOT_SP;                                              // SP
    c.cpu.reg(6) = 0;                                                    // SR (CS/DS/flags)
    c.cpu.irq_enable() = 0; c.cpu.fiq_enable() = 0;
    c.cpu.clear_irq_state();                                             // not inside a handler after the jump
    c.cpu.set_pc(IMG_BASE + 0x10);                                       // entry_init
}

uint16_t FirmwareHle::arg(int i) { return c.mem[(c.cpu.reg(0) + 3 + i) & 0xffff]; }            // [sp+3+i]
uint32_t FirmwareHle::arg32(int i) { return arg(i) | ((uint32_t)arg(i + 1) << 16); }

void FirmwareHle::api_return(uint32_t r1, uint32_t r2) {
    const uint32_t sp = c.cpu.reg(0);
    c.cpu.reg(6) = c.mem[(sp + 1) & 0xffff];                             // SR (restores CS)
    c.cpu.reg(7) = c.mem[(sp + 2) & 0xffff];                             // PC
    c.cpu.reg(0) = (sp + 2) & 0xffff;
    c.cpu.reg(1) = r1 & 0xffff; c.cpu.reg(2) = r2 & 0xffff;
}

// crt0 .data initialisation table at the fw01 destination: count, then {dst, seg, off, len} records.
void FirmwareHle::data_walk_table(uint32_t tbl) {
    const uint16_t count = c.mem[tbl];
    if (count == 0 || count > 32) { c.log("data-walk: implausible count " + std::to_string(count)); return; }
    for (int i = 0; i < count; i++) {
        const uint32_t p = tbl + 1 + 4 * i;
        const uint16_t dst = c.mem[p], seg = c.mem[p + 1], off = c.mem[p + 2], len = c.mem[p + 3];
        char b[160]; snprintf(b, sizeof b, "data-walk rec%d: dst=%04x <- %02x:%04x len=%04x", i, dst, seg, off, len); c.log(b);
        if (dst + len > 0x2800) { c.log("  (skipped: outside iRAM)"); continue; }
        const uint32_t src = ((uint32_t)seg << 16) | off;
        for (uint32_t k = 0; k < len; k++) c.mem[dst + k] = c.mem[(src + k) & 0x3fffff];
    }
}

void FirmwareHle::api(int k) {
    api_calls[k]++;
    char b[200];
    switch (k) {
    case 0: {                                                            // IRQ/FIQ enable; returns the previous mask
        snprintf(b, sizeof b, "fw00(mask=%04x)", arg(0)); c.log(b);
        c.cpu.irq_enable() = arg(0) & 1;
        if (arg(0) & 2) c.log("  fw00: FIQ requested (not enabled)");
        const uint16_t old = c.mem[0x0009];
        c.mem[0x0009] = arg(0); api_return(old); return; }
    case 1: {                                                            // window copy + crt0 .data walk
        const uint32_t src = arg32(0), end = arg32(2), dst = arg32(4);
        snprintf(b, sizeof b, "fw01(src=%06x end=%06x dst=%06x)", src, end, dst); c.log(b);
        for (uint32_t a = src; a <= end; a++) c.mem[(dst + (a - src)) & 0x3fffff] = c.mem[a & 0x3fffff];
        if (c.opt.data_walk) data_walk_table(dst);
        api_return(0); return; }
    case 3: {                                                            // load and run directory entry (no return)
        const int entry = arg(0); const uint32_t param = arg32(1);
        snprintf(b, sizeof b, "fw03(entry=%d param=%08x) from %06x -> load and run", entry, param, c.last_pc); c.log(b);
        if (!load_entry(entry)) { api_return(0xffff); return; }
        enter_image(); return; }
    case 12:                                                             // exit to system (no return): park the CPU
        snprintf(b, sizeof b, "fw12() from %06x -> exit to system (stopping)", c.last_pc); c.log(b);
        stopped = true;
        c.cpu.irq_enable() = 0; c.cpu.fiq_enable() = 0; c.cpu.set_pc(HALT_STUB);
        return;
    case 18: if (api_calls[k] <= 2) c.log("fw18() -> &profile 0x27FE (further calls not logged)"); api_return(CAM_PROFILE); return;
    case 19: if (api_calls[k] <= 2) c.log("fw19() -> &{4, 0x3408} threshold descriptor (further calls not logged)");
        c.mem[THR_DESC] = 4; c.mem[THR_DESC + 1] = 0x3408; api_return(THR_DESC); return;
    case 20: if (api_calls[k] <= 2) c.log("fw20() -> &{4, 0x3000} sensor-table descriptor (further calls not logged)");
        c.mem[REG_DESC] = 4; c.mem[REG_DESC + 1] = 0x3000; api_return(REG_DESC); return;
    default:
        snprintf(b, sizeof b, "fw%02d(unimplemented) args %04x %04x %04x", k, arg(0), arg(1), arg(2)); c.log(b);
        api_return(0); return;
    }
}

}  // namespace ion
