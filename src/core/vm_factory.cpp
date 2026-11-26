#include "emulator/vm.h"
#include "emulator/mock_vm.h"
#include "emulator/qemu_vm.h"
#include "emulator/android_vm.h"
#include "emulator/logger.h"

namespace emu {

IVirtualMachine* createVirtualMachine(const std::string& backend) {
    if (backend == "android" || backend == "qemu") {
        LOG_INFO("Creating Android VM (real Play Store)");
        return new AndroidVirtualMachine();
    }
    LOG_INFO("Creating Mock VM (backend=%s)", backend.c_str());
    return new MockVirtualMachine();
}

} // namespace emu