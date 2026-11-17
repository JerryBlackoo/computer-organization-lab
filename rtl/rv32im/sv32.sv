// Standalone Sv32/Svade translation unit with four fully associative entries.
// The embedding CPU supplies effective privilege, PMP decisions and satp.
module colab_sv32 (
    input logic clk, reset,
    input logic req_valid, output logic req_ready,
    input logic [31:0] req_va, req_satp,
    input logic [1:0] req_access, req_privilege, // access: 0 fetch, 1 read, 2 write; privilege: U=0, S=1, M=3
    input logic req_sum, req_mxr,
    output logic result_valid, input logic result_ready,
    output logic [31:0] result_pa, result_cause, result_tval,
    output logic result_fault,
    output logic pte_valid, input logic pte_ready, output logic [31:0] pte_address,
    input logic pte_response_valid, output logic pte_response_ready,
    input logic [31:0] pte_response_data, input logic pte_response_error,
    input logic pte_allowed, final_allowed, output logic [31:0] final_address,
    input logic flush_valid, flush_all_va, flush_all_asid,
    input logic [31:0] flush_va, input logic [8:0] flush_asid,
    output logic walk_busy, output logic walk_level
);
    localparam logic [3:0] IDLE=0, LOOKUP=1, PTE_REQUEST=2, PTE_WAIT=3, CHECK_LEAF=4, RESULT=5, DRAIN=6;
    logic [3:0] state;
    logic [31:0] va, satp, leaf_pte;
    logic [1:0] access, privilege;
    logic sum, mxr, level, global_seen, leaf_level, leaf_global, leaf_insert;
    logic [1:0] victim;
    logic [3:0] valid, global_entry, entry_level;
    logic [8:0] entry_asid [0:3];
    logic [19:0] entry_vpn [0:3];
    logic [31:0] entry_pte [0:3];
    logic hit;
    logic [1:0] hit_index;
    integer k;
    logic permission, page_bad, address_bad;
    logic [33:0] physical;

    assign req_ready = state == IDLE && !flush_valid;
    assign result_valid = state == RESULT;
    assign pte_valid = state == PTE_REQUEST && pte_allowed && !flush_valid;
    assign pte_response_ready = state == PTE_WAIT || state == DRAIN;
    assign walk_busy = state == PTE_REQUEST || state == PTE_WAIT || state == DRAIN;
    assign walk_level = level;
    assign final_address = !satp[31] || privilege == 3 ? va : physical[31:0];

    function automatic [31:0] fault_code(input logic [1:0] kind, input logic page);
        if (page) fault_code = kind == 0 ? 12 : kind == 1 ? 13 : 15;
        else fault_code = kind == 0 ? 1 : kind == 1 ? 5 : 7;
    endfunction
    task automatic fail(input logic page);
        begin result_fault <= 1; result_cause <= fault_code(access, page); result_tval <= va; state <= RESULT; end
    endtask

    always_comb begin
        hit = 0; hit_index = 0;
        // Prefer a 4 KiB entry if overlapping stale entries exist.
        for (integer n=0; n<4; n=n+1)
            if (!hit && valid[n] && !entry_level[n] && entry_vpn[n] == va[31:12] &&
                (global_entry[n] || entry_asid[n] == satp[30:22])) begin hit = 1; hit_index = n; end
        for (integer n=0; n<4; n=n+1)
            if (!hit && valid[n] && entry_level[n] && entry_vpn[n][19:10] == va[31:22] &&
                (global_entry[n] || entry_asid[n] == satp[30:22])) begin hit = 1; hit_index = n; end

        permission = access == 0 ? leaf_pte[3] : access == 2 ? leaf_pte[2] :
            (leaf_pte[1] || (mxr && leaf_pte[3]));
        page_bad = !leaf_pte[0] || (!leaf_pte[1] && leaf_pte[2]) ||
            !(leaf_pte[1] || leaf_pte[3]) || !permission || !leaf_pte[6] ||
            (access == 2 && !leaf_pte[7]) ||
            (privilege == 0 && !leaf_pte[4]) ||
            (privilege == 1 && leaf_pte[4] && (access == 0 || !sum)) ||
            (leaf_level && leaf_pte[19:10] != 0);
        physical = leaf_level ? {leaf_pte[31:20], va[21:0]} : {leaf_pte[31:10], va[11:0]};
        address_bad = physical[33:32] != 0;
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            state <= IDLE; valid <= 0; global_entry <= 0; entry_level <= 0; victim <= 0;
            va <= 0; satp <= 0; access <= 0; privilege <= 0; sum <= 0; mxr <= 0;
            level <= 0; global_seen <= 0; leaf_level <= 0; leaf_global <= 0; leaf_insert <= 0;
            leaf_pte <= 0; pte_address <= 0; result_pa <= 0; result_cause <= 0; result_tval <= 0; result_fault <= 0;
            for (k=0; k<4; k=k+1) begin entry_asid[k] <= 0; entry_vpn[k] <= 0; entry_pte[k] <= 0; end
        end else if (flush_valid) begin
            for (k=0; k<4; k=k+1)
                if (valid[k] && (flush_all_va || (entry_level[k] ? entry_vpn[k][19:10] == flush_va[31:22]
                                                               : entry_vpn[k] == flush_va[31:12])) &&
                    (flush_all_asid || (!global_entry[k] && entry_asid[k] == flush_asid))) valid[k] <= 0;
            // A submitted PTE read must be drained, not reused by the next walk.
            if (state == PTE_WAIT && !pte_response_valid) state <= DRAIN;
            else if (state == DRAIN && !pte_response_valid) state <= DRAIN;
            else state <= IDLE;
        end else begin
            case (state)
            IDLE: if (req_valid) begin
                va <= req_va; satp <= req_satp; access <= req_access; privilege <= req_privilege;
                sum <= req_sum; mxr <= req_mxr; result_fault <= 0; result_cause <= 0; result_tval <= 0;
                state <= LOOKUP;
            end
            LOOKUP: begin
                if (!satp[31] || privilege == 3) begin
                    if (!final_allowed) fail(0);
                    else begin result_pa <= va; result_fault <= 0; state <= RESULT; end
                end else if (hit) begin
                    leaf_pte <= entry_pte[hit_index]; leaf_level <= entry_level[hit_index];
                    leaf_insert <= 0; state <= CHECK_LEAF;
                end else if (satp[21:20] != 0) fail(0);
                else begin
                    level <= 1; global_seen <= 0;
                    pte_address <= {satp[19:0], 12'b0} + {20'b0, va[31:22], 2'b00};
                    state <= PTE_REQUEST;
                end
            end
            PTE_REQUEST: begin
                if (!pte_allowed) fail(0);
                else if (pte_ready) state <= PTE_WAIT;
            end
            PTE_WAIT: if (pte_response_valid) begin
                if (pte_response_error) fail(0);
                else if (!pte_response_data[0] || (!pte_response_data[1] && pte_response_data[2])) fail(1);
                else if (pte_response_data[1] || pte_response_data[3]) begin
                    leaf_pte <= pte_response_data; leaf_level <= level;
                    leaf_global <= global_seen || pte_response_data[5]; leaf_insert <= 1; state <= CHECK_LEAF;
                end else if (!level || pte_response_data[4] || pte_response_data[6] || pte_response_data[7]) fail(1);
                else if (pte_response_data[31:30] != 0) fail(0);
                else begin
                    global_seen <= global_seen || pte_response_data[5]; level <= 0;
                    pte_address <= {pte_response_data[29:10], 12'b0} + {20'b0, va[21:12], 2'b00};
                    state <= PTE_REQUEST;
                end
            end
            CHECK_LEAF: begin
                if (page_bad) fail(1);
                else if (address_bad || !final_allowed) fail(0);
                else begin
                    result_pa <= physical[31:0]; result_fault <= 0;
                    if (leaf_insert) begin
                        valid[victim] <= 1; global_entry[victim] <= leaf_global; entry_level[victim] <= leaf_level;
                        entry_asid[victim] <= satp[30:22]; entry_vpn[victim] <= va[31:12]; entry_pte[victim] <= leaf_pte;
                        victim <= victim + 1'b1;
                    end
                    state <= RESULT;
                end
            end
            RESULT: if (result_ready) state <= IDLE;
            DRAIN: if (pte_response_valid) state <= IDLE;
            default: state <= IDLE;
            endcase
        end
    end
endmodule
