`timescale 1ns/1ps
module rtl_microcoded_lockstep_tb;
    parameter VERTICAL = 0;
    parameter ROM_FILE = "rtl/microcoded/rv32_control.hex";
    reg clk = 0, reset = 1;
    always #5 clk = ~clk;
    wire [31:0] ia, da, dw, pc, ir, debug_value;
    wire iread, dread, dwrite, retire, fault;
    wire [7:0] upc;
    wire [63:0] uir;
    reg [31:0] imem [0:31], dmem [0:31];
    colab_rv32_microcoded #(.VERTICAL(VERTICAL), .ROM_FILE(ROM_FILE)) dut(.clk(clk), .reset(reset), .enable(1'b1),
        .instr_address(ia), .instr_read(iread), .instr_data(imem[ia[6:2]]),
        .data_address(da), .data_read(dread), .data_read_data(dmem[da[6:2]]),
        .data_write(dwrite), .data_write_data(dw), .debug_reg(5'b0), .debug_value(debug_value),
        .pc(pc), .ir(ir), .upc(upc), .uir(uir), .fault(fault), .retire(retire));
    integer i, retired = 0, cycles = 0;
    always @(posedge clk) if (!reset) begin
        if (fault || cycles > 200) $fatal(1,"unexpected fault or timeout");
        $display("TRACE %0d %0d %016h %08h %08h %08h %08h %08h %08h %08h %08h %08h %08h %08h %08h %08h",
            pc, upc, uir, ir, dut.mar, dut.mdr, dut.a, dut.b, dut.alu_out,
            dut.regs[1], dut.regs[2], dut.regs[3], dut.regs[4], dut.regs[5], dut.regs[6], dmem[0]);
        if (dwrite) dmem[da[6:2]] <= dw;
        cycles = cycles + 1;
        if (retire) begin
            retired = retired + 1;
            if (retired == 9) #1 $finish;
        end
    end
    initial begin
        for (i = 0; i < 32; i = i + 1) begin imem[i] = 0; dmem[i] = 0; end
        imem[0]=32'h00a00093; imem[1]=32'h01400113; imem[2]=32'h002081b3;
        imem[3]=32'h00302023; imem[4]=32'h00002203; imem[5]=32'h00418463;
        imem[6]=32'h06300293; imem[7]=32'h401202b3; imem[8]=32'h00208463;
        imem[9]=32'h00700313;
        repeat (2) @(negedge clk); reset = 0;
    end
endmodule
