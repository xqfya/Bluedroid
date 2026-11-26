#pragma once
#include <string>
#include <vector>

namespace emu {

// Google Mobile Services / Play Store compatibility layer.
// Handles: installing GMS core + Play Store, device fingerprint registration,
// Google account sign-in flow, Play Integrity (SafetyNet) attestation hooks.
class GmsManager {
public:
    static GmsManager& instance();

    struct Status {
        bool gmsCoreInstalled = false;
        bool playStoreInstalled = false;
        bool googleAccountSignedIn = false;
        bool deviceRegistered = false;
        bool playIntegrityPassed = false;
        std::string gmsVersion;
        std::string playStoreVersion;
    };
    Status status() const { return status_; }

    // Install GMS core (com.google.android.gms) + Play Store (com.android.vending)
    bool installGmsCore(const std::string& systemImageDir);
    bool installPlayStore(const std::string& systemImageDir);

    // Device fingerprint / props needed for Play Store to consider device certified
    bool applyDeviceFingerprint(const std::string& brand, const std::string& model,
                               const std::string& device, const std::string& fingerprint);
    bool registerDevice(); // calls Google's device registration endpoint

    // Account
    bool signIn(const std::string& email, const std::string& oauthToken);
    void signOut();

    // Play Integrity / SafetyNet attestation (emulator usually fails; needs spoofing)
    bool checkPlayIntegrity();
    std::string integrityVerdict() const { return verdict_; }

    // MicroG alternative for open-source builds (no proprietary GMS)
    bool useMicroG(bool enable);

    std::vector<std::string> requiredApks() const;

private:
    GmsManager();
    Status status_;
    std::string verdict_;
    bool microG_ = false;
};

} // namespace emu