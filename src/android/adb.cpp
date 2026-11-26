#include "emulator/adb.h"
#include "emulator/logger.h"
#include <sstream>

namespace emu {

AdbClient::AdbClient() {}

void AdbClient::emit(const std::string& m) {
    if (logCb_) logCb_(m);
    LOG_DEBUG("adb: %s", m.c_str());
}

bool AdbClient::connect(const std::string& host, int port) {
    host_ = host; port_ = port;
    // Stub: pretend connected. Real impl uses TCP socket to adbd.
    connected_ = true;
    emit("connected to " + host + ":" + std::to_string(port));
    return true;
}

void AdbClient::disconnect() {
    connected_ = false;
    emit("disconnected");
}

std::string AdbClient::shell(const std::string& cmd) {
    if (!connected_) return "not connected";
    emit("$ " + cmd);
    // Stub response
    return "[stub] " + cmd + " -> OK";
}

bool AdbClient::push(const std::string& localPath, const std::string& remotePath) {
    if (!connected_) return false;
    emit("push " + localPath + " -> " + remotePath);
    return true;
}

bool AdbClient::pull(const std::string& remotePath, const std::string& localPath) {
    if (!connected_) return false;
    emit("pull " + remotePath + " -> " + localPath);
    return true;
}

bool AdbClient::installApk(const std::string& apkPath) {
    if (!connected_) return false;
    emit("install " + apkPath);
    return true;
}

std::vector<std::string> AdbClient::listPackages() {
    std::vector<std::string> pkgs;
    if (!connected_) return pkgs;
    pkgs.push_back("com.android.settings");
    pkgs.push_back("com.google.android.gms");
    pkgs.push_back("com.android.vending"); // Play Store
    return pkgs;
}

bool AdbClient::uninstall(const std::string& packageName) {
    if (!connected_) return false;
    emit("uninstall " + packageName);
    return true;
}

} // namespace emu