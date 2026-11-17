module rtl_pipeline_tb;
    logic clk=0, reset=1;
    wire [31:0] imem_address, imem_data, dmem_address, dmem_wdata, dmem_rdata;
    wire imem_error, imem_request, dmem_valid, dmem_write, dmem_ready, dmem_error, md_busy;
    wire [3:0] dmem_wstrb;
    wire retire_valid, retire_write, trap_valid;
    wire [31:0] retire_pc, retire_instruction, retire_value, trap_pc, trap_cause, trap_value;
    wire [31:0] debug_value, debug_pc; wire [4:0] retire_rd;
    logic [4:0] debug_index=0;
    reg [31:0] memory [0:16383];
    reg [1023:0] filename;
    integer program_words, cycle=0, n, lane, overlap=0, busy_cycles=0, memory_stalls=0;

    always #5 clk=~clk;
    assign imem_error=imem_address >= 65536;
    assign imem_data=imem_error ? 0 : memory[imem_address >> 2];
    assign dmem_ready=dmem_valid && cycle % 5 != 0 && cycle % 7 != 0;
    assign dmem_error=dmem_address >= 65536;
    assign dmem_rdata=dmem_error ? 0 : memory[dmem_address >> 2];
    colab_rv32im_pipeline cpu(.clk(clk), .reset(reset),
        .imem_request(imem_request), .imem_address(imem_address), .imem_ready(1'b1),
        .imem_data(imem_data), .imem_error(imem_error), .imem_page_fault(1'b0),
        .dmem_valid(dmem_valid), .dmem_write(dmem_write), .dmem_address(dmem_address),
        .dmem_wdata(dmem_wdata), .dmem_wstrb(dmem_wstrb), .dmem_ready(dmem_ready),
        .dmem_error(dmem_error), .dmem_page_fault(1'b0), .dmem_rdata(dmem_rdata),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc), .md_busy(md_busy));
    always @(posedge clk) if (!reset) begin
        cycle<=cycle+1;
        if (md_busy) busy_cycles<=busy_cycles+1;
        if (dmem_valid && !dmem_ready) memory_stalls<=memory_stalls+1;
        if ((cpu.ifid.valid && cpu.idex.valid && cpu.exmem.valid) ||
            (cpu.ifid.valid && cpu.idex.valid && cpu.memwb.valid) ||
            (cpu.ifid.valid && cpu.exmem.valid && cpu.memwb.valid) ||
            (cpu.idex.valid && cpu.exmem.valid && cpu.memwb.valid)) overlap<=overlap+1;
        if (dmem_valid && dmem_ready && dmem_write && !dmem_error)
            for (lane=0; lane<4; lane=lane+1)
                if (dmem_wstrb[lane]) memory[dmem_address >> 2][8*lane +: 8]<=dmem_wdata[8*lane +: 8];
    end
    initial begin
        if (!$value$plusargs("program=%s",filename) || !$value$plusargs("words=%d",program_words))
            $fatal(1,"program and word count required");
        for (n=0; n<16384; n=n+1) memory[n]=0;
        $readmemh(filename,memory,0,program_words-1);
        repeat (2) @(negedge clk); reset=0;
        while (!trap_valid && cycle < 100000) begin
            @(negedge clk);
            if (retire_valid)
                $display("RET %08h %08h %0d %0d %08h",retire_pc,retire_instruction,
                    retire_write,retire_write ? retire_rd : 0,retire_write ? retire_value : 0);
        end
        if (!trap_valid) $fatal(1,"pipeline exceeded cycle limit");
        $display("TRAP %08h %08h %08h",trap_pc,trap_cause,trap_value);
        for (n=0; n<32; n=n+1) begin
            debug_index=n; #1; $display("REG %0d %08h",n,debug_value);
        end
        $display("MEM 00008000 %08h",memory['h8000 >> 2]);
        $display("MEM 00008004 %08h",memory['h8004 >> 2]);
        $display("MEM 00008008 %08h",memory['h8008 >> 2]);
        $display("PIPE_ACTIVITY %0d %0d %0d",overlap,busy_cycles,memory_stalls);
        $finish;
    end
endmodule
