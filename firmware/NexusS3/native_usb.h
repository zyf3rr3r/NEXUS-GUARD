#pragma once

#include <cstdint>

namespace nexus {
void setupNativeUsb();
void pollNativeUsb(uint64_t nowMs, bool allowed);
}
