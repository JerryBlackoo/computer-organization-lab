// Binary, horizontal microcoded teaching datapath. The ROM format is shared
// with the C++ RV32 control store; this RTL currently implements ADD, ADDI,
// SUB, LW, SW and BEQ only. Other instructions halt with illegal=1.
// Memory inputs are zero-wait, combinational read ports; write commits on clk.
module colab_rv32_microcoded #(
    parameter ROM_FILE = "rtl/microcoded/rv32_control.hex",
    parameter VERTICAL = 0
) (
    input wire clk, reset, enable,
    output wire [31:0] instr_address,
    output wire instr_read,
    input wire [31:0] instr_data,
    output wire [31:0] data_address,
    output wire data_read,
    input wire [31:0] data_read_data,
    output wire data_write,
    output wire [31:0] data_write_data,
    input wire [4:0] debug_reg,
    output wire [31:0] debug_value,
    output reg [31:0] pc, ir,
    output reg [7:0] upc,
    output reg [63:0] uir,
    output reg fault,
    output wire retire
);
    // One extra validity bit identifies holes in the 256 x 64 control store;
    // it is a simulation/debug protection bit, not part of the microinstruction.
    reg [64:0] rom [0:255];
    initial $readmemh(ROM_FILE, rom);

    reg [31:0] regs [0:31];
    reg [31:0] mar, mdr, a, b, alu_out;
    reg [31:0] pending_instruction, pending_data;
    reg instruction_pending, data_pending, equal_flag;
    wire [6:0] opcode = ir[6:0];
    wire [2:0] funct3 = ir[14:12];
    wire [6:0] funct7 = ir[31:25];
    wire [4:0] rs1 = ir[19:15], rs2 = ir[24:20], rd = ir[11:7];
    wire [31:0] imm_i = {{20{ir[31]}}, ir[31:20]};
    wire [31:0] imm_s = {{20{ir[31]}}, ir[31:25], ir[11:7]};
    wire [31:0] imm_b = {{19{ir[31]}}, ir[31], ir[7], ir[30:25], ir[11:8], 1'b0};

    // Decode the actual binary IR: dispatch is not a host-side operation enum.
    reg [7:0] entry;
    reg illegal;
    always @* begin
        entry = 0; illegal = 1;
        case (opcode)
          7'h33: if (funct3 == 0 && funct7 == 0) begin entry = 16; illegal = 0; end
                 else if (funct3 == 0 && funct7 == 7'h20) begin entry = 24; illegal = 0; end
          7'h13: if (funct3 == 0) begin entry = 20; illegal = 0; end
          7'h03: if (funct3 == 2) begin entry = 28; illegal = 0; end
          7'h23: if (funct3 == 2) begin entry = 36; illegal = 0; end
          7'h63: if (funct3 == 0) begin entry = 40; illegal = 0; end
          default: begin end
        endcase
    end

    // Vertical μOP expands to control enables; parameter and sequencing
    // fields are mapped into the shared datapath control wiring.
    reg [17:0] vertical_enables;
    always @* begin
        case (uir[4:0])
          0: vertical_enables=18'h00000; 1: vertical_enables=18'h00001;
          2: vertical_enables=18'h00002; 3: vertical_enables=18'h00006;
          4: vertical_enables=18'h00008; 5: vertical_enables=18'h00010;
          6: vertical_enables=18'h00020; 7: vertical_enables=18'h00040;
          8: vertical_enables=18'h00080; 9: vertical_enables=18'h00100;
          10: vertical_enables=18'h00300; 11: vertical_enables=18'h00400;
          12: vertical_enables=18'h00800; 13: vertical_enables=18'h01800;
          14: vertical_enables=18'h18000; 15: vertical_enables=18'h1a000;
          16: vertical_enables=18'h1c000; 17: vertical_enables=18'h38000;
          default: vertical_enables=0;
        endcase
    end
    wire [63:0] control_word = VERTICAL ? {10'b0, uir[40:22], uir[21:5], vertical_enables} : uir;
    wire vertical_invalid = VERTICAL && ((uir[63:41] != 0) || uir[4:0] > 17);

    // Fixed 64-bit horizontal format: enables 0..17, selectors 18..34,
    // sequencer 35..37, true/next address 38..45, alternate 46..53.
    wire [2:0] seq = control_word[37:35];
    wire [7:0] target = control_word[45:38], alternate = control_word[53:46];
    reg [7:0] next_upc;
    reg next_invalid;
    always @* begin
        next_upc = 0; next_invalid = vertical_invalid;
        case (seq)
          0: begin next_upc = upc + 8'd1; if (upc == 8'hff) next_invalid = 1; end
          1: next_upc = target;
          2: begin next_upc = entry; if (illegal) next_invalid = 1; end
          3: next_upc = equal_flag ? target : alternate;
          // RV32M is deliberately outside this educational datapath's scope.
          default: next_invalid = 1;
        endcase
        if (!rom[next_upc][64]) next_invalid = 1;
    end
    wire active = enable && !reset && !fault && !next_invalid;
    assign instr_address = mar;
    assign instr_read = active && control_word[1];
    assign data_address = mar;
    assign data_read = active && control_word[11];
    assign data_write = active && control_word[13];
    assign data_write_data = b;
    assign debug_value = debug_reg == 0 ? 0 : regs[debug_reg];
    assign retire = active && control_word[16];

    wire [31:0] src_a = rs1 == 0 ? 0 : regs[rs1];
    wire [31:0] src_b = rs2 == 0 ? 0 : regs[rs2];
    wire [31:0] immediate = opcode == 7'h23 ? imm_s : imm_i;
    wire [31:0] alu_b = control_word[26] ? immediate : b;
    wire [4:0] alu_op = control_word[25:21];
    wire [31:0] alu_result = alu_op == 5'd3 ? a - alu_b : a + alu_b;
    wire [2:0] wb_select = control_word[29:27];
    wire [31:0] wb_value = wb_select == 1 ? mdr : alu_out;
    wire [1:0] pc_select = control_word[31:30];

    integer i;
    always @(posedge clk) begin
        if (reset) begin
            pc <= 0; ir <= 0; mar <= 0; mdr <= 0; a <= 0; b <= 0; alu_out <= 0;
            pending_instruction <= 0; pending_data <= 0;
            instruction_pending <= 0; data_pending <= 0; equal_flag <= 0;
            upc <= 0; uir <= rom[0][63:0]; fault <= !rom[0][64];
            for (i = 0; i < 32; i = i + 1) regs[i] <= 0;
        end else if (enable && !fault) begin
            if (next_invalid) fault <= 1;
            else begin
                if (control_word[0]) mar <= pc;
                if (control_word[1] && !instruction_pending) begin
                    pending_instruction <= instr_data; instruction_pending <= 1;
                end
                if (control_word[2]) begin mdr <= pending_instruction; instruction_pending <= 0; end
                if (control_word[3]) ir <= mdr;
                if (control_word[5]) begin a <= src_a; b <= src_b; end
                if (control_word[8]) alu_out <= alu_result;
                if (control_word[9]) mar <= alu_result;
                if (control_word[10]) equal_flag <= a == b;
                if (control_word[11] && !data_pending) begin
                    pending_data <= data_read_data; data_pending <= 1;
                end
                if (control_word[12]) begin mdr <= pending_data; data_pending <= 0; end
                if (control_word[14] && rd != 0) regs[rd] <= wb_value;
                if (control_word[15]) pc <= pc_select == 1 ? pc + imm_b : pc + 4;
                upc <= next_upc; uir <= rom[next_upc][63:0];
            end
        end
    end
endmodule
