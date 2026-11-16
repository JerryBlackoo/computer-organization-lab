#pragma once
#include "pcie.hpp"
namespace colab::interconnect::pcie {
// Existing controller::Memory and simulation::MemoryAccess both expose read/write.
// Addresses remain ABSOLUTE. The backend and captured callbacks must outlive the fabric.
template <class MemoryPort>
void map_memory(Fabric& fabric, Address base, std::uint64_t size, MemoryPort& memory,
                unsigned preparation_ticks = 1, std::string name = "memory") {
    fabric.map({base, size, preparation_ticks,
        [&memory](Address address, std::size_t count) { return memory.read(address, count); },
        [&memory](Address address, const Bytes& bytes) { memory.write(address, bytes); },
        std::move(name)});
}
}
