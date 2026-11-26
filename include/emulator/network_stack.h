#pragma once
#include <string>

namespace emu {

class NetworkStack {
public:
    static NetworkStack& instance();
    bool init();
    void shutdown();
    bool isConnected() const { return connected_; }
    std::string ipAddress() const { return ip_; }
    // Port forwarding host:guest
    bool forward(int hostPort, int guestPort);
    bool removeForward(int hostPort);
private:
    NetworkStack();
    bool connected_ = false;
    std::string ip_ = "10.0.2.15";
};

} // namespace emu