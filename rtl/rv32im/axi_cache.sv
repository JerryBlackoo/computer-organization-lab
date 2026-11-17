// Blocking, unified, direct-mapped, write-through AXI4-Lite cache.
// One aligned 32-bit word per line; one transaction may be in flight. RAM is
// [0, RAM_BYTES); addresses outside it are forwarded without allocation.
// AW and W are accepted independently. External writes must invalidate the
// affected word through snoop_valid. This is not a multicore coherence protocol.
module colab_axi_cache #(
    parameter [31:0] RAM_BYTES = 32'h00010000
) (
    input logic clk, reset, invalidate_all,
    input logic snoop_valid, input logic [31:0] snoop_address,
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
    localparam [2:0] IDLE=0, READ_ADDR=1, READ_DATA=2, READ_REPLY=3,
                     WRITE_SEND=4, WRITE_RESP=5, WRITE_REPLY=6;
    logic [2:0] state;
    logic [15:0] valid;
    logic [25:0] tags [0:15];
    logic [31:0] data [0:15];
    logic [31:0] read_address, write_address, write_data;
    logic [2:0] read_prot, write_prot;
    logic [3:0] write_strobe;
    logic got_aw, got_w, sent_aw, sent_w, invalidate_read;
    wire [3:0] read_index = s_araddr[5:2];
    wire [3:0] pending_read_index = read_address[5:2];
    wire [3:0] write_index = write_address[5:2];
    wire [3:0] snoop_index = snoop_address[5:2];
    wire read_hit = s_araddr < RAM_BYTES && valid[read_index] && tags[read_index] == s_araddr[31:6];
    wire write_hit = write_address < RAM_BYTES && valid[write_index] && tags[write_index] == write_address[31:6];
    wire snoop_read = snoop_valid && snoop_address[31:2] == read_address[31:2];
    wire snoop_write = snoop_valid && snoop_address[31:2] == write_address[31:2];

    // Give write channels priority. Once either channel is accepted, keep the
    // other open and do not admit a read until the write response is consumed.
    assign s_awready = state == IDLE && !got_aw && !invalidate_all;
    assign s_wready = state == IDLE && !got_w && !invalidate_all;
    assign s_arready = state == IDLE && !got_aw && !got_w && !s_awvalid && !s_wvalid &&
                       !snoop_valid && !invalidate_all;
    assign s_rvalid = state == READ_REPLY;
    assign s_bvalid = state == WRITE_REPLY;
    assign m_arvalid = state == READ_ADDR;
    assign m_araddr = read_address; assign m_arprot = read_prot;
    assign m_rready = state == READ_DATA;
    assign m_awvalid = state == WRITE_SEND && !sent_aw;
    assign m_awaddr = write_address; assign m_awprot = write_prot;
    assign m_wvalid = state == WRITE_SEND && !sent_w;
    assign m_wdata = write_data; assign m_wstrb = write_strobe;
    assign m_bready = state == WRITE_RESP;

    integer i, lane;
    always_ff @(posedge clk) begin
        if (reset) begin
            state <= IDLE; valid <= 0; got_aw <= 0; got_w <= 0; sent_aw <= 0; sent_w <= 0;
            read_address <= 0; read_prot <= 0; write_address <= 0; write_prot <= 0;
            write_data <= 0; write_strobe <= 0; invalidate_read <= 0;
            s_rdata <= 0; s_rresp <= 0; s_bresp <= 0;
            for (i=0; i<16; i=i+1) begin tags[i] <= 0; data[i] <= 0; end
        end else begin
            if (invalidate_all) valid <= 0;
            else if (snoop_valid && valid[snoop_index] && tags[snoop_index] == snoop_address[31:6])
                valid[snoop_index] <= 0;
            if ((state == READ_ADDR || state == READ_DATA) && (invalidate_all || snoop_read))
                invalidate_read <= 1;
            case (state)
            IDLE: begin
                if (s_awvalid && s_awready) begin write_address <= s_awaddr; write_prot <= s_awprot; got_aw <= 1; end
                if (s_wvalid && s_wready) begin write_data <= s_wdata; write_strobe <= s_wstrb; got_w <= 1; end
                if (got_aw && got_w) begin sent_aw <= 0; sent_w <= 0; state <= WRITE_SEND; end
                else if (s_arvalid && s_arready) begin
                    read_address <= s_araddr; read_prot <= s_arprot; invalidate_read <= 0;
                    if (read_hit) begin s_rdata <= data[read_index]; s_rresp <= 0; state <= READ_REPLY; end
                    else state <= READ_ADDR;
                end
            end
            READ_ADDR: if (m_arready) state <= READ_DATA;
            READ_DATA: if (m_rvalid) begin
                s_rdata <= m_rdata; s_rresp <= m_rresp;
                if (m_rresp == 0 && read_address < RAM_BYTES && !invalidate_read && !invalidate_all && !snoop_read) begin
                    tags[pending_read_index] <= read_address[31:6]; data[pending_read_index] <= m_rdata;
                    valid[pending_read_index] <= 1;
                end
                state <= READ_REPLY;
            end
            READ_REPLY: if (s_rready) state <= IDLE;
            WRITE_SEND: begin
                if (m_awready && !sent_aw) sent_aw <= 1;
                if (m_wready && !sent_w) sent_w <= 1;
                if ((sent_aw || m_awready) && (sent_w || m_wready)) state <= WRITE_RESP;
            end
            WRITE_RESP: if (m_bvalid) begin
                s_bresp <= m_bresp;
                if (write_hit && !invalidate_all && !snoop_write) begin
                    if (m_bresp == 0) begin
                        for (lane=0; lane<4; lane=lane+1)
                            if (write_strobe[lane]) data[write_index][8*lane +: 8] <= write_data[8*lane +: 8];
                    end else valid[write_index] <= 0;
                end
                got_aw <= 0; got_w <= 0; state <= WRITE_REPLY;
            end
            WRITE_REPLY: if (s_bready) state <= IDLE;
            default: state <= IDLE;
            endcase
        end
    end
endmodule
