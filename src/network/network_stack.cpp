#include "emulator/network_stack.h"
#include "emulator/logger.h"

namespace emu {

NetworkStack::NetworkStack() {}
NetworkStack& NetworkStack::instance() { static NetworkStack inst; return inst; }
bool NetworkStack::init() {
    connected_ = true;
    LOG_INFO("Network stack initialized (NAT) ip=%s", ip_.c_str());
    return true;
}
void NetworkStack::shutdown() { connected_ = false; }
bool NetworkStack::forward(int hostPort, int guestPort) {
    LOG_INFO("Port forward %d -> %d", hostPort, guestPort);
    return true;
}
bool NetworkStack::removeForward(int hostPort) {
    LOG_INFO("Remove forward %d", hostPort);
    return true;
}

} // namespace emu