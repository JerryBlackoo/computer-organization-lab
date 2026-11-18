`timescale 1ns/1ps
module rtl_microcoded_tb;
    parameter VERTICAL = 0;
    parameter ROM_FILE = "rtl/microcoded/rv32_control.hex";
    reg clk = 0, reset = 1, enable = 1;
    always #5 clk = ~clk;
    wire [31:0] ia, idata, da, drdata, dwdata, pc, ir, debug_value;
    wire iread, dread, dwrite, retire, fault;
    wire [7:0] upc;
    wire [63:0] uir;
    reg [4:0] debug_reg = 0;
    reg [31:0] imem [0:31];
    reg [31:0] dmem [0:31];
    assign idata = imem[ia[6:2]];
    assign drdata = dmem[da[6:2]];
    colab_rv32_microcoded #(.VERTICAL(VERTICAL), .ROM_FILE(ROM_FILE)) dut(.clk(clk), .reset(reset), .enable(enable),
        .instr_address(ia), .instr_read(iread), .instr_data(idata),
        .data_address(da), .data_read(dread), .data_read_data(drdata),
        .data_write(dwrite), .data_write_data(dwdata), .debug_reg(debug_reg),
        .debug_value(debug_value), .pc(pc), .ir(ir), .upc(upc), .uir(uir),
        .fault(fault), .retire(retire));
    integer i, retired = 0, cycles = 0;
    reg held = 0, patched_run = 0;
    always @(posedge clk) begin
        if (!reset && dwrite) begin
            if (patched_run || da !== 0 || dwdata !== 30) $fatal(1, "incorrect store");
            dmem[da[6:2]] <= dwdata;
        end
        if (!reset && retire) begin
            if (!patched_run) case (retired)
            0: if (pc !== 0 || ir !== 32'h00a00093) $fatal(1,"retire 0");
            1: if (pc !== 4 || ir !== 32'h01400113) $fatal(1,"retire 1");
            2: if (pc !== 8 || ir !== 32'h002081b3) $fatal(1,"retire 2");
            3: if (pc !== 12 || ir !== 32'h00302023) $fatal(1,"retire 3");
            4: if (pc !== 16 || ir !== 32'h00002203) $fatal(1,"retire 4");
            5: if (pc !== 20 || ir !== 32'h00418463) $fatal(1,"branch taken");
            6: if (pc !== 28 || ir !== 32'h401202b3) $fatal(1,"retire SUB");
            7: if (pc !== 32 || ir !== 32'h00208463) $fatal(1,"branch not taken");
            8: if (pc !== 36 || ir !== 32'h00700313) $fatal(1,"retire ADDI");
            default: $fatal(1,"unexpected retirement");
            endcase
            retired = retired + 1;
        end
    end
    task check_reg(input [4:0] index, input [31:0] expected);
        begin debug_reg = index; #1;
        if (debug_value !== expected) $fatal(1,"x%0d got %h expected %h", index, debug_value, expected);
        end
    endtask
    initial begin
        for (i = 0; i < 32; i = i + 1) begin imem[i] = 0; dmem[i] = 0; end
        imem[0]=32'h00a00093; imem[1]=32'h01400113; imem[2]=32'h002081b3;
        imem[3]=32'h00302023; imem[4]=32'h00002203; imem[5]=32'h00418463;
        imem[6]=32'h06300293; imem[7]=32'h401202b3; imem[8]=32'h00208463;
        imem[9]=32'h00700313; imem[10]=0;
        repeat (2) @(negedge clk); reset = 0;
        while (!fault && cycles < 300) begin
            @(negedge clk); cycles = cycles + 1;
            if (pc == 8 && upc == 16 && uir !== (VERTICAL ? 64'h106 : 64'h200020))
                $fatal(1,"raw read-operands microinstruction");
            if (pc == 8 && upc == 17 && !held) begin
                if (uir !== (VERTICAL ? 64'h109 : 64'h200100)) $fatal(1,"raw add microinstruction");
                held = 1; enable = 0;
                repeat (3) begin
                    @(negedge clk);
                    if (upc !== 17 || uir !== (VERTICAL ? 64'h109 : 64'h200100) || pc !== 8)
                        $fatal(1,"microcontroller did not hold");
                    if (dwrite || retire) $fatal(1,"write while stalled");
                end
                enable = 1;
            end
        end
        if (!fault || retired != 9 || !held) $fatal(1,"fault=%b retired=%0d", fault, retired);
        if (pc !== 40 || dmem[0] !== 30) $fatal(1,"final PC/memory");
        check_reg(0, 0); check_reg(1, 10); check_reg(2, 20); check_reg(3, 30);
        check_reg(4, 30); check_reg(5, 20); check_reg(6, 7);
        // Change only the ALU operation bits in the real ROM word at address 17.
        // ADD decodes to the same microprogram, but the patched bits select SUBU.
        reset = 1; patched_run = 1; retired = 0; cycles = 0;
        imem[0] = 32'h01400093; // addi x1,x0,20
        imem[1] = 32'h00700113; // addi x2,x0,7
        imem[2] = 32'h002081b3; // add x3,x1,x2
        imem[3] = 0;
        if (VERTICAL) dut.rom[17] = (dut.rom[17] & ~(65'h1f << 8)) | (65'h3 << 8);
        else dut.rom[17] = (dut.rom[17] & ~(65'h1f << 21)) | (65'h3 << 21);
        repeat (2) @(negedge clk); reset = 0;
        while (!fault && cycles < 100) begin @(negedge clk); cycles = cycles + 1; end
        if (!fault || retired != 3) $fatal(1,"patched ROM run");
        check_reg(3, 13);
        $display("PASS rtl_microcoded: binary ROM patch, IR dispatch, ADD/ADDI/SUB/LW/SW/BEQ, stall, illegal");
        $finish;
    end
endmodule
