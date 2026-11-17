// Five-stage in-order RV32IM teaching pipeline with separate SRAM-style
// instruction and data ports. RAW hazards interlock until writeback; EX owns
// the shared ALU during a multi-cycle M operation. Memory may backpressure.
// Faults stop at retirement; privileged CSR, IRQ and caches are not included.
module colab_rv32im_pipeline (
    input logic clk, reset,
    output logic imem_request, output logic [31:0] imem_address,
    input logic imem_ready, input logic [31:0] imem_data, input logic imem_error, imem_page_fault,
    output logic dmem_valid, dmem_write, output logic [31:0] dmem_address, dmem_wdata,
    output logic [3:0] dmem_wstrb,
    input logic dmem_ready, dmem_error, dmem_page_fault, input logic [31:0] dmem_rdata,
    output logic retire_valid, output logic [31:0] retire_pc, retire_instruction,
    output logic retire_write, output logic [4:0] retire_rd, output logic [31:0] retire_value,
    output logic trap_valid, output logic [31:0] trap_pc, trap_cause, trap_value,
    input logic [4:0] debug_index, output logic [31:0] debug_value, debug_pc,
    output logic md_busy
);
    typedef struct packed {
        logic valid, fault;
        logic [31:0] pc, insn, cause, tval;
    } ifid_t;
    typedef struct packed {
        logic valid, fault;
        logic [31:0] pc, insn, cause, tval, a, b;
    } idex_t;
    typedef struct packed {
        logic valid, fault, write_reg, load, store;
        logic [31:0] pc, insn, cause, tval, value, address, wdata;
        logic [4:0] rd;
        logic [2:0] kind;
        logic [3:0] strobe;
    } exmem_t;
    typedef struct packed {
        logic valid, fault, write_reg;
        logic [31:0] pc, insn, cause, tval, value;
        logic [4:0] rd;
    } memwb_t;
    ifid_t ifid;
    idex_t idex;
    exmem_t exmem, execute;
    memwb_t memwb, memory_result;
    logic [31:0] regs [0:31], fetch_pc;
    logic halted, fault_pending, md_started;
    logic hazard, redirect, execute_fault;
    logic [31:0] redirect_pc;
    wire [6:0] opcode=idex.insn[6:0], funct7=idex.insn[31:25];
    wire [2:0] funct3=idex.insn[14:12];
    wire [4:0] rd=idex.insn[11:7];
    wire [31:0] imm_i={{20{idex.insn[31]}},idex.insn[31:20]};
    wire [31:0] imm_s={{20{idex.insn[31]}},idex.insn[31:25],idex.insn[11:7]};
    wire [31:0] imm_b={{19{idex.insn[31]}},idex.insn[31],idex.insn[7],idex.insn[30:25],idex.insn[11:8],1'b0};
    wire [31:0] imm_u={idex.insn[31:12],12'b0};
    wire [31:0] imm_j={{11{idex.insn[31]}},idex.insn[31],idex.insn[19:12],idex.insn[20],idex.insn[30:21],1'b0};
    wire is_m=idex.valid && !idex.fault && opcode == 7'h33 && funct7 == 1;
    wire mem_wait=exmem.valid && (exmem.load || exmem.store) && !dmem_ready;
    wire memory_fault=exmem.valid && (exmem.load || exmem.store) && dmem_ready && dmem_error;
    wire wb_fault=memwb.valid && memwb.fault;
    wire md_done, md_alu_valid, md_alu_sub, alu_carry;
    wire [31:0] md_result, md_q, md_alu_a, md_alu_b, alu_result;
    wire [32:0] md_partial;
    wire [5:0] md_count;
    wire md_start=is_m && !md_started && !mem_wait && !memory_fault && !wb_fault && !halted;
    wire execute_wait=is_m && !(md_started && md_done);
    wire core_alu_sub=opcode == 7'h33 && funct3 == 0 && funct7 == 7'h20;
    wire [31:0] core_alu_b=opcode == 7'h13 ? imm_i : idex.b;
    colab_alu32 alu(.subtract(md_busy && md_alu_valid ? md_alu_sub : core_alu_sub),
        .a(md_busy && md_alu_valid ? md_alu_a : idex.a), .b(md_busy && md_alu_valid ? md_alu_b : core_alu_b),
        .result(alu_result), .carry_or_borrow(alu_carry));
    colab_muldiv mdu(.clk(clk), .reset(reset || memory_fault || wb_fault), .start(md_start), .funct3(funct3),
        .operand_a(idex.a), .operand_b(idex.b), .busy(md_busy), .done(md_done), .result(md_result),
        .partial(md_partial), .q(md_q), .count(md_count), .alu_valid(md_alu_valid),
        .alu_subtract(md_alu_sub), .alu_a(md_alu_a), .alu_b(md_alu_b),
        .alu_result(alu_result), .alu_carry_or_borrow(alu_carry));

    assign imem_address=fetch_pc;
    assign imem_request=!halted && !fault_pending && !wb_fault;
    assign debug_pc=fetch_pc; assign debug_value=debug_index == 0 ? 0 : regs[debug_index];
    assign dmem_valid=exmem.valid && (exmem.load || exmem.store) && !halted && !wb_fault;
    assign dmem_write=exmem.store; assign dmem_address=exmem.address;
    assign dmem_wdata=exmem.wdata; assign dmem_wstrb=exmem.strobe;

    function automatic logic writes(input logic [31:0] insn);
        case (insn[6:0])
        7'h37,7'h17,7'h6f,7'h67,7'h03,7'h13,7'h33: writes=insn[11:7] != 0;
        default: writes=0;
        endcase
    endfunction
    function automatic logic uses_a(input logic [31:0] insn);
        case (insn[6:0])
        7'h67,7'h63,7'h03,7'h23,7'h13,7'h33: uses_a=1;
        default: uses_a=0;
        endcase
    endfunction
    function automatic logic uses_b(input logic [31:0] insn);
        case (insn[6:0])
        7'h63,7'h23,7'h33: uses_b=1;
        default: uses_b=0;
        endcase
    endfunction
    function automatic logic depends(input logic [31:0] insn, input logic [4:0] dst);
        depends=dst != 0 && ((uses_a(insn) && insn[19:15] == dst) || (uses_b(insn) && insn[24:20] == dst));
    endfunction
    always_comb begin
        hazard=ifid.valid && !ifid.fault &&
            ((idex.valid && writes(idex.insn) && depends(ifid.insn,idex.insn[11:7])) ||
             (exmem.valid && exmem.write_reg && depends(ifid.insn,exmem.rd)) ||
             (memwb.valid && memwb.write_reg && depends(ifid.insn,memwb.rd)));
    end

    logic [31:0] effective, target, calc;
    logic legal, condition;
    always_comb begin
        execute='0; execute.valid=idex.valid; execute.pc=idex.pc; execute.insn=idex.insn;
        execute.fault=idex.fault; execute.cause=idex.cause; execute.tval=idex.tval;
        execute.rd=rd; redirect=0; redirect_pc=0;
        effective=0; target=0; calc=0; legal=1; condition=0;
        if (idex.valid && !idex.fault) begin
            case (opcode)
            7'h37: begin execute.write_reg=1; execute.value=imm_u; end
            7'h17: begin execute.write_reg=1; execute.value=idex.pc+imm_u; end
            7'h6f: begin
                target=idex.pc+imm_j;
                if (target[1:0] != 0) begin execute.fault=1; execute.cause=0; execute.tval=target; end
                else begin execute.write_reg=1; execute.value=idex.pc+4; redirect=1; redirect_pc=target; end
            end
            7'h67: begin
                target=(idex.a+imm_i)&32'hfffffffe;
                if (funct3 != 0) legal=0;
                else if (target[1:0] != 0) begin execute.fault=1; execute.cause=0; execute.tval=target; end
                else begin execute.write_reg=1; execute.value=idex.pc+4; redirect=1; redirect_pc=target; end
            end
            7'h63: begin
                case (funct3)
                0: condition=idex.a==idex.b; 1: condition=idex.a!=idex.b;
                4: condition=$signed(idex.a)<$signed(idex.b); 5: condition=$signed(idex.a)>=$signed(idex.b);
                6: condition=idex.a<idex.b; 7: condition=idex.a>=idex.b;
                default: legal=0;
                endcase
                target=condition ? idex.pc+imm_b : idex.pc+4;
                if (legal && target[1:0] != 0) begin execute.fault=1; execute.cause=0; execute.tval=target; end
                else if (legal && condition) begin redirect=1; redirect_pc=target; end
            end
            7'h03: begin
                effective=idex.a+imm_i; execute.address=effective; execute.kind=funct3;
                if (!(funct3 == 0 || funct3 == 1 || funct3 == 2 || funct3 == 4 || funct3 == 5)) legal=0;
                else if (((funct3 == 1 || funct3 == 5) && effective[0]) || (funct3 == 2 && effective[1:0] != 0))
                    begin execute.fault=1; execute.cause=4; execute.tval=effective; end
                else begin execute.load=1; execute.write_reg=1; end
            end
            7'h23: begin
                effective=idex.a+imm_s; execute.address=effective; execute.kind=funct3;
                if (funct3 > 2) legal=0;
                else if ((funct3 == 1 && effective[0]) || (funct3 == 2 && effective[1:0] != 0))
                    begin execute.fault=1; execute.cause=6; execute.tval=effective; end
                else begin
                    execute.store=1; execute.wdata=idex.b << (8*effective[1:0]);
                    case (funct3)
                    0: execute.strobe=4'b0001 << effective[1:0];
                    1: execute.strobe=4'b0011 << effective[1:0];
                    default: execute.strobe=4'b1111;
                    endcase
                end
            end
            7'h13: begin
                case (funct3)
                0: calc=alu_result;
                1: if (funct7 == 0) calc=idex.a << idex.insn[24:20]; else legal=0;
                2: calc=$signed(idex.a)<$signed(imm_i); 3: calc=idex.a<imm_i; 4: calc=idex.a^imm_i;
                5: if (funct7 == 0) calc=idex.a >> idex.insn[24:20];
                   else if (funct7 == 7'h20) calc=$signed(idex.a) >>> idex.insn[24:20]; else legal=0;
                6: calc=idex.a|imm_i; 7: calc=idex.a&imm_i;
                endcase
                execute.write_reg=1; execute.value=calc;
            end
            7'h33: begin
                if (funct7 == 1) calc=md_result;
                else begin
                    legal=funct7 == 0 || (funct7 == 7'h20 && (funct3 == 0 || funct3 == 5));
                    case (funct3)
                    0: calc=alu_result; 1: calc=idex.a << idex.b[4:0];
                    2: calc=$signed(idex.a)<$signed(idex.b); 3: calc=idex.a<idex.b; 4: calc=idex.a^idex.b;
                    5: if (funct7 == 7'h20) calc=$signed(idex.a) >>> idex.b[4:0]; else calc=idex.a >> idex.b[4:0];
                    6: calc=idex.a|idex.b; 7: calc=idex.a&idex.b;
                    endcase
                end
                execute.write_reg=1; execute.value=calc;
            end
            7'h0f: if (funct3 != 0) legal=0;
            7'h73: if (idex.insn == 32'h00000073) begin execute.fault=1; execute.cause=11; execute.tval=0; end
                   else if (idex.insn == 32'h00100073) begin execute.fault=1; execute.cause=3; execute.tval=0; end
                   else legal=0;
            default: legal=0;
            endcase
            if (!legal) begin execute.fault=1; execute.cause=2; execute.tval=idex.insn; end
        end
        if (execute.fault) begin execute.write_reg=0; execute.load=0; execute.store=0; redirect=0; end
        execute_fault=execute.valid && execute.fault;
    end

    logic [31:0] loaded;
    always_comb begin
        memory_result='0; memory_result.valid=exmem.valid; memory_result.fault=exmem.fault;
        memory_result.pc=exmem.pc; memory_result.insn=exmem.insn; memory_result.cause=exmem.cause;
        memory_result.tval=exmem.tval; memory_result.value=exmem.value;
        memory_result.write_reg=exmem.write_reg; memory_result.rd=exmem.rd;
        loaded=dmem_rdata >> (8*exmem.address[1:0]);
        if (memory_fault) begin
            memory_result.fault=1;
            memory_result.cause=dmem_page_fault ? (exmem.load ? 13 : 15) : (exmem.load ? 5 : 7);
            memory_result.tval=exmem.address; memory_result.write_reg=0;
        end else if (exmem.load) begin
            case (exmem.kind)
            0: memory_result.value={{24{loaded[7]}},loaded[7:0]};
            1: memory_result.value={{16{loaded[15]}},loaded[15:0]};
            2: memory_result.value=loaded;
            4: memory_result.value={24'b0,loaded[7:0]};
            default: memory_result.value={16'b0,loaded[15:0]};
            endcase
        end
    end

    integer n;
    always_ff @(posedge clk) begin
        if (reset) begin
            ifid<='0; idex<='0; exmem<='0; memwb<='0; fetch_pc<=0;
            halted<=0; fault_pending<=0; md_started<=0; retire_valid<=0; retire_pc<=0;
            retire_instruction<=0; retire_write<=0; retire_rd<=0; retire_value<=0;
            trap_valid<=0; trap_pc<=0; trap_cause<=0; trap_value<=0;
            for (n=0; n<32; n=n+1) regs[n]<=0;
        end else begin
            retire_valid<=0;
            if (!halted) begin
                if (wb_fault) begin
                    trap_valid<=1; trap_pc<=memwb.pc; trap_cause<=memwb.cause; trap_value<=memwb.tval;
                    halted<=1; ifid.valid<=0; idex.valid<=0; exmem.valid<=0; memwb.valid<=0;
                end else begin
                    if (memwb.valid) begin
                        retire_valid<=1; retire_pc<=memwb.pc; retire_instruction<=memwb.insn;
                        retire_write<=memwb.write_reg && memwb.rd != 0; retire_rd<=memwb.rd; retire_value<=memwb.value;
                        if (memwb.write_reg && memwb.rd != 0) regs[memwb.rd]<=memwb.value;
                    end
                    memwb.valid<=0;
                    if (!mem_wait) begin
                        memwb<=memory_result; exmem.valid<=0;
                        if (memory_fault) begin
                            ifid.valid<=0; idex.valid<=0; md_started<=0; fault_pending<=1;
                        end else if (execute_wait) begin
                            if (md_start) md_started<=1;
                        end else begin
                            exmem<=execute; md_started<=0;
                            if (execute_fault || redirect) begin
                                ifid.valid<=0; idex.valid<=0;
                                if (execute_fault) fault_pending<=1; else fetch_pc<=redirect_pc;
                            end else if (hazard) idex.valid<=0;
                            else if (!fault_pending) begin
                                idex.valid<=ifid.valid; idex.fault<=ifid.fault; idex.pc<=ifid.pc; idex.insn<=ifid.insn;
                                idex.cause<=ifid.cause; idex.tval<=ifid.tval;
                                idex.a<=ifid.insn[19:15] == 0 ? 0 : regs[ifid.insn[19:15]];
                                idex.b<=ifid.insn[24:20] == 0 ? 0 : regs[ifid.insn[24:20]];
                                ifid.valid<=imem_ready;
                                if (imem_ready) begin
                                    ifid.pc<=fetch_pc; ifid.insn<=imem_data;
                                    ifid.fault<=imem_error; ifid.cause<=imem_page_fault ? 12 : 1; ifid.tval<=fetch_pc;
                                    fetch_pc<=fetch_pc+4;
                                end
                            end else begin ifid.valid<=0; idex.valid<=0; end
                        end
                    end
                end
            end
        end
    end
endmodule
