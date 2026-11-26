#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int ppm_write(const char* path, int w, int h, const unsigned char* rgba);
#ifdef __cplusplus
}
#endif