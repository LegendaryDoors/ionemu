// iON disc layer: data sectors hidden in CD-DA, the sector-1 directory, the block load rule,
// and identification by logical-image SHA-1. Opens a .cue (every referenced file is scanned), any raw container that
// holds the data track (.bin/.img/.mdx...), or a directory of extracted programs (NN_name.mem.bin).
// Damaged dumps such as Danny Phantom's are rebuilt by ion::repair_disc, not here.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ion {

class Disc {
public:
    bool open(const std::string &path);
    const std::string &error() const { return m_error; }

    int entry_count() const { return (int)m_entries.size(); }       // loadable entries; directory entry k-1 = entry k
    const std::string &entry_label(int k) const { return m_entries[k - 1].label; }   // "NN_name.mem.bin"
    bool entry_image(int k, std::vector<uint16_t> &words) const;      // memory image from word 0x40000 (block rule)

    const std::string &sha1() const { return m_sha1; }                // logical-image SHA-1 (sectors 1..N), "" if unknown
    const std::string &title() const { return m_title; }              // known title, or ""
    int sectors() const { return m_top; }
    int bad_checksums() const { return m_bad; }                       // excluding the directory sector
    int missing_sectors() const { return m_missing; }

private:
    struct Entry { std::string label, path; uint32_t offset = 0, bytes = 0; };
    std::vector<Entry> m_entries;
    std::vector<uint8_t> m_logical;                                    // 2048 bytes per logical sector (image mode)
    bool m_from_dir = false;
    std::string m_error, m_sha1, m_title;
    int m_top = 0, m_bad = 0, m_missing = 0;

    bool open_dir(const std::string &dir);
    bool open_image(const std::vector<std::string> &files);
    void identify();
};

std::string sha1_hex(const uint8_t *data, size_t len);

}  // namespace ion
