// RV32M sequential multiply/divide unit. One iteration uses at most one
// external 32-bit add/sub ALU operation and latches the shifted registers.
module colab_muldiv (
    input  logic        clk,
    input  logic        reset,
    input  logic        start,
    input  logic [2:0]  funct3,
    input  logic [31:0] operand_a,
    input  logic [31:0] operand_b,
    output logic        busy,
    output logic        done,
    output logic [31:0] result,
    output logic [32:0] partial,
    output logic [31:0] q,
    output logic [5:0]  count,
    output logic        alu_valid,
    output logic        alu_subtract,
    output logic [31:0] alu_a,
    output logic [31:0] alu_b,
    input  logic [31:0] alu_result,
    input  logic        alu_carry_or_borrow
);
    localparam logic [2:0] IDLE = 0, RUN = 1, FIX_MIXED = 2, FIX_QUOT = 3, FIX_REM = 4;
    logic [2:0] state, op;
    logic [31:0] x, divisor;
    logic previous, negative_quotient, negative_remainder, mixed_high;
    logic [32:0] next_partial, shifted, booth_value;
    logic [31:0] next_q;
    logic negative_trial;

    assign busy = state != IDLE;

    always_comb begin
        alu_valid = 0; alu_subtract = 0; alu_a = 0; alu_b = 0;
        next_partial = partial; next_q = q;
        shifted = 0; booth_value = partial; negative_trial = 0;
        if (state == RUN) begin
            if (op == 0 || op == 3) begin
                if (q[0]) begin
                    alu_valid = 1; alu_a = partial[31:0]; alu_b = x;
                    next_partial = {1'b0, alu_carry_or_borrow, alu_result[31:1]};
                    next_q = {alu_result[0], q[31:1]};
                end else begin
                    next_partial = {2'b00, partial[31:1]};
                    next_q = {partial[0], q[31:1]};
                end
            end else if (op == 1 || op == 2) begin
                if (q[0] != previous) begin
                    alu_valid = 1; alu_subtract = q[0]; alu_a = partial[31:0]; alu_b = x;
                    booth_value = {partial[32] ^ x[31] ^ alu_carry_or_borrow, alu_result};
                end
                next_partial = {booth_value[32], booth_value[32:1]};
                next_q = {booth_value[0], q[31:1]};
            end else begin
                shifted = {partial[31:0], q[31]};
                alu_valid = 1; alu_subtract = 1; alu_a = shifted[31:0]; alu_b = divisor;
                negative_trial = !shifted[32] && alu_carry_or_borrow;
                if (!negative_trial)
                    next_partial = {shifted[32] - alu_carry_or_borrow, alu_result};
                else next_partial = shifted;
                next_q = {q[30:0], !negative_trial};
            end
        end else if (state == FIX_MIXED) begin
            alu_valid = 1; alu_a = partial[31:0]; alu_b = x;
        end else if (state == FIX_QUOT) begin
            alu_valid = 1; alu_subtract = 1; alu_a = 0; alu_b = q;
        end else if (state == FIX_REM) begin
            alu_valid = 1; alu_subtract = 1; alu_a = 0; alu_b = partial[31:0];
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            state <= IDLE; done <= 0; result <= 0; partial <= 0; q <= 0; count <= 0;
            x <= 0; divisor <= 0; op <= 0; previous <= 0;
            negative_quotient <= 0; negative_remainder <= 0; mixed_high <= 0;
        end else if (start && state == IDLE) begin
            done <= 0; result <= 0; partial <= 0; count <= 0;
            x <= operand_a; op <= funct3; previous <= 0;
            negative_quotient <= 0; negative_remainder <= 0; mixed_high <= funct3 == 2 && operand_b[31];
            if (funct3 >= 4 && operand_b == 0) begin
                q <= 32'hffffffff;
                partial <= {(funct3 == 4 || funct3 == 6) && operand_a[31], operand_a};
                result <= (funct3 == 4 || funct3 == 5) ? 32'hffffffff : operand_a;
                done <= 1;
            end else if ((funct3 == 4 || funct3 == 6) && operand_a == 32'h80000000 && operand_b == 32'hffffffff) begin
                q <= operand_a; partial <= 0;
                result <= funct3 == 4 ? operand_a : 0; done <= 1;
            end else begin
                if (funct3 == 4 || funct3 == 6) begin
                    q <= operand_a[31] ? -operand_a : operand_a;
                    divisor <= operand_b[31] ? -operand_b : operand_b;
                    negative_quotient <= operand_a[31] ^ operand_b[31];
                    negative_remainder <= operand_a[31];
                end else if (funct3 >= 4) begin
                    q <= operand_a; divisor <= operand_b;
                end else q <= operand_b;
                state <= RUN;
            end
        end else if (state == RUN) begin
            partial <= next_partial; q <= next_q; count <= count + 1'b1;
            if (op == 1 || op == 2) previous <= q[0];
            if (count == 31) begin
                if (mixed_high) state <= FIX_MIXED;
                else if (op >= 4 && negative_quotient && next_q != 0) state <= FIX_QUOT;
                else if (op >= 4 && negative_remainder && next_partial[31:0] != 0) state <= FIX_REM;
                else begin
                    if (op == 0) result <= next_q;
                    else if (op <= 3) result <= next_partial[31:0];
                    else if (op == 4 || op == 5) result <= next_q;
                    else result <= next_partial[31:0];
                    state <= IDLE; done <= 1;
                end
            end
        end else if (state == FIX_MIXED) begin
            partial <= {alu_result[31], alu_result}; result <= alu_result;
            state <= IDLE; done <= 1;
        end else if (state == FIX_QUOT) begin
            q <= alu_result;
            if (negative_remainder && partial[31:0] != 0) state <= FIX_REM;
            else begin
                result <= (op == 4 || op == 5) ? alu_result : partial[31:0];
                state <= IDLE; done <= 1;
            end
        end else if (state == FIX_REM) begin
            partial <= {1'b1, alu_result};
            result <= (op == 4 || op == 5) ? q : alu_result;
            state <= IDLE; done <= 1;
        end
    end
endmodule
