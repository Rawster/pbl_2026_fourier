// --- PLIK: hdl/sim/copro_top.sv ---
//
// Wrapper do symulacji w Verilatorze z testbenchem w C++.
// Interfejs SystemVerilog nie może być portem najwyższego modułu w Verilatorze,
// więc tu interfejs jest tworzony wewnątrz, a strona rdzenia
// wychodzi na zwykłe, płaskie porty (widoczne w C++ jako pola Vcopro_top).

`default_nettype none

module copro_top #(
  parameter int X_ID_WIDTH = 4
)(
  input  logic                  clk_i,
  input  logic                  rst_ni,

  // compressed
  input  logic                  compressed_valid_i,
  input  logic [15:0]           compressed_instr_i,
  output logic                  compressed_ready_o,
  output logic                  compressed_accept_o,

  // issue
  input  logic                  issue_valid_i,
  output logic                  issue_ready_o,
  input  logic [31:0]           issue_instr_i,
  input  logic [X_ID_WIDTH-1:0] issue_id_i,
  input  logic [31:0]           issue_rs0_i,
  input  logic [31:0]           issue_rs1_i,
  input  logic [31:0]           issue_rs2_i,
  input  logic [2:0]            issue_rs_valid_i,
  output logic                  issue_accept_o,
  output logic                  issue_writeback_o,
  output logic                  issue_dualwrite_o,
  output logic [2:0]            issue_dualread_o,
  output logic                  issue_loadstore_o,
  output logic                  issue_ecswrite_o,
  output logic                  issue_exc_o,

  // commit
  input  logic                  commit_valid_i,
  input  logic [X_ID_WIDTH-1:0] commit_id_i,
  input  logic                  commit_kill_i,

  // result
  output logic                  result_valid_o,
  input  logic                  result_ready_i,
  output logic [X_ID_WIDTH-1:0] result_id_o,
  output logic [4:0]            result_rd_o,
  output logic [31:0]           result_data_o,
  output logic                  result_we_o,
  output logic                  result_exc_o,
  output logic                  result_err_o,
  output logic                  result_dbg_o,
  output logic [2:0]            result_ecswe_o,

  // mem (tylko podgląd - koprocesor nie powinien z niego korzystać)
  output logic                  mem_valid_o
);

  cv32e40x_if_xif #(.X_NUM_RS(3), .X_ID_WIDTH(X_ID_WIDTH)) xif();

  // strona rdzenia -> interfejs
  always_comb begin
    xif.compressed_valid     = compressed_valid_i;
    xif.compressed_req       = '0;
    xif.compressed_req.instr = compressed_instr_i;
    xif.compressed_req.mode  = 2'b11;

    xif.issue_valid          = issue_valid_i;
    xif.issue_req            = '0;
    xif.issue_req.instr      = issue_instr_i;
    xif.issue_req.mode       = 2'b11;
    xif.issue_req.id         = issue_id_i;
    xif.issue_req.rs[0]      = issue_rs0_i;
    xif.issue_req.rs[1]      = issue_rs1_i;
    xif.issue_req.rs[2]      = issue_rs2_i;
    xif.issue_req.rs_valid   = issue_rs_valid_i;

    xif.commit_valid         = commit_valid_i;
    xif.commit.id            = commit_id_i;
    xif.commit.commit_kill   = commit_kill_i;

    xif.mem_ready            = 1'b0;
    xif.mem_resp             = '0;
    xif.mem_result_valid     = 1'b0;
    xif.mem_result           = '0;

    xif.result_ready         = result_ready_i;
  end

  // interfejs -> strona rdzenia
  assign compressed_ready_o  = xif.compressed_ready;
  assign compressed_accept_o = xif.compressed_resp.accept;

  assign issue_ready_o       = xif.issue_ready;
  assign issue_accept_o      = xif.issue_resp.accept;
  assign issue_writeback_o   = xif.issue_resp.writeback;
  assign issue_dualwrite_o   = xif.issue_resp.dualwrite;
  assign issue_dualread_o    = xif.issue_resp.dualread;
  assign issue_loadstore_o   = xif.issue_resp.loadstore;
  assign issue_ecswrite_o    = xif.issue_resp.ecswrite;
  assign issue_exc_o         = xif.issue_resp.exc;

  assign result_valid_o      = xif.result_valid;
  assign result_id_o         = xif.result.id;
  assign result_rd_o         = xif.result.rd;
  assign result_data_o       = xif.result.data;
  assign result_we_o         = xif.result.we[0];
  assign result_exc_o        = xif.result.exc;
  assign result_err_o        = xif.result.err;
  assign result_dbg_o        = xif.result.dbg;
  assign result_ecswe_o      = xif.result.ecswe;

  assign mem_valid_o         = xif.mem_valid;

  xif_copro u_copro (
    .clk_i             ( clk_i  ),
    .rst_ni            ( rst_ni ),
    .xif_compressed_if ( xif    ),
    .xif_issue_if      ( xif    ),
    .xif_commit_if     ( xif    ),
    .xif_mem_if        ( xif    ),
    .xif_mem_result_if ( xif    ),
    .xif_result_if     ( xif    )
  );

endmodule

`default_nettype wire
// --- KONIEC PLIKU: hdl/sim/copro_top.sv ---