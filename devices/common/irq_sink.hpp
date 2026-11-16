#pragma once

namespace colab::devices::common {
class IrqSink {
public:
    virtual ~IrqSink() = default;
    virtual void set_irq(unsigned source, bool asserted) = 0;
};
}
