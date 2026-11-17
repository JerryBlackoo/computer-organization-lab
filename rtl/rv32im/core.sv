// In-order RV32IM multicycle core with one outstanding AXI4-Lite transfer.
// Trap output is a halt/debug interface; privileged CSRs/interrupts are not yet implemented.
module colab_rv32im_core #(parameter TRANSLATION_ERRORS = 0) (
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
    localparam logic [3:0] FETCH_AR=0, FETCH_R=1, EXEC=2, M_START=3, M_WAIT=4,
        LOAD_AR=5, LOAD_R=6, STORE_SEND=7, STORE_B=8, HALT=9;
    logic [3:0] state;
    logic [31:0] regs [0:31];
    logic [31:0] pc, instruction, address, store_data, md_a, md_b;
    logic [3:0] store_strobe;
    logic [4:0] destination;
    logic [2:0] memory_kind, md_kind;
    logic aw_sent, w_sent;
    wire [6:0] opcode = instruction[6:0], funct7 = instruction[31:25];
    wire [2:0] funct3 = instruction[14:12];
    wire [4:0] rd = instruction[11:7], rs1 = instruction[19:15], rs2 = instruction[24:20];
    wire [31:0] a = rs1 == 0 ? 0 : regs[rs1];
    wire [31:0] b = rs2 == 0 ? 0 : regs[rs2];
    wire [31:0] imm_i = {{20{instruction[31]}}, instruction[31:20]};
    wire [31:0] imm_s = {{20{instruction[31]}}, instruction[31:25], instruction[11:7]};
    wire [31:0] imm_b = {{19{instruction[31]}}, instruction[31], instruction[7], instruction[30:25], instruction[11:8], 1'b0};
    wire [31:0] imm_u = {instruction[31:12], 12'b0};
    wire [31:0] imm_j = {{11{instruction[31]}}, instruction[31], instruction[19:12], instruction[20], instruction[30:21], 1'b0};

    wire md_busy, md_done, md_alu_valid, md_alu_sub, alu_carry;
    wire [31:0] md_result, md_q, md_alu_a, md_alu_b, alu_result;
    wire [32:0] md_partial;
    wire [5:0] md_count;
    wire core_alu_sub = opcode == 7'h33 && funct3 == 0 && funct7 == 7'h20;
    wire [31:0] core_alu_b = opcode == 7'h13 ? imm_i : b;
    colab_alu32 alu(.subtract(md_busy && md_alu_valid ? md_alu_sub : core_alu_sub),
        .a(md_busy && md_alu_valid ? md_alu_a : a), .b(md_busy && md_alu_valid ? md_alu_b : core_alu_b),
        .result(alu_result), .carry_or_borrow(alu_carry));
    colab_muldiv mdu(.clk(clk), .reset(reset), .start(state == M_START), .funct3(md_kind),
        .operand_a(md_a), .operand_b(md_b), .busy(md_busy), .done(md_done), .result(md_result),
        .partial(md_partial), .q(md_q), .count(md_count), .alu_valid(md_alu_valid),
        .alu_subtract(md_alu_sub), .alu_a(md_alu_a), .alu_b(md_alu_b),
        .alu_result(alu_result), .alu_carry_or_borrow(alu_carry));

    assign m_axi_arvalid = (state == FETCH_AR && pc[1:0] == 0) || state == LOAD_AR;
    assign m_axi_araddr = state == FETCH_AR ? pc : {address[31:2], 2'b00};
    assign m_axi_arprot = state == FETCH_AR ? 3'b100 : 3'b000;
    assign m_axi_rready = state == FETCH_R || state == LOAD_R;
    assign m_axi_awvalid = state == STORE_SEND && !aw_sent;
    assign m_axi_awaddr = {address[31:2], 2'b00};
    assign m_axi_awprot = 0;
    assign m_axi_wvalid = state == STORE_SEND && !w_sent;
    assign m_axi_wdata = store_data;
    assign m_axi_wstrb = store_strobe;
    assign m_axi_bready = state == STORE_B;
    assign debug_pc = pc;
    assign debug_value = debug_index == 0 ? 0 : regs[debug_index];

    task automatic finish_instruction(input logic [31:0] next_pc, input logic write_reg,
                                      input logic [4:0] target, input logic [31:0] value);
        begin
            if (write_reg && target != 0) regs[target] <= value;
            retire_valid <= 1; retire_pc <= pc; retire_instruction <= instruction;
            retire_write <= write_reg && target != 0; retire_rd <= target; retire_value <= value;
            pc <= next_pc; state <= FETCH_AR;
        end
    endtask
    task automatic fault(input logic [31:0] cause, input logic [31:0] value);
        begin
            trap_valid <= 1; trap_pc <= pc; trap_cause <= cause; trap_value <= value; state <= HALT;
        end
    endtask

    integer i;
    logic [31:0] effective, target_pc, loaded, calc;
    logic condition, legal;
    always_ff @(posedge clk) begin
        if (reset) begin
            state <= FETCH_AR; pc <= 0; instruction <= 0; address <= 0;
            store_data <= 0; store_strobe <= 0; destination <= 0; memory_kind <= 0;
            md_a <= 0; md_b <= 0; md_kind <= 0; aw_sent <= 0; w_sent <= 0;
            retire_valid <= 0; retire_pc <= 0; retire_instruction <= 0;
            retire_write <= 0; retire_rd <= 0; retire_value <= 0;
            trap_valid <= 0; trap_pc <= 0; trap_cause <= 0; trap_value <= 0;
            for (i=0; i<32; i=i+1) regs[i] <= 0;
        end else begin
            retire_valid <= 0;
            case (state)
            FETCH_AR: if (pc[1:0] != 0) fault(0, pc); else if (m_axi_arready) state <= FETCH_R;
            FETCH_R: if (m_axi_rvalid) begin
                if (m_axi_rresp != 0) fault(TRANSLATION_ERRORS && m_axi_rresp == 3 ? 12 : 1, pc);
                else begin instruction <= m_axi_rdata; state <= EXEC; end
            end
            EXEC: begin
                case (opcode)
                7'h37: finish_instruction(pc+4, 1, rd, imm_u);
                7'h17: finish_instruction(pc+4, 1, rd, pc+imm_u);
                7'h6f: begin
                    target_pc = pc + imm_j;
                    if (target_pc[1:0] != 0) fault(0, target_pc);
                    else finish_instruction(target_pc, 1, rd, pc+4);
                end
                7'h67: begin
                    target_pc = (a + imm_i) & 32'hfffffffe;
                    if (funct3 != 0) fault(2, instruction);
                    else if (target_pc[1:0] != 0) fault(0, target_pc);
                    else finish_instruction(target_pc, 1, rd, pc+4);
                end
                7'h63: begin
                    condition = 0; legal = 1;
                    case (funct3)
                    0: condition = a == b; 1: condition = a != b;
                    4: condition = $signed(a) < $signed(b); 5: condition = $signed(a) >= $signed(b);
                    6: condition = a < b; 7: condition = a >= b;
                    default: legal = 0;
                    endcase
                    target_pc = condition ? pc+imm_b : pc+4;
                    if (!legal) fault(2, instruction);
                    else if (target_pc[1:0] != 0) fault(0, target_pc);
                    else finish_instruction(target_pc, 0, 0, 0);
                end
                7'h03: begin
                    effective = a + imm_i;
                    if (!(funct3 == 0 || funct3 == 1 || funct3 == 2 || funct3 == 4 || funct3 == 5)) fault(2, instruction);
                    else if ((funct3 == 1 || funct3 == 5) && effective[0]) fault(4, effective);
                    else if (funct3 == 2 && effective[1:0] != 0) fault(4, effective);
                    else begin address <= effective; destination <= rd; memory_kind <= funct3; state <= LOAD_AR; end
                end
                7'h23: begin
                    effective = a + imm_s;
                    if (funct3 > 2) fault(2, instruction);
                    else if (funct3 == 1 && effective[0]) fault(6, effective);
                    else if (funct3 == 2 && effective[1:0] != 0) fault(6, effective);
                    else begin
                        address <= effective; store_data <= b << (8 * effective[1:0]);
                        case (funct3)
                        0: store_strobe <= 4'b0001 << effective[1:0];
                        1: store_strobe <= 4'b0011 << effective[1:0];
                        default: store_strobe <= 4'b1111;
                        endcase
                        aw_sent <= 0; w_sent <= 0; state <= STORE_SEND;
                    end
                end
                7'h13: begin
                    calc = 0; legal = 1;
                    case (funct3)
                    0: calc = alu_result;
                    1: if (funct7 == 0) calc = a << instruction[24:20]; else legal = 0;
                    2: calc = $signed(a) < $signed(imm_i);
                    3: calc = a < imm_i;
                    4: calc = a ^ imm_i;
                    5: if (funct7 == 0) calc = a >> instruction[24:20];
                       else if (funct7 == 7'h20) calc = $signed(a) >>> instruction[24:20]; else legal = 0;
                    6: calc = a | imm_i; 7: calc = a & imm_i;
                    endcase
                    if (legal) finish_instruction(pc+4, 1, rd, calc); else fault(2, instruction);
                end
                7'h33: begin
                    if (funct7 == 1) begin
                        md_a <= a; md_b <= b; md_kind <= funct3; destination <= rd; state <= M_START;
                    end else begin
                        calc = 0; legal = funct7 == 0 || (funct7 == 7'h20 && (funct3 == 0 || funct3 == 5));
                        case (funct3)
                        0: calc = alu_result; 1: calc = a << b[4:0];
                        2: calc = $signed(a) < $signed(b); 3: calc = a < b; 4: calc = a ^ b;
                        5: if (funct7 == 7'h20) calc = $signed(a) >>> b[4:0]; else calc = a >> b[4:0];
                        6: calc = a | b; 7: calc = a & b;
                        endcase
                        if (legal) finish_instruction(pc+4, 1, rd, calc); else fault(2, instruction);
                    end
                end
                7'h0f: if (funct3 == 0) finish_instruction(pc+4, 0, 0, 0); else fault(2, instruction);
                7'h73: if (instruction == 32'h00000073) fault(11, 0);
                       else if (instruction == 32'h00100073) fault(3, 0); else fault(2, instruction);
                default: fault(2, instruction);
                endcase
            end
            M_START: state <= M_WAIT;
            M_WAIT: if (md_done) finish_instruction(pc+4, 1, destination, md_result);
            LOAD_AR: if (m_axi_arready) state <= LOAD_R;
            LOAD_R: if (m_axi_rvalid) begin
                if (m_axi_rresp != 0) fault(TRANSLATION_ERRORS && m_axi_rresp == 3 ? 13 : 5, address);
                else begin
                    loaded = m_axi_rdata >> (8 * address[1:0]);
                    case (memory_kind)
                    0: calc = {{24{loaded[7]}}, loaded[7:0]};
                    1: calc = {{16{loaded[15]}}, loaded[15:0]};
                    2: calc = loaded;
                    4: calc = {24'b0, loaded[7:0]};
                    default: calc = {16'b0, loaded[15:0]};
                    endcase
                    finish_instruction(pc+4, 1, destination, calc);
                end
            end
            STORE_SEND: begin
                if (m_axi_awready && !aw_sent) aw_sent <= 1;
                if (m_axi_wready && !w_sent) w_sent <= 1;
                if ((aw_sent || m_axi_awready) && (w_sent || m_axi_wready)) state <= STORE_B;
            end
            STORE_B: if (m_axi_bvalid) begin
                if (m_axi_bresp != 0) fault(TRANSLATION_ERRORS && m_axi_bresp == 3 ? 15 : 7, address);
                else finish_instruction(pc+4, 0, 0, 0);
            end
            default: state <= HALT;
            endcase
        end
    end
endmodule
