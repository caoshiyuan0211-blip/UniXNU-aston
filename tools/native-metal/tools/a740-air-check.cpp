// a740-air-check CHIP FILE.bc... -- compile each AIR module with hakios-metal's compiler (AIR front end + Mesa ir3) for CHIP
// (a635 or a740) and print, per file: ok/refused, the stage, instrlen, a hash of the words. Diff two runs to see what the A740
// target changes and whether anything compiles for one chip only.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/chip.hpp"
#include "hakios-metal/compiler.hpp"

using namespace hakios::metal;

static uint64_t fnv(const uint32_t* w, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s a635|a740 FILE.bc...\n", argv[0]); return 2; }
    setTargetChip(std::strcmp(argv[1], "a740") == 0 ? kChipA740 : a6xx::kChipId);
    unsigned ok = 0, refused = 0;
    for (int i = 2; i < argc; i++) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<char> bc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string why;
        const char* name = std::strrchr(argv[i], '/') ? std::strrchr(argv[i], '/') + 1 : argv[i];
        auto s = compileStage(bc.data(), bc.size(), &why);
        if (s) {
            const Kernel& k = s->stage.k;
            std::printf("%-28s ok    stage %u instrlen %3u maxreg %2d words %4u hash %016llx\n", name, unsigned(s->stage.kind), k.instrlen,
                        k.maxReg, k.numWords, (unsigned long long)fnv(k.words, k.numWords));
            ok++;
            continue;
        }
        auto c = compileAir(bc.data(), bc.size(), &why);
        if (c) {
            const Kernel& k = c->kernel;
            std::printf("%-28s ok    kernel  instrlen %3u maxreg %2d words %4u hash %016llx\n", name, k.instrlen, k.maxReg, k.numWords,
                        (unsigned long long)fnv(k.words, k.numWords));
            ok++;
        } else {
            std::printf("%-28s REFUSED %s\n", name, why.substr(0, 100).c_str());
            refused++;
        }
    }
    std::fprintf(stderr, "%s: %u compiled, %u refused\n", argv[1], ok, refused);
    return 0;
}
