// --- PLIK: hdl/sim/tb_main.cpp ---
//
// Testbench w C++ (Verilator) dla koprocesora CV-X-IF z instrukcjami
// CCVT, CADD, CMLA. Udaje rdzeń: wysyła instrukcje przez issue, commit/kill
// w kolejności programu, odbiera wyniki z losowym result_ready.
//
// Model referencyjny: czysta arytmetyka całkowita (te same funkcje co
// w testbenchu HLS), porównanie bit w bit.
//
// Sprawdzane:
//   - accept / pola issue_resp dla przyjętych i odrzuconych instrukcji
//   - dokładny takt wyniku (testy kierunkowe): issue + max(latencja, opóźnienie commita)
//   - wynik nigdy przed commitem, nigdy dla zabitej / odrzuconej instrukcji
//   - wyniki w kolejności programu, poprawne rd i dane
//   - stabilność pakietu result, dopóki result_ready = 0
//   - kilka jednostek kończących w tym samym takcie
//   - ponowne użycie id zabitej instrukcji, która jest jeszcze w potoku HLS
//   - test losowy z wieloma instrukcjami w locie
//
// Uruchomienie:  ./obj_dir/Vcopro_top [+seed=N] [+n=LICZBA]

#include <verilated.h>
#include "Vcopro_top.h"
#ifdef TRACE
#include <verilated_vcd_c.h>
#endif

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

// ===========================================================================
// Modele referencyjne (z testbencha HLS)
// ===========================================================================
static inline int64_t lo16(uint32_t x) { return (int16_t)(x & 0xFFFF); }
static inline int64_t hi16(uint32_t x) { return (int16_t)(x >> 16); }
static inline uint32_t pack(int64_t re, int64_t im) {
    return ((uint32_t)(uint16_t)im << 16) | (uint16_t)re;
}
static inline int64_t sat16(int64_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
static inline int64_t floor_shift(int64_t x, int sh) {
    if (sh == 0) return x;
    return x >= 0 ? (x >> sh) : -((-x + (int64_t(1) << sh) - 1) >> sh);
}
static inline int64_t rnd_shift(int64_t x, int sh) {
    if (sh == 0) return x;
    return floor_shift(x + (int64_t(1) << (sh - 1)), sh);
}
static inline void rot_k(unsigned k, int64_t re, int64_t im, int64_t &o_re, int64_t &o_im) {
    switch (k & 3) {
        case 0:  o_re =  re; o_im =  im; break;
        case 1:  o_re = -im; o_im =  re; break;
        case 2:  o_re = -re; o_im = -im; break;
        default: o_re =  im; o_im = -re; break;
    }
}
static uint32_t ref_ccvt(unsigned n, unsigned s, unsigned h, uint32_t rs1, uint32_t rs2) {
    static const int sh[4] = {6, 4, 2, 0};
    uint16_t v[2] = {(uint16_t)(rs1 & 0xFFFF), (uint16_t)(rs2 & 0xFFFF)};
    for (int i = 0; i < 2; i++) {
        v[i] = (uint16_t)(v[i] << sh[n & 3]);
        if (!s) v[i] ^= 0x8000;
        if (h)  v[i] = (uint16_t)floor_shift((int16_t)v[i], 1);
    }
    return ((uint32_t)v[1] << 16) | v[0];
}
static uint32_t ref_cadd(unsigned k, unsigned h, uint32_t rs1, uint32_t rs2) {
    int64_t r_re, r_im;
    rot_k(k, lo16(rs2), hi16(rs2), r_re, r_im);
    int64_t s_re = lo16(rs1) + r_re, s_im = hi16(rs1) + r_im;
    if (h) { s_re = rnd_shift(s_re, 1); s_im = rnd_shift(s_im, 1); }
    return pack(sat16(s_re), sat16(s_im));
}
static uint32_t ref_cmla(unsigned k, unsigned c, unsigned h, uint32_t rs1, uint32_t rs2, uint32_t rs3) {
    int64_t ar = lo16(rs1), ai = hi16(rs1), br = lo16(rs2), bi = hi16(rs2);
    int64_t m_re, m_im;
    if (!c) { m_re = ar * br - ai * bi; m_im = ar * bi + ai * br; }
    else    { m_re = ar * br + ai * bi; m_im = ai * br - ar * bi; }
    int64_t r_re, r_im;
    rot_k(k, m_re, m_im, r_re, r_im);
    int64_t t_re = lo16(rs3) * 32768 + r_re, t_im = hi16(rs3) * 32768 + r_im;
    int sh = h ? 16 : 15;
    return pack(sat16(rnd_shift(t_re, sh)), sat16(rnd_shift(t_im, sh)));
}

// ===========================================================================
// Instrukcje
// ===========================================================================
enum Op { OP_CCVT, OP_CADD, OP_CMLA, OP_BAD };
static const char *op_name[] = {"CCVT", "CADD", "CMLA", "BAD"};
static const int   op_lat[]  = {1, 2, 3, 0};     // latencja od handshake'u issue

static const uint32_t OPC_CUSTOM0 = 0x0B, OPC_CUSTOM1 = 0x2B, OPC_CUSTOM2 = 0x5B;

struct Ins {
    Op       op;
    uint32_t instr;
    unsigned rd;
    uint32_t rs1, rs2, rs3;
    uint32_t exp;      // oczekiwany wynik (tylko przyjęte)
    bool     acc;      // czy koprocesor powinien przyjąć
};

static uint32_t rtype(uint32_t f7, unsigned rs2, unsigned rs1, uint32_t f3, unsigned rd, uint32_t opc) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | opc;
}

static Ins mk_ccvt(unsigned rd, unsigned n, unsigned s, unsigned h, uint32_t a, uint32_t b) {
    Ins i{OP_CCVT, 0, rd, a, b, 0xDEADBEEF, 0, true};
    i.instr = rtype((h << 3) | (s << 2) | n, 2, 1, 0, rd, OPC_CUSTOM0);
    i.exp   = ref_ccvt(n, s, h, a, b);
    return i;
}
static Ins mk_cadd(unsigned rd, unsigned k, unsigned h, uint32_t a, uint32_t b) {
    Ins i{OP_CADD, 0, rd, a, b, 0xDEADBEEF, 0, true};
    i.instr = rtype((h << 2) | k, 2, 1, 1, rd, OPC_CUSTOM0);
    i.exp   = ref_cadd(k, h, a, b);
    return i;
}
static Ins mk_cmla(unsigned rd, unsigned k, unsigned c, unsigned h, uint32_t a, uint32_t b, uint32_t d) {
    Ins i{OP_CMLA, 0, rd, a, b, d, 0, true};
    // R4: [31:27] rs3, [26:25] funct2 = k, funct3 = {0, h, c}
    i.instr = (3u << 27) | (k << 25) | (2u << 20) | (1u << 15) | (((h << 1) | c) << 12) | (rd << 7) | OPC_CUSTOM1;
    i.exp   = ref_cmla(k, c, h, a, b, d);
    return i;
}
static Ins mk_bad(uint32_t instr) {
    return Ins{OP_BAD, instr, (instr >> 7) & 31, 1, 2, 3, 0, false};
}

// ===========================================================================
// Symulacja
// ===========================================================================
static Vcopro_top *top = nullptr;
#ifdef TRACE
static VerilatedVcdC *tfp = nullptr;
#endif
static uint64_t cyc    = 0;
static long     errors = 0;
static int      printed = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            errors++;                                                       \
            if (printed++ < 30) {                                           \
                printf("[takt %6llu] BLAD: ", (unsigned long long)cyc);     \
                printf(__VA_ARGS__);                                        \
                printf("\n");                                               \
            }                                                               \
        }                                                                   \
    } while (0)

static void eval() {
    top->eval();
}

// Zbocze narastające (wejścia ustawione wcześniej przy clk = 0)
static void posedge() {
#ifdef TRACE
    tfp->dump(cyc * 10 + 4);
#endif
    top->clk_i = 1;
    top->eval();
#ifdef TRACE
    tfp->dump(cyc * 10 + 5);
#endif
    top->clk_i = 0;
    top->eval();
    cyc++;
}

static void idle_inputs() {
    top->compressed_valid_i = 0;
    top->issue_valid_i      = 0;
    top->commit_valid_i     = 0;
    top->result_ready_i     = 1;
}

static void reset(int cycles) {
    idle_inputs();
    top->clk_i  = 0;
    top->rst_ni = 0;
    eval();
    for (int i = 0; i < cycles; i++) posedge();
    top->rst_ni = 1;
    eval();
}

// ===========================================================================
// Model rdzenia
// ===========================================================================
static const int NID = 16;   // 2^X_ID_WIDTH

struct Item {
    Ins  ins;
    int  force_id     = -1;    // >= 0: wymuszone id (czeka, aż będzie wolne)
    int  start        = 0;     // najwcześniejszy takt wystawienia (względem początku scenariusza)
    int  rs_delay     = 0;     // przez ile taktów rs1 nieważne
    int  commit_delay = 0;     // 0: commit w takcie handshake'u (jeśli możliwe), N: N taktów później
    bool kill         = false;
    // wyniki przebiegu (takty względem początku scenariusza)
    int     id          = -1;
    int64_t t_present   = -1;
    int64_t t_issue     = -1;
    int64_t t_commit    = -1;
    int64_t t_first_res = -1;
    int64_t t_res       = -1;
};

struct Stats {
    long issued = 0, results = 0, killed = 0, rejected = 0, max_inflight = 0, cycles = 0;
};

// Przeprowadza listę instrukcji przez koprocesor, cykl po cyklu.
//   ready     - result_ready w danym takcie
//   gate      - czy w danym takcie wolno wystawić nową instrukcję
//   reuse_low - przydzielaj najniższe wolne id (agresywne ponowne użycie)
static Stats run(std::vector<Item> &it,
                 std::function<bool(int64_t)> ready,
                 std::function<bool(int64_t)> gate,
                 std::mt19937 &rng, bool random_ids)
{
    Stats st;
    size_t next = 0;
    int    cur = -1, cur_rs_wait = 0;
    std::deque<int> commit_q;   // przyjęte przez handshake, czekają na commit
    std::deque<int> await_q;    // po commicie (kill = 0), czekają na wynik - kolejność programu
    bool   inflight[NID] = {};
    int    next_id = 0;
    const  uint64_t t0 = cyc;

    bool     prev_valid = false, prev_ready = false;
    unsigned prev_id = 0, prev_rd = 0;
    uint32_t prev_data = 0;

    for (int64_t guard = 0;; guard++) {
        const int64_t t = (int64_t)(cyc - t0);
        if (next >= it.size() && cur < 0 && commit_q.empty() && await_q.empty()) break;
        if (guard > 200 + 60 * (int64_t)it.size()) {
            CHECK(false, "timeout scenariusza (zakleszczenie?) cur=%d commit_q=%zu await_q=%zu",
                  cur, commit_q.size(), await_q.size());
            break;
        }

        std::vector<int> free_after;   // id zwalniane po tym zboczu

        // ---------------- wejścia rdzenia (clk = 0) ----------------
        top->result_ready_i = ready(t);

        // commit najstarszej instrukcji, gdy minęło jej opóźnienie
        int commit_idx = -1;
        top->commit_valid_i = 0;
        if (!commit_q.empty()) {
            Item &h = it[commit_q.front()];
            if (t >= h.t_issue + std::max(1, h.commit_delay)) {
                commit_idx = commit_q.front();
                commit_q.pop_front();
                top->commit_valid_i = 1;
                top->commit_id_i    = h.id;
                top->commit_kill_i  = h.kill;
            }
        }

        // nowa instrukcja na issue
        if (cur < 0 && next < it.size() && t >= it[next].start && gate(t)) {
            Item &x = it[next];
            int id = -1;
            if (x.force_id >= 0) {
                if (!inflight[x.force_id]) id = x.force_id;
            } else {
                int start = (random_ids && (rng() & 1)) ? 0 : next_id;
                for (int k = 0; k < NID; k++) {
                    int c = (start + k) % NID;
                    if (!inflight[c]) { id = c; break; }
                }
            }
            if (id >= 0) {
                cur = (int)next++;
                x.id = id;
                x.t_present = t;
                inflight[id] = true;
                next_id = (id + 1) % NID;
                cur_rs_wait = x.rs_delay;
            }
        }

        if (cur >= 0) {
            const Item &x = it[cur];
            top->issue_valid_i    = 1;
            top->issue_instr_i    = x.ins.instr;
            top->issue_id_i       = x.id;
            // dopóki rs1 nieważne, podajemy śmieci (nie wolno ich zatrzasnąć)
            top->issue_rs0_i      = cur_rs_wait > 0 ? 0xBADC0FFE : x.ins.rs1;
            top->issue_rs1_i      = x.ins.rs2;
            top->issue_rs2_i      = x.ins.rs3;
            top->issue_rs_valid_i = cur_rs_wait > 0 ? 0b110 : 0b111;
        } else {
            top->issue_valid_i    = 0;
            top->issue_instr_i    = (uint32_t)rng();          // śmieci przy valid = 0
            top->issue_id_i       = rng() % NID;
        }
        eval();

        // ---------------- handshake issue ----------------
        bool hs = cur >= 0 && top->issue_ready_o;
        if (hs) {
            Item &x = it[cur];
            x.t_issue = t;
            st.issued++;
            CHECK(top->issue_accept_o == x.ins.acc,
                  "%s id=%d: accept=%d, oczekiwano %d", op_name[x.ins.op], x.id, top->issue_accept_o, x.ins.acc);
            if (x.ins.acc) {
                CHECK(top->issue_writeback_o && !top->issue_loadstore_o && !top->issue_exc_o &&
                      !top->issue_dualwrite_o && !top->issue_dualread_o && !top->issue_ecswrite_o,
                      "%s id=%d: zle pola issue_resp", op_name[x.ins.op], x.id);
                CHECK(t - x.t_present >= x.rs_delay,
                      "%s id=%d: przyjeta zanim rs1 byl wazny", op_name[x.ins.op], x.id);
            } else {
                CHECK(!top->issue_writeback_o && !top->issue_loadstore_o && !top->issue_exc_o &&
                      !top->issue_dualwrite_o && !top->issue_dualread_o && !top->issue_ecswrite_o,
                      "odrzucona instrukcja 0x%08x: issue_resp != 0", x.ins.instr);
            }
            // commit w tym samym takcie (tylko gdy nie ma starszych czekających)
            if (commit_idx < 0 && commit_q.empty() && x.commit_delay == 0) {
                commit_idx = cur;
                top->commit_valid_i = 1;
                top->commit_id_i    = x.id;
                top->commit_kill_i  = x.kill;
                eval();
            } else {
                commit_q.push_back(cur);
            }
        }

        // ---------------- skutki commita ----------------
        if (commit_idx >= 0) {
            Item &c = it[commit_idx];
            c.t_commit = t;
            if (c.ins.acc && !c.kill) {
                await_q.push_back(commit_idx);
            } else {
                free_after.push_back(c.id);
                if (c.ins.acc) st.killed++; else st.rejected++;
            }
        }

        // ---------------- result ----------------
        const bool rv = top->result_valid_o, rr = top->result_ready_i;
        if (prev_valid && !prev_ready) {
            CHECK(rv, "result_valid wycofany przed result_ready");
            CHECK(rv && top->result_id_o == prev_id && top->result_rd_o == prev_rd &&
                  top->result_data_o == prev_data,
                  "pakiet result zmienil sie w trakcie transakcji");
        }
        if (rv) {
            CHECK(!await_q.empty(), "result (id=%d) gdy zadna instrukcja nie czeka na wynik",
                  top->result_id_o);
            CHECK(top->result_we_o == 1 && !top->result_exc_o && !top->result_err_o &&
                  !top->result_dbg_o && top->result_ecswe_o == 0, "zle pola pakietu result");
            if (!await_q.empty()) {
                Item &e = it[await_q.front()];
                CHECK(top->result_id_o == (unsigned)e.id,
                      "result dla id=%d, a w kolejnosci programu czeka id=%d (%s)",
                      top->result_id_o, e.id, op_name[e.ins.op]);
                if (e.t_first_res < 0) e.t_first_res = t;
                if (rr) {
                    CHECK(top->result_rd_o == e.ins.rd, "%s id=%d: rd=%d, oczekiwano %d",
                          op_name[e.ins.op], e.id, top->result_rd_o, e.ins.rd);
                    CHECK(top->result_data_o == e.ins.exp,
                          "%s id=%d: data=0x%08x, oczekiwano 0x%08x (rs1=%08x rs2=%08x rs3=%08x instr=%08x)",
                          op_name[e.ins.op], e.id, top->result_data_o, e.ins.exp,
                          e.ins.rs1, e.ins.rs2, e.ins.rs3, e.ins.instr);
                    e.t_res = t;
                    free_after.push_back(e.id);
                    await_q.pop_front();
                    st.results++;
                }
            }
        }
        prev_valid = rv; prev_ready = rr;
        prev_id = top->result_id_o; prev_rd = top->result_rd_o; prev_data = top->result_data_o;

        CHECK(!top->mem_valid_o, "mem_valid = 1");

        // ile przyjętych instrukcji jest w koprocesorze
        long inflight_acc = (long)await_q.size();
        for (int i : commit_q) if (it[i].ins.acc) inflight_acc++;
        st.max_inflight = std::max(st.max_inflight, inflight_acc);

        // ---------------- zbocze ----------------
        posedge();

        for (int id : free_after) inflight[id] = false;
        if (hs) cur = -1;
        else if (cur >= 0 && cur_rs_wait > 0) cur_rs_wait--;
    }

    idle_inputs();
    eval();
    st.cycles = (long)(cyc - t0);
    return st;
}

static std::function<bool(int64_t)> always() { return [](int64_t) { return true; }; }

// ===========================================================================
// Testy kierunkowe
// ===========================================================================
static int n_dir = 0;

static void report(const char *name, long e0) {
    n_dir++;
    printf("  %-4s %s\n", errors == e0 ? "OK" : "FAIL", name);
}

// Pojedyncza instrukcja: wynik dokładnie w takcie issue + max(latencja, opóźnienie commita)
static void single(const char *name, Ins ins, int commit_delay, bool kill, int rs_delay,
                   int ready_delay, std::mt19937 &rng)
{
    long e0 = errors;
    std::vector<Item> v(1);
    v[0].ins = ins; v[0].commit_delay = commit_delay; v[0].kill = kill; v[0].rs_delay = rs_delay;

    int64_t ready_from = -1;   // od którego taktu result_ready = 1
    if (ready_delay > 0) ready_from = rs_delay + std::max(op_lat[ins.op], commit_delay) + ready_delay;
    auto rdy = [&](int64_t t) { return ready_from < 0 || t >= ready_from; };

    run(v, rdy, always(), rng, false);

    Item &x = v[0];
    if (ins.acc && !kill) {
        int64_t exp_first = x.t_issue + std::max<int64_t>(op_lat[ins.op], x.t_commit - x.t_issue);
        CHECK(x.t_first_res == exp_first, "%s: pierwszy result_valid w takcie %lld, oczekiwano %lld",
              name, (long long)x.t_first_res, (long long)exp_first);
        if (ready_delay > 0)
            CHECK(x.t_res == ready_from, "%s: wynik odebrany w takcie %lld, oczekiwano %lld",
                  name, (long long)x.t_res, (long long)ready_from);
    } else {
        CHECK(x.t_first_res < 0, "%s: pojawil sie wynik dla odrzuconej/zabitej instrukcji", name);
    }
    report(name, e0);
}

static void directed(std::mt19937 &rng) {
    printf("=== Testy kierunkowe ===\n");

    // --- compressed: zawsze odrzucane
    {
        long e0 = errors;
        top->compressed_valid_i = 1;
        top->compressed_instr_i = 0x0001;
        eval();
        CHECK(top->compressed_ready_o && !top->compressed_accept_o, "compressed: oczekiwano ready=1, accept=0");
        posedge();
        top->compressed_valid_i = 0;
        eval();
        report("compressed: odrzucona", e0);
    }

    // --- wektory z testbencha HLS, latencja przy commicie razem z issue
    single("CCVT 12b unsigned",         mk_ccvt(10, 1, 0, 0, 0x00000FFF, 0x00000000), 0, false, 0, 0, rng);
    single("CCVT 10b signed h=1",       mk_ccvt(11, 0, 1, 1, 0x000003FF, 0x00000200), 0, false, 0, 0, rng);
    single("CCVT smieci w rs[31:16]",   mk_ccvt(12, 2, 1, 0, 0xDEADC001, 0xBEEF1FFF), 0, false, 0, 0, rng);
    single("CADD 0.5+0.5 -> sat",       mk_cadd(13, 0, 0, 0x00004000, 0x00004000), 0, false, 0, 0, rng);
    single("CADD k=3, Im(rs2)!=0",      mk_cadd(14, 3, 0, 0x00000000, 0x40000000), 0, false, 0, 0, rng);
    single("CADD h=1 AP_RND",           mk_cadd(15, 0, 1, 0x0000FFFF, 0x00000000), 0, false, 0, 0, rng);
    single("CMLA 0.5*0.5",              mk_cmla(16, 0, 0, 0, 0x00004000, 0x00004000, 0), 0, false, 0, 0, rng);
    single("CMLA 0.5j*conj(0.5j)",      mk_cmla(17, 0, 1, 0, 0x40000000, 0x40000000, 0), 0, false, 0, 0, rng);
    single("CMLA akumulacja rs3, k=1",  mk_cmla(18, 1, 0, 1, 0x00004000, 0x00004000, 0x00001000), 0, false, 0, 0, rng);

    // --- protokół
    single("CCVT commit +4 (wynik w takcie commita)", mk_ccvt(1, 1, 0, 0, 0x123, 0x456), 4, false, 0, 0, rng);
    single("CMLA commit +1 (dane pozniej niz commit)", mk_cmla(2, 2, 0, 0, 0x7FFF8000, 0x12345678, 0x0F0F), 1, false, 0, 0, rng);
    single("CADD commit +5",            mk_cadd(3, 1, 1, 0x80007FFF, 0x7FFF8000), 5, false, 0, 0, rng);
    single("CCVT zabita w takcie issue", mk_ccvt(4, 0, 0, 0, 1, 2), 0, true, 0, 0, rng);
    single("CMLA zabita takt pozniej",  mk_cmla(5, 0, 0, 0, 1, 2, 3), 1, true, 0, 0, rng);
    single("CADD zabita po wyniku z HLS", mk_cadd(6, 0, 0, 1, 2), 4, true, 0, 0, rng);
    single("CMLA hazard rs1 (2 takty)", mk_cmla(7, 3, 1, 1, 0x11112222, 0x33334444, 0x55556666), 0, false, 2, 0, rng);
    single("CCVT backpressure 3 takty", mk_ccvt(8, 3, 1, 1, 0x8001, 0x7FFF), 0, false, 0, 3, rng);
    single("CMLA backpressure + commit +2", mk_cmla(9, 1, 1, 0, 0xC0004000, 0x2000E000, 0x10001000), 2, false, 0, 2, rng);

    // --- odrzucane
    single("zly bit zarezerwowany CCVT [29]", mk_bad(mk_ccvt(1, 0, 0, 0, 0, 0).instr | (1u << 29)), 0, false, 0, 0, rng);
    single("zly bit zarezerwowany CADD [28]", mk_bad(mk_cadd(1, 0, 0, 0, 0).instr | (1u << 28)), 0, false, 0, 0, rng);
    single("custom-0 funct3=010",       mk_bad(rtype(0, 2, 1, 2, 1, OPC_CUSTOM0)), 0, false, 0, 0, rng);
    single("CMLA funct3[2]=1",          mk_bad(mk_cmla(1, 0, 0, 0, 0, 0, 0).instr | (4u << 12)), 0, false, 0, 0, rng);
    single("opcode custom-2",           mk_bad(rtype(0, 2, 1, 0, 1, OPC_CUSTOM2)), 0, false, 0, 0, rng);
    single("odrzucona + kill (ignorowany)", mk_bad(rtype(0, 2, 1, 7, 1, OPC_CUSTOM0)), 1, true, 0, 0, rng);

    // --- trzy jednostki kończą w tym samym takcie: CMLA, CADD, CCVT w kolejnych taktach
    {
        long e0 = errors;
        std::vector<Item> v(3);
        v[0].ins = mk_cmla(20, 1, 0, 0, 0x10002000, 0x30004000, 0x00500060); v[0].start = 0;
        v[1].ins = mk_cadd(21, 2, 1, 0x7FFF7FFF, 0x80008000);                v[1].start = 1;
        v[2].ins = mk_ccvt(22, 1, 0, 1, 0x00000ABC, 0x00000123);             v[2].start = 2;
        run(v, always(), always(), rng, false);
        int64_t done = v[0].t_issue + 3;
        CHECK(v[1].t_issue + 2 == done && v[2].t_issue + 1 == done,
              "kolizja: jednostki nie skonczyly w tym samym takcie (issue %lld %lld %lld)",
              (long long)v[0].t_issue, (long long)v[1].t_issue, (long long)v[2].t_issue);
        CHECK(v[0].t_res == done && v[1].t_res == done + 1 && v[2].t_res == done + 2,
              "kolizja: wyniki w taktach %lld %lld %lld, oczekiwano %lld %lld %lld",
              (long long)v[0].t_res, (long long)v[1].t_res, (long long)v[2].t_res,
              (long long)done, (long long)done + 1, (long long)done + 2);
        report("3 wyniki w jednym takcie -> po kolei", e0);
    }

    // --- przepustowość: 8 x CCVT jedna po drugiej, wynik co takt
    {
        long e0 = errors;
        std::vector<Item> v(8);
        for (int i = 0; i < 8; i++) v[i].ins = mk_ccvt(i + 1, i & 3, i & 1, (i >> 1) & 1, 0x100 * i, 0x1000 + i);
        run(v, always(), always(), rng, false);
        for (int i = 1; i < 8; i++) {
            CHECK(v[i].t_issue == v[i - 1].t_issue + 1, "przepustowosc: issue %d nie w kolejnym takcie", i);
            CHECK(v[i].t_res == v[i - 1].t_res + 1,     "przepustowosc: wynik %d nie w kolejnym takcie", i);
        }
        report("8 x CCVT: 1 instrukcja / takt", e0);
    }

    // --- ponowne użycie id zabitej CMLA, która jest jeszcze w potoku HLS
    {
        long e0 = errors;
        std::vector<Item> v(2);
        v[0].ins = mk_cmla(23, 0, 0, 0, 0x40004000, 0x40004000, 0x11111111);
        v[0].force_id = 5; v[0].commit_delay = 1; v[0].kill = true;
        v[1].ins = mk_ccvt(24, 1, 1, 0, 0x00000555, 0x00000AAA);
        v[1].force_id = 5; v[1].start = 1;
        run(v, always(), always(), rng, false);
        // CMLA: issue t0, kill t1, wynik wychodzi z HLS w t3 -> id wolne od t4
        CHECK(v[1].t_present == 2 && v[1].t_issue == 4,
              "reuse id: CCVT wystawiona w t%lld, przyjeta w t%lld (oczekiwano t2, t4)",
              (long long)v[1].t_present, (long long)v[1].t_issue);
        report("reuse id zabitej CMLA: wstrzymanie 2 takty, poprawny wynik", e0);
    }
}

// ===========================================================================
// Test losowy
// ===========================================================================
static uint16_t rnd_half(std::mt19937 &rng) {
    static const uint16_t corners[] = {0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x4000, 0xC000};
    uint32_t r = rng();
    return (r & 3) == 0 ? corners[(r >> 2) & 7] : (uint16_t)(r >> 16);
}
static uint32_t rnd_word(std::mt19937 &rng) {
    uint32_t hi = rnd_half(rng), lo = rnd_half(rng);
    return (hi << 16) | lo;
}

static Ins rnd_ins(std::mt19937 &rng) {
    unsigned rd = rng() % 32;
    uint32_t a = rnd_word(rng), b = rnd_word(rng), c = rnd_word(rng);
    switch (rng() % 10) {
        case 0: case 1: case 2: return mk_ccvt(rd, rng() & 3, rng() & 1, rng() & 1, a, b);
        case 3: case 4: case 5: return mk_cadd(rd, rng() & 3, rng() & 1, a, b);
        case 6: case 7: case 8: return mk_cmla(rd, rng() & 3, rng() & 1, rng() & 1, a, b, c);
        default: {
            static const uint32_t bad[] = {
                rtype(0x10, 2, 1, 0, 0, OPC_CUSTOM0),   // CCVT, bit zarezerwowany
                rtype(0x08, 2, 1, 1, 0, OPC_CUSTOM0),   // CADD, bit zarezerwowany
                rtype(0,    2, 1, 5, 0, OPC_CUSTOM0),   // nieznane funct3
                rtype(0,    2, 1, 4, 0, OPC_CUSTOM1),   // CMLA, funct3[2] = 1
                rtype(0,    2, 1, 0, 0, OPC_CUSTOM2),   // inny opcode
            };
            return mk_bad(bad[rng() % 5] | (rd << 7));
        }
    }
}

static void random_test(long n, std::mt19937 &rng) {
    printf("=== Test losowy: %ld instrukcji ===\n", n);
    long e0 = errors;
    std::vector<Item> v((size_t)n);
    for (auto &x : v) {
        x.ins          = rnd_ins(rng);
        x.kill         = (rng() % 7) == 0;
        x.rs_delay     = (rng() % 4) == 0 ? 1 + rng() % 2 : 0;
        x.commit_delay = rng() % 4;
    }
    // fazy: szybkie (result_ready 70%) i wolne (10%) na zmianę co 64 takty
    auto rdy  = [&](int64_t t) { return ((t / 64) % 2 == 0) ? (rng() % 10) < 7 : (rng() % 10) < 1; };
    auto gate = [&](int64_t)   { return (rng() % 4) != 0; };
    Stats st = run(v, rdy, gate, rng, true);
    printf("  %-4s %ld przyjetych przez handshake, %ld wynikow, %ld zabitych, %ld odrzuconych,\n"
           "       max. %ld w locie, %ld taktow\n",
           errors == e0 ? "OK" : "FAIL", st.issued, st.results, st.killed, st.rejected,
           st.max_inflight, st.cycles);
    CHECK(st.max_inflight >= 3, "test losowy: za malo instrukcji naraz w locie (%ld)", st.max_inflight);
}

// ===========================================================================
int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::randReset(2);   // losowy stan początkowy (rejestry HLS nie mają resetu)

    unsigned seed = 1;
    long     n    = 20000;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "+seed=", 6)) seed = (unsigned)strtoul(argv[i] + 6, nullptr, 0);
        if (!strncmp(argv[i], "+n=", 3))    n    = strtol(argv[i] + 3, nullptr, 0);
    }
    std::mt19937 rng(seed);

    top = new Vcopro_top;
#ifdef TRACE
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("wave.vcd");
#endif

    printf("==============================================\n");
    printf(" Koprocesor CCVT/CADD/CMLA, seed = %u\n", seed);
    printf("==============================================\n");

    reset(5);
    directed(rng);
    random_test(n, rng);

    printf("----------------------------------------------\n");
    if (errors == 0) printf("*** PASS: %d testow kierunkowych + test losowy, 0 bledow ***\n", n_dir);
    else             printf("*** FAIL: %ld bledow ***\n", errors);

#ifdef TRACE
    tfp->close();
#endif
    top->final();
    delete top;
    return errors == 0 ? 0 : 1;
}