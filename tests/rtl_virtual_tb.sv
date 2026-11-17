module rtl_virtual_tb;
    parameter PIPELINED=0;
    logic clk=0, reset=1, flush_valid=0, pte_allowed=1, final_allowed=1;
    wire [31:0] pte_address, final_address, araddr, rdata, awaddr, wdata, debug_value, debug_pc;
    wire [2:0] arprot, awprot; wire [3:0] wstrb;
    wire arvalid, arready, rvalid, rready, awvalid, awready, wvalid, wready, bvalid, bready;
    wire [1:0] rresp, bresp;
    wire retire_valid, retire_write, trap_valid, walk_busy, walk_level, mmu_busy;
    wire [31:0] retire_pc, retire_instruction, retire_value, trap_pc, trap_cause, trap_value;
    wire [4:0] retire_rd;
    logic [4:0] debug_index=5;
    reg [31:0] memory [0:16383];
    reg [255:0] scenario;
    logic pending_read=0, read_valid=0, pending_aw=0, pending_w=0, write_valid=0;
    logic [31:0] saved_read, saved_aw, saved_w, read_data;
    logic [1:0] read_resp=0, write_resp=0; logic [3:0] saved_strobe;
    integer n, lane, delay_count=0, cycle=0, retired=0, pte_reads=0;
    logic flushed=0;
    always #5 clk=~clk;
    assign arready=!pending_read && !read_valid && (scenario == "flush_stalled" ? cycle % 11 == 10 : cycle % 3 != 0);
    assign rvalid=read_valid; assign rdata=read_data; assign rresp=read_resp;
    assign awready=!pending_aw && cycle % 3 == 0;
    assign wready=!pending_w && cycle % 4 == 0;
    assign bvalid=write_valid; assign bresp=write_resp;
    logic stalled_ar=0;
    logic [31:0] stalled_address=0;
    always @(posedge clk) if (!reset) begin
        if (stalled_ar && (!arvalid || araddr !== stalled_address))
            $fatal(1,"AXI read address changed while stalled");
        stalled_ar <= arvalid && !arready;
        stalled_address <= araddr;
    end
    colab_rv32im_virtual_core #(.PIPELINED(PIPELINED)) dut(.clk(clk), .reset(reset),
        .satp(32'h80000001), .privilege(2'd1), .sum(1'b0), .mxr(1'b0),
        .pte_allowed(pte_allowed), .final_allowed(final_allowed), .pte_address(pte_address), .final_address(final_address),
        .flush_valid(flush_valid), .flush_all_va(1'b1), .flush_all_asid(1'b1), .flush_va(32'b0), .flush_asid(9'b0),
        .walk_busy(walk_busy), .walk_level(walk_level), .mmu_busy(mmu_busy),
        .m_axi_araddr(araddr), .m_axi_arprot(arprot), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rvalid(rvalid), .m_axi_rready(rready),
        .m_axi_awaddr(awaddr), .m_axi_awprot(awprot), .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc));
    always @(posedge clk) if (!reset) begin
        cycle<=cycle+1;
        if (arvalid && arready) begin
            if (araddr[1:0] != 0 || pending_read) $fatal(1,"bad AR");
            saved_read<=araddr; pending_read<=1; delay_count<=3;
            if (araddr == 'h1000 || araddr == 'h2000 || araddr == 'h2010) pte_reads<=pte_reads+1;
        end
        if (pending_read && !read_valid) begin
            if (delay_count != 0) delay_count<=delay_count-1;
            else begin
                read_data<=saved_read >= 65536 ? 0 : memory[saved_read >> 2];
                read_resp<=saved_read >= 65536 ? 3 : 0; read_valid<=1;
            end
        end
        if (read_valid && rready) begin read_valid<=0; pending_read<=0; end
        if (awvalid && awready) begin saved_aw<=awaddr; pending_aw<=1; end
        if (wvalid && wready) begin saved_w<=wdata; saved_strobe<=wstrb; pending_w<=1; end
        if (pending_aw && pending_w && !write_valid) begin
            if (saved_aw < 65536)
                for (lane=0; lane<4; lane=lane+1)
                    if (saved_strobe[lane]) memory[saved_aw >> 2][8*lane +: 8]<=saved_w[8*lane +: 8];
            write_resp<=saved_aw >= 65536 ? 3 : 0;
            pending_aw<=0; pending_w<=0; write_valid<=1;
        end
        if (write_valid && bready) write_valid<=0;
    end
    initial begin
        if (!$value$plusargs("scenario=%s",scenario)) $fatal(1,"scenario required");
        for (n=0; n<16384; n=n+1) memory[n]=0;
        memory['h1000 >> 2]=32'h801; // root -> physical L0 0x2000
        memory['h2000 >> 2]=32'hc4b; // VA 0 -> PA 0x3000, supervisor RXA
        memory['h2010 >> 2]=32'h14c7; // VA 0x4000 -> PA 0x5000, supervisor RWAD
        memory['h3000 >> 2]=32'h000040b7; memory['h3004 >> 2]=32'h00d00113;
        memory['h3008 >> 2]=32'h00300193; memory['h300c >> 2]=32'h02310233;
        memory['h3010 >> 2]=32'h0040a023; memory['h3014 >> 2]=32'h0000a283;
        memory['h3018 >> 2]=32'h00000073;
        if (scenario == "execute_fault") memory['h2000 >> 2]=32'hc43; // no X
        if (scenario == "accessed_fault") memory['h2000 >> 2]=32'hc0b; // A=0
        if (scenario == "store_fault") memory['h2010 >> 2]=32'h1447; // D=0
        if (scenario == "load_fault") begin memory['h2010 >> 2]=0; memory['h3010 >> 2]=32'h0000a283; end
        if (scenario == "physical_fault") memory['h2000 >> 2]=32'h804b; // PA beyond RAM
        if (scenario == "deny_pte") pte_allowed=0;
        if (scenario == "deny_final") final_allowed=0;
        repeat (2) @(negedge clk); reset=0;
        while (!trap_valid && cycle < 30000) begin
            @(negedge clk);
            if (retire_valid) retired=retired+1;
            if (flush_valid) flush_valid=0;
            else if (scenario == "flush_walk" && !flushed && dut.mmu.state == 3) begin
                flush_valid=1; flushed=1;
            end
            else if (scenario == "flush_stalled" && !flushed && dut.mmu.state == 10 && stalled_ar && !arready) begin
                flush_valid=1; flushed=1;
            end
        end
        if (!trap_valid) $fatal(1,"virtual core timed out");
        if (scenario == "normal" || scenario == "flush_walk" || scenario == "flush_stalled") begin
            if (trap_cause != 11 || trap_pc != 24 || trap_value != 0 || retired != 6 ||
                debug_value != 39 || memory['h5000 >> 2] != 39 || pte_reads < 4)
                $fatal(1,"translated program failed cause=%d pc=%h retired=%d x5=%h mem=%h PTE=%d",
                    trap_cause,trap_pc,retired,debug_value,memory['h5000 >> 2],pte_reads);
            if (scenario == "flush_walk" && (!flushed || pte_reads < 5)) $fatal(1,"in-flight walk did not restart");
            if (scenario == "flush_stalled" && !flushed) $fatal(1,"stalled PTE flush was not exercised");
        end else if (scenario == "store_fault" || scenario == "load_fault") begin
            if (trap_cause != (scenario == "store_fault" ? 15 : 13) || trap_pc != 16 || trap_value != 'h4000 ||
                retired != 4 || memory['h5000 >> 2] != 0)
                $fatal(1,"data page fault lost precise context cause=%d pc=%h value=%h retired=%d",
                    trap_cause,trap_pc,trap_value,retired);
        end else begin
            if (trap_pc != 0 || trap_value != 0 || retired != 0 ||
                trap_cause != ((scenario == "execute_fault" || scenario == "accessed_fault") ? 12 : 1))
                $fatal(1,"instruction fault lost context cause=%d pc=%h value=%h retired=%d",
                    trap_cause,trap_pc,trap_value,retired);
        end
        $display("PASS RTL virtual core pipelined=%0d scenario=%s retired=%0d pte_reads=%0d",PIPELINED,scenario,retired,pte_reads);
        $finish;
    end
    initial begin #2000000; $fatal(1,"virtual core wall-clock timeout"); end
endmodule
