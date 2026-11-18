#pragma once
#include "gprs/gprs.hpp"
#include "fr/fr.hpp"
#include "ir/ir.hpp"
#include "mar/mar.hpp"
#include "mdr/mdr.hpp"
#include "pc/pc.hpp"
#include "y/y.hpp"
#include "z/z.hpp"

namespace colab::soc::core {
struct Registers {
    registers::PC pc;
    registers::IR ir;
    registers::GPRs gprs;
    registers::MAR mar;
    registers::MDR mdr;
    registers::Y y;
    registers::Z z;
    registers::FR fr;
};
}
