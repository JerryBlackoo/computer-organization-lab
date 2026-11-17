module rtl_sv32_tb;
    logic clk=0, reset=1, req_valid=0, result_ready=0, pte_ready=1;
    logic [31:0] va=0, satp=0;
    logic [1:0] access=1, privilege=1;
    logic sum=0, mxr=0, pte_allowed=1, final_allowed=1;
    logic flush_valid=0, flush_all_va=1, flush_all_asid=1;
    logic [31:0] flush_va=0; logic [8:0] flush_asid=0;
    wire req_ready, result_valid, result_fault, pte_valid, pte_response_ready, walk_busy, walk_level;
    wire [31:0] result_pa, result_cause, result_tval, pte_address, final_address;
    logic pte_response_valid=0, pte_response_error=0;
    logic [31:0] pte_response_data=0;
    logic inject_error=0, allow_response=1, pending=0;
    logic [31:0] pending_address;
    integer latency=0, reads=0, i;
    reg [31:0] memory [0:4095];

    always #5 clk=~clk;
    colab_sv32 mmu(.clk(clk), .reset(reset), .req_valid(req_valid), .req_ready(req_ready),
        .req_va(va), .req_satp(satp), .req_access(access), .req_privilege(privilege), .req_sum(sum), .req_mxr(mxr),
        .result_valid(result_valid), .result_ready(result_ready), .result_pa(result_pa), .result_cause(result_cause),
        .result_tval(result_tval), .result_fault(result_fault), .pte_valid(pte_valid), .pte_ready(pte_ready),
        .pte_address(pte_address), .pte_response_valid(pte_response_valid), .pte_response_ready(pte_response_ready),
        .pte_response_data(pte_response_data), .pte_response_error(pte_response_error),
        .pte_allowed(pte_allowed), .final_allowed(final_allowed), .final_address(final_address),
        .flush_valid(flush_valid), .flush_all_va(flush_all_va), .flush_all_asid(flush_all_asid),
        .flush_va(flush_va), .flush_asid(flush_asid), .walk_busy(walk_busy), .walk_level(walk_level));
    always @(posedge clk) if (!reset) begin
        if (pte_valid && pte_ready) begin
            if (pending || pte_response_valid || pte_address >= 16384) $fatal(1, "bad PTE transaction");
            pending <= 1; pending_address <= pte_address; latency <= 2; reads <= reads + 1;
        end
        if (pending && !pte_response_valid && allow_response) begin
            if (latency != 0) latency <= latency - 1;
            else begin
                pte_response_data <= memory[pending_address >> 2];
                pte_response_error <= inject_error; pte_response_valid <= 1;
            end
        end
        if (pte_response_valid && pte_response_ready) begin
            pte_response_valid <= 0; pending <= 0; pte_response_error <= 0;
        end
    end
    task automatic flush(input logic all_va, input logic all_asid, input [31:0] addr, input [8:0] asid);
        begin
            @(negedge clk); flush_valid=1; flush_all_va=all_va; flush_all_asid=all_asid; flush_va=addr; flush_asid=asid;
            @(negedge clk); flush_valid=0;
        end
    endtask
    task automatic translate(input [31:0] address, input [31:0] root, input [1:0] kind, input [1:0] priv,
                             input logic use_sum, input logic use_mxr, input logic fault,
                             input [31:0] expected, input integer expected_reads);
        integer before_reads, timeout;
        begin
            while (!req_ready) @(negedge clk);
            before_reads=reads;
            va=address; satp=root; access=kind; privilege=priv; sum=use_sum; mxr=use_mxr; req_valid=1;
            @(negedge clk); req_valid=0; timeout=0;
            while (!result_valid && timeout < 100) begin @(negedge clk); timeout=timeout+1; end
            if (!result_valid || result_fault !== fault || (fault ? result_cause !== expected : result_pa !== expected) ||
                (fault && result_tval !== address) || reads-before_reads != expected_reads)
                $fatal(1, "translation va=%h fault=%b cause=%0d pa=%h reads=%0d expected=%h/%0d",
                    address, result_fault, result_cause, result_pa, reads-before_reads, expected, expected_reads);
            result_ready=1; @(negedge clk); result_ready=0;
        end
    endtask
    initial begin
        for (i=0; i<4096; i=i+1) memory[i]=0;
        memory['h1400 >> 2]=32'h00000801; memory['h200c >> 2]=32'h000014cf;
        repeat (2) @(negedge clk); reset=0;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 0, 32'h00005000, 2);
        translate(32'h40003004, 32'h80000001, 2, 1, 0, 0, 0, 32'h00005004, 0);
        // Svade: A/D faults are not silently written back.
        flush(1,1,0,0); memory['h200c >> 2]=32'h0000148f;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 1, 13, 2);
        if (memory['h200c >> 2] != 32'h0000148f) $fatal(1, "A bit changed");
        memory['h200c >> 2]=32'h0000144f;
        translate(32'h40003000, 32'h80000001, 2, 1, 0, 0, 1, 15, 2);
        // Supervisor SUM does not allow executing a user page.
        memory['h200c >> 2]=32'h000014df;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 1, 13, 2);
        translate(32'h40003000, 32'h80000001, 1, 1, 1, 0, 0, 32'h00005000, 2);
        translate(32'h40003000, 32'h80000001, 0, 1, 1, 0, 1, 12, 0);
        translate(32'h40003000, 32'h80000001, 1, 0, 0, 0, 0, 32'h00005000, 0);
        // Execute-only pages require MXR for data reads.
        flush(1,1,0,0); memory['h200c >> 2]=32'h000014c9;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 1, 13, 2);
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 1, 0, 32'h00005000, 2);
        // A 4 MiB superpage needs only the upper-level read.
        flush(1,1,0,0); memory['h1400 >> 2]=32'h001000cf;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 0, 32'h00403000, 1);
        // Addressed ASID flush must not discard inherited global mappings.
        flush(1,1,0,0); memory['h1400 >> 2]=32'h00000821; memory['h200c >> 2]=32'h000014cf;
        translate(32'h40003000, 32'h80400001, 1, 1, 0, 0, 0, 32'h00005000, 2);
        flush(1,0,0,1);
        translate(32'h40003000, 32'h80800001, 1, 1, 0, 0, 0, 32'h00005000, 0);
        flush(1,1,0,0);
        // PMP and bus errors are access faults, not page faults.
        pte_allowed=0;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 1, 5, 0);
        pte_allowed=1; inject_error=1;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 1, 5, 1);
        inject_error=0; final_allowed=0;
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 1, 5, 2);
        final_allowed=1;
        translate(32'h12345000, 0, 1, 1, 0, 0, 0, 32'h12345000, 0);
        // Cancel a submitted walk and drain the old response before accepting a new request.
        flush(1,1,0,0); allow_response=0;
        while (!req_ready) @(negedge clk);
        va=32'h40003000; satp=32'h80000001; access=1; privilege=1; req_valid=1;
        @(negedge clk); req_valid=0;
        while (!pending) @(negedge clk);
        flush(1,1,0,0);
        if (req_ready || !walk_busy) $fatal(1, "cancel failed to drain outstanding PTE");
        allow_response=1;
        while (!req_ready) @(negedge clk);
        if (result_valid) $fatal(1, "cancelled PTE produced a result");
        translate(32'h40003000, 32'h80000001, 1, 1, 0, 0, 0, 32'h00005000, 2);
        $display("PASS RTL Sv32: two-level walk, Svade, PMP inputs, TLB/ASID/global, superpage and cancellation");
        $finish;
    end
    initial begin #200000; $fatal(1, "Sv32 test timeout"); end
endmodule
