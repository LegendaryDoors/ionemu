#!/usr/bin/env python3
"""Port MAME's u'nSP interpreter (GPL-2.0+) to a standalone build.

Fetch (or reuse) the upstream files into mame_orig/, strip the DRC recompiler and the
disassembler hook, add a small public API (run/reset/registers/IRQ state/save-state access),
and write the result to unsp/. The MAME framework is provided by shim/emu.h (no process globals).

usage: port_unsp.py [--fetch]    (fetching needs raw.githubusercontent.com)
"""
import os, re, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ORIG, OUT = os.path.join(HERE, 'mame_orig'), os.path.join(HERE, 'unsp')
URL = 'https://raw.githubusercontent.com/mamedev/mame/master/src/devices/cpu/unsp/'
FILES = ['unsp.cpp', 'unsp.h', 'unspdefs.h', 'unsp_extended.cpp', 'unsp_exxx.cpp',
         'unsp_fxxx.cpp', 'unsp_jumps.cpp', 'unsp_other.cpp']

def remove_block(s, start):
    """Remove text from `start` through the matching close brace of the next '{'."""
    i = s.index('{', start); depth = 0
    for j in range(i, len(s)):
        if s[j] == '{': depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0: return s[:start] + s[j + 1:]
    raise ValueError('unbalanced')

def drop_lines(s, pats):
    return '\n'.join(l for l in s.split('\n') if not any(p in l for p in pats))

def must(s, old, new):
    assert old in s, old[:60]
    return s.replace(old, new)

DRC_PATS = ['drc_cache', 'drcuml', 'frontend', 'm_drcoptions', 'm_cache_dirty', 'uml::', 'execute_run_drc',
            'flush_drc_cache', 'code_flush_cache', 'code_compile_block', 'fast_iregs', 'generate_',
            'log_add_disasm_comment', 'class opcode_desc;', 'create_disassembler']

def port():
    os.makedirs(OUT, exist_ok=True)
    src = {f: open(os.path.join(ORIG, f)).read() for f in FILES}

    h = src['unsp.h']
    h = drop_lines(h, DRC_PATS + ['#include "cpu/drcuml.h"'])
    h = must(h, 'virtual ~unsp_device();', '''virtual ~unsp_device();

	// ---- standalone API (added by port_unsp.py) ----
	void start() { device_start(); }
	void reset() { device_reset(); }
	int run(int cycles) { m_core->m_icount = cycles; execute_run(); return cycles - m_core->m_icount; }
	void set_input(int line, int state) { execute_set_input(line, state); }
	uint32_t &reg(int i) { return m_core->m_r[i]; }
	uint32_t pc() const { return (m_core->m_r[REG_PC] | ((m_core->m_r[REG_SR] & 0x3f) << 16)); }
	void set_pc(uint32_t lpc) { set_ds_or_cs_pc(lpc); }
	uint32_t &irq_enable() { return m_core->m_enable_irq; }
	uint32_t &fiq_enable() { return m_core->m_enable_fiq; }
	void clear_irq_state() { m_core->m_irq = 0; m_core->m_fiq = 0; m_core->m_ine = 0; m_core->m_pri = 8; }   // not-in-handler, as after reset
	void *core_state() { return m_core; }                                   // POD register/flag state, for save states
	static constexpr size_t core_state_size() { return sizeof(internal_unsp_state); }
	void set_ds_or_cs_pc(uint32_t lpc) { m_core->m_r[REG_PC] = lpc & 0xffff; m_core->m_r[REG_SR] = (m_core->m_r[REG_SR] & 0xffc0) | ((lpc >> 16) & 0x3f); }''')
    open(os.path.join(OUT, 'unsp.h'), 'w').write(h)

    c = src['unsp.cpp']
    c = drop_lines(c, ['#include "unspfe.h"', '#include "emuopts.h"', '#include "unspdasm.h"',
                       ', m_drccache', ', m_drcuml', ', m_drcfe', ', m_drcoptions', ', m_cache_dirty', ', m_entry',
                       ', m_nocode', ', m_out_of_cycles', ', m_check_interrupts', ', m_trigger_fiq', ', m_trigger_irq',
                       ', m_mem_read', ', m_mem_write', 'm_drcfe.reset();', 'm_drcuml.reset();'])
    # create_disassembler bodies
    while True:
        m = re.search(r'std::unique_ptr<util::disasm_interface> unsp\w*::create_disassembler\(\)', c)
        if not m: break
        c = remove_block(c, m.start())
    # device_start: DRC allocation -> local core
    i = c.index('\tif (m_enable_drc)\n\t{\n\t\tm_drccache.allocate_cache')
    j = c.index('m_core = &m_local_core;\n\t}', i) + len('m_core = &m_local_core;\n\t}')
    c = c[:i] + '\tm_core = &m_local_core;' + c[j:]
    # remaining DRC-only blocks (device_start symbol setup, execute_run dispatch)
    while True:
        k = c.find('if (m_enable_drc)\n\t{')
        if k < 0: break
        c = remove_block(c, k)
    open(os.path.join(OUT, 'unsp.cpp'), 'w').write(c)

    for f in FILES:
        if f in ('unsp.h', 'unsp.cpp'): continue
        s = drop_lines(src[f], ['#include "unspfe.h"', '#include "unspdasm.h"'])
        open(os.path.join(OUT, f), 'w').write(s)
    print('ported ->', OUT)

if __name__ == '__main__':
    os.makedirs(ORIG, exist_ok=True)
    if '--fetch' in sys.argv or not all(os.path.exists(os.path.join(ORIG, f)) for f in FILES):
        for f in FILES:
            urllib.request.urlretrieve(URL + f, os.path.join(ORIG, f))
    port()
