// --- PLIK: hdl/src/coprocesor.sv ---
//
// Koprocesor CV-X-IF z instrukcjami zespolonymi CCVT, CADD, CMLA
// (jednostki wygenerowane w Vitis HLS: top_ccvt, top_cadd, top_cmla).
//
// ---------------------------------------------------------------------------
// Kodowanie instrukcji
// ---------------------------------------------------------------------------
//   CCVT rd, rs1, rs2        R-type, opcode custom-0 (0001011), funct3 = 000
//     funct7 = {3'b000, h, s, n[1:0]}
//     .insn r 0x0B, 0, FUNCT7, rd, rs1, rs2
//
//   CADD rd, rs1, rs2        R-type, opcode custom-0 (0001011), funct3 = 001
//     funct7 = {4'b0000, h, k[1:0]}
//     .insn r 0x0B, 1, FUNCT7, rd, rs1, rs2
//
//   CMLA rd, rs1, rs2, rs3   R4-type, opcode custom-1 (0101011)
//     instr[31:27] = rs3, funct2 = instr[26:25] = k[1:0]
//     funct3 = {1'b0, h, c}
//     .insn r 0x2B, FUNCT3, K, rd, rs1, rs2, rs3
//
//   Bity zarezerwowane (oznaczone 0) muszą być 0, inaczej instrukcja jest
//   odrzucana (rdzeń zgłosi illegal instruction).
//
// ---------------------------------------------------------------------------
// Latencje (od handshake'u issue do result_valid, przy commicie razem z issue)
// ---------------------------------------------------------------------------
//   CCVT 1 takt  = rejestr wejściowy + top_ccvt (LATENCY 0)
//   CADD 2 takty = rejestr wejściowy + top_cadd (LATENCY 1)
//   CMLA 3 takty = rejestr wejściowy + top_cmla (LATENCY 2)
//
// ---------------------------------------------------------------------------
// Budowa
// ---------------------------------------------------------------------------
//   1. Dekoder (kombinacyjnie z issue_req): rodzaj operacji i flagi.
//   2. Rejestr wejściowy in_q: zapis tylko na handshake'u issue. Wszystkie
//      jednostki HLS dostają te same dane z in_q (rs przychodzi późno w takcie,
//      więc z rs nic nie jest liczone w takcie issue).
//   3. Jednostki HLS (ap_ctrl_none, II = 1) liczą w każdym takcie. Każda
//      dostaje bit vld = 1 tylko wtedy, gdy nowa instrukcja jest jej, i zwraca
//      go razem z id i wynikiem (HLS opóźnia vld i id o swoją latencję), więc
//      adapter nie musi znać latencji jednostek.
//   4. Tablica stanu indeksowana id (2^X_ID_WIDTH pozycji): commit, kill,
//      gotowość wyniku, rd, dane. Jednostki zapisują wyniki pod swoje id,
//      więc kilka wyników w jednym takcie nie jest problemem.
//   5. Kolejka id (tylko numery, nie dane) w kolejności przyjęcia: wyniki
//      wychodzą do rdzenia w kolejności programu. Gdy wynik czoła powstaje
//      właśnie w tym takcie, idzie na result.data bezpośrednio z jednostki
//      (bez dodatkowego taktu na zapis do tablicy).
//
//   Tablica ma pozycję na każde możliwe id, więc nigdy się nie zapełnia:
//   issue_ready nie zależy od result_ready. Jedyny przypadek wstrzymania to
//   ponowne użycie przez rdzeń id instrukcji zabitej, której wynik jeszcze
//   liczy jednostka HLS (czekamy 1-2 takty, aż stary wynik wyjdzie z potoku,
//   żeby nie nadpisał wyniku nowej instrukcji).

`default_nettype none

module xif_copro (
  input  logic                      clk_i,
  input  logic                      rst_ni,
  cv32e40x_if_xif.coproc_compressed xif_compressed_if,
  cv32e40x_if_xif.coproc_issue      xif_issue_if,
  cv32e40x_if_xif.coproc_commit     xif_commit_if,
  cv32e40x_if_xif.coproc_mem        xif_mem_if,
  cv32e40x_if_xif.coproc_mem_result xif_mem_result_if,
  cv32e40x_if_xif.coproc_result     xif_result_if
);

  // ------------------------------------------------------------------
  // Parametry
  // ------------------------------------------------------------------
  localparam int X_NUM_RS   = int'(xif_issue_if.X_NUM_RS);
  localparam int X_ID_WIDTH = int'(xif_issue_if.X_ID_WIDTH);
  localparam int ID_W       = X_ID_WIDTH;
  localparam int NID        = 1 << ID_W;     // liczba pozycji tablicy = liczba możliwych id
  localparam int HLS_ID_W   = 4;             // szerokość id w strukturach HLS (ap_uint<4>)

  localparam logic [6:0] OPC_CUSTOM0 = 7'b0001011;
  localparam logic [6:0] OPC_CUSTOM1 = 7'b0101011;
  localparam logic [2:0] F3_CCVT     = 3'b000;
  localparam logic [2:0] F3_CADD     = 3'b001;

  typedef enum logic [1:0] {OP_NONE, OP_CCVT, OP_CADD, OP_CMLA} op_e;

  generate
    if (X_NUM_RS < 3) begin : g_check_num_rs
      $fatal(1, "xif_copro: CMLA wymaga interfejsu z X_NUM_RS >= 3 (jest %0d)", X_NUM_RS);
    end
    if (X_ID_WIDTH > HLS_ID_W) begin : g_check_id_w
      $fatal(1, "xif_copro: X_ID_WIDTH = %0d, a id w HLS ma %0d bity - zwieksz ap_uint id w HLS",
             X_ID_WIDTH, HLS_ID_W);
    end
  endgenerate

  // ------------------------------------------------------------------
  // Nieużywane kanały: skompresowane odrzucamy, pamięci nie używamy
  // ------------------------------------------------------------------
  assign xif_compressed_if.compressed_ready = 1'b1;
  assign xif_compressed_if.compressed_resp  = '0;   // accept = 0
  assign xif_mem_if.mem_valid               = 1'b0;
  assign xif_mem_if.mem_req                 = '0;

  // ------------------------------------------------------------------
  // 1. Dekoder (kombinacyjnie z bieżącego issue_req)
  // ------------------------------------------------------------------
  logic [31:0] instr;
  logic [6:0]  opcode, funct7;
  logic [2:0]  funct3;
  logic [1:0]  funct2;

  assign instr  = xif_issue_if.issue_req.instr;
  assign opcode = instr[6:0];
  assign funct3 = instr[14:12];
  assign funct7 = instr[31:25];
  assign funct2 = instr[26:25];

  op_e        dec_op;
  logic [1:0] dec_n, dec_k;
  logic       dec_s, dec_c, dec_h;
  logic       ops_ready;

  always_comb begin
    dec_op    = OP_NONE;
    dec_n     = '0;
    dec_k     = '0;
    dec_s     = 1'b0;
    dec_c     = 1'b0;
    dec_h     = 1'b0;
    ops_ready = 1'b1;
    if (opcode == OPC_CUSTOM0 && funct3 == F3_CCVT && funct7[6:4] == 3'b000) begin
      dec_op    = OP_CCVT;
      dec_n     = funct7[1:0];
      dec_s     = funct7[2];
      dec_h     = funct7[3];
      ops_ready = &xif_issue_if.issue_req.rs_valid[1:0];
    end else if (opcode == OPC_CUSTOM0 && funct3 == F3_CADD && funct7[6:3] == 4'b0000) begin
      dec_op    = OP_CADD;
      dec_k     = funct7[1:0];
      dec_h     = funct7[2];
      ops_ready = &xif_issue_if.issue_req.rs_valid[1:0];
    end else if (opcode == OPC_CUSTOM1 && funct3[2] == 1'b0) begin
      dec_op    = OP_CMLA;
      dec_k     = funct2;
      dec_c     = funct3[0];
      dec_h     = funct3[1];
      ops_ready = &xif_issue_if.issue_req.rs_valid[2:0];
    end
  end

  // ------------------------------------------------------------------
  // Tablica stanu (deklaracje - używana już przy issue_ready)
  // ------------------------------------------------------------------
  logic [NID-1:0] t_alloc;    // id zajęte (od przyjęcia do oddania wyniku / wyjścia z potoku po kill)
  logic [NID-1:0] t_comm;     // commit z kill = 0
  logic [NID-1:0] t_kill;     // commit z kill = 1
  logic [NID-1:0] t_done;     // wynik zapisany w tablicy
  logic [NID-1:0] t_inq;      // id jest w kolejce kolejności
  logic [4:0]     t_rd   [NID];
  logic [31:0]    t_data [NID];

  // ------------------------------------------------------------------
  // ISSUE
  // ------------------------------------------------------------------
  logic [ID_W-1:0] iss_id;
  assign iss_id = xif_issue_if.issue_req.id;

  always_comb begin
    xif_issue_if.issue_resp  = '0;     // accept = 0, writeback = 0, loadstore = 0, ...
    xif_issue_if.issue_ready = 1'b1;   // obcą instrukcję odrzucamy od razu
    if (dec_op != OP_NONE) begin
      // !t_alloc: czekamy tylko, gdy id zabitej instrukcji jest jeszcze w potoku HLS
      xif_issue_if.issue_ready          = ops_ready && !t_alloc[iss_id];
      xif_issue_if.issue_resp.accept    = 1'b1;
      xif_issue_if.issue_resp.writeback = 1'b1;
    end
  end

  logic issue_hs, iss_commit, iss_kill, launch;

  assign issue_hs   = xif_issue_if.issue_valid && xif_issue_if.issue_ready && (dec_op != OP_NONE);
  assign iss_commit = xif_commit_if.commit_valid && (xif_commit_if.commit.id == iss_id) &&
                      !xif_commit_if.commit.commit_kill;
  assign iss_kill   = xif_commit_if.commit_valid && (xif_commit_if.commit.id == iss_id) &&
                       xif_commit_if.commit.commit_kill;
  // instrukcja zabita w takcie issue nie trafia nigdzie (nie wchodzi też do HLS)
  assign launch     = issue_hs && !iss_kill;

  // ------------------------------------------------------------------
  // 2. Rejestr wejściowy (wspólny dla wszystkich jednostek)
  // ------------------------------------------------------------------
  logic            q_vld;
  op_e             q_op;
  logic [ID_W-1:0] q_id;
  logic [1:0]      q_n, q_k;
  logic            q_s, q_c, q_h;
  logic [31:0]     q_rs1, q_rs2, q_rs3;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      q_vld <= 1'b0;
      q_op  <= OP_NONE;
      q_id  <= '0;
      q_n   <= '0;
      q_k   <= '0;
      q_s   <= 1'b0;
      q_c   <= 1'b0;
      q_h   <= 1'b0;
      q_rs1 <= '0;
      q_rs2 <= '0;
      q_rs3 <= '0;
    end else begin
      q_vld <= launch;
      if (launch) begin               // poza launch dane stoją (HLS liczy wciąż to samo)
        q_op  <= dec_op;
        q_id  <= iss_id;
        q_n   <= dec_n;
        q_k   <= dec_k;
        q_s   <= dec_s;
        q_c   <= dec_c;
        q_h   <= dec_h;
        q_rs1 <= xif_issue_if.issue_req.rs[0][31:0];
        q_rs2 <= xif_issue_if.issue_req.rs[1][31:0];
        q_rs3 <= xif_issue_if.issue_req.rs[2][31:0];
      end
    end
  end

  // ------------------------------------------------------------------
  // 3. Jednostki HLS
  //
  // in_r (struct dataIn z Vitis HLS, pola wyrównane do bajtów):
  //   [159:128] rs3   [127:96] rs2   [95:64] rs1   [63:56] -
  //   [55:48] h       [47:40]  c     [39:32] k     [31:24] s
  //   [23:16] n       [15:8]   vld   [7:0]   id
  // out_r: [36] vld, [35:32] id, [31:16] Im, [15:0] Re
  //
  // Wszystkie jednostki dostają te same dane z in_q; różni się tylko bit vld.
  // ------------------------------------------------------------------
  localparam int VLD_IN  = 8;    // pozycja vld w in_r
  localparam int VLD_OUT = 36;   // pozycja vld w out_r

  logic [159:0] hls_common, hls_in_ccvt, hls_in_cadd, hls_in_cmla;

  assign hls_common = {q_rs3, q_rs2, q_rs1,
                       8'd0,                 // [63:56]
                       7'd0, q_h,            // [55:48]
                       7'd0, q_c,            // [47:40]
                       6'd0, q_k,            // [39:32]
                       7'd0, q_s,            // [31:24]
                       6'd0, q_n,            // [23:16]
                       8'd0,                 // [15:8]  vld - wstawiane niżej
                       (8 - ID_W)'(0), q_id};// [7:0]

  always_comb begin
    hls_in_ccvt = hls_common;
    hls_in_cadd = hls_common;
    hls_in_cmla = hls_common;
    hls_in_ccvt[VLD_IN] = q_vld && (q_op == OP_CCVT);
    hls_in_cadd[VLD_IN] = q_vld && (q_op == OP_CADD);
    hls_in_cmla[VLD_IN] = q_vld && (q_op == OP_CMLA);
  end

  logic [36:0] ccvt_out, cadd_out, cmla_out;

  top_ccvt u_ccvt (
    .in_r   ( hls_in_ccvt ),
    .out_r  ( ccvt_out    ),
    .ap_rst ( !rst_ni     )
  );

  top_cadd u_cadd (
    .ap_clk ( clk_i       ),
    .ap_rst ( !rst_ni     ),
    .in_r   ( hls_in_cadd ),
    .out_r  ( cadd_out    )
  );

  top_cmla u_cmla (
    .ap_clk ( clk_i       ),
    .ap_rst ( !rst_ni     ),
    .in_r   ( hls_in_cmla ),
    .out_r  ( cmla_out    )
  );

  // Wyniki jednostek: vld i id zwrócone przez HLS, dane
  localparam int NU = 3;
  logic            u_vld  [NU];
  logic [ID_W-1:0] u_id   [NU];
  logic [31:0]     u_data [NU];

  assign u_vld[0]  = ccvt_out[VLD_OUT];
  assign u_id[0]   = ccvt_out[32 +: ID_W];
  assign u_data[0] = ccvt_out[31:0];

  assign u_vld[1]  = cadd_out[VLD_OUT];
  assign u_id[1]   = cadd_out[32 +: ID_W];
  assign u_data[1] = cadd_out[31:0];

  assign u_vld[2]  = cmla_out[VLD_OUT];
  assign u_id[2]   = cmla_out[32 +: ID_W];
  assign u_data[2] = cmla_out[31:0];

  // Zakończenia w tym takcie, rozłożone na id (różne jednostki = różne id)
  logic [NID-1:0] cpl;
  logic [31:0]    cpl_data [NID];

  always_comb begin
    for (int i = 0; i < NID; i++) begin
      cpl[i]      = 1'b0;
      cpl_data[i] = '0;
      for (int u = 0; u < NU; u++) begin
        if (u_vld[u] && (u_id[u] == ID_W'(i))) begin
          cpl[i]      = 1'b1;
          cpl_data[i] = u_data[u];
        end
      end
    end
  end

  // ------------------------------------------------------------------
  // 5. Kolejka kolejności (same id) i czoło
  // ------------------------------------------------------------------
  logic [ID_W-1:0] oq [NID];
  logic [ID_W-1:0] oq_wr, oq_rd;
  logic [ID_W:0]   oq_cnt;
  logic            oq_empty;
  logic [ID_W-1:0] hid;

  assign oq_empty = (oq_cnt == '0);
  assign hid      = oq[oq_rd];

  // commit przychodzący teraz dla czoła
  logic head_live, head_comm_now, head_kill_now;
  assign head_live     = !oq_empty && t_alloc[hid] && !t_comm[hid] && !t_kill[hid];
  assign head_comm_now = head_live && xif_commit_if.commit_valid &&
                         (xif_commit_if.commit.id == hid) && !xif_commit_if.commit.commit_kill;
  assign head_kill_now = head_live && xif_commit_if.commit_valid &&
                         (xif_commit_if.commit.id == hid) &&  xif_commit_if.commit.commit_kill;

  logic        head_dead, head_ready, result_hs, oq_pop;
  logic [31:0] head_data;

  // zabita instrukcja na czele: zdejmujemy ją z kolejki od razu (bez wyniku)
  assign head_dead  = !oq_empty && (t_kill[hid] || head_kill_now);
  // wynik gotowy: ma commit (zapamiętany albo właśnie przychodzący)
  //               i dane (w tablicy albo właśnie wychodzące z jednostki)
  assign head_ready = !oq_empty && t_alloc[hid] && !t_kill[hid] &&
                      (t_comm[hid] || head_comm_now) &&
                      (t_done[hid] || cpl[hid]);
  assign head_data  = t_done[hid] ? t_data[hid] : cpl_data[hid];

  assign xif_result_if.result_valid = head_ready;
  assign result_hs = head_ready && xif_result_if.result_ready;
  assign oq_pop    = result_hs || head_dead;

  always_comb begin
    xif_result_if.result      = '0;
    xif_result_if.result.id   = hid;
    xif_result_if.result.rd   = t_rd[hid];
    xif_result_if.result.data = head_data;
    xif_result_if.result.we   = 1'b1;
  end

  // ------------------------------------------------------------------
  // Aktualizacja kolejki kolejności
  // (pojemność NID: tyle, ile może być różnych id w locie - nigdy się nie przepełni)
  // ------------------------------------------------------------------
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      for (int i = 0; i < NID; i++) oq[i] <= '0;
      oq_wr  <= '0;
      oq_rd  <= '0;
      oq_cnt <= '0;
    end else begin
      if (launch) begin
        oq[oq_wr] <= iss_id;
        oq_wr     <= oq_wr + 1'b1;
      end
      if (oq_pop) oq_rd <= oq_rd + 1'b1;
      oq_cnt <= oq_cnt + (ID_W+1)'(launch) - (ID_W+1)'(oq_pop);
    end
  end

  // ------------------------------------------------------------------
  // 4. Aktualizacja tablicy stanu
  // ------------------------------------------------------------------
  logic [NID-1:0] t_launch;   // nowa instrukcja z tym id
  logic [NID-1:0] t_chit;     // commit dla żywej pozycji
  logic [NID-1:0] t_free;     // zwolnienie id

  always_comb begin
    for (int i = 0; i < NID; i++) begin
      logic popped, kill_eff, done_eff, inq_eff;
      t_launch[i] = launch && (iss_id == ID_W'(i));
      t_chit[i]   = xif_commit_if.commit_valid && (xif_commit_if.commit.id == ID_W'(i)) &&
                    t_alloc[i] && !t_comm[i] && !t_kill[i];
      popped      = oq_pop && (hid == ID_W'(i));
      kill_eff    = t_kill[i] || (t_chit[i] && xif_commit_if.commit.commit_kill);
      done_eff    = t_done[i] || cpl[i];
      inq_eff     = t_inq[i] && !popped;
      // zwolnienie id:
      //  - wynik odebrany przez rdzeń, albo
      //  - instrukcja zabita, jej wynik już wyszedł z jednostki HLS
      //    i nie ma jej w kolejce kolejności
      t_free[i]   = (result_hs && (hid == ID_W'(i))) ||
                    (t_alloc[i] && kill_eff && done_eff && !inq_eff);
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      t_alloc <= '0;
      t_comm  <= '0;
      t_kill  <= '0;
      t_done  <= '0;
      t_inq   <= '0;
      for (int i = 0; i < NID; i++) begin
        t_rd[i]   <= '0;
        t_data[i] <= '0;
      end
    end else begin
      for (int i = 0; i < NID; i++) begin
        if (t_launch[i]) begin
          // nowa instrukcja (issue_ready gwarantuje, że pozycja była wolna)
          t_alloc[i] <= 1'b1;
          t_comm[i]  <= iss_commit;
          t_kill[i]  <= 1'b0;
          t_done[i]  <= 1'b0;
          t_inq[i]   <= 1'b1;
          t_rd[i]    <= instr[11:7];
        end else begin
          if (t_chit[i]) begin
            if (xif_commit_if.commit.commit_kill) t_kill[i] <= 1'b1;
            else                                  t_comm[i] <= 1'b1;
          end
          if (cpl[i] && t_alloc[i]) begin
            t_done[i] <= 1'b1;
            t_data[i] <= cpl_data[i];
          end
          if (oq_pop && (hid == ID_W'(i))) t_inq[i]   <= 1'b0;
          if (t_free[i])                   t_alloc[i] <= 1'b0;
        end
      end
    end
  end

`ifndef SYNTHESIS
  // Kontrola spójności: id zwrócone przez jednostkę musi być zajęte.
  // (Uwaga: rejestry danych w HLS nie mają resetu - reset musi trwać co
  //  najmniej tyle taktów, ile wynosi najdłuższa latencja jednostki, żeby
  //  z potoków wyszły śmieci z vld = 1. Testbench trzyma reset 5 taktów.)
  always @(posedge clk_i) begin
    if (rst_ni) begin
      for (int u = 0; u < NU; u++)
        if (u_vld[u] && !t_alloc[u_id[u]])
          $error("xif_copro: jednostka %0d zwrocila wynik dla wolnego id %0d",
                 u, u_id[u]);
    end
  end
`endif

  // Nieużywane sygnały (żeby lint nie marudził)
  logic unused;
  assign unused = ^{xif_compressed_if.compressed_valid, xif_compressed_if.compressed_req,
                    xif_mem_if.mem_ready, xif_mem_if.mem_resp,
                    xif_mem_result_if.mem_result_valid, xif_mem_result_if.mem_result,
                    xif_issue_if.issue_req, ccvt_out, cadd_out, cmla_out, hls_common[15:8]};

endmodule

`default_nettype wire
// --- KONIEC PLIKU: hdl/src/coprocesor.sv ---