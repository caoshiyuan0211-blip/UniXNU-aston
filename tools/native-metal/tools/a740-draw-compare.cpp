// a740-draw-compare VS.bc FS.bc -- the A740 draw stream (after the a7xx rewrite) of tri_color as hakios's fixture program (which drew
// right on the 12R, stage 4b) and as compiled from AIR for the A740 and linked (stage 4c-2): the registers that differ.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/a7xx_rewrite.hpp"
#include "hakios-metal/chip.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
using namespace hakios::metal;
static std::vector<char> load(const char* p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static std::map<uint32_t, uint32_t> regs(const std::vector<uint32_t>& w) {
    std::map<uint32_t, uint32_t> r;
    for (size_t i = 0; i < w.size();) { auto h = pm4::decode(w[i]); if (h.kind == pm4::Header::kRegisters) for (uint32_t k = 0; k < h.count; k++) r[h.reg + k] = w[i + 1 + k]; i += 1 + h.count; }
    return r;
}
static std::vector<uint32_t> draw(Gpu& gpu, const Program& p) {
    std::string why;
    TextureDesc d; d.format = PixelFormat::kRGBA8Unorm; d.width = 128; d.height = 96; d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    auto t = Texture::make(gpu, d, &why);
    RenderPass pass; pass.color0 = t.get();
    Draw dr; dr.vertexCount = 6; dr.viewport = Viewport{0, 0, 128, 96, 0, 1};
    auto vs = newBuffer(gpu, 4ull * p.vs.k.numWords, &why), fs = newBuffer(gpu, 4ull * p.fs.k.numWords, &why), table = newBuffer(gpu, render::kTableBytes, &why);
    pm4::Stream s;
    if (!render::encode(s, p, pass, dr, Bindings{}, Bindings{}, vs->gpuAddress(), fs->gpuAddress(), table->gpuAddress(), static_cast<uint8_t*>(table->contents()), &why, render::Options{})) {
        std::printf("refused: %s\n", why.c_str()); return {};
    }
    return a7xxRewrite(s.words());
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    setTargetChip(kChipA740);
    auto f = std::make_unique<FakeGpu>();
    Gpu gpu(std::move(f));
    std::string why;
    auto vb = load(argv[1]), fb = load(argv[2]);
    std::shared_ptr<CompiledStage> vs(compileStage(vb.data(), vb.size(), &why)), fs(compileStage(fb.data(), fb.size(), &why));
    auto linked = vs && fs ? linkProgram(vs, fs, PixelFormat::kRGBA8Unorm, &why) : nullptr;
    if (!linked) { std::printf("compile/link: %s\n", why.c_str()); return 1; }
    Program fixture{kernels::tri_color_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_color"};
    auto a = regs(draw(gpu, fixture)), b = regs(draw(gpu, linked->program));
    unsigned same = 0;
    for (auto& [r, v] : a) {
        auto it = b.find(r);
        if (it == b.end()) std::printf("  0x%05x only in the fixture draw: %08x\n", r, v);
        else if (it->second != v) std::printf("  0x%05x fixture %08x  compiled %08x\n", r, v, it->second);
        else same++;
    }
    for (auto& [r, v] : b) if (!a.count(r)) std::printf("  0x%05x only in the compiled draw: %08x\n", r, v);
    std::printf("%u registers equal\n", same);
}
