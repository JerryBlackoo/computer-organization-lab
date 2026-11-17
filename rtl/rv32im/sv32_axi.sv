// Single-outstanding virtual AXI-Lite to physical AXI-Lite adapter. Page
// table reads use the same physical bus. On this *internal* CPU-facing link,
// response 2'b11 denotes a page fault; downstream AXI DECERR is normalized
// to SLVERR so it remains an access fault. CPU must set TRANSLATION_ERRORS=1.
// satp/effective privilege/PMP decisions are supplied by the integrator.
module colab_sv32_axi (
    input logic clk, reset,
    input logic [31:0] satp, input logic [1:0] privilege, input logic sum, mxr,
    input logic pte_allowed, final_allowed, output logic [31:0] pte_address, final_address,
    input logic flush_valid, flush_all_va, flush_all_asid,
    input logic [31:0] flush_va, input logic [8:0] flush_asid,
    output logic walk_busy, walk_level, busy,
    input logic [31:0] s_araddr, input logic [2:0] s_arprot,
    input logic s_arvalid, output logic s_arready,
    output logic [31:0] s_rdata, output logic [1:0] s_rresp,
    output logic s_rvalid, input logic s_rready,
    input logic [31:0] s_awaddr, input logic [2:0] s_awprot,
    input logic s_awvalid, output logic s_awready,
    input logic [31:0] s_wdata, input logic [3:0] s_wstrb,
    input logic s_wvalid, output logic s_wready,
    output logic [1:0] s_bresp, output logic s_bvalid, input logic s_bready,
    output logic [31:0] m_araddr, output logic [2:0] m_arprot,
    output logic m_arvalid, input logic m_arready,
    input logic [31:0] m_rdata, input logic [1:0] m_rresp,
    input logic m_rvalid, output logic m_rready,
    output logic [31:0] m_awaddr, output logic [2:0] m_awprot,
    output logic m_awvalid, input logic m_awready,
    output logic [31:0] m_wdata, output logic [3:0] m_wstrb,
    output logic m_wvalid, input logic m_wready,
    input logic [1:0] m_bresp, input logic m_bvalid, output logic m_bready
);
    localparam [3:0] IDLE=0, X_START=1, X_WAIT=2, PTE_R=3, DATA_AR=4, DATA_R=5,
                     WRITE_SEND=6, WRITE_B=7, READ_REPLY=8, WRITE_REPLY=9, PTE_AR=10;
    logic [3:0] state;
    logic [31:0] va, pa, latched_satp, write_data, pte_araddr;
    logic [2:0] prot;
    logic [1:0] access, latched_privilege;
    logic latched_sum, latched_mxr, got_aw, got_w, aw_sent, w_sent, retry_walk;
    logic [3:0] write_strobe;
    wire x_ready, x_result_valid, x_fault, pte_valid, pte_ready, pte_response_ready;
    wire [31:0] x_pa, x_cause, x_tval;

    assign busy=state != IDLE || got_aw || got_w;
    assign s_awready=state == IDLE && !got_aw && !flush_valid;
    assign s_wready=state == IDLE && !got_w && !flush_valid;
    assign s_arready=state == IDLE && !got_aw && !got_w && !s_awvalid && !s_wvalid && !flush_valid;
    assign s_rvalid=state == READ_REPLY;
    assign s_bvalid=state == WRITE_REPLY;
    assign pte_ready=state == X_WAIT && !flush_valid;
    colab_sv32 walker(.clk(clk), .reset(reset), .req_valid(state == X_START), .req_ready(x_ready),
        .req_va(va), .req_satp(latched_satp), .req_access(access), .req_privilege(latched_privilege),
        .req_sum(latched_sum), .req_mxr(latched_mxr), .result_valid(x_result_valid),
        .result_ready(state == X_WAIT && !flush_valid), .result_pa(x_pa), .result_cause(x_cause),
        .result_tval(x_tval), .result_fault(x_fault), .pte_valid(pte_valid), .pte_ready(pte_ready),
        .pte_address(pte_address), .pte_response_valid(state == PTE_R && m_rvalid),
        .pte_response_ready(pte_response_ready), .pte_response_data(m_rdata), .pte_response_error(m_rresp != 0),
        .pte_allowed(pte_allowed), .final_allowed(final_allowed), .final_address(final_address),
        .flush_valid(flush_valid), .flush_all_va(flush_all_va), .flush_all_asid(flush_all_asid),
        .flush_va(flush_va), .flush_asid(flush_asid), .walk_busy(walk_busy), .walk_level(walk_level));
    assign m_arvalid=state == PTE_AR || state == DATA_AR;
    assign m_araddr=state == PTE_AR ? pte_araddr : pa;
    assign m_arprot=state == PTE_AR ? 3'b000 : prot;
    assign m_rready=state == PTE_R ? pte_response_ready : state == DATA_R;
    assign m_awvalid=state == WRITE_SEND && !aw_sent; assign m_awaddr=pa; assign m_awprot=prot;
    assign m_wvalid=state == WRITE_SEND && !w_sent; assign m_wdata=write_data; assign m_wstrb=write_strobe;
    assign m_bready=state == WRITE_B;

    always_ff @(posedge clk) begin
        if (reset) begin
            state<=IDLE; va<=0; pa<=0; pte_araddr<=0; latched_satp<=0; write_data<=0; prot<=0; access<=0;
            latched_privilege<=0; latched_sum<=0; latched_mxr<=0; write_strobe<=0;
            got_aw<=0; got_w<=0; aw_sent<=0; w_sent<=0; retry_walk<=0;
            s_rdata<=0; s_rresp<=0; s_bresp<=0;
        end else begin
            case (state)
            IDLE: begin
                if (s_awvalid && s_awready) begin va<=s_awaddr; prot<=s_awprot; got_aw<=1; end
                if (s_wvalid && s_wready) begin write_data<=s_wdata; write_strobe<=s_wstrb; got_w<=1; end
                if (got_aw && got_w && !flush_valid) begin
                    access<=2; latched_satp<=satp; latched_privilege<=privilege; latched_sum<=sum; latched_mxr<=mxr;
                    state<=X_START;
                end else if (s_arvalid && s_arready) begin
                    va<=s_araddr; prot<=s_arprot; access<=s_arprot[2] ? 0 : 1;
                    latched_satp<=satp; latched_privilege<=privilege; latched_sum<=sum; latched_mxr<=mxr;
                    state<=X_START;
                end
            end
            X_START: if (x_ready) state<=X_WAIT;
            X_WAIT: begin
                if (flush_valid) state<=X_START;
                else if (pte_valid) begin pte_araddr<=pte_address; retry_walk<=0; state<=PTE_AR; end
                else if (x_result_valid) begin
                    if (x_fault) begin
                        if (access == 2) begin s_bresp<=x_cause == 15 ? 3 : 2; got_aw<=0; got_w<=0; state<=WRITE_REPLY; end
                        else begin s_rdata<=0; s_rresp<=x_cause == 12 || x_cause == 13 ? 3 : 2; state<=READ_REPLY; end
                    end else begin
                        pa<=x_pa; aw_sent<=0; w_sent<=0;
                        state<=access == 2 ? WRITE_SEND : DATA_AR;
                    end
                end
            end
            PTE_AR: begin
                if (flush_valid) retry_walk<=1;
                if (m_arready) state<=PTE_R;
            end
            PTE_R: begin
                if (flush_valid) retry_walk<=1;
                if (m_rvalid && m_rready) begin
                    state<=retry_walk || flush_valid ? X_START : X_WAIT; retry_walk<=0;
                end
            end
            DATA_AR: if (m_arready) state<=DATA_R;
            DATA_R: if (m_rvalid) begin
                s_rdata<=m_rdata; s_rresp<=m_rresp == 0 ? 0 : 2; state<=READ_REPLY;
            end
            WRITE_SEND: begin
                if (m_awready && !aw_sent) aw_sent<=1;
                if (m_wready && !w_sent) w_sent<=1;
                if ((aw_sent || m_awready) && (w_sent || m_wready)) state<=WRITE_B;
            end
            WRITE_B: if (m_bvalid) begin
                s_bresp<=m_bresp == 0 ? 0 : 2; got_aw<=0; got_w<=0; state<=WRITE_REPLY;
            end
            READ_REPLY: if (s_rready) state<=IDLE;
            WRITE_REPLY: if (s_bready) state<=IDLE;
            default: state<=IDLE;
            endcase
        end
    end
endmodule
