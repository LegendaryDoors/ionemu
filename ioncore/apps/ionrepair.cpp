// ionrepair: repair a damaged iON dump from donor images (ion::repair_disc).
// usage: ionrepair <image .cue/.bin/.mdx> <out_dir> [donor images...]
#include "ioncore/ioncore.h"
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: ionrepair <image> <out_dir> [donor images...]\n"); return 1; }
    std::vector<std::string> donors(argv + 3, argv + argc);
    const ion::RepairReport r = ion::repair_disc(argv[1], donors, argv[2]);
    for (auto &n : r.notes) printf("%s\n", n.c_str());
    if (!r.ok) { fprintf(stderr, "failed: %s\n", r.error.c_str()); return 2; }
    printf("damaged sectors: %d, repaired: %d\noutput: %s\nSHA-1 after: %s (%s)\n", r.damaged, r.repaired, r.output.c_str(), r.sha1_after.c_str(),
           r.title_after.empty() ? "unknown title" : r.title_after.c_str());
    return r.damaged == r.repaired ? 0 : 3;
}
