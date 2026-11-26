#include "emulator/gms.h"
#include "emulator/logger.h"
#include "emulator/adb.h"
#include "emulator/config.h"

namespace emu {

GmsManager::GmsManager() {}

GmsManager& GmsManager::instance() {
    static GmsManager inst;
    return inst;
}

std::vector<std::string> GmsManager::requiredApks() const {
    if (microG_) {
        return { "microg-services.apk", "microg-store.apk" };
    }
    return {
        "com.google.android.gms.apk",          // Google Play Services
        "com.google.android.gsf.apk",          // Google Services Framework
        "com.android.vending.apk",             // Google Play Store
        "com.google.android.ims.apk",          // Carrier Services (optional)
    };
}

bool GmsManager::installGmsCore(const std::string& systemImageDir) {
    LOG_INFO("Installing GMS core into %s", systemImageDir.c_str());
    // Real steps:
    // 1. Push GMS core APK to /system/priv-app/ (requires root/system image)
    // 2. Set permissions (chmod 644, chown root:root)
    // 3. Clear dalvik cache, reboot
    status_.gmsCoreInstalled = true;
    status_.gmsVersion = "24.09.15";
    return true;
}

bool GmsManager::installPlayStore(const std::string& systemImageDir) {
    LOG_INFO("Installing Play Store into %s", systemImageDir.c_str());
    status_.playStoreInstalled = true;
    status_.playStoreVersion = "40.1.19";
    return true;
}

bool GmsManager::applyDeviceFingerprint(const std::string& brand, const std::string& model,
                                       const std::string& device, const std::string& fingerprint) {
    // These props must be set in build.prop / default.prop of the system image
    // so Google's servers consider the device "certified".
    // ro.product.brand, ro.product.model, ro.product.device, ro.build.fingerprint,
    // ro.build.id, ro.build.version.release, ro.build.version.sdk, etc.
    LOG_INFO("Applying device fingerprint: %s/%s/%s", brand.c_str(), model.c_str(), device.c_str());
    LOG_INFO("  fingerprint=%s", fingerprint.c_str());
    status_.deviceRegistered = false; // needs registerDevice()
    return true;
}

bool GmsManager::registerDevice() {
    // Calls https://android.clients.google.com/checkin (device checkin) and
    // https://android.clients.google.com/c2dm/register3 for GCM.
    // Requires a valid Google account token. Returns a device ID (androidId).
    LOG_INFO("Registering device with Google (stub)");
    status_.deviceRegistered = true;
    return true;
}

bool GmsManager::signIn(const std::string& email, const std::string& oauthToken) {
    LOG_INFO("Signing in Google account: %s", email.c_str());
    // Real: use GoogleAuthUtil / OAuth2 token flow, store in accounts.db
    status_.googleAccountSignedIn = true;
    return true;
}

void GmsManager::signOut() {
    status_.googleAccountSignedIn = false;
}

bool GmsManager::checkPlayIntegrity() {
    // Play Integrity API: MEETS_DEVICE_INTEGRITY / MEETS_BASIC_INTEGRITY / MEETS_STRONG_INTEGRITY
    // Emulators typically fail MEETS_DEVICE_INTEGRITY unless fingerprint is spoofed
    // and the bootloader is reported as locked.
    LOG_INFO("Checking Play Integrity...");
    if (status_.deviceRegistered && status_.gmsCoreInstalled) {
        verdict_ = "MEETS_BASIC_INTEGRITY (emulator - device integrity may fail)";
        status_.playIntegrityPassed = false;
    } else {
        verdict_ = "FAILED";
        status_.playIntegrityPassed = false;
    }
    return status_.playIntegrityPassed;
}

bool GmsManager::useMicroG(bool enable) {
    microG_ = enable;
    LOG_INFO("MicroG mode: %s", enable ? "ON" : "OFF");
    return true;
}

} // namespace emu