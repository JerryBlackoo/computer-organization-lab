#pragma once

#include "../alu/alu.hpp"
#include "../datapath/bus.hpp"
#include "../datapath/dedicated/alu_control.hpp"

namespace colab::soc::core::control_unit {

enum class RegisterSelect { Rs1, Rs2 };

struct ControlSignals {
    datapath::BusSource bus_source = datapath::BusSource::None;
    RegisterSelect register_select = RegisterSelect::Rs1;
    bool mar_in = false;
    bool pc_in = false;
    bool mdr_in = false;
    bool ir_in = false;
    bool y_in = false;
    bool z_in = false;
    bool gpr_in = false;
    bool fr_in = false;
    bool read = false;
    bool write = false;
    bool memory_to_mdr = false;
    bool increment_pc = false;
    bool decode = false;
    bool halt = false;
    ALUOperation alu_op = ALUOperation::Add;
};

// 这组控制线由两种 RV32 控制器共同驱动，与旧单总线控制线分开命名。
namespace rv32 {
enum class OperandB { Register, Immediate };
enum class WriteBack { ALU, Memory, UpperImmediate, PcRelative, ReturnAddress };
enum class PCSource { Sequential, Branch, JumpImmediate, JumpRegister };
enum class Compare { Equal, NotEqual, LessSigned, GreaterEqualSigned, LessUnsigned, GreaterEqualUnsigned };

// Historical field name: for all branch types it carries the selected comparator's result.
struct Feedback { bool equal = false; };

// 水平型控制字：独立使能允许相容微操作并行；枚举选择器表示互斥输入。
struct ControlSignals {
    bool mar_from_pc = false;
    bool instruction_read = false;
    bool instruction_to_mdr = false;
    bool ir_in = false;
    bool decode = false;
    bool operands_in = false;
    bool alu_in = false;
    bool address_in = false;
    bool condition_in = false;
    bool data_read = false;
    bool data_to_mdr = false;
    bool memory_write = false;
    bool register_write = false;
    bool pc_write = false;
    bool retire = false;
    bool system = false; // CSR writeback / trap return through the privileged state port.
    unsigned memory_bytes = 4;
    bool load_signed = false;
    datapath::dedicated::ALUCtr alu_op = datapath::dedicated::ALUCtr::Addu;
    OperandB operand_b = OperandB::Register;
    WriteBack write_back = WriteBack::ALU;
    PCSource pc_source = PCSource::Sequential;
    Compare compare = Compare::Equal;
};
} // namespace rv32
} // namespace colab::soc::core::control_unit
