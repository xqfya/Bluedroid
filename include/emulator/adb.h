#pragma once
#include <string>
#include <vector>
#include <functional>

namespace emu {

// Lightweight ADB client talking to the emulator's adbd over TCP (5555).
class AdbClient {
public:
    AdbClient();
    bool connect(const std::string& host, int port = 5555);
    void disconnect();
    bool isConnected() const { return connected_; }

    std::string shell(const std::string& cmd);
    bool push(const std::string& localPath, const std::string& remotePath);
    bool pull(const std::string& remotePath, const std::string& localPath);
    bool installApk(const std::string& apkPath);
    std::vector<std::string> listPackages();
    bool uninstall(const std::string& packageName);

    using LogCallback = std::function<void(const std::string&)>;
    void setLogCallback(LogCallback cb) { logCb_ = std::move(cb); }

private:
    bool connected_ = false;
    std::string host_;
    int port_ = 5555;
    LogCallback logCb_;
    void emit(const std::string& m);
};

} // namespace emu