// 32-bit combinational adder/subtractor. For subtraction, carry is borrow.
module colab_alu32 (
    input  logic        subtract,
    input  logic [31:0] a,
    input  logic [31:0] b,
    output logic [31:0] result,
    output logic        carry_or_borrow
);
    always_comb begin
        if (subtract) begin
            result = a - b;
            carry_or_borrow = a < b;
        end else begin
            result = a + b;
            carry_or_borrow = result < a;
        end
    end
endmodule
