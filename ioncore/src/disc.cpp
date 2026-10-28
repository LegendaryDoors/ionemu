// iON disc layer (see disc.h): deframing, the directory, program images, identification and repair.
#include "disc.h"
#include "ioncore/ioncore.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <climits>
#include <fstream>
#include <map>

namespace ion {
namespace fs = std::filesystem;

namespace {
constexpr size_t RAW = 2352, BODY = 2048;
constexpr uint32_t CHUNK = 0x1000, BLOCK = 0x40000;       // 4 KB chunks stored in descending order per 256 KB block

struct Known { const char *sha1, *title; };
const Known k_known[] = {                                 // logical-image SHA-1 (from sector 1) -> title
    {"58b566e71a801063bc0a81bdea7b086f65fdfa58", "Blue's Room - Birthday Party Surprise"},
    {"ef9eb885cee363c97dbc9d6efd67166a10336fb6", "Dora the Explorer - The Great Soccer Ball Adventure"},
    {"4402f14adf50596f6bdefc36316e15e91f50c012", "Batman - Shadows of Gotham City"},
    {"8b3e0260583738b7caa02936d44ff73c13ad3256", "Best of Nickelodeon"},
    {"18373e026cabdcb2f1c2f5100cfc6a1ab1f58641", "SpongeBob SquarePants - Best Day Ever"},
    {"9e3e7a773608492356dbd8cce6fb8ac8246b8176", "Danny Phantom - Ghosts on the Loose"},
    // The only Danny Phantom dump in circulation ("[b]") has 2 bad-checksum sectors (IonIntro.bin and
    // MG0.bin, both 129024 bytes in). It plays the 20,000-frame golden scenario identically to the donor-repaired
    // image, so it is accepted under the same title; Disc::bad_checksums() still reports the damage.
    {"61fe7ea24c0efffe953406044101775ffc1cabf6", "Danny Phantom - Ghosts on the Loose"},
};

uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t wordsum(const uint8_t *p) { uint32_t s = 0; for (size_t i = 0; i < BODY; i += 2) s += rd16(p + i); return (uint16_t)s; }

// Where a sector's frame sits in an image file, and the checksum stored with it (for repairs).
struct SectorLoc { std::string file; uint64_t offset; uint16_t stored_ck; };

// Scan a file for framed sectors: 40 x 0x5A sync | u32 (n << 16) | u32 complement | 2048 data | u16 word-sum | pad.
// The first copy of each sector number wins. Streams in chunks.
void scan(const std::string &path, std::map<uint32_t, std::vector<uint8_t>> &secs, std::map<uint32_t, bool> &bad,
          std::map<uint32_t, SectorLoc> *locs = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> buf;
    const size_t READ = 16 << 20;
    size_t pos = 0;                                       // next offset in buf to test
    uint64_t base = 0;                                    // file offset of buf[0]
    bool eof = false;
    while (true) {
        if (buf.size() - pos < RAW && !eof) {             // refill, keeping the unscanned tail
            buf.erase(buf.begin(), buf.begin() + pos);
            base += pos;
            pos = 0;
            const size_t old = buf.size();
            buf.resize(old + READ);
            f.read(reinterpret_cast<char *>(buf.data() + old), READ);
            buf.resize(old + (size_t)f.gcount());
            eof = f.gcount() == 0 || !f;
            continue;
        }
        if (buf.size() - pos < RAW) break;
        const uint8_t *p = buf.data() + pos;
        if (p[0] == 0x5A && p[39] == 0x5A && std::all_of(p, p + 40, [](uint8_t b) { return b == 0x5A; })) {
            const uint32_t h = rd32(p + 40), c = rd32(p + 44);
            if ((h ^ c) == 0xffffffffu && (h & 0xffff) == 0) {
                const uint32_t n = h >> 16;
                if (!secs.count(n)) {
                    secs[n].assign(p + 48, p + 48 + BODY);
                    if (wordsum(p + 48) != rd16(p + 48 + BODY)) bad[n] = true;
                    if (locs) (*locs)[n] = {path, base + pos, rd16(p + 48 + BODY)};
                }
                pos += RAW;
                continue;
            }
        }
        pos++;
    }
}

std::string lower_ext(const std::string &p) {
    std::string e = fs::path(p).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// The image files behind a path: the FILE entries of a cue sheet, or the file itself.
std::vector<std::string> image_files(const std::string &path, std::string &error) {
    std::vector<std::string> files;
    if (lower_ext(path) == ".cue") {
        std::ifstream cue(path);
        std::string line;
        while (std::getline(cue, line)) {
            const size_t a = line.find("FILE \""), b = line.rfind('"');
            if (a != std::string::npos && b > a + 6) files.push_back((fs::path(path).parent_path() / line.substr(a + 6, b - a - 6)).string());
        }
        if (files.empty()) error = "no FILE entries in " + path;
        return files;
    }
    return {path};
}

std::vector<uint8_t> mem_image(const uint8_t *data, size_t len) {       // disc file bytes -> memory image bytes
    std::vector<uint8_t> out;
    for (size_t b = 0; b < len; b += BLOCK) {
        std::vector<uint8_t> blk(BLOCK, 0);
        memcpy(blk.data(), data + b, std::min<size_t>(BLOCK, len - b));
        for (uint32_t i = BLOCK; i > 0; i -= CHUNK) out.insert(out.end(), blk.begin() + (i - CHUNK), blk.begin() + i);
    }
    return out;
}
}

// ---------------------------------------------------------------- SHA-1 (FIPS 180-1)
std::string sha1_hex(const uint8_t *data, size_t len) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    auto block = [&](const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) block(data + i);
    uint8_t tail[128] = {};
    const size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    const size_t tl = rem < 56 ? 64 : 128;
    const uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    block(tail);
    if (tl == 128) block(tail + 64);
    char out[41];
    for (int k = 0; k < 5; k++) snprintf(out + 8 * k, 9, "%08x", h[k]);
    return out;
}

// ---------------------------------------------------------------- Disc
bool Disc::open(const std::string &path) {
    *this = Disc();
    std::error_code ec;
    if (fs::is_directory(path, ec)) return open_dir(path);
    if (!fs::exists(path, ec)) { m_error = "not found: " + path; return false; }
    std::string err;
    const std::vector<std::string> files = image_files(path, err);   // a cue sheet's FILE entries, or the file itself
    if (files.empty()) { m_error = err; return false; }
    return open_image(files);
}

bool Disc::open_dir(const std::string &dir) {             // a directory of extracted programs
    m_from_dir = true;
    std::vector<std::string> names;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 8 && n.compare(n.size() - 8, 8, ".mem.bin") == 0) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (auto &n : names) m_entries.push_back({n, (fs::path(dir) / n).string(), 0, 0});
    if (m_entries.empty()) { m_error = "no *.mem.bin files in " + dir; return false; }
    std::ifstream lf(fs::path(dir) / "logical.bin", std::ios::binary);   // for identification, if present
    if (lf) {
        m_logical.assign(std::istreambuf_iterator<char>(lf), {});
        m_top = (int)(m_logical.size() / BODY) - 1;
        identify();
        m_logical.clear();
        m_logical.shrink_to_fit();
    }
    return true;
}

bool Disc::open_image(const std::vector<std::string> &files) {
    std::map<uint32_t, std::vector<uint8_t>> secs;
    std::map<uint32_t, bool> bad;
    for (auto &f : files) scan(f, secs, bad);
    if (secs.empty()) { m_error = "no iON data sectors found"; return false; }
    m_top = (int)secs.rbegin()->first;
    m_logical.assign((size_t)(m_top + 1) * BODY, 0);
    for (auto &[n, body] : secs) memcpy(&m_logical[(size_t)n * BODY], body.data(), BODY);
    for (int n = 1; n <= m_top; n++) if (!secs.count(n)) m_missing++;
    for (auto &[n, b] : bad) if (n != 1) m_bad++;         // the directory sector never matches its checksum
    const uint8_t *d = &m_logical[BODY];
    if (m_top < 1 || memcmp(d, "This is Hasbro property ", 24) != 0) { m_error = "directory not found in sector 1"; return false; }
    const int count = rd16(d + 24);
    struct Raw { std::string name; uint32_t start, bytes; };
    std::vector<Raw> raw;
    for (int i = 0; i < count && 26 + 42 * (i + 1) <= (int)BODY; i++) {
        const uint8_t *e = d + 26 + 42 * i;
        raw.push_back({std::string(reinterpret_cast<const char *>(e), strnlen(reinterpret_cast<const char *>(e), 28)), rd32(e + 32),
                       (uint32_t)rd16(e + 40) * 256});
    }
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i].name == "lastfile.bin") continue;      // not a program
        const uint64_t end = i + 1 < raw.size() ? raw[i + 1].start : (uint64_t)m_top + 1;
        const uint64_t off = (uint64_t)raw[i].start * BODY;
        const uint64_t lim = std::min<uint64_t>({end * BODY, off + raw[i].bytes, m_logical.size()});
        char label[64];
        snprintf(label, sizeof label, "%02zu_%s.mem.bin", i, raw[i].name.c_str());
        m_entries.push_back({label, "", (uint32_t)off, (uint32_t)(lim > off ? lim - off : 0)});
    }
    identify();
    return true;
}

void Disc::identify() {
    if (m_logical.size() <= BODY) return;
    m_sha1 = sha1_hex(m_logical.data() + BODY, m_logical.size() - BODY);
    for (auto &k : k_known) if (m_sha1 == k.sha1) m_title = k.title;
}

// ---------------------------------------------------------------- repair
// A damaged sector keeps its stored checksum, so a replacement must (1) sum to it and (2) be close to the damaged body:
// real donors differ in the burst-error bytes (110-204 seen), random 16-bit collisions in ~2000 of 2048.
RepairReport repair_disc(const std::string &path, const std::vector<std::string> &donors, const std::string &out_dir) {
    RepairReport rep;
    std::string err;
    const std::vector<std::string> files = image_files(path, err);
    if (files.empty()) { rep.error = err.empty() ? "not a disc image: " + path : err; return rep; }
    std::map<uint32_t, std::vector<uint8_t>> secs;
    std::map<uint32_t, bool> bad;
    std::map<uint32_t, SectorLoc> locs;
    for (auto &f : files) scan(f, secs, bad, &locs);
    if (secs.empty()) { rep.error = "no iON data sectors found in " + path; return rep; }
    std::map<uint32_t, uint16_t> wanted;                  // damaged sector -> the checksum a donor must have
    for (auto &[n, b] : bad) if (n != 1) wanted[n] = locs[n].stored_ck;
    rep.damaged = (int)wanted.size();
    std::map<uint16_t, std::vector<std::vector<uint8_t>>> cands;
    auto collect = [&](const std::map<uint32_t, std::vector<uint8_t>> &pool, const std::map<uint32_t, bool> &poolBad) {
        for (auto &[n, body] : pool) {
            if (poolBad.count(n)) continue;
            const uint16_t ck = wordsum(body.data());
            for (auto &[m, want] : wanted) if (want == ck) { cands[ck].push_back(body); break; }
        }
    };
    if (!wanted.empty()) {
        collect(secs, bad);
        for (auto &d : donors) {
            std::string derr;
            std::map<uint32_t, std::vector<uint8_t>> dsecs;
            std::map<uint32_t, bool> dbad;
            for (auto &f : image_files(d, derr)) scan(f, dsecs, dbad);
            if (dsecs.empty()) rep.notes.push_back("donor ignored (no iON data sectors): " + d);
            else collect(dsecs, dbad);
        }
    }
    std::map<uint32_t, std::vector<uint8_t>> patched;     // sector -> new body
    for (auto &[n, ck] : wanted) {
        const std::vector<uint8_t> &cur = secs[n];
        int best = INT32_MAX;
        const std::vector<uint8_t> *pick = nullptr;
        for (auto &body : cands[ck]) {
            int diff = 0;
            for (size_t i = 0; i < BODY; i++) diff += body[i] != cur[i];
            if (diff < best) { best = diff; pick = &body; }
        }
        char note[128];
        if (pick && best < 512) {
            patched[n] = *pick;
            snprintf(note, sizeof note, "sector %u: %d bytes replaced from a matching block", n, best);
        } else {
            snprintf(note, sizeof note, "sector %u: no donor found%s", n, cands[ck].empty() ? "" : " (only unrelated blocks share its checksum)");
        }
        rep.notes.push_back(note);
    }
    rep.repaired = (int)patched.size();
    // the repaired copy: every image file copied with " (repaired)" in its name, bodies patched in place, cue rewritten
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    auto renamed = [&](const std::string &f) { const fs::path p(f); return (fs::path(out_dir) / (p.stem().string() + " (repaired)" + p.extension().string())).string(); };
    for (auto &f : files) {
        const std::string dst = renamed(f);
        fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { rep.error = "cannot write " + dst + ": " + ec.message(); return rep; }
        std::fstream out(dst, std::ios::binary | std::ios::in | std::ios::out);
        for (auto &[n, body] : patched)
            if (locs[n].file == f) { out.seekp((std::streamoff)(locs[n].offset + 48)); out.write(reinterpret_cast<const char *>(body.data()), BODY); }
        if (!out) { rep.error = "cannot patch " + dst; return rep; }
    }
    if (files.size() == 1 && files[0] == path) rep.output = renamed(path);
    else {                                                // cue sheet: same lines, FILE names pointing at the copies
        std::ifstream in(path);
        const std::string cue_out = renamed(path);
        std::ofstream out(cue_out);
        std::string line;
        while (std::getline(in, line)) {
            const size_t a = line.find("FILE \""), b = line.rfind('"');
            if (a != std::string::npos && b > a + 6) line = line.substr(0, a + 6) + fs::path(renamed(line.substr(a + 6, b - a - 6))).filename().string() + line.substr(b);
            out << line << "\n";
        }
        rep.output = cue_out;
    }
    // identification of the result
    for (auto &[n, body] : patched) secs[n] = body;
    const uint32_t top = secs.rbegin()->first;
    std::vector<uint8_t> logical((size_t)(top + 1) * BODY, 0);
    for (auto &[n, body] : secs) memcpy(&logical[(size_t)n * BODY], body.data(), BODY);
    rep.sha1_after = sha1_hex(logical.data() + BODY, logical.size() - BODY);
    for (auto &k : k_known) if (rep.sha1_after == k.sha1) rep.title_after = k.title;
    rep.ok = true;
    return rep;
}

bool Disc::entry_image(int k, std::vector<uint16_t> &words) const {
    if (k < 1 || k > (int)m_entries.size()) return false;
    const Entry &e = m_entries[k - 1];
    std::vector<uint8_t> bytes;
    if (m_from_dir) {
        std::ifstream f(e.path, std::ios::binary);
        if (!f) return false;
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    } else {
        bytes = mem_image(m_logical.data() + e.offset, e.bytes);
    }
    words.resize(bytes.size() / 2);
    for (size_t i = 0; i < words.size(); i++) words[i] = rd16(&bytes[2 * i]);
    return true;
}

}  // namespace ion
