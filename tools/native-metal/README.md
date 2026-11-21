# Native Metal on the A740 (OnePlus 12R): step 1, a compute dispatch without Vulkan

`gen-a740-fill.cc` writes `A740FillProgram.h` (in `unixnu-patches/AdrenoMetal/0002-12r-native-a740-fill-test.patch`): Mesa
computerator's a7xx compute sequence, packed with Mesa 25.0.7's a7xx register headers and the A740 device row, around
hakios-metal's `fill_id` kernel (MIT) assembled for chip 0x43050a01 by hakios-metal's `tools/ir3-kernel`
(`fixtures/fill_id_a740.hpp`; the words equal the a6xx build's). The probe daemon runs it with `native-a740-fill=1` and reports
`[gpu client text] adrenoprobed: N fill v<right>/1024 c<seqno> w<wait> e<error> s<step>` (log-gpu=1).

Rebuild: a host Mesa 25.0.7 build (meson, -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dshader-cache=disabled, the
freedreno/ir3/isa/util archives) at ext/mesa-host-build, then `build-gen.sh` (paths in it).
