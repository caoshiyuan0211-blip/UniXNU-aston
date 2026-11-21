// a740-compile: compile one SPIR-V shader for the OnePlus 12R's Adreno 740 (chip 0x43050a01) with Mesa's ir3 compiler, the way
// Mesa's own standalone compiler does (gallium/drivers/freedreno/ir3/ir3_cmdline.c, SPIR-V path), and print the disassembly and
// the variant's layout: the inputs a draw's shader registers are made from (native Metal stage 2b, 12r/METAL-DRIVER-PLAN.md).
//
// usage: a740-compile vs|fs FILE.spv [OUT.bin]
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

static void *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(*size);
    if (fread(buf, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: a740-compile vs|fs FILE.spv [OUT.bin]\n"); return 2; }
    const gl_shader_stage stage = !strcmp(argv[1], "fs") ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
    setvbuf(stdout, NULL, _IONBF, 0);
    glsl_type_singleton_init_or_ref();

    struct fd_dev_id dev_id = {.gpu_id = 0, .chip_id = 0x43050a01};
    struct ir3_compiler *compiler = ir3_compiler_create(NULL, &dev_id, fd_dev_info_raw(&dev_id), &(struct ir3_compiler_options){0});
    if (!compiler) { fprintf(stderr, "no A740 compiler\n"); return 1; }

    size_t size;
    void *spirv = read_all(argv[2], &size);
    const struct spirv_capabilities caps = {.Shader = true, .DrawParameters = true};
    const struct spirv_to_nir_options spirv_options = {.capabilities = &caps};
    fprintf(stderr, "step 0\n", 0);
    nir_shader *nir = spirv_to_nir(spirv, size / 4, NULL, 0, stage, "main", &spirv_options, ir3_get_compiler_options(compiler));
    if (!nir) { fprintf(stderr, "spirv_to_nir failed\n"); return 1; }
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
    fprintf(stderr, "step 1\n", 1);
    nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, stage);
    nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, stage);
    NIR_PASS_V(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, ir3_glsl_type_size,
               nir_lower_io_use_interpolated_input_intrinsics);

    // Push constants: explicit offsets, then (as Turnip's lower_load_push_constant) load_const_ir3 into a range of the const file
    // reserved with ir3_const_alloc (PER_STAGE push constants: the driver uploads them with CP_LOAD_STATE6 ST6_CONSTANTS).
    NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
    unsigned push_dwords = 0;
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
        if (instr->type != nir_instr_type_intrinsic) continue;
        nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
        if (in->intrinsic != nir_intrinsic_load_push_constant) continue;
        const unsigned end = (nir_intrinsic_base(in) + nir_intrinsic_range(in)) / 4;
        if (end > push_dwords) push_dwords = end;
    }
    fprintf(stderr, "step 2\n", 2);
    struct ir3_const_allocations const_allocs = {0};
    if (push_dwords) ir3_const_alloc(&const_allocs, IR3_CONST_ALLOC_PUSH_CONSTS, align(DIV_ROUND_UP(push_dwords, 4), compiler->const_upload_unit), 1);
    const unsigned push_offset_vec4 = const_allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].offset_vec4;
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_push_constant) continue;
            b.cursor = nir_before_instr(instr);
            nir_def *load = nir_load_const_ir3(&b, in->num_components, in->def.bit_size, nir_ushr_imm(&b, in->src[0].ssa, 2),
                                               .base = nir_intrinsic_base(in) + push_offset_vec4 * 4);
            nir_def_replace(&in->def, load);
        }
        nir_metadata_preserve(impl, nir_metadata_none);
    }
    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

    fprintf(stderr, "step 3\n", 3);
    struct ir3_shader_nir_options nir_options = {0};
    ir3_finalize_nir(compiler, &nir_options, nir);
    const struct ir3_shader_options options = {
        .api_wavesize = IR3_SINGLE_OR_DOUBLE, .real_wavesize = IR3_SINGLE_OR_DOUBLE,
        .push_consts_type = push_dwords ? IR3_PUSH_CONSTS_PER_STAGE : IR3_PUSH_CONSTS_NONE,
        .push_consts_base = 0, .push_consts_dwords = push_dwords,
        .const_allocs = const_allocs, .nir_options = nir_options,
    };
    fprintf(stderr, "step 4\n", 4);
    struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &options, NULL);
    fprintf(stderr, "step 5\n", 5);
    struct ir3_shader_key key = {0};
    struct ir3_shader_variant *v = ir3_shader_create_variant(shader, &key, false);
    if (!v) { fprintf(stderr, "compile failed\n"); return 1; }
    fprintf(stderr, "step 6\n", 6);
    const struct ir3_const_state *cs = ir3_const_state(v);
    printf("consts: push constants %u dwords at c%u (vec4); constant-data UBO idx %d, %u bytes\n", push_dwords, push_offset_vec4,
           cs->consts_ubo.idx, v->constant_data_size);

    fprintf(stderr, "step 7\n", 7);
    uint32_t *bin = v->bin;   // ir3_shader_create_variant already assembled it (and dropped the IR)
    fprintf(stderr, "step 8\n", 8);
    // ir3_shader_disasm also reads the freed IR: decode the words alone (as disasm-a740.c)
    struct isa_decode_options dopts = {.gpu_id = 740, .show_errors = true, .branch_labels = true};
    ir3_isa_disasm(bin, v->info.sizedwords * 4, stdout, &dopts);
    const struct ir3_info *i = &v->info;
    printf("layout: instrlen %u constlen %u max_reg %d max_half_reg %d double_threadsize %d mergedregs %d early_preamble %d "
           "branchstack %u sizedwords %u constant_data_size %u inputs %u outputs %u total_in %u\n",
           v->instrlen, v->constlen, i->max_reg, i->max_half_reg, i->double_threadsize, v->mergedregs, v->early_preamble,
           v->branchstack, i->sizedwords, v->constant_data_size, v->inputs_count, v->outputs_count, v->total_in);
    for (unsigned k = 0; k < v->outputs_count; k++)
        printf("output %u: slot %u regid r%u.%c\n", k, v->outputs[k].slot, v->outputs[k].regid >> 2, "xyzw"[v->outputs[k].regid & 3]);
    for (unsigned k = 0; k < v->inputs_count; k++)
        printf("input %u: slot %u regid r%u.%c sysval %d\n", k, v->inputs[k].slot, v->inputs[k].regid >> 2,
               "xyzw"[v->inputs[k].regid & 3], v->inputs[k].sysval);
    if (argc > 3) {   // OUT.json: the layout the generator builds the shader registers from, and the binary (code + constant data)
        FILE *f = fopen(argv[3], "w");
        const struct ir3_const_state *c = ir3_const_state(v);
        fprintf(f, "{\"stage\": \"%s\", \"instrlen\": %u, \"constlen\": %u, \"max_reg\": %d, \"max_half_reg\": %d, "
                   "\"double_threadsize\": %d, \"mergedregs\": %d, \"early_preamble\": %d, \"branchstack_hw\": %u, "
                   "\"total_in\": %u, \"need_full_quad\": %d, \"need_pixlod\": %d, \"constant_data_offset\": %u, "
                   "\"constant_data_size\": %u, \"consts_ubo\": %d, \"push_consts_vec4\": %u, \"push_consts_dwords\": %u, ",
                argv[1], v->instrlen, v->constlen, i->max_reg, i->max_half_reg, i->double_threadsize, v->mergedregs, v->early_preamble,
                ir3_shader_branchstack_hw(v), v->total_in, v->need_full_quad, v->need_pixlod, i->constant_data_offset,
                v->constant_data_size, c->consts_ubo.idx, push_offset_vec4, push_dwords);
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
        for (unsigned k = 0; k < i->size / 4; k++) fprintf(f, "%s%u", k ? ", " : "", bin[k]);
        fprintf(f, "]}\n");
        fclose(f);
    }
    return 0;
}
