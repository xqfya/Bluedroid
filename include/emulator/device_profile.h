#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Generates an Android build.prop snippet for a certified device profile,
// which is required for Google Play Store to treat the emulator as a
// certified device.
typedef struct {
    const char* brand;
    const char* manufacturer;
    const char* model;
    const char* device;
    const char* product;
    const char* fingerprint;
    const char* securityPatch;
    int sdkLevel;
} DeviceProfile;

int dp_write_build_prop(const DeviceProfile* p, const char* outPath);
const DeviceProfile* dp_default_profile(void);
int dp_count(void);
const DeviceProfile* dp_get(int index);

#ifdef __cplusplus
}
#endif