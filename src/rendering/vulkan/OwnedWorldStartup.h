#pragma once
#include <string_view>
namespace OptiCraft::Vulkan {
// Automatic admission by default; explicit opt-out also honors master switch.
constexpr bool ownedWorldRequested(const char* owned,const char* master) noexcept {
    const auto disabled=[](const char* s){return s && (std::string_view(s)=="0" || std::string_view(s)=="off");};
    return !disabled(owned) && !disabled(master);
}
}
