// a740-compile: compile SPIR-V shaders for the OnePlus 12R's Adreno 740 (chip 0x43050a01) with Mesa's ir3 compiler, the way Turnip
// prepares them (Vulkan-runtime NIR passes, push constants in the const file), and export what a draw's registers are made from
// (native Metal, 12r/METAL-DRIVER-PLAN.md).
//
//   a740-compile vs|fs FILE.spv [OUT.json]       one stage: disassembly, layout, binary
//   a740-compile link VS.spv FS.spv OUTDIR       both stages: OUTDIR/vs.json, OUTDIR/fs.json and OUTDIR/link.json, the registers
//                                                Turnip's tu6_emit_vpc (VS last, no streamout) and tu6_emit_fs_inputs write for them
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "common/freedreno_dev_info.h"
#include "compiler/nir/nir_builder.h"
#include "ir3-isa.h"
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a6xx.xml.h"

static void *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(*size);
    if (fread(buf, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    return buf;
}

struct compiled {struct ir3_shader_variant *v; unsigned push_vec4, push_dwords;};

static struct compiled compile(struct ir3_compiler *compiler, gl_shader_stage stage, const char *path) {
    size_t size;
    void *spirv = read_all(path, &size);
    const struct spirv_capabilities caps = {.Shader = true, .DrawParameters = true};
    const struct spirv_to_nir_options spirv_options = {.capabilities = &caps};
    nir_shader *nir = spirv_to_nir(spirv, size / 4, NULL, 0, stage, "main", &spirv_options, ir3_get_compiler_options(compiler));
    if (!nir) { fprintf(stderr, "spirv_to_nir failed\n"); exit(1); }
    // As the Vulkan runtime's vk_spirv_to_nir (vulkan/runtime/vk_nir.c), which Turnip uses.
    NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_function_temp);
    NIR_PASS_V(nir, nir_lower_returns);
    NIR_PASS_V(nir, nir_inline_functions);
    NIR_PASS_V(nir, nir_copy_prop);
    NIR_PASS_V(nir, nir_opt_deref);
    nir_remove_non_entrypoints(nir);
    NIR_PASS_V(nir, nir_lower_variable_initializers, ~0);
    NIR_PASS_V(nir, nir_split_var_copies);
    NIR_PASS_V(nir, nir_split_per_member_structs);
    NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_in | nir_var_shader_out | nir_var_system_value | nir_var_shader_call_data |
               nir_var_ray_hit_attrib, NULL);
    // Then as tu_spirv_to_nir and Mesa's standalone ir3 compiler.
    const struct nir_lower_sysvals_to_varyings_options sysvals = {.frag_coord = true, .point_coord = true};
    NIR_PASS_V(nir, nir_lower_sysvals_to_varyings, &sysvals);
    NIR_PASS_V(nir, nir_lower_global_vars_to_local);
    NIR_PASS_V(nir, nir_split_var_copies);
    NIR_PASS_V(nir, nir_lower_var_copies);
    NIR_PASS_V(nir, nir_lower_system_values);
    nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, stage);
    nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, stage);
    NIR_PASS_V(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, ir3_glsl_type_size,
               nir_lower_io_use_interpolated_input_intrinsics);
    // Push constants (as Turnip's lower_load_push_constant): load_const_ir3 into a const-file range reserved with ir3_const_alloc;
    // PER_STAGE, so the driver uploads them with CP_LOAD_STATE6 ST6_CONSTANTS.
    NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
    unsigned push_dwords = 0;
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
        if (instr->type != nir_instr_type_intrinsic) continue;
        nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
        if (in->intrinsic != nir_intrinsic_load_push_constant) continue;
        const unsigned end = (nir_intrinsic_base(in) + nir_intrinsic_range(in)) / 4;
        if (end > push_dwords) push_dwords = end;
    }
    struct ir3_const_allocations const_allocs = {0};
    if (push_dwords)
        ir3_const_alloc(&const_allocs, IR3_CONST_ALLOC_PUSH_CONSTS, align(DIV_ROUND_UP(push_dwords, 4), compiler->const_upload_unit), 1);
    const unsigned push_vec4 = const_allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].offset_vec4;
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_push_constant) continue;
            b.cursor = nir_before_instr(instr);
            nir_def *load = nir_load_const_ir3(&b, in->num_components, in->def.bit_size, nir_ushr_imm(&b, in->src[0].ssa, 2),
                                               .base = nir_intrinsic_base(in) + push_vec4 * 4);
            nir_def_replace(&in->def, load);
        }
        nir_metadata_preserve(impl, nir_metadata_none);
    }
    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
    struct ir3_shader_nir_options nir_options = {0};
    ir3_finalize_nir(compiler, &nir_options, nir);
    const struct ir3_shader_options options = {
        .api_wavesize = IR3_SINGLE_OR_DOUBLE, .real_wavesize = IR3_SINGLE_OR_DOUBLE,
        .push_consts_type = push_dwords ? IR3_PUSH_CONSTS_PER_STAGE : IR3_PUSH_CONSTS_NONE,
        .push_consts_base = 0, .push_consts_dwords = push_dwords, .const_allocs = const_allocs, .nir_options = nir_options,
    };
    struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &options, NULL);
    struct ir3_shader_key key = {0};
    struct ir3_shader_variant *v = ir3_shader_create_variant(shader, &key, false);
    if (!v) { fprintf(stderr, "compile failed: %s\n", path); exit(1); }
    return (struct compiled){v, push_vec4, push_dwords};
}

static void print_variant(const char *name, struct compiled c) {
    struct ir3_shader_variant *v = c.v;
    printf("== %s\n", name);
    struct isa_decode_options dopts = {.gpu_id = 740, .show_errors = true, .branch_labels = true};
    ir3_isa_disasm(v->bin, v->info.sizedwords * 4, stdout, &dopts);   // v->bin: ir3_shader_create_variant assembled it, the IR is gone
    const struct ir3_info *i = &v->info;
    printf("layout: instrlen %u constlen %u max_reg %d max_half_reg %d double_threadsize %d mergedregs %d total_in %u; push consts %u "
           "dwords at c%u; constant data %u bytes (UBO %d)\n", v->instrlen, v->constlen, i->max_reg, i->max_half_reg,
           i->double_threadsize, v->mergedregs, v->total_in, c.push_dwords, c.push_vec4, v->constant_data_size,
           ir3_const_state(v)->consts_ubo.idx);
    for (unsigned k = 0; k < v->outputs_count; k++) printf("output %u: slot %u regid %u\n", k, v->outputs[k].slot, v->outputs[k].regid);
    for (unsigned k = 0; k < v->inputs_count; k++)
        printf("input %u: slot %u regid %u sysval %d inloc %u compmask %x flat %d\n", k, v->inputs[k].slot, v->inputs[k].regid,
               v->inputs[k].sysval, v->inputs[k].inloc, v->inputs[k].compmask, v->inputs[k].flat);
}

static void write_json(const char *path, const char *stage, struct compiled cc) {
    struct ir3_shader_variant *v = cc.v;
    const struct ir3_info *i = &v->info;
    const struct ir3_const_state *c = ir3_const_state(v);
    FILE *f = fopen(path, "w");
    fprintf(f, "{\"stage\": \"%s\", \"instrlen\": %u, \"constlen\": %u, \"max_reg\": %d, \"max_half_reg\": %d, "
               "\"double_threadsize\": %d, \"mergedregs\": %d, \"early_preamble\": %d, \"branchstack_hw\": %u, "
               "\"total_in\": %u, \"need_full_quad\": %d, \"need_pixlod\": %d, \"constant_data_offset\": %u, "
               "\"constant_data_size\": %u, \"consts_ubo\": %d, \"push_consts_vec4\": %u, \"push_consts_dwords\": %u, ",
            stage, v->instrlen, v->constlen, i->max_reg, i->max_half_reg, i->double_threadsize, v->mergedregs, v->early_preamble,
            ir3_shader_branchstack_hw(v), v->total_in, v->need_full_quad, v->need_pixlod, i->constant_data_offset,
            v->constant_data_size, c->consts_ubo.idx, cc.push_vec4, cc.push_dwords);
    fprintf(f, "\"ubo_ranges\": [");
    for (int k = 0; k < c->ubo_state.num_enabled; k++)
        fprintf(f, "%s{\"block\": %u, \"bindless\": %d, \"offset\": %u, \"start\": %u, \"end\": %u}", k ? ", " : "",
                c->ubo_state.range[k].ubo.block, c->ubo_state.range[k].ubo.bindless, c->ubo_state.range[k].offset,
                c->ubo_state.range[k].start, c->ubo_state.range[k].end);
    fprintf(f, "], \"outputs\": [");
    for (unsigned k = 0; k < v->outputs_count; k++) fprintf(f, "%s[%u, %u]", k ? ", " : "", v->outputs[k].slot, v->outputs[k].regid);
    fprintf(f, "], \"inputs\": [");
    for (unsigned k = 0; k < v->inputs_count; k++)
        fprintf(f, "%s[%u, %u, %d]", k ? ", " : "", v->inputs[k].slot, v->inputs[k].regid, v->inputs[k].sysval);
    fprintf(f, "], \"words\": [");
    for (unsigned k = 0; k < i->size / 4; k++) fprintf(f, "%s%u", k ? ", " : "", v->bin[k]);
    fprintf(f, "]}\n");
    fclose(f);
}

// ---- Turnip's linkage registers, transcribed (src/freedreno/vulkan/tu_pipeline.cc tu6_emit_vpc with the VS as the last geometry
// stage and no stream output; tu_shader.cc tu6_emit_fs_inputs for A7XX). Register numbers and fields from Mesa's a6xx.xml.h. ----
static FILE *regs_out;
static int regs_n;
static void reg(uint32_t r, uint32_t v) { fprintf(regs_out, "%s[%u, %u]", regs_n++ ? ", " : "", r, v); }
#define CONDREG(r, val) COND(VALIDREG(r), (val))

static void emit_vpc(const struct ir3_shader_variant *vs, const struct ir3_shader_variant *fs) {
    struct ir3_shader_linkage linkage = {.primid_loc = 0xff, .clip0_loc = 0xff, .clip1_loc = 0xff};
    ir3_link_shaders(&linkage, vs, fs, true);
    const uint32_t pointsize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
    const uint32_t layer_regid = ir3_find_output_regid(vs, VARYING_SLOT_LAYER);
    const uint32_t view_regid = ir3_find_output_regid(vs, VARYING_SLOT_VIEWPORT);
    const uint32_t clip0_regid = ir3_find_output_regid(vs, VARYING_SLOT_CLIP_DIST0);
    const uint32_t clip1_regid = ir3_find_output_regid(vs, VARYING_SLOT_CLIP_DIST1);
    const uint32_t shading_rate_regid = ir3_find_output_regid(vs, VARYING_SLOT_PRIMITIVE_SHADING_RATE);
    uint32_t pointsize_loc = 0xff, position_loc = 0xff, layer_loc = 0xff, view_loc = 0xff, shading_rate_loc = 0xff;
    if (layer_regid != regid(63, 0)) { layer_loc = linkage.max_loc; ir3_link_add(&linkage, VARYING_SLOT_LAYER, layer_regid, 0x1, linkage.max_loc); }
    if (view_regid != regid(63, 0)) { view_loc = linkage.max_loc; ir3_link_add(&linkage, VARYING_SLOT_VIEWPORT, view_regid, 0x1, linkage.max_loc); }
    if (shading_rate_regid != regid(63, 0)) {
        shading_rate_loc = linkage.max_loc;
        ir3_link_add(&linkage, VARYING_SLOT_PRIMITIVE_SHADING_RATE, shading_rate_regid, 0x1, linkage.max_loc);
    }
    unsigned extra_pos = 0;
    for (unsigned i = 0; i < vs->outputs_count; i++) {
        if (vs->outputs[i].slot != VARYING_SLOT_POS) continue;
        if (position_loc == 0xff) position_loc = linkage.max_loc;
        ir3_link_add(&linkage, vs->outputs[i].slot, vs->outputs[i].regid, 0xf, position_loc + 4 * vs->outputs[i].view);
        extra_pos = MAX2(extra_pos, vs->outputs[i].view);
    }
    if (pointsize_regid != regid(63, 0)) { pointsize_loc = linkage.max_loc; ir3_link_add(&linkage, VARYING_SLOT_PSIZ, pointsize_regid, 0x1, linkage.max_loc); }
    const uint8_t clip_cull_mask = vs->clip_mask | vs->cull_mask;
    uint32_t clip0_loc = linkage.clip0_loc, clip1_loc = linkage.clip1_loc;
    if (clip0_loc == 0xff && clip0_regid != regid(63, 0)) {
        clip0_loc = linkage.max_loc; ir3_link_add(&linkage, VARYING_SLOT_CLIP_DIST0, clip0_regid, clip_cull_mask & 0xf, linkage.max_loc);
    }
    if (clip1_loc == 0xff && clip1_regid != regid(63, 0)) {
        clip1_loc = linkage.max_loc; ir3_link_add(&linkage, VARYING_SLOT_CLIP_DIST1, clip1_regid, clip_cull_mask >> 4, linkage.max_loc);
    }
    if (linkage.cnt == 0) ir3_link_add(&linkage, 0, 0, 0x1, linkage.max_loc);
    const uint32_t sp_out_count = DIV_ROUND_UP(linkage.cnt, 2), sp_vpc_dst_count = DIV_ROUND_UP(linkage.cnt, 4);
    uint32_t sp_out[16] = {0}, sp_vpc_dst[8] = {0};
    for (uint32_t i = 0; i < linkage.cnt; i++) {
        ((uint16_t *)sp_out)[i] = A6XX_SP_VS_OUT_REG_A_REGID(linkage.var[i].regid) | A6XX_SP_VS_OUT_REG_A_COMPMASK(linkage.var[i].compmask);
        ((uint8_t *)sp_vpc_dst)[i] = A6XX_SP_VS_VPC_DST_REG_OUTLOC0(linkage.var[i].loc);
    }
    for (uint32_t i = 0; i < sp_out_count; i++) reg(REG_A6XX_SP_VS_OUT_REG(i), sp_out[i]);
    for (uint32_t i = 0; i < sp_vpc_dst_count; i++) reg(REG_A6XX_SP_VS_VPC_DST_REG(i), sp_vpc_dst[i]);
    reg(REG_A6XX_VPC_VS_PACK, A6XX_VPC_VS_PACK_POSITIONLOC(position_loc) | A6XX_VPC_VS_PACK_PSIZELOC(pointsize_loc) |
                              A6XX_VPC_VS_PACK_STRIDE_IN_VPC(linkage.max_loc) | A6XX_VPC_VS_PACK_EXTRAPOS(extra_pos));
    const uint32_t clip = A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK(clip_cull_mask) | A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC(clip0_loc) |
                          A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC(clip1_loc);
    reg(REG_A6XX_VPC_VS_CLIP_CNTL, clip);
    reg(REG_A6XX_VPC_VS_CLIP_CNTL_V2, clip);
    reg(REG_A6XX_GRAS_VS_CL_CNTL, A6XX_GRAS_VS_CL_CNTL_CLIP_MASK(vs->clip_mask) | A6XX_GRAS_VS_CL_CNTL_CULL_MASK(vs->cull_mask));
    reg(REG_A6XX_PC_VS_OUT_CNTL, A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC(linkage.max_loc) | CONDREG(pointsize_regid, A6XX_PC_VS_OUT_CNTL_PSIZE) |
                                 CONDREG(layer_regid, A6XX_PC_VS_OUT_CNTL_LAYER) | CONDREG(view_regid, A6XX_PC_VS_OUT_CNTL_VIEW) |
                                 A6XX_PC_VS_OUT_CNTL_CLIP_MASK(clip_cull_mask) |
                                 CONDREG(shading_rate_regid, A6XX_PC_VS_OUT_CNTL_SHADINGRATE));
    reg(REG_A6XX_SP_VS_PRIMITIVE_CNTL, A6XX_SP_VS_PRIMITIVE_CNTL_OUT(linkage.cnt));
    const uint32_t layer = A6XX_VPC_VS_LAYER_CNTL_LAYERLOC(layer_loc) | A6XX_VPC_VS_LAYER_CNTL_VIEWLOC(view_loc) |
                           A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC(shading_rate_loc);
    reg(REG_A6XX_VPC_VS_LAYER_CNTL, layer);
    reg(REG_A6XX_VPC_VS_LAYER_CNTL_V2, layer);
    reg(REG_A6XX_GRAS_VS_LAYER_CNTL, CONDREG(layer_regid, A6XX_GRAS_GS_LAYER_CNTL_WRITES_LAYER) |
                                     CONDREG(view_regid, A6XX_GRAS_GS_LAYER_CNTL_WRITES_VIEW));
    // tu6_emit_vpc_varying_modes: only flat (and point-coord/layer/viewport) inputs set bits
    uint32_t interp_modes[8] = {0}, ps_repl_modes[8] = {0}, interp_regs = 0;
    for (int i = -1; (i = ir3_next_varying(fs, i)) < (int)fs->inputs_count;) {
        const uint32_t compmask = fs->inputs[i].compmask;
        uint8_t interp_mode = 0;
        int shift = 0;
        if (fs->inputs[i].slot == VARYING_SLOT_PNTC || fs->inputs[i].slot == VARYING_SLOT_LAYER || fs->inputs[i].slot == VARYING_SLOT_VIEWPORT) {
            fprintf(stderr, "point-coord/layer/viewport FS inputs not handled\n"); exit(1);
        } else if (fs->inputs[i].flat) {
            for (int c = 0; c < 4; c++) if (compmask & (1 << c)) { interp_mode |= INTERP_FLAT << shift; shift += 2; }
        }
        const int bits = util_bitcount(compmask) * 2;
        const uint32_t inloc = fs->inputs[i].inloc * 2;
        uint32_t n = inloc / 32, sh = inloc % 32;
        interp_modes[n] |= interp_mode << sh;
        if (sh + bits > 32) { n++; sh = 32 - sh; interp_modes[n] |= interp_mode >> sh; }
        interp_regs = MAX2(interp_regs, n + 1);
    }
    for (uint32_t i = 0; i < interp_regs; i++) reg(REG_A6XX_VPC_VARYING_INTERP_MODE(i), interp_modes[i]);
    for (uint32_t i = 0; i < interp_regs; i++) reg(REG_A6XX_VPC_VARYING_PS_REPL_MODE(i), ps_repl_modes[i]);
}

static void emit_fs_inputs(const struct fd_dev_info *info, const struct ir3_shader_variant *fs) {
    uint32_t ij_regid[IJ_COUNT];
    const bool sample_shading = fs->per_samp || fs->key.sample_shading;
    const bool enable_varyings = fs->total_in > 0;
    const uint32_t samp_id_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
    const uint32_t smask_in_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);
    const uint32_t face_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
    const uint32_t coord_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
    const uint32_t zwcoord_regid = VALIDREG(coord_regid) ? coord_regid + 2 : regid(63, 0);
    const uint32_t shading_rate_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_SHADING_RATE);
    for (unsigned i = 0; i < IJ_COUNT; i++) ij_regid[i] = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + i);
    if (fs->num_sampler_prefetch) { fprintf(stderr, "sampler prefetch not handled\n"); exit(1); }
    reg(REG_A6XX_SP_FS_PREFETCH_CNTL, A6XX_SP_FS_PREFETCH_CNTL_COUNT(0) | A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID(0x1ff) |
                                      A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID4COORD(0x1ff) |
                                      COND(!VALIDREG(ij_regid[IJ_PERSP_PIXEL]), A6XX_SP_FS_PREFETCH_CNTL_IJ_WRITE_DISABLE) |
                                      COND(fs->prefetch_end_of_quad, A6XX_SP_FS_PREFETCH_CNTL_ENDOFQUAD));
    reg(REG_A7XX_HLSQ_CONTROL_1_REG, A7XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(info->a6xx.prim_alloc_threshold));
    reg(REG_A7XX_HLSQ_CONTROL_2_REG, A7XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) | A7XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
                                     A7XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(smask_in_regid) |
                                     A7XX_HLSQ_CONTROL_2_REG_CENTERRHW(ij_regid[IJ_PERSP_CENTER_RHW]));
    reg(REG_A7XX_HLSQ_CONTROL_3_REG, A7XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(ij_regid[IJ_PERSP_PIXEL]) |
                                     A7XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(ij_regid[IJ_LINEAR_PIXEL]) |
                                     A7XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(ij_regid[IJ_PERSP_CENTROID]) |
                                     A7XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(ij_regid[IJ_LINEAR_CENTROID]));
    reg(REG_A7XX_HLSQ_CONTROL_4_REG, A7XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(ij_regid[IJ_PERSP_SAMPLE]) |
                                     A7XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(ij_regid[IJ_LINEAR_SAMPLE]) |
                                     A7XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
                                     A7XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid));
    reg(REG_A7XX_HLSQ_CONTROL_5_REG, A7XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID(0xfc) |
                                     A7XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID(shading_rate_regid));
    uint32_t sysval_regs = 0;
    for (unsigned i = 0; i < IJ_COUNT; i++) if (VALIDREG(ij_regid[i])) sysval_regs += i == IJ_PERSP_CENTER_RHW ? 1 : 2;
    const uint32_t one[] = {face_regid, samp_id_regid, smask_in_regid, shading_rate_regid}, two[] = {coord_regid, zwcoord_regid};
    for (unsigned i = 0; i < 4; i++) if (VALIDREG(one[i])) sysval_regs += 1;
    for (unsigned i = 0; i < 2; i++) if (VALIDREG(two[i])) sysval_regs += 2;
    reg(REG_A7XX_HLSQ_UNKNOWN_A9AE, A7XX_HLSQ_UNKNOWN_A9AE_SYSVAL_REGS_COUNT(sysval_regs) | A7XX_HLSQ_UNKNOWN_A9AE_UNK8 | A7XX_HLSQ_UNKNOWN_A9AE_UNK9);
    reg(REG_A7XX_HLSQ_FS_CNTL_0, A7XX_HLSQ_FS_CNTL_0_THREADSIZE(fs->info.double_threadsize ? THREAD128 : THREAD64) |
                                 COND(enable_varyings, A7XX_HLSQ_FS_CNTL_0_VARYINGS));
    bool need_size = fs->frag_face || fs->fragcoord_compmask != 0, need_size_persamp = false;
    if (VALIDREG(ij_regid[IJ_PERSP_CENTER_RHW])) { if (sample_shading) need_size_persamp = true; else need_size = true; }
    reg(REG_A6XX_GRAS_CNTL, CONDREG(ij_regid[IJ_PERSP_PIXEL], A6XX_GRAS_CNTL_IJ_PERSP_PIXEL) |
                            CONDREG(ij_regid[IJ_PERSP_CENTROID], A6XX_GRAS_CNTL_IJ_PERSP_CENTROID) |
                            CONDREG(ij_regid[IJ_PERSP_SAMPLE], A6XX_GRAS_CNTL_IJ_PERSP_SAMPLE) |
                            CONDREG(ij_regid[IJ_LINEAR_PIXEL], A6XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
                            CONDREG(ij_regid[IJ_LINEAR_CENTROID], A6XX_GRAS_CNTL_IJ_LINEAR_CENTROID) |
                            CONDREG(ij_regid[IJ_LINEAR_SAMPLE], A6XX_GRAS_CNTL_IJ_LINEAR_SAMPLE) |
                            COND(need_size, A6XX_GRAS_CNTL_IJ_LINEAR_PIXEL) | COND(need_size_persamp, A6XX_GRAS_CNTL_IJ_LINEAR_SAMPLE) |
                            COND(fs->fragcoord_compmask != 0, A6XX_GRAS_CNTL_COORD_MASK(fs->fragcoord_compmask)));
    reg(REG_A6XX_RB_RENDER_CONTROL0, CONDREG(ij_regid[IJ_PERSP_PIXEL], A6XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL) |
                                     CONDREG(ij_regid[IJ_PERSP_CENTROID], A6XX_RB_RENDER_CONTROL0_IJ_PERSP_CENTROID) |
                                     CONDREG(ij_regid[IJ_PERSP_SAMPLE], A6XX_RB_RENDER_CONTROL0_IJ_PERSP_SAMPLE) |
                                     CONDREG(ij_regid[IJ_LINEAR_PIXEL], A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
                                     CONDREG(ij_regid[IJ_LINEAR_CENTROID], A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_CENTROID) |
                                     CONDREG(ij_regid[IJ_LINEAR_SAMPLE], A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE) |
                                     COND(need_size, A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
                                     COND(enable_varyings, A6XX_RB_RENDER_CONTROL0_UNK10) |
                                     COND(need_size_persamp, A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE) |
                                     COND(fs->fragcoord_compmask != 0, A6XX_RB_RENDER_CONTROL0_COORD_MASK(fs->fragcoord_compmask)));
    reg(REG_A6XX_RB_RENDER_CONTROL1, A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE(sample_shading ? FRAGCOORD_SAMPLE : FRAGCOORD_CENTER) |
                                     CONDREG(smask_in_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEMASK) |
                                     CONDREG(samp_id_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEID) |
                                     CONDREG(ij_regid[IJ_PERSP_CENTER_RHW], A6XX_RB_RENDER_CONTROL1_CENTERRHW) |
                                     COND(fs->post_depth_coverage, A6XX_RB_RENDER_CONTROL1_POSTDEPTHCOVERAGE) |
                                     COND(fs->frag_face, A6XX_RB_RENDER_CONTROL1_FACENESS) |
                                     CONDREG(shading_rate_regid, A6XX_RB_RENDER_CONTROL1_FOVEATION));
    reg(REG_A6XX_RB_SAMPLE_CNTL, COND(sample_shading, A6XX_RB_SAMPLE_CNTL_PER_SAMP_MODE));
    reg(REG_A6XX_GRAS_LRZ_PS_INPUT_CNTL, CONDREG(samp_id_regid, A6XX_GRAS_LRZ_PS_INPUT_CNTL_SAMPLEID) |
                                         A6XX_GRAS_LRZ_PS_INPUT_CNTL_FRAGCOORDSAMPLEMODE(sample_shading ? FRAGCOORD_SAMPLE : FRAGCOORD_CENTER));
    reg(REG_A6XX_GRAS_SAMPLE_CNTL, COND(sample_shading, A6XX_GRAS_SAMPLE_CNTL_PER_SAMP_MODE));
    uint32_t varmask[4] = {0};
    for (int i = ir3_next_varying(fs, -1); i < (int)fs->inputs_count; i = ir3_next_varying(fs, i)) {
        if (fs->inputs[i].inloc >= fs->total_in) continue;
        const unsigned loc = fs->inputs[i].inloc;
        for (int j = 0; j < util_last_bit(fs->inputs[i].compmask); j++) {const uint8_t c = loc + j; varmask[c / 32] |= 1u << (c % 32);}
    }
    for (int i = 0; i < 4; i++) reg(REG_A6XX_VPC_VAR_DISABLE(i), ~varmask[i]);
    const unsigned primid_loc = ir3_find_input_loc(fs, VARYING_SLOT_PRIMITIVE_ID), viewid_loc = ir3_find_input_loc(fs, VARYING_SLOT_VIEW_INDEX);
    reg(REG_A6XX_VPC_CNTL_0, A6XX_VPC_CNTL_0_NUMNONPOSVAR(fs->total_in) | COND(fs->total_in, A6XX_VPC_CNTL_0_VARYING) |
                             A6XX_VPC_CNTL_0_PRIMIDLOC(primid_loc) | A6XX_VPC_CNTL_0_VIEWIDLOC(viewid_loc));
}

// tu_shader.cc tu6_emit_fs_outputs (A7XX)
static void emit_fs_outputs(const struct ir3_shader_variant *fs) {
    const uint32_t posz_regid = ir3_find_output_regid(fs, FRAG_RESULT_DEPTH);
    const uint32_t smask_regid = ir3_find_output_regid(fs, FRAG_RESULT_SAMPLE_MASK);
    const uint32_t stencilref_regid = ir3_find_output_regid(fs, FRAG_RESULT_STENCIL);
    int output_reg_count = 0;
    uint32_t fragdata_regid[8], fragdata_aliased_components = 0;
    if (fs->color0_mrt) { fprintf(stderr, "color0_mrt not handled\n"); exit(1); }
    for (uint32_t i = 0; i < 8; i++) {
        const int idx = ir3_find_output(fs, (gl_varying_slot)(FRAG_RESULT_DATA0 + i));
        if (idx < 0) { fragdata_regid[i] = INVALID_REG; continue; }
        const struct ir3_shader_output *fragdata = &fs->outputs[idx];
        fragdata_regid[i] = ir3_get_output_regid(fragdata);
        if (VALIDREG(fragdata_regid[i]) || fragdata->aliased_components) {
            output_reg_count = i + 1;
            fragdata_aliased_components |= fragdata->aliased_components << (i * 4);
        }
    }
    reg(REG_A6XX_SP_FS_OUTPUT_CNTL0, A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID(posz_regid) | A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID(smask_regid) |
                                     A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID(stencilref_regid) |
                                     COND(fs->dual_src_blend, A6XX_SP_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE));
    uint32_t fs_render_components = 0;
    for (int i = 0; i < output_reg_count; i++) {
        reg(REG_A6XX_SP_FS_OUTPUT_REG(i), A6XX_SP_FS_OUTPUT_REG_REGID(fragdata_regid[i]) |
                                          COND(fragdata_regid[i] & HALF_REG_ID, A6XX_SP_FS_OUTPUT_REG_HALF_PRECISION));
        if (VALIDREG(fragdata_regid[i]) || (fragdata_aliased_components & (0xf << (i * 4)))) fs_render_components |= 0xf << (i * 4);
    }
    reg(REG_A6XX_SP_FS_RENDER_COMPONENTS, fs_render_components);
    reg(REG_A7XX_SP_PS_ALIASED_COMPONENTS_CONTROL, COND(fragdata_aliased_components != 0, A7XX_SP_PS_ALIASED_COMPONENTS_CONTROL_ENABLED));
    reg(REG_A7XX_SP_PS_ALIASED_COMPONENTS, fragdata_aliased_components);
    reg(REG_A6XX_RB_FS_OUTPUT_CNTL0, COND(fs->writes_pos, A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_Z) |
                                     COND(fs->writes_smask, A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_SAMPMASK) |
                                     COND(fs->writes_stencilref, A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_STENCILREF) |
                                     COND(fs->dual_src_blend, A6XX_RB_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE));
    reg(REG_A6XX_RB_RENDER_COMPONENTS, fs_render_components);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: a740-compile vs|fs FILE.spv [OUT.json]\n       a740-compile link VS.spv FS.spv OUTDIR\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    glsl_type_singleton_init_or_ref();
    struct fd_dev_id dev_id = {.gpu_id = 0, .chip_id = 0x43050a01};
    struct ir3_compiler *compiler = ir3_compiler_create(NULL, &dev_id, fd_dev_info_raw(&dev_id), &(struct ir3_compiler_options){0});
    if (!compiler) { fprintf(stderr, "no A740 compiler\n"); return 1; }
    if (!strcmp(argv[1], "link")) {
        if (argc < 5) return 2;
        struct compiled vs = compile(compiler, MESA_SHADER_VERTEX, argv[2]), fs = compile(compiler, MESA_SHADER_FRAGMENT, argv[3]);
        print_variant("vs", vs); print_variant("fs", fs);
        char path[1024];
        snprintf(path, sizeof path, "%s/vs.json", argv[4]); write_json(path, "vs", vs);
        snprintf(path, sizeof path, "%s/fs.json", argv[4]); write_json(path, "fs", fs);
        snprintf(path, sizeof path, "%s/link.json", argv[4]);
        regs_out = fopen(path, "w");
        fprintf(regs_out, "{\"regs\": [");
        emit_vpc(vs.v, fs.v);
        emit_fs_inputs(fd_dev_info_raw(&dev_id), fs.v);
        emit_fs_outputs(fs.v);
        fprintf(regs_out, "]}\n");
        fclose(regs_out);
        printf("wrote %s/{vs,fs,link}.json (%d linkage registers)\n", argv[4], regs_n);
        return 0;
    }
    const gl_shader_stage stage = !strcmp(argv[1], "fs") ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
    struct compiled c = compile(compiler, stage, argv[2]);
    print_variant(argv[1], c);
    if (argc > 3) write_json(argv[3], argv[1], c);
    return 0;
}
