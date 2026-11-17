// Single-outstanding AXI4-Lite adapter for the five-stage pipeline. A tagged
// one-entry fetch response is held until the fetch PC advances; redirected
// speculative reads are discarded. Older data accesses take priority.
module colab_rv32im_pipeline_axi #(parameter TRANSLATION_ERRORS = 0) (
    input logic clk, reset,
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
    localparam [2:0] IDLE=0, I_AR=1, I_R=2, D_AR=3, D_R=4, D_SEND=5, D_B=6, D_DONE=7;
    logic [2:0] state;
    wire imem_request, imem_ready, dmem_valid, dmem_write, dmem_ready, md_busy;
    wire [31:0] imem_address, dmem_address, dmem_wdata;
    wire [3:0] dmem_wstrb;
    logic [31:0] if_address, if_data, request_address, request_wdata, data_result;
    logic [3:0] request_strobe;
    logic if_valid, if_error, if_page_fault, data_error, data_page_fault, aw_sent, w_sent;

    assign imem_ready=if_valid && if_address == imem_address;
    assign dmem_ready=state == D_DONE;
    colab_rv32im_pipeline cpu(.clk(clk), .reset(reset), .imem_request(imem_request),
        .imem_address(imem_address), .imem_ready(imem_ready), .imem_data(if_data),
        .imem_error(if_error), .imem_page_fault(if_page_fault),
        .dmem_valid(dmem_valid), .dmem_write(dmem_write), .dmem_address(dmem_address),
        .dmem_wdata(dmem_wdata), .dmem_wstrb(dmem_wstrb), .dmem_ready(dmem_ready),
        .dmem_error(data_error), .dmem_page_fault(data_page_fault), .dmem_rdata(data_result),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_instruction(retire_instruction),
        .retire_write(retire_write), .retire_rd(retire_rd), .retire_value(retire_value),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .debug_index(debug_index), .debug_value(debug_value), .debug_pc(debug_pc), .md_busy(md_busy));

    assign m_axi_arvalid=state == I_AR || state == D_AR;
    assign m_axi_araddr=request_address; assign m_axi_arprot=state == I_AR ? 3'b100 : 3'b000;
    assign m_axi_rready=state == I_R || state == D_R;
    assign m_axi_awvalid=state == D_SEND && !aw_sent; assign m_axi_awaddr=request_address;
    assign m_axi_awprot=0; assign m_axi_wvalid=state == D_SEND && !w_sent;
    assign m_axi_wdata=request_wdata; assign m_axi_wstrb=request_strobe;
    assign m_axi_bready=state == D_B;

    always_ff @(posedge clk) begin
        if (reset) begin
            state<=IDLE; if_address<=0; if_data<=0; if_valid<=0; if_error<=0; if_page_fault<=0;
            request_address<=0; request_wdata<=0; request_strobe<=0;
            data_result<=0; data_error<=0; data_page_fault<=0; aw_sent<=0; w_sent<=0;
        end else begin
            if (if_valid && (!imem_request || if_address != imem_address)) if_valid<=0;
            case (state)
            IDLE: begin
                if (dmem_valid) begin
                    request_address<={dmem_address[31:2],2'b00}; request_wdata<=dmem_wdata;
                    request_strobe<=dmem_wstrb; aw_sent<=0; w_sent<=0;
                    state<=dmem_write ? D_SEND : D_AR;
                end else if (imem_request && (!if_valid || if_address != imem_address)) begin
                    request_address<=imem_address; state<=I_AR;
                end
            end
            I_AR: if (m_axi_arready) state<=I_R;
            I_R: if (m_axi_rvalid) begin
                if_address<=request_address; if_data<=m_axi_rdata; if_error<=m_axi_rresp != 0;
                if_page_fault<=TRANSLATION_ERRORS && m_axi_rresp == 3;
                if_valid<=imem_request && imem_address == request_address;
                state<=IDLE;
            end
            D_AR: if (m_axi_arready) state<=D_R;
            D_R: if (m_axi_rvalid) begin
                data_result<=m_axi_rdata; data_error<=m_axi_rresp != 0;
                data_page_fault<=TRANSLATION_ERRORS && m_axi_rresp == 3; state<=D_DONE;
            end
            D_SEND: begin
                if (m_axi_awready && !aw_sent) aw_sent<=1;
                if (m_axi_wready && !w_sent) w_sent<=1;
                if ((aw_sent || m_axi_awready) && (w_sent || m_axi_wready)) state<=D_B;
            end
            D_B: if (m_axi_bvalid) begin
                data_error<=m_axi_bresp != 0; data_page_fault<=TRANSLATION_ERRORS && m_axi_bresp == 3; state<=D_DONE;
            end
            D_DONE: if (dmem_valid) state<=IDLE;
            default: state<=IDLE;
            endcase
        end
    end
endmodule
