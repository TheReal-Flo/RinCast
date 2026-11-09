#pragma once

#include <cstdint>
#include <string>

#include "../capture/mac_monitors.h"

namespace rcm {
namespace input {

void MoveMouse(const MacMonitorInfo& monitor, float u, float v);
void MouseButton(int button, bool down);
void MouseWheel(float dx, float dy);
void Key(uint16_t winVk, bool down);
void Text(const std::string& utf8Text);
void ReleaseAll();

}  // namespace input
}  // namespace rcm
