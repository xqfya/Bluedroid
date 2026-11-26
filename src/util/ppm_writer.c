#include "emulator/ppm_writer.h"
#include <stdio.h>

int ppm_write(const char* path, int w, int h, const unsigned char* rgba) {
    if (!path || !rgba || w <= 0 || h <= 0) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        fputc(rgba[i*4+0], f);
        fputc(rgba[i*4+1], f);
        fputc(rgba[i*4+2], f);
    }
    fclose(f);
    return 0;
}