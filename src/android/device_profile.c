#include "emulator/device_profile.h"
#include <stdio.h>
#include <string.h>

static const DeviceProfile profiles[] = {
    { "google", "Google", "Pixel 7", "panther", "panther",
      "google/panther/panther:13/TQ3A.230901.001/10756687:user/release-keys",
      "2023-09-01", 33 },
    { "google", "Google", "Pixel 6", "oriole", "oriole",
      "google/oriole/oriole:13/TQ3A.230901.001/10756687:user/release-keys",
      "2023-09-01", 33 },
    { "samsung", "samsung", "SM-G991B", "o1s", "o1sxxx",
      "samsung/o1s/o1s:13/TP1A.220624.014/G991BXXU5DWA4:user/release-keys",
      "2023-01-01", 33 },
    { "xiaomi", "Xiaomi", "2211133C", "nuwa", "nuwa",
      "xiaomi/nuwa/nuwa:13/TKQ1.221114.001/V14.0.5.0.TMBCNXM:user/release-keys",
      "2023-02-01", 33 },
};

int dp_count(void) { return (int)(sizeof(profiles)/sizeof(profiles[0])); }
const DeviceProfile* dp_get(int index) {
    if (index < 0 || index >= dp_count()) return 0;
    return &profiles[index];
}
const DeviceProfile* dp_default_profile(void) { return &profiles[0]; }

int dp_write_build_prop(const DeviceProfile* p, const char* outPath) {
    if (!p || !outPath) return -1;
    FILE* f = fopen(outPath, "w");
    if (!f) return -2;
    fprintf(f, "ro.product.brand=%s\n", p->brand);
    fprintf(f, "ro.product.manufacturer=%s\n", p->manufacturer);
    fprintf(f, "ro.product.model=%s\n", p->model);
    fprintf(f, "ro.product.device=%s\n", p->device);
    fprintf(f, "ro.product.name=%s\n", p->product);
    fprintf(f, "ro.build.fingerprint=%s\n", p->fingerprint);
    fprintf(f, "ro.build.id=TQ3A.230901.001\n");
    fprintf(f, "ro.build.version.release=13\n");
    fprintf(f, "ro.build.version.sdk=%d\n", p->sdkLevel);
    fprintf(f, "ro.build.version.security_patch=%s\n", p->securityPatch);
    fprintf(f, "ro.build.type=user\n");
    fprintf(f, "ro.build.tags=release-keys\n");
    fprintf(f, "ro.debuggable=0\n");
    fprintf(f, "ro.secure=1\n");
    fprintf(f, "ro.boot.verifiedbootstate=green\n");
    fprintf(f, "ro.boot.flash.locked=1\n");
    fprintf(f, "ro.boot.vbmeta.device_state=locked\n");
    fclose(f);
    return 0;
}