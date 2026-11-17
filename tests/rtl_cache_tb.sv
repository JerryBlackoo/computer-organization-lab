module rtl_cache_tb;
    logic clk=0, reset=1, invalidate_all=0, snoop_valid=0;
    logic [31:0] snoop_address=0, s_araddr=0, s_awaddr=0, s_wdata=0;
    logic [2:0] s_arprot=0, s_awprot=0;
    logic s_arvalid=0, s_rready=0, s_awvalid=0, s_wvalid=0, s_bready=0;
    logic [3:0] s_wstrb=0;
    wire s_arready, s_rvalid, s_awready, s_wready, s_bvalid;
    wire [31:0] s_rdata; wire [1:0] s_rresp, s_bresp;
    wire [31:0] m_araddr, m_awaddr, m_wdata;
    wire [2:0] m_arprot, m_awprot; wire [3:0] m_wstrb;
    wire m_arvalid, m_rready, m_awvalid, m_wvalid, m_bready;
    logic m_arready=1, m_awready=1, m_wready=1, m_rvalid=0, m_bvalid=0;
    logic [31:0] m_rdata=0; logic [1:0] m_rresp=0, m_bresp=0;
    logic [31:0] memory [0:127];
    logic pending_read=0, pending_aw=0, pending_w=0, allow_read=1;
    logic [31:0] read_address, write_address, write_data;
    logic [3:0] write_strobe;
    integer reads=0, writes=0, i, lane, before_reads;

    always #5 clk=~clk;
    colab_axi_cache #(.RAM_BYTES(256)) cache(.*);
    always @(posedge clk) if (!reset) begin
        if (m_arvalid && m_arready) begin
            if (pending_read || m_rvalid) $fatal(1, "multiple reads");
            pending_read <= 1; read_address <= m_araddr; reads <= reads+1;
        end
        if (pending_read && !m_rvalid && allow_read) begin
            m_rdata <= memory[read_address >> 2]; m_rresp <= read_address == 'hfc ? 2 : 0;
            m_rvalid <= 1;
        end
        if (m_rvalid && m_rready) begin m_rvalid <= 0; pending_read <= 0; end
        if (m_awvalid && m_awready) begin
            if (pending_aw) $fatal(1, "multiple AW");
            pending_aw <= 1; write_address <= m_awaddr;
        end
        if (m_wvalid && m_wready) begin
            if (pending_w) $fatal(1, "multiple W");
            pending_w <= 1; write_data <= m_wdata; write_strobe <= m_wstrb;
        end
        if (pending_aw && pending_w && !m_bvalid) begin
            m_bresp <= write_address == 'hf8 ? 2 : 0;
            if (write_address != 'hf8)
                for (lane=0; lane<4; lane=lane+1)
                    if (write_strobe[lane]) memory[write_address >> 2][8*lane +: 8] <= write_data[8*lane +: 8];
            m_bvalid <= 1; pending_aw <= 0; pending_w <= 0; writes <= writes+1;
        end
        if (m_bvalid && m_bready) m_bvalid <= 0;
    end

    task automatic read_word(input [31:0] address, input [31:0] expected, input [1:0] response,
                             input integer backend_reads);
        integer first;
        begin
            first=reads; @(negedge clk); s_araddr=address; s_arvalid=1;
            while (!s_arready) @(negedge clk);
            @(negedge clk); s_arvalid=0;
            while (!s_rvalid) @(negedge clk);
            if (s_rdata !== expected || s_rresp !== response || reads-first != backend_reads)
                $fatal(1,"read %h got %h/%d, backend %0d (want %h/%d, %0d)",
                    address, s_rdata, s_rresp, reads-first, expected, response, backend_reads);
            repeat (2) begin @(negedge clk); if (!s_rvalid || s_rdata !== expected) $fatal(1,"read response not held"); end
            s_rready=1; @(negedge clk); s_rready=0;
        end
    endtask
    task automatic write_word(input [31:0] address, input [31:0] value, input [3:0] strobe, input [1:0] response);
        begin
            @(negedge clk); s_awaddr=address; s_awvalid=1;
            while (!s_awready) @(negedge clk);
            @(negedge clk); s_awvalid=0;
            repeat (2) @(negedge clk); // W arrives independently of AW.
            s_wdata=value; s_wstrb=strobe; s_wvalid=1;
            while (!s_wready) @(negedge clk);
            @(negedge clk); s_wvalid=0;
            while (!s_bvalid) @(negedge clk);
            if (s_bresp !== response) $fatal(1,"write response %d",s_bresp);
            repeat (2) begin @(negedge clk); if (!s_bvalid) $fatal(1,"write response not held"); end
            s_bready=1; @(negedge clk); s_bready=0;
        end
    endtask
    task automatic write_word_data_first(input [31:0] address, input [31:0] value);
        begin
            @(negedge clk); s_wdata=value; s_wstrb=4'b1111; s_wvalid=1;
            while (!s_wready) @(negedge clk);
            @(negedge clk); s_wvalid=0;
            repeat (2) @(negedge clk);
            s_awaddr=address; s_awvalid=1;
            while (!s_awready) @(negedge clk);
            @(negedge clk); s_awvalid=0;
            while (!s_bvalid) @(negedge clk);
            if (s_bresp !== 0) $fatal(1,"data-first write failed");
            s_bready=1; @(negedge clk); s_bready=0;
        end
    endtask
    initial begin
        for (i=0; i<128; i=i+1) memory[i]=0;
        memory[0]=32'h12345678; memory['hfc >> 2]=32'h01020304; memory['hf8 >> 2]=32'hffffffff;
        memory['h100 >> 2]=32'h87654321;
        repeat (2) @(negedge clk); reset=0;
        read_word(0,32'h12345678,0,1); read_word(0,32'h12345678,0,0);
        write_word(0,32'h00ab0000,4'b0100,0); read_word(0,32'h12ab5678,0,0);
        if (memory[0] !== 32'h12ab5678) $fatal(1,"write-through failed");
        write_word(4,32'habcdef01,4'b1111,0); read_word(4,32'habcdef01,0,1); // no write allocate
        m_awready=0; m_wready=0;
        fork
            write_word_data_first(8,32'h55aa55aa);
            begin
                while (!m_awvalid || !m_wvalid) @(negedge clk);
                repeat (2) @(negedge clk);
                if (!m_awvalid || !m_wvalid || m_awaddr != 8 || m_wdata != 32'h55aa55aa)
                    $fatal(1,"write channels not held during backpressure");
                m_wready=1; @(negedge clk); m_wready=0;
                repeat (2) @(negedge clk);
                if (!m_awvalid || m_wvalid) $fatal(1,"write channels not independent");
                m_awready=1; @(negedge clk);
            end
        join
        m_wready=1; read_word(8,32'h55aa55aa,0,1);
        read_word('h100,32'h87654321,0,1); read_word('h100,32'h87654321,0,1); // MMIO bypass
        read_word('hfc,32'h01020304,2,1); read_word('hfc,32'h01020304,2,1); // fault is not cached
        read_word('hf8,32'hffffffff,0,1); write_word('hf8,0,4'b1111,2);
        read_word('hf8,32'hffffffff,0,1); // error invalidates an existing line
        read_word(0,32'h12ab5678,0,0);
        @(negedge clk); memory[0]=32'h0badcafe; snoop_address=0; snoop_valid=1;
        @(negedge clk); snoop_valid=0;
        read_word(0,32'h0badcafe,0,1);
        @(negedge clk); invalidate_all=1; @(negedge clk); invalidate_all=0;
        read_word(0,32'h0badcafe,0,1);
        // A snoop while a fill is in flight prevents stale fill allocation.
        @(negedge clk); invalidate_all=1; @(negedge clk); invalidate_all=0;
        allow_read=0; before_reads=reads;
        fork
            read_word(0,32'h76543210,0,1);
            begin
                while (!pending_read) @(negedge clk);
                memory[0]=32'h76543210; snoop_address=0; snoop_valid=1;
                @(negedge clk); snoop_valid=0; allow_read=1;
            end
        join
        if (reads-before_reads != 1) $fatal(1,"unexpected in-flight transaction count");
        read_word(0,32'h76543210,0,1); read_word(0,32'h76543210,0,0);
        $display("PASS RTL AXI cache: hits, bypass, write-through, backpressure, errors and snoop");
        $finish;
    end
    initial begin #200000; $fatal(1,"cache test timeout"); end
endmodule
