#pragma once

namespace colab::soc::core::registers {

struct Flags {
    bool sf = false;
    bool of = false;
    // Append after SF/OF so historical Flags{sf, of} keeps its meaning.
    bool cf = false;
    bool zf = false;
};

class FR {
public:
    Flags read() const { return flags_; }
    void write(Flags flags) { flags_ = flags; }

private:
    Flags flags_{};
};
}
