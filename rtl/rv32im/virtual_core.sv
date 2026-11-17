// RV32IM core with external privilege/PMP control and an Sv32/Svade AXI
// adapter. This integration is useful for PTW experiments; the core itself
// does not yet implement the CSRs or privilege transitions that drive it.
module colab_rv32im_virtual_core #(parameter PIPELINED=0) (
    input logic clk, reset,
    input logic [31:0] satp, input logic [1:0] privilege, input logic sum, mxr,
    input logic pte_allowed, final_allowed, output logic [31:0] pte_address, final_address,
    input logic flush_valid, flush_all_va, flush_all_asid,
    input logic [31:0] flush_va, input logic [8:0] flush_asid,
    output logic walk_busy, walk_level, mmu_busy,
    output logic [31:0] m_axi_araddr, output logic [2:0] m_axi_arprot,
    output logic m_axi_arvalid, input logic m_axi_arready,
    input logic [31:0] m_axi_rdata, input logic [1:0] m_axi_rresp,
    input logic m_axi_rvalid, output logic m_axi_rready,
    output logic [31:0] m_axi_awaddr, output logic [2:0] m_axi_awprot,
    output logic m_axi_awvalid, input logic m_axi_awready,
    output logic [31:0] m_axi_wdata, output logic [3:0] m_axi_wstrb,
    output logic m_axi_wvalid, input logic m_axi_wready,
    input logic [1:0] m_axi_bresp, input logic m_axi_bvalid, output logic m_axi_bready,
    output logic retire_valid, output logic [31:0] retire_pc, retire_instruction,
    output logic retire_write, output logic [4:0] retire_rd, output logic [31:0] retire_value,
    output logic trap_valid, output logic [31:0] trap_pc, trap_cause, trap_value,
    input logic [4:0] debug_index, output logic [31:0] debug_value, debug_pc
);
    wire [31:0] va_araddr, va_rdata, va_awaddr, va_wdata;
    wire [2:0] va_arprot, va_awprot; wire [3:0] va_wstrb;
    wire va_arvalid, va_arready, va_rvalid, va_rready, va_awvalid, va_awready;
    wire va_wvalid, va_wready, va_bvalid, va_bready; wire [1:0] va_rresp, va_bresp;

    generate if (PIPELINED) begin: g_pipeline
    colab_rv32im_pipeline_axi #(.TRANSLATION_ERRORS(1)) cpu(.clk(clk), .reset(reset),
        .m_axi_araddr(va_araddr), .m_axi_arprot(va_arprot), .m_axi_arvalid(va_arvalid), .m_axi_arready(va_arready),
        .m_axi_rdata(va_rdata), .m_axi_rresp(va_rresp), .m_axi_rvalid(va_rvalid), .m_axi_rready(va_rready),
        .m_axi_awaddr(va_awaddr), .m_axi_awprot(va_awprot), .m_axi_awvalid(va_awvalid), .m_axi_awready(va_awready),
        .m_axi_wdata(va_wdata), .m_axi_wstrb(va_wstrb), .m_axi_wvalid(va_wvalid), .m_axi_wready(va_wready),
        .m_axi_bresp(va_bresp), .m_axi_bvalid(va_bvalid), .m_axi_bready(va_bready),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc));
    end else begin: g_multicycle
    colab_rv32im_core #(.TRANSLATION_ERRORS(1)) cpu(.clk(clk), .reset(reset),
        .m_axi_araddr(va_araddr), .m_axi_arprot(va_arprot), .m_axi_arvalid(va_arvalid), .m_axi_arready(va_arready),
        .m_axi_rdata(va_rdata), .m_axi_rresp(va_rresp), .m_axi_rvalid(va_rvalid), .m_axi_rready(va_rready),
        .m_axi_awaddr(va_awaddr), .m_axi_awprot(va_awprot), .m_axi_awvalid(va_awvalid), .m_axi_awready(va_awready),
        .m_axi_wdata(va_wdata), .m_axi_wstrb(va_wstrb), .m_axi_wvalid(va_wvalid), .m_axi_wready(va_wready),
        .m_axi_bresp(va_bresp), .m_axi_bvalid(va_bvalid), .m_axi_bready(va_bready),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc));
    end endgenerate

    colab_sv32_axi mmu(.clk(clk), .reset(reset), .satp(satp), .privilege(privilege), .sum(sum), .mxr(mxr),
        .pte_allowed(pte_allowed), .final_allowed(final_allowed), .pte_address(pte_address), .final_address(final_address),
        .flush_valid(flush_valid), .flush_all_va(flush_all_va), .flush_all_asid(flush_all_asid),
        .flush_va(flush_va), .flush_asid(flush_asid), .walk_busy(walk_busy), .walk_level(walk_level), .busy(mmu_busy),
        .s_araddr(va_araddr), .s_arprot(va_arprot), .s_arvalid(va_arvalid), .s_arready(va_arready),
        .s_rdata(va_rdata), .s_rresp(va_rresp), .s_rvalid(va_rvalid), .s_rready(va_rready),
        .s_awaddr(va_awaddr), .s_awprot(va_awprot), .s_awvalid(va_awvalid), .s_awready(va_awready),
        .s_wdata(va_wdata), .s_wstrb(va_wstrb), .s_wvalid(va_wvalid), .s_wready(va_wready),
        .s_bresp(va_bresp), .s_bvalid(va_bvalid), .s_bready(va_bready),
        .m_araddr(m_axi_araddr), .m_arprot(m_axi_arprot), .m_arvalid(m_axi_arvalid), .m_arready(m_axi_arready),
        .m_rdata(m_axi_rdata), .m_rresp(m_axi_rresp), .m_rvalid(m_axi_rvalid), .m_rready(m_axi_rready),
        .m_awaddr(m_axi_awaddr), .m_awprot(m_axi_awprot), .m_awvalid(m_axi_awvalid), .m_awready(m_axi_awready),
        .m_wdata(m_axi_wdata), .m_wstrb(m_axi_wstrb), .m_wvalid(m_axi_wvalid), .m_wready(m_axi_wready),
        .m_bresp(m_axi_bresp), .m_bvalid(m_axi_bvalid), .m_bready(m_axi_bready));
endmodule
