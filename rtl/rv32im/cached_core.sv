// Multicycle RV32IM and a blocking unified write-through AXI-Lite cache.
// The integrator must supply external-write invalidations and invalidate_all
// when needed (for example, a system-wide instruction-cache synchronization).
module colab_rv32im_cached_core #(
    parameter [31:0] RAM_BYTES = 32'h00010000,
    parameter PIPELINED = 0
) (
    input logic clk, reset, invalidate_all,
    input logic snoop_valid, input logic [31:0] snoop_address,
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
    wire [31:0] core_araddr, core_rdata, core_awaddr, core_wdata;
    wire [2:0] core_arprot, core_awprot; wire [3:0] core_wstrb;
    wire core_arvalid, core_arready, core_rvalid, core_rready, core_awvalid, core_awready;
    wire core_wvalid, core_wready, core_bvalid, core_bready; wire [1:0] core_rresp, core_bresp;

    generate if (PIPELINED) begin: g_pipeline
    colab_rv32im_pipeline_axi cpu(.clk(clk), .reset(reset),
        .m_axi_araddr(core_araddr), .m_axi_arprot(core_arprot), .m_axi_arvalid(core_arvalid), .m_axi_arready(core_arready),
        .m_axi_rdata(core_rdata), .m_axi_rresp(core_rresp), .m_axi_rvalid(core_rvalid), .m_axi_rready(core_rready),
        .m_axi_awaddr(core_awaddr), .m_axi_awprot(core_awprot), .m_axi_awvalid(core_awvalid), .m_axi_awready(core_awready),
        .m_axi_wdata(core_wdata), .m_axi_wstrb(core_wstrb), .m_axi_wvalid(core_wvalid), .m_axi_wready(core_wready),
        .m_axi_bresp(core_bresp), .m_axi_bvalid(core_bvalid), .m_axi_bready(core_bready),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc));
    end else begin: g_multicycle
    colab_rv32im_core cpu(.clk(clk), .reset(reset),
        .m_axi_araddr(core_araddr), .m_axi_arprot(core_arprot), .m_axi_arvalid(core_arvalid), .m_axi_arready(core_arready),
        .m_axi_rdata(core_rdata), .m_axi_rresp(core_rresp), .m_axi_rvalid(core_rvalid), .m_axi_rready(core_rready),
        .m_axi_awaddr(core_awaddr), .m_axi_awprot(core_awprot), .m_axi_awvalid(core_awvalid), .m_axi_awready(core_awready),
        .m_axi_wdata(core_wdata), .m_axi_wstrb(core_wstrb), .m_axi_wvalid(core_wvalid), .m_axi_wready(core_wready),
        .m_axi_bresp(core_bresp), .m_axi_bvalid(core_bvalid), .m_axi_bready(core_bready),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc));
    end endgenerate

    colab_axi_cache #(.RAM_BYTES(RAM_BYTES)) cache(.clk(clk), .reset(reset), .invalidate_all(invalidate_all),
        .snoop_valid(snoop_valid), .snoop_address(snoop_address),
        .s_araddr(core_araddr), .s_arprot(core_arprot), .s_arvalid(core_arvalid), .s_arready(core_arready),
        .s_rdata(core_rdata), .s_rresp(core_rresp), .s_rvalid(core_rvalid), .s_rready(core_rready),
        .s_awaddr(core_awaddr), .s_awprot(core_awprot), .s_awvalid(core_awvalid), .s_awready(core_awready),
        .s_wdata(core_wdata), .s_wstrb(core_wstrb), .s_wvalid(core_wvalid), .s_wready(core_wready),
        .s_bresp(core_bresp), .s_bvalid(core_bvalid), .s_bready(core_bready),
        .m_araddr(m_axi_araddr), .m_arprot(m_axi_arprot), .m_arvalid(m_axi_arvalid), .m_arready(m_axi_arready),
        .m_rdata(m_axi_rdata), .m_rresp(m_axi_rresp), .m_rvalid(m_axi_rvalid), .m_rready(m_axi_rready),
        .m_awaddr(m_axi_awaddr), .m_awprot(m_axi_awprot), .m_awvalid(m_axi_awvalid), .m_awready(m_axi_awready),
        .m_wdata(m_axi_wdata), .m_wstrb(m_axi_wstrb), .m_wvalid(m_axi_wvalid), .m_wready(m_axi_wready),
        .m_bresp(m_axi_bresp), .m_bvalid(m_axi_bvalid), .m_bready(m_axi_bready));
endmodule
