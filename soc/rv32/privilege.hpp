#pragma once

#include <cstdint>

namespace colab::soc::core {
enum class PrivilegeMode : std::uint32_t { User = 0, Supervisor = 1, Machine = 3 };
} // namespace colab::soc::core
