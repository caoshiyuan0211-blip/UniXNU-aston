// gen-a740-fill: the first native (no Vulkan) compute dispatch for the OnePlus 12R's Adreno 740, as a header for the on-phone GPU
// user test (UniXNU drivers/AdrenoMetal/NativeAdrenoUserTest.h).
//
// SPDX-License-Identifier: MIT
//
// The command stream is Mesa's computerator a7xx path (src/freedreno/computerator/a6xx.cc, CHIP == A7XX: cs_program_emit,
// cs_const_emit, cs_ibo_emit, a6xx_emit_grid, cache_flush), with the register values packed by Mesa's own generated a7xx headers
// (a6xx-pack.xml.h) and the A740's row of Mesa's device table (magic registers, instruction cache size). It is preceded by the
// a7xx part of Turnip's tu6_init_hw prefix: CP_THREAD_CONTROL(BR, concurrent binning off) and a cache invalidate.
// The kernel is hakios-metal's fill_id (tools/kernels/fill_id.asm), assembled for chip 0x43050a01 by hakios's ir3-kernel
// (12r/metal/fixtures/fill_id_a740.hpp): out[gid] = gid * 0x9E3779B1 + c0.x, 16 workgroups of 64.
//
// GPU addresses are not known on the host: each one is a relocation (which buffer, byte offset, low or high half) that the phone
// patches after mapping the buffers.
//
// build: see 12r/metal/build-gen.sh
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util/u_math.h"   // fui(), used by the generated a6xx headers
extern "C" {
#include "common/freedreno_dev_info.h"
}
// The generated headers first: freedreno_pm4.h defines CP_TYPE*_PKT macros that collide with their enum (computerator's order).
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a6xx.xml.h"
#include "common/freedreno_pm4.h"

struct fd_bo;
struct fd_reg_pair {   // as fd6_pack.h
    uint32_t reg;
    uint64_t value;
    struct fd_bo *bo;
    bool is_address;
    bool bo_write;
    uint32_t bo_offset;
    uint32_t bo_shift;
    uint32_t bo_low;
};
#define __bo_type struct fd_bo *
#include "a6xx-pack.xml.h"
#include "adreno-pm4-pack.xml.h"

#include "fixtures/fill_id_a740.hpp"

namespace {
enum Kind : uint8_t {kShader = 1, kBuffer = 2, kDesc = 3, kControl = 4};
struct Reloc {uint32_t dword; Kind kind; uint32_t offset; bool high;};

struct Stream {
    std::vector<uint32_t> words;
    std::vector<Reloc> relocs;
    void ring(uint32_t v) {words.push_back(v);}
    void pkt4(uint32_t reg, uint32_t count) {ring(pm4_pkt4_hdr(reg, count));}
    void pkt7(uint32_t op, uint32_t count) {ring(pm4_pkt7_hdr(op, count));}
    void reloc(Kind kind, uint32_t offset) {
        relocs.push_back({uint32_t(words.size()), kind, offset, false}); ring(0);
        relocs.push_back({uint32_t(words.size()), kind, offset, true}); ring(0);
    }
    // OUT_REG: consecutive registers go out in one PKT4, as fd6_pack.h's OUT_REG does.
    void regs(std::initializer_list<fd_reg_pair> pairs) {
        std::vector<fd_reg_pair> p(pairs);
        for (size_t i = 0; i < p.size();) {
            size_t n = 1;
            while (i + n < p.size() && p[i + n].reg == p[i].reg + n) ++n;
            pkt4(p[i].reg, uint32_t(n));
            for (size_t k = 0; k < n; ++k) ring(uint32_t(p[i + k].value));
            i += n;
        }
    }
};

uint32_t regid(unsigned reg, unsigned comp) {return (reg << 2) | comp;}

void eventWrite(Stream &s, vgt_event_type evt, bool timestamp, uint32_t seqno) {
    s.pkt7(CP_EVENT_WRITE7, timestamp ? 4 : 1);
    s.ring(CP_EVENT_WRITE7_0_EVENT(evt) |
           (timestamp ? CP_EVENT_WRITE7_0_WRITE_ENABLED | CP_EVENT_WRITE7_0_WRITE_SRC(EV_WRITE_USER_32B) : 0));
    if (timestamp) {s.reloc(kControl, 0); s.ring(seqno);}
}

void print(FILE *f, const char *name, const std::vector<uint32_t> &w) {
    fprintf(f, "static const uint32_t %s[%zu] = {", name, w.size());
    for (size_t i = 0; i < w.size(); ++i) fprintf(f, "%s0x%08x,", i % 8 ? " " : "\n    ", w[i]);
    fprintf(f, "\n};\n");
}
void printRelocs(FILE *f, const char *name, const std::vector<Reloc> &r) {
    fprintf(f, "static const struct A740FillReloc %s[%zu] = {", name, r.size());
    for (size_t i = 0; i < r.size(); ++i)
        fprintf(f, "%s{%u, %u, %u, %u},", i % 4 ? " " : "\n    ", r[i].dword, unsigned(r[i].kind), r[i].offset, unsigned(r[i].high));
    fprintf(f, "\n};\n");
}
}  // namespace

int main(int argc, char **argv) {
    if (argc != 2 && !(argc == 3 && !strcmp(argv[1], "--magic"))) {fprintf(stderr, "usage: gen-a740-fill OUT.h | --magic OUT.txt\n"); return 2;}
    const auto &k = hakios::metal::kernels::fill_id_a740;
    const fd_dev_id id = {0, 0x43050a01};
    const struct fd_dev_info *info = fd_dev_info_raw(&id);
    if (!info || info->chip != 7) {fprintf(stderr, "no A740 (gen 7) row in Mesa's device table\n"); return 1;}
    if (argc == 3) {   // the A740's magic registers and instruction cache size, for hakios-metal's a7xx.hpp
        FILE *m = fopen(argv[2], "w");
        if (!m) return 1;
        for (size_t i = 0; i < sizeof info->a6xx.magic_raw / sizeof info->a6xx.magic_raw[0] && info->a6xx.magic_raw[i].reg; ++i)
            fprintf(m, "0x%x 0x%x\n", unsigned(info->a6xx.magic_raw[i].reg), unsigned(info->a6xx.magic_raw[i].value));
        fclose(m);
        printf("instr_cache_size %u\n", info->a6xx.instr_cache_size);
        return 0;
    }
    const uint32_t groups[3] = {16, 1, 1};
    const unsigned threadsize = k.doubleThreadsize ? THREAD128 : THREAD64;

    Stream s;
    // Turnip's tu6_init_hw, a7xx: BR thread with concurrent binning off, then invalidate the caches and wait for idle.
    s.pkt7(CP_THREAD_CONTROL, 1);
    s.ring(CP_THREAD_CONTROL_0_THREAD(CP_SET_THREAD_BR) | CP_THREAD_CONTROL_0_CONCURRENT_BIN_DISABLE);
    eventWrite(s, CACHE_INVALIDATE7, false, 0);
    s.pkt7(CP_WAIT_FOR_IDLE, 0);

    // ---- cs_program_emit<A7XX> ----
    s.regs({A6XX_SP_MODE_CONTROL(.constant_demotion_enable = true, .isammode = ISAMMODE_GL, .shared_consts_enable = false)});
    s.pkt4(REG_A6XX_SP_PERFCTR_ENABLE, 1); s.ring(A6XX_SP_PERFCTR_ENABLE_CS);
    s.pkt4(REG_A6XX_SP_FLOAT_CNTL, 1); s.ring(0);
    unsigned magic = 0;
    for (size_t i = 0; i < sizeof info->a6xx.magic_raw / sizeof info->a6xx.magic_raw[0]; ++i) {
        const auto m = info->a6xx.magic_raw[i];
        if (!m.reg) break;
        s.pkt4(m.reg, 1); s.ring(uint32_t(m.value)); ++magic;
    }
    s.regs({A7XX_HLSQ_INVALIDATE_CMD(.vs_state = true, .hs_state = true, .ds_state = true, .gs_state = true, .fs_state = true,
                                     .cs_state = true, .gfx_ibo = true)});
    s.regs({A7XX_HLSQ_CS_CNTL(.constlen = (k.constlen + 3) & ~3u, .enabled = true)});
    s.pkt4(REG_A6XX_SP_CS_CONFIG, 2);
    s.ring(A6XX_SP_CS_CONFIG_ENABLED | A6XX_SP_CS_CONFIG_NIBO(k.numBufs) | A6XX_SP_CS_CONFIG_NTEX(k.numSamp) |
           A6XX_SP_CS_CONFIG_NSAMP(k.numSamp));
    s.ring(k.instrlen);
    s.pkt4(REG_A6XX_SP_CS_CTRL_REG0, 1);
    s.ring(A6XX_SP_CS_CTRL_REG0_THREADSIZE((a6xx_threadsize)threadsize) | A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(k.maxReg + 1) |
           A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(k.maxHalfReg + 1) | (k.mergedRegs ? A6XX_SP_CS_CTRL_REG0_MERGEDREGS : 0) |
           (k.earlyPreamble ? A6XX_SP_CS_CTRL_REG0_EARLYPREAMBLE : 0) | A6XX_SP_CS_CTRL_REG0_BRANCHSTACK(k.branchstackHw));
    s.regs({A7XX_HLSQ_FS_CNTL_0(.threadsize = THREAD64)});
    s.regs({A7XX_HLSQ_CONTROL_2_REG(.dword = 0xfcfcfcfc), A7XX_HLSQ_CONTROL_3_REG(.dword = 0xfcfcfcfc),
            A7XX_HLSQ_CONTROL_4_REG(.dword = 0xfcfcfcfc), A7XX_HLSQ_CONTROL_5_REG(.dword = 0x0000fc00)});
    const int sharedSize = ((int)k.sharedSize - 1) / 1024 > 1 ? ((int)k.sharedSize - 1) / 1024 : 1;
    const a6xx_const_ram_mode mode = k.constlen > 256 ? CONSTLEN_512 : k.constlen > 192 ? CONSTLEN_256 :
                                     k.constlen > 128 ? CONSTLEN_192 : CONSTLEN_128;
    s.pkt4(REG_A6XX_SP_CS_CTRL_REG1, 1);
    s.ring(A6XX_SP_CS_CTRL_REG1_SHARED_SIZE(sharedSize) | A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE(mode));
    const unsigned tileHeight = k.localSize[1] % 8 == 0 ? 3 : k.localSize[1] % 4 == 0 ? 5 : k.localSize[1] % 2 == 0 ? 9 : 17;
    s.regs({A7XX_HLSQ_CS_CNTL_1(.linearlocalidregid = regid(63, 0), .threadsize = (a6xx_threadsize)threadsize,
                                .workgrouprastorderzfirsten = true, .wgtilewidth = 4, .wgtileheight = tileHeight)});
    s.pkt4(REG_A6XX_SP_CS_CNTL_0, 1);
    s.ring(A6XX_SP_CS_CNTL_0_WGIDCONSTID(k.workGroupIdRegid) | A6XX_SP_CS_CNTL_0_WGSIZECONSTID(regid(63, 0)) |
           A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID(regid(63, 0)) | A6XX_SP_CS_CNTL_0_LOCALIDREGID(k.localIdRegid));
    s.regs({A7XX_SP_CS_CNTL_1(.linearlocalidregid = regid(63, 0), .threadsize = (a6xx_threadsize)threadsize,
                              .workitemrastorder = WORKITEMRASTORDER_LINEAR)});
    s.pkt4(REG_A6XX_SP_CS_OBJ_START, 2); s.reloc(kShader, 0);
    s.pkt4(REG_A6XX_SP_CS_INSTRLEN, 1); s.ring(k.instrlen);
    s.pkt4(REG_A6XX_SP_CS_OBJ_START, 2); s.reloc(kShader, 0);
    const uint32_t preload = k.instrlen < info->a6xx.instr_cache_size ? k.instrlen : info->a6xx.instr_cache_size;
    s.pkt7(CP_LOAD_STATE6_FRAG, 3);
    s.ring(CP_LOAD_STATE6_0_DST_OFF(0) | CP_LOAD_STATE6_0_STATE_TYPE(ST6_SHADER) | CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
           CP_LOAD_STATE6_0_STATE_BLOCK(SB6_CS_SHADER) | CP_LOAD_STATE6_0_NUM_UNIT(preload));
    s.reloc(kShader, 0);
    if (k.pvtmemSize) {fprintf(stderr, "pvtmem not supported here\n"); return 1;}

    // ---- cs_const_emit<A7XX>: the immediates at their offset (vec4), truncated to constlen ----
    {
        const uint32_t base = k.immediatesOffsetVec4;
        uint32_t size = (k.numImmediates + 3) / 4;
        size = (size + base < k.constlen ? size + base : k.constlen) - base;
        const uint32_t dwords = size * 4;
        s.pkt7(CP_LOAD_STATE6_FRAG, 3 + dwords);
        s.ring(CP_LOAD_STATE6_0_DST_OFF(base) | CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) | CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
               CP_LOAD_STATE6_0_STATE_BLOCK(SB6_CS_SHADER) | CP_LOAD_STATE6_0_NUM_UNIT(size));
        s.ring(CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
        s.ring(CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
        for (uint32_t i = 0; i < dwords; ++i) s.ring(i < k.numImmediates ? k.immediates[i] : 0);
    }

    // ---- cs_ibo_emit<A7XX>: one 32-bit uint buffer of 1024 elements, its descriptor in its own buffer ----
    Stream d;
    {
        const unsigned sz = k.bufs[0].sizeBytes / 4, width = sz & 0x7fff, height = sz >> 15;
        d.ring(A6XX_TEX_CONST_0_FMT(FMT6_32_UINT) | A6XX_TEX_CONST_0_TILE_MODE(TILE6_LINEAR));
        d.ring(A6XX_TEX_CONST_1_WIDTH(width) | A6XX_TEX_CONST_1_HEIGHT(height));
        d.ring(A6XX_TEX_CONST_2_PITCH(0) | A6XX_TEX_CONST_2_STRUCTSIZETEXELS(1) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_BUFFER));
        d.ring(A6XX_TEX_CONST_3_ARRAY_PITCH(0));
        d.reloc(kBuffer, 0);
        for (int i = 0; i < 10; ++i) d.ring(0);
    }
    s.pkt7(CP_LOAD_STATE6_FRAG, 3);
    s.ring(CP_LOAD_STATE6_0_DST_OFF(0) | CP_LOAD_STATE6_0_STATE_TYPE(ST6_IBO) | CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
           CP_LOAD_STATE6_0_STATE_BLOCK(SB6_CS_SHADER) | CP_LOAD_STATE6_0_NUM_UNIT(k.numBufs));
    s.reloc(kDesc, 0);
    s.pkt4(REG_A6XX_SP_CS_IBO, 2); s.reloc(kDesc, 0);
    s.pkt4(REG_A6XX_SP_CS_IBO_COUNT, 1); s.ring(k.numBufs);

    // ---- a6xx_emit_grid<A7XX> ----
    s.pkt7(CP_SET_MARKER, 1); s.ring(A6XX_CP_SET_MARKER_0_MODE(RM6_COMPUTE));
    unsigned workDim = 0;
    for (int i = 0; i < 3 && groups[i]; ++i) ++workDim;
    s.regs({A7XX_HLSQ_CS_NDRANGE_0(.kerneldim = workDim, .localsizex = k.localSize[0] - 1, .localsizey = k.localSize[1] - 1,
                                   .localsizez = k.localSize[2] - 1)});
    s.regs({A7XX_HLSQ_CS_LAST_LOCAL_SIZE(.localsizex = k.localSize[0] - 1, .localsizey = k.localSize[1] - 1,
                                         .localsizez = k.localSize[2] - 1)});
    s.regs({A7XX_HLSQ_CS_NDRANGE_1(.globalsize_x = k.localSize[0] * groups[0])});
    s.regs({A7XX_HLSQ_CS_NDRANGE_2(0)});
    s.regs({A7XX_HLSQ_CS_NDRANGE_3(.globalsize_y = k.localSize[1] * groups[1])});
    s.regs({A7XX_HLSQ_CS_NDRANGE_4(0)});
    s.regs({A7XX_HLSQ_CS_NDRANGE_5(.globalsize_z = k.localSize[2] * groups[2])});
    s.regs({A7XX_HLSQ_CS_NDRANGE_6(0)});
    s.regs({A7XX_HLSQ_CS_KERNEL_GROUP_X(1)});
    s.regs({A7XX_HLSQ_CS_KERNEL_GROUP_Y(1)});
    s.regs({A7XX_HLSQ_CS_KERNEL_GROUP_Z(1)});
    s.pkt7(CP_EXEC_CS, 4);
    s.ring(0);
    s.ring(CP_EXEC_CS_1_NGROUPS_X(groups[0]));
    s.ring(CP_EXEC_CS_2_NGROUPS_Y(groups[1]));
    s.ring(CP_EXEC_CS_3_NGROUPS_Z(groups[2]));
    s.pkt7(CP_WAIT_FOR_IDLE, 0);

    // ---- cache_flush<A7XX>: RB_DONE_TS writes seqno 1 into the control buffer, the CP waits for it, then CACHE_FLUSH7 ----
    eventWrite(s, RB_DONE_TS, true, 1);
    s.pkt7(CP_WAIT_REG_MEM, 6);
    s.ring(CP_WAIT_REG_MEM_0_FUNCTION(WRITE_EQ) | CP_WAIT_REG_MEM_0_POLL(POLL_MEMORY));
    s.reloc(kControl, 0);
    s.ring(CP_WAIT_REG_MEM_3_REF(1));
    s.ring(CP_WAIT_REG_MEM_4_MASK(~0u));
    s.ring(CP_WAIT_REG_MEM_5_DELAY_LOOP_CYCLES(16));
    eventWrite(s, CACHE_FLUSH7, false, 0);

    FILE *f = fopen(argv[1], "w");
    if (!f) return 1;
    fprintf(f, "// GENERATED by 12r/metal/gen-a740-fill.cc from Mesa 25.0.7's a7xx register definitions and A740 device row\n"
               "// (magic registers: %u, instr_cache_size: %u). Do not edit. SPDX-License-Identifier: MIT\n"
               "#pragma once\n#include <stdint.h>\n\n"
               "// Relocation kinds: the buffer whose GPU address (plus offset) goes into ib[dword] (low half) or ib[dword] (high half).\n"
               "enum {A740FillShader = 1, A740FillBuffer = 2, A740FillDesc = 3, A740FillControl = 4};\n"
               "struct A740FillReloc {uint16_t dword; uint8_t kind; uint32_t offset; uint8_t high;};\n"
               "#define A740_FILL_K 0x9e3779b1u\n#define A740_FILL_C 0x%08xu\n#define A740_FILL_COUNT %u\n\n",
            magic, info->a6xx.instr_cache_size, k.immediates[0], k.bufs[0].sizeBytes / 4);
    print(f, "kA740FillIb", s.words);
    printRelocs(f, "kA740FillIbRelocs", s.relocs);
    print(f, "kA740FillDesc", d.words);
    printRelocs(f, "kA740FillDescRelocs", d.relocs);
    std::vector<uint32_t> shader(k.words, k.words + k.numWords);
    print(f, "kA740FillShader", shader);
    fclose(f);
    fprintf(stderr, "%s: IB %zu dwords, %zu relocs; descriptor %zu dwords; shader %u dwords; %u magic registers\n", argv[1],
            s.words.size(), s.relocs.size(), d.words.size(), k.numWords, magic);
    return 0;
}
