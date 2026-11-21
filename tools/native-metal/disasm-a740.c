// disasm-a740: print Mesa's ir3 disassembly of raw A740 shader words (12r native Metal tooling).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ir3-isa.h"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    static uint32_t w[4096]; size_t n = fread(w, 4, 4096, f); fclose(f);
    struct isa_decode_options o = {.gpu_id = 740, .show_errors = true, .branch_labels = true};
    ir3_isa_disasm(w, n * 4, stdout, &o);
    return 0;
}
