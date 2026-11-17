`ifdef WITH_PIPELINE_CACHE
`define TB_HAS_CACHE
`endif
`ifdef WITH_CACHE
`define TB_HAS_CACHE
`endif
module rtl_core_tb;
    logic clk = 0, reset = 1;
    wire [31:0] araddr, rdata, awaddr, wdata, debug_value, debug_pc;
    wire [2:0] arprot, awprot;
    wire arvalid, arready, rvalid, rready, awvalid, awready, wvalid, wready, bvalid, bready;
    wire [3:0] wstrb;
    wire [1:0] rresp, bresp;
    wire c_arvalid, c_arready;
    wire retire_valid, retire_write, trap_valid;
    wire [31:0] retire_pc, retire_instruction, retire_value, trap_pc, trap_cause, trap_value;
    wire [4:0] retire_rd;
    logic [4:0] debug_index = 0;
    reg [31:0] memory [0:16383];
    reg [1023:0] filename;
    integer program_words, cycle = 0, n, lane, core_reads = 0, bus_reads = 0;
    logic pending_read = 0, read_valid = 0, pending_aw = 0, pending_w = 0, write_valid = 0;
    logic [31:0] saved_read, saved_aw, saved_w, read_data;
    logic [1:0] read_resp=0, write_resp=0;
    logic [3:0] saved_strobe;
    integer read_delay = 0;

    always #5 clk = ~clk;
    assign arready = !pending_read && !read_valid && cycle % 3 != 0;
    assign rvalid = read_valid; assign rdata = read_data; assign rresp = read_resp;
    assign awready = !pending_aw && cycle % 3 == 0;
    assign wready = !pending_w && cycle % 4 == 0;
    assign bvalid = write_valid; assign bresp = write_resp;
`ifdef WITH_PIPELINE_CACHE
    colab_rv32im_cached_core #(.PIPELINED(1))
`elsif WITH_PIPELINE
    colab_rv32im_pipeline_axi
`elsif WITH_CACHE
    colab_rv32im_cached_core
`else
    colab_rv32im_core
`endif
    cpu(.clk(clk), .reset(reset),
`ifdef TB_HAS_CACHE
        .invalidate_all(1'b0), .snoop_valid(1'b0), .snoop_address(32'b0),
`endif
        .m_axi_araddr(araddr), .m_axi_arprot(arprot), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rvalid(rvalid), .m_axi_rready(rready),
        .m_axi_awaddr(awaddr), .m_axi_awprot(awprot), .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc));

`ifdef TB_HAS_CACHE
    assign c_arvalid=cpu.core_arvalid; assign c_arready=cpu.core_arready;
`else
    assign c_arvalid=arvalid; assign c_arready=arready;
`endif

    always @(posedge clk) if (!reset) begin
        cycle <= cycle + 1;
        if (c_arvalid && c_arready) core_reads <= core_reads + 1;
        if (arvalid && arready) begin
            bus_reads <= bus_reads + 1;
            if (araddr[1:0] != 0) $fatal(1, "unaligned read address %h", araddr);
            saved_read <= araddr; pending_read <= 1; read_delay <= 2;
        end
        if (pending_read && !read_valid) begin
            if (read_delay != 0) read_delay <= read_delay - 1;
            else begin
                read_data <= saved_read >= 65536 ? 0 : memory[saved_read >> 2];
                read_resp <= saved_read >= 65536 ? 2 : 0; read_valid <= 1;
            end
        end
        if (read_valid && rready) begin read_valid <= 0; pending_read <= 0; end
        if (awvalid && awready) begin
            if (awaddr[1:0] != 0) $fatal(1, "unaligned write address %h", awaddr);
            saved_aw <= awaddr; pending_aw <= 1;
        end
        if (wvalid && wready) begin saved_w <= wdata; saved_strobe <= wstrb; pending_w <= 1; end
        if (pending_aw && pending_w && !write_valid) begin
            if (saved_aw < 65536)
                for (lane = 0; lane < 4; lane = lane + 1)
                    if (saved_strobe[lane]) memory[saved_aw >> 2][8*lane +: 8] <= saved_w[8*lane +: 8];
            write_resp <= saved_aw >= 65536 ? 2 : 0;
            pending_aw <= 0; pending_w <= 0; write_valid <= 1;
        end
        if (write_valid && bready) write_valid <= 0;
    end

    initial begin
        if (!$value$plusargs("program=%s", filename) || !$value$plusargs("words=%d", program_words))
            $fatal(1, "program and word count required");
        for (n = 0; n < 16384; n = n + 1) memory[n] = 0;
        $readmemh(filename, memory, 0, program_words - 1);
        repeat (2) @(negedge clk); reset = 0;
        while (!trap_valid && cycle < 100000) begin
            @(negedge clk);
            if (retire_valid)
                $display("RET %08h %08h %0d %0d %08h", retire_pc, retire_instruction,
                    retire_write, retire_write ? retire_rd : 0, retire_write ? retire_value : 0);
        end
        if (!trap_valid) $fatal(1, "core exceeded cycle limit");
        $display("TRAP %08h %08h %08h", trap_pc, trap_cause, trap_value);
        for (n = 0; n < 32; n = n + 1) begin
            debug_index = n; #1; $display("REG %0d %08h", n, debug_value);
        end
        $display("MEM 00008000 %08h", memory['h8000 >> 2]);
        $display("MEM 00008004 %08h", memory['h8004 >> 2]);
        $display("MEM 00008008 %08h", memory['h8008 >> 2]);
        $display("CACHE_ACTIVITY %0d %0d", core_reads, bus_reads);
        $finish;
    end
endmodule
