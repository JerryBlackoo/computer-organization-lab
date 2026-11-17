module rtl_muldiv_tb;
    logic clk = 0, reset = 1, start = 0;
    logic [2:0] funct3 = 0;
    logic [31:0] a = 0, b = 0;
    wire busy, done, alu_valid, alu_sub, carry;
    wire [31:0] result, q, alu_a, alu_b, alu_result;
    wire [32:0] partial;
    wire [5:0] count;
    integer file, status, beats, expected_beats, vectors = 0;
    reg [31:0] expected, in_a, in_b;
    reg [2:0] in_op;
    reg [1023:0] filename;

    always #5 clk = ~clk;
    colab_alu32 alu(.subtract(alu_sub), .a(alu_a), .b(alu_b), .result(alu_result), .carry_or_borrow(carry));
    colab_muldiv mdu(.clk(clk), .reset(reset), .start(start), .funct3(funct3), .operand_a(a), .operand_b(b),
        .busy(busy), .done(done), .result(result), .partial(partial), .q(q), .count(count),
        .alu_valid(alu_valid), .alu_subtract(alu_sub), .alu_a(alu_a), .alu_b(alu_b),
        .alu_result(alu_result), .alu_carry_or_borrow(carry));

    initial begin
        if (!$value$plusargs("vectors=%s", filename)) $fatal(1, "missing +vectors=...");
        file = $fopen(filename, "r");
        if (!file) $fatal(1, "cannot open vectors");
        repeat (2) @(negedge clk);
        reset = 0;
        while (!$feof(file)) begin
            status = $fscanf(file, "%h %h %h %h %d\n", in_op, in_a, in_b, expected, expected_beats);
            if (status == 5) begin
                @(negedge clk); funct3 = in_op; a = in_a; b = in_b; start = 1;
                @(negedge clk); start = 0; a = ~in_a; b = ~in_b; // Sources must already be latched.
                beats = 0;
                while (!done && beats < 40) begin
                    if (!busy) $fatal(1, "unit went idle early");
                    @(negedge clk); beats = beats + 1;
                end
                if (!done || busy || result !== expected || beats != expected_beats ||
                    count != (expected_beats == 0 ? 0 : 32))
                    $fatal(1, "op=%h a=%h b=%h got=%h expected=%h beats=%0d expected=%0d count=%0d",
                        in_op, in_a, in_b, result, expected, beats, expected_beats, count);
                vectors = vectors + 1;
            end else if (status != -1) $fatal(1, "bad vector line (%0d)", status);
        end
        $fclose(file);
        $display("PASS RTL RV32M: %0d vectors, operand latching, 32-bit ALU and exact busy/done timing", vectors);
        $finish;
    end
endmodule
