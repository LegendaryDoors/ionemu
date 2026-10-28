// Minimal stand-in for MAME's emu.h, just enough to build the u'nSP interpreter and the SPG renderer standalone.
// No process-global state: each cpu_device owns its bus pointer and hook pointers (set_bus / set_hooks), and each
// address_space carries its own bus, so any number of machines can run in one process.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <functional>
#include <iterator>
#include <cmath>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uint32_t offs_t;
#define ATTR_COLD
template <typename T> constexpr T BIT(T x, int n) noexcept { return (x >> n) & T(1); }
template <typename T> constexpr T BIT(T x, int n, int w) noexcept { return (x >> n) & ((T(1) << w) - 1); }
template <typename... A> std::string string_format(const char *fmt, A... a) { char b[256]; std::snprintf(b, sizeof b, fmt, a...); return b; }
#define NAME(x) x, #x
enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
enum { AS_PROGRAM = 0 };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };

struct unsp_bus {
    virtual ~unsp_bus() = default;
    virtual uint16_t read(uint32_t addr) = 0;
    virtual void write(uint32_t addr, uint16_t data) = 0;
};

template <int A, int W, int S, endianness_t E> struct memory_access {
    struct specific {
        unsp_bus *const *bus = nullptr;                  // points at the owning cpu_device's bus pointer
        uint16_t read_word(offs_t a) { return (*bus)->read(a & 0x3fffff); }
        void write_word(offs_t a, uint16_t d) { (*bus)->write(a & 0x3fffff, d); }
    };
    typedef specific cache;
};

struct machine_config {};
struct device_t;
typedef const void *device_type;
typedef int address_map_constructor;
struct address_space_config { address_space_config(const char *, endianness_t, int, int, int, address_map_constructor = 0) {} };
struct address_space_binder {
    unsp_bus *const *bus;
    template <typename T> void cache(T &t) { t.bus = bus; }
    template <typename T> void specific(T &t) { t.bus = bus; }
};
struct device_state_entry { int index() const { return 0; } };
struct state_entry_chain {
    state_entry_chain &formatstr(const char *) { return *this; }
    state_entry_chain &noshow() { return *this; }
    state_entry_chain &callimport() { return *this; }
    state_entry_chain &callexport() { return *this; }
    state_entry_chain &mask(uint64_t) { return *this; }
};
struct running_machine_stub { const char *describe_context() const { return ""; } };

struct address_space { unsp_bus *bus; uint16_t read_word(offs_t a) { return bus->read(a & 0x3fffff); } };
struct rectangle { int min_x = 0, max_x = 319, min_y = 0, max_y = 239; rectangle() = default; rectangle(int a, int b, int c, int d) : min_x(a), max_x(b), min_y(c), max_y(d) {} };
struct bitmap_rgb32 {
    int w = 320, h = 240; std::vector<uint32_t> px = std::vector<uint32_t>(320 * 240);
    uint32_t &pix(int y, int x = 0) { return px[y * w + x]; }
};
struct device_memory_interface { typedef std::vector<std::pair<int, const address_space_config *>> space_config_vector; };

struct device_t {
    device_t() = default;
    device_t(const machine_config &, device_type, const char *, device_t *, uint32_t) {}
    virtual ~device_t() = default;
    virtual void device_start() {}
    virtual void device_reset() {}
    template <typename T> void save_item(T &&, const char *) {}
    template <typename T> void save_pointer(T &&, const char *, size_t) {}
    running_machine_stub &machine() { static running_machine_stub m; return m; }
    template <typename... A> void logerror(const char *fmt, A... a) { std::fprintf(stderr, fmt, a...); }
};

struct cpu_device : device_t, device_memory_interface {
    cpu_device(const machine_config &, device_type, const char *, device_t *, uint32_t) {}
    typedef device_memory_interface::space_config_vector space_config_vector;
    // per-instance wiring (set before start())
    unsp_bus *m_bus = nullptr;
    void (*m_insn_hook)(void *, uint32_t) = nullptr;     // before every instruction (HLE traps, tracing)
    void (*m_irq_hook)(void *, int) = nullptr;           // on IRQ/FIQ acknowledge (line: 0 = FIQ, k+1 = IRQk)
    void *m_hook_ctx = nullptr;
    void set_bus(unsp_bus *b) { m_bus = b; }
    void set_hooks(void *ctx, void (*insn)(void *, uint32_t), void (*irq)(void *, int)) { m_hook_ctx = ctx; m_insn_hook = insn; m_irq_hook = irq; }
    address_space_binder space(int) { return address_space_binder{&m_bus}; }
    template <typename T> state_entry_chain &state_add(int, const char *, T &) { static state_entry_chain c; return c; }
    template <typename T> void set_icountptr(T &) {}
    void debugger_instruction_hook(uint32_t pc) { if (m_insn_hook) m_insn_hook(m_hook_ctx, pc); }
    int standard_irq_callback(int line, uint32_t) { if (m_irq_hook) m_irq_hook(m_hook_ctx, line); return 0; }
    bool allow_drc() const { return false; }
    // overridden by unsp_device
    virtual void device_stop() {}
    virtual uint32_t execute_min_cycles() const noexcept { return 1; }
    virtual uint32_t execute_max_cycles() const noexcept { return 1; }
    virtual void execute_run() {}
    virtual void execute_set_input(int, int) {}
    virtual space_config_vector memory_space_config() const { return {}; }
    virtual void state_import(const device_state_entry &) {}
    virtual void state_export(const device_state_entry &) {}
    virtual void state_string_export(const device_state_entry &, std::string &) const {}
};

[[noreturn]] inline void fatalerror_impl(const char *msg) { std::fprintf(stderr, "FATAL: %s", msg); std::exit(3); }
template <typename... A> [[noreturn]] void fatalerror(const char *fmt, A... a) {
    char b[512]; std::snprintf(b, sizeof b, fmt, a...); fatalerror_impl(b);
}

#define DECLARE_DEVICE_TYPE(Type, Class) extern const void *Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) const void *Type = #Short;
