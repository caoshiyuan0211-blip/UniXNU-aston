// a740-air-listing CHIP FILE.bc -- one AIR stage compiled for CHIP (a635|a740) by hakios-metal's compiler: its parameters and ir3 listing.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/chip.hpp"
#include "hakios-metal/compiler.hpp"
using namespace hakios::metal;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    setTargetChip(std::strcmp(argv[1], "a740") == 0 ? kChipA740 : a6xx::kChipId);
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<char> bc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string why;
    auto s = compileStage(bc.data(), bc.size(), &why);
    if (!s) { std::printf("refused: %s\n", why.c_str()); return 1; }
    const Kernel& k = s->stage.k;
    std::printf("instrlen %u constlen %u maxReg %d maxHalfReg %d double %d merged %d earlyPreamble %d branchstack %u pvtmem %u imm %u@%u\n",
                k.instrlen, k.constlen, k.maxReg, k.maxHalfReg, k.doubleThreadsize, k.mergedRegs, k.earlyPreamble, k.branchstackHw, k.pvtmemSize,
                k.numImmediates, k.immediatesOffsetVec4);
    std::printf("%s\n", s->listing.c_str());
}
