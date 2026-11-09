// ============================================================================
// tb_top.cpp - testbench dla:
//   top_0t()  CCVT   (LATENCY 0)
//   top_1t()  CADD   (LATENCY 1)
//   top_2t()  CMLA   (LATENCY 2)
//
// Model referencyjny jest napisany w czystej arytmetyce calkowitej (int64),
// niezaleznie od ap_fixed, wiec porownanie z DUT jest bit-exact.
//
// Wybor testowanego DUT (makro w CFLAGS testbencha, zgodne z syn.top):
//   -DTB_DUT_0T  - tylko top_0t (CCVT)   -> syn.top=top_0t
//   -DTB_DUT_1T  - tylko top_1t (CADD)   -> syn.top=top_1t
//   -DTB_DUT_2T  - tylko top_2t (CMLA)   -> syn.top=top_2t
//   brak makra   - wszystkie trzy (tylko do C-sim!)
//
// Tryby:
//   domyslnie   - pelny test (C-sim): testy reczne + sweep naroznikow +
//                 wyczerpujacy CCVT + 200k losowych wektorow
//   -DTB_LIGHT  - lekki test (do C/RTL co-simulation)
// ============================================================================
#include <cstdio>
#include <cstdint>
#include <random>
#include <ap_int.h>

// ---------------------------------------------------------------------------
// Wybor DUT
// ---------------------------------------------------------------------------
#if (defined(TB_DUT_0T) + defined(TB_DUT_1T) + defined(TB_DUT_2T)) > 1
  #error "Zdefiniuj co najwyzej jedno z: TB_DUT_0T, TB_DUT_1T, TB_DUT_2T"
#endif

#if defined(TB_DUT_0T)
  #define TEST_0T 1
  #define TEST_1T 0
  #define TEST_2T 0
  #define DUT_NAME "top_0t (CCVT)"
#elif defined(TB_DUT_1T)
  #define TEST_0T 0
  #define TEST_1T 1
  #define TEST_2T 0
  #define DUT_NAME "top_1t (CADD)"
#elif defined(TB_DUT_2T)
  #define TEST_0T 0
  #define TEST_1T 0
  #define TEST_2T 1
  #define DUT_NAME "top_2t (CMLA)"
#else
  #define TEST_0T 1
  #define TEST_1T 1
  #define TEST_2T 1
  #define DUT_NAME "top_0t + top_1t + top_2t (tylko C-sim)"
#endif

// ---------------------------------------------------------------------------
// Deklaracje musza byc IDENTYCZNE jak w pliku zrodlowym.
// (Czysciej: przenies struct dataIn/dataOut + prototypy do top.h
//  i dolacz go w obu plikach - wtedy niezgodnosc wyjdzie przy kompilacji.)
// ---------------------------------------------------------------------------
struct dataIn {
    ap_uint<2> n;
    ap_uint<1> s;
    ap_uint<2> k;
    ap_uint<1> c;
    ap_uint<1> h;
    ap_uint<32> rs1;
    ap_uint<32> rs2;
    ap_uint<32> rs3;
};

struct dataOut {
    ap_uint<32> data;
};

#if TEST_0T
void top_0t(dataIn in, dataOut &outCCVT);
#endif
#if TEST_1T
void top_1t(dataIn in, dataOut &outCADD);
#endif
#if TEST_2T
void top_2t(dataIn in, dataOut &outCMLA);
#endif

// ---------------------------------------------------------------------------
// Konfiguracja
// ---------------------------------------------------------------------------
#ifdef TB_LIGHT
static const long N_RANDOM        = 2000;
static const int  SWEEP_STRIDE    = 8;
static const bool CCVT_EXHAUSTIVE = false;
#else
static const long N_RANDOM        = 200000;
static const int  SWEEP_STRIDE    = 1;
static const bool CCVT_EXHAUSTIVE = true;
#endif
static const int MAX_PRINT = 20;

// ---------------------------------------------------------------------------
// Pomocnicze
// ---------------------------------------------------------------------------
static inline int64_t lo16(uint32_t x) { return (int16_t)(x & 0xFFFF); }
static inline int64_t hi16(uint32_t x) { return (int16_t)(x >> 16); }

static inline uint32_t pack(int64_t re, int64_t im) {
    return ((uint32_t)(uint16_t)im << 16) | (uint16_t)re;
}

// saturacja do Q15 (AP_SAT)
static inline int64_t sat16(int64_t v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

// floor(x / 2^sh) - niezalezne od implementacji >> dla liczb ujemnych
static inline int64_t floor_shift(int64_t x, int sh) {
    if (sh == 0) return x;
    return x >= 0 ? (x >> sh) : -((-x + (int64_t(1) << sh) - 1) >> sh);
}

// AP_RND: zaokraglenie do najblizszej, remisy w strone +inf = floor(x/2^sh + 1/2)
static inline int64_t rnd_shift(int64_t x, int sh) {
    if (sh == 0) return x;
    return floor_shift(x + (int64_t(1) << (sh - 1)), sh);
}

// mnozenie przez j^k
static inline void rot_k(unsigned k, int64_t re, int64_t im, int64_t &o_re, int64_t &o_im) {
    switch (k & 3) {
        case 0:  o_re =  re; o_im =  im; break;
        case 1:  o_re = -im; o_im =  re; break;
        case 2:  o_re = -re; o_im = -im; break;
        default: o_re =  im; o_im = -re; break;
    }
}

// ---------------------------------------------------------------------------
// Modele referencyjne
// ---------------------------------------------------------------------------
// CCVT: rs1/rs2 obcinane do 16 bitow (parametry ap_uint<16>)
static uint32_t ref_ccvt(unsigned n, unsigned s, unsigned h, uint32_t rs1, uint32_t rs2) {
    static const int sh[4] = {6, 4, 2, 0};   // BIT10, BIT12, BIT14, BIT16
    uint16_t v[2] = {(uint16_t)(rs1 & 0xFFFF), (uint16_t)(rs2 & 0xFFFF)};
    for (int i = 0; i < 2; i++) {
        v[i] = (uint16_t)(v[i] << sh[n & 3]);
        if (!s) v[i] ^= 0x8000;                                 // offset-binary -> U2
        if (h)  v[i] = (uint16_t)floor_shift((int16_t)v[i], 1); // shift arytmetyczny
    }
    return ((uint32_t)v[1] << 16) | v[0];
}

// CADD: rs1 + j^k * rs2, opcjonalnie /2 (AP_RND), saturacja Q15
static uint32_t ref_cadd(unsigned k, unsigned h, uint32_t rs1, uint32_t rs2) {
    int64_t r_re, r_im;
    rot_k(k, lo16(rs2), hi16(rs2), r_re, r_im);
    int64_t s_re = lo16(rs1) + r_re;   // jednostka 2^-15
    int64_t s_im = hi16(rs1) + r_im;
    if (h) {
        s_re = rnd_shift(s_re, 1);
        s_im = rnd_shift(s_im, 1);
    }
    return pack(sat16(s_re), sat16(s_im));
}

// CMLA: rs3 + j^k * (rs1 * rs2) lub rs3 + j^k * (rs1 * conj(rs2)) gdy c=1
static uint32_t ref_cmla(unsigned k, unsigned c, unsigned h,
                         uint32_t rs1, uint32_t rs2, uint32_t rs3) {
    int64_t ar = lo16(rs1), ai = hi16(rs1);
    int64_t br = lo16(rs2), bi = hi16(rs2);
    int64_t m_re, m_im;                       // jednostka 2^-30
    if (!c) { m_re = ar * br - ai * bi; m_im = ar * bi + ai * br; }
    else    { m_re = ar * br + ai * bi; m_im = ai * br - ar * bi; }

    int64_t r_re, r_im;
    rot_k(k, m_re, m_im, r_re, r_im);

    int64_t t_re = lo16(rs3) * 32768 + r_re;  // rs3 do jednostki 2^-30
    int64_t t_im = hi16(rs3) * 32768 + r_im;

    int sh = h ? 16 : 15;                     // 2^-30 -> 2^-15 (i ewentualnie /2)
    return pack(sat16(rnd_shift(t_re, sh)), sat16(rnd_shift(t_im, sh)));
}

// ---------------------------------------------------------------------------
// Infrastruktura testu
// ---------------------------------------------------------------------------
enum { F_CCVT = 0, F_CADD = 1, F_CMLA = 2 };
static const char *fn_name[3] = {"CCVT", "CADD", "CMLA"};

static long g_vectors  = 0;         // liczba wektorow wejsciowych
static long g_calls[3] = {0, 0, 0}; // wywolania top_0t / top_1t / top_2t
static long g_errors   = 0;
static int  g_printed  = 0;

static dataIn make_in(unsigned n, unsigned s, unsigned k, unsigned c, unsigned h,
                      uint32_t rs1, uint32_t rs2, uint32_t rs3) {
    dataIn in;
    in.n = n; in.s = s; in.k = k; in.c = c; in.h = h;
    in.rs1 = rs1; in.rs2 = rs2; in.rs3 = rs3;
    return in;
}

static void report(const char *tag, const char *fn, const dataIn &in,
                   uint32_t got, uint32_t exp) {
    g_errors++;
    if (g_printed++ < MAX_PRINT) {
        printf("BLAD [%s] %s: n=%u s=%u k=%u c=%u h=%u rs1=%08X rs2=%08X rs3=%08X\n"
               "      got=%08X (Re=%6d Im=%6d)  exp=%08X (Re=%6d Im=%6d)\n",
               tag, fn,
               in.n.to_uint(), in.s.to_uint(), in.k.to_uint(), in.c.to_uint(), in.h.to_uint(),
               in.rs1.to_uint(), in.rs2.to_uint(), in.rs3.to_uint(),
               got, (int)lo16(got), (int)hi16(got),
               exp, (int)lo16(exp), (int)hi16(exp));
    } else if (g_printed == MAX_PRINT + 1) {
        printf("... (kolejne bledy nie sa wypisywane)\n");
    }
}

static uint32_t ref_fn(int fn, const dataIn &in) {
    unsigned n = in.n.to_uint(), s = in.s.to_uint(), k = in.k.to_uint();
    unsigned c = in.c.to_uint(), h = in.h.to_uint();
    uint32_t rs1 = in.rs1.to_uint(), rs2 = in.rs2.to_uint(), rs3 = in.rs3.to_uint();
    switch (fn) {
        case F_CCVT: return ref_ccvt(n, s, h, rs1, rs2);
        case F_CADD: return ref_cadd(k, h, rs1, rs2);
        default:     return ref_cmla(k, c, h, rs1, rs2, rs3);
    }
}

// Czy funkcja fn jest testowana w tej kompilacji
static bool fn_enabled(int fn) {
    switch (fn) {
        case F_CCVT: return TEST_0T;
        case F_CADD: return TEST_1T;
        default:     return TEST_2T;
    }
}

// Wywoluje DUT odpowiedzialny za funkcje fn; false gdy ten DUT nie jest testowany
static bool dut_call(int fn, const dataIn &in, uint32_t &got) {
    dataOut o;
    switch (fn) {
        case F_CCVT:
#if TEST_0T
            top_0t(in, o);
            break;
#else
            return false;
#endif
        case F_CADD:
#if TEST_1T
            top_1t(in, o);
            break;
#else
            return false;
#endif
        default:
#if TEST_2T
            top_2t(in, o);
            break;
#else
            return false;
#endif
    }
    g_calls[fn]++;
    got = o.data.to_uint();
    return true;
}

// Uruchamia wszystkie testowane DUT na jednym wektorze i porownuje z modelem
static void run_vector(const dataIn &in, const char *tag) {
    g_vectors++;
    for (int fn = F_CCVT; fn <= F_CMLA; fn++) {
        uint32_t got;
        if (!dut_call(fn, in, got)) continue;
        uint32_t exp = ref_fn(fn, in);
        if (got != exp) report(tag, fn_name[fn], in, got, exp);
    }
}

// ---------------------------------------------------------------------------
// 1. Testy reczne - wartosci policzone recznie (sprawdzaja DUT i model)
// ---------------------------------------------------------------------------
struct Directed {
    const char *name;
    unsigned n, s, k, c, h;
    uint32_t rs1, rs2, rs3;
    int      fn;
    uint32_t expected;
};

static const Directed directed[] = {
    // ---- CCVT ----
    {"CCVT 12b unsigned",            1,0,0,0,0, 0x00000FFF, 0x00000000, 0, F_CCVT, 0x80007FF0},
    {"CCVT 10b signed, h=1",         0,1,0,0,1, 0x000003FF, 0x00000200, 0, F_CCVT, 0xC000FFE0},
    {"CCVT 16b unsigned",            3,0,0,0,0, 0x00008000, 0x0000FFFF, 0, F_CCVT, 0x7FFF0000},
    {"CCVT 14b, obciecie gornych",   2,1,0,0,0, 0x0000C001, 0x00001FFF, 0, F_CCVT, 0x7FFC0004},
    // ---- CADD ----
    {"CADD 0.5+0.5 -> sat",          0,0,0,0,0, 0x00004000, 0x00004000, 0, F_CADD, 0x00007FFF},
    {"CADD (0.5+0.5)/2",             0,0,0,0,1, 0x00004000, 0x00004000, 0, F_CADD, 0x00004000},
    {"CADD k=1 (x j)",               0,0,1,0,0, 0x00000000, 0x00004000, 0, F_CADD, 0x40000000},
    {"CADD k=2 -(-1) -> sat",        0,0,2,0,0, 0x00000000, 0x00008000, 0, F_CADD, 0x00007FFF},
    {"CADD k=3 (x -j)",              0,0,3,0,0, 0x00000000, 0x00004000, 0, F_CADD, 0xC0000000},
    {"CADD k=1, Im(rs2)!=0",         0,0,1,0,0, 0x00000000, 0x40000000, 0, F_CADD, 0x0000C000},
    {"CADD k=3, Im(rs2)!=0",         0,0,3,0,0, 0x00000000, 0x40000000, 0, F_CADD, 0x00004000},
    {"CADD -1 + -1 -> sat",          0,0,0,0,0, 0x00008000, 0x00008000, 0, F_CADD, 0x00008000},
    {"CADD (-1 + -1)/2",             0,0,0,0,1, 0x00008000, 0x00008000, 0, F_CADD, 0x00008000},
    {"CADD h=1 AP_RND +0.5 LSB",     0,0,0,0,1, 0x00000001, 0x00000000, 0, F_CADD, 0x00000001},
    {"CADD h=1 AP_RND -0.5 LSB",     0,0,0,0,1, 0x0000FFFF, 0x00000000, 0, F_CADD, 0x00000000},
    // ---- CMLA ----
    {"CMLA 0.5*0.5",                 0,0,0,0,0, 0x00004000, 0x00004000, 0x00000000, F_CMLA, 0x00002000},
    {"CMLA (-1)*(-1) -> sat",        0,0,0,0,0, 0x00008000, 0x00008000, 0x00000000, F_CMLA, 0x00007FFF},
    {"CMLA (-1)*(-1)/2",             0,0,0,0,1, 0x00008000, 0x00008000, 0x00000000, F_CMLA, 0x00004000},
    {"CMLA 0.5j * 0.5j",             0,0,0,0,0, 0x40000000, 0x40000000, 0x00000000, F_CMLA, 0x0000E000},
    {"CMLA 0.5j * conj(0.5j)",       0,0,0,1,0, 0x40000000, 0x40000000, 0x00000000, F_CMLA, 0x00002000},
    {"CMLA akumulacja rs3",          0,0,0,0,0, 0x00004000, 0x00004000, 0x00001000, F_CMLA, 0x00003000},
    {"CMLA k=1",                     0,0,1,0,0, 0x00004000, 0x00004000, 0x00000000, F_CMLA, 0x20000000},
    {"CMLA AP_RND +0.5 LSB",         0,0,0,0,0, 0x00000001, 0x00004000, 0x00000000, F_CMLA, 0x00000001},
    {"CMLA AP_RND -0.5 LSB",         0,0,0,0,0, 0x0000FFFF, 0x00004000, 0x00000000, F_CMLA, 0x00000000},
    {"CMLA ujemna saturacja",        0,0,0,0,0, 0x00008000, 0x00007FFF, 0x00008000, F_CMLA, 0x00008000},
};

static void test_directed() {
    long err0 = g_errors;
    int  run = 0, skipped = 0;
    for (const Directed &d : directed) {
        dataIn in = make_in(d.n, d.s, d.k, d.c, d.h, d.rs1, d.rs2, d.rs3);

        // sprawdzenie samego modelu (zawsze, niezaleznie od DUT)
        uint32_t model = ref_fn(d.fn, in);
        if (model != d.expected) report(d.name, "MODEL", in, model, d.expected);

        // sprawdzenie DUT - tylko jesli testujemy odpowiedni modul
        uint32_t got;
        if (!dut_call(d.fn, in, got)) { skipped++; continue; }
        g_vectors++;
        run++;
        if (got != d.expected) report(d.name, fn_name[d.fn], in, got, d.expected);
    }
    printf("[1] Testy reczne:        %3d przypadkow (pominietych %d), bledow: %ld\n",
           run, skipped, g_errors - err0);
}

// ---------------------------------------------------------------------------
// 2. Sweep wartosci naroznikowych (wszystkie k,c,h; n,s rotowane)
// ---------------------------------------------------------------------------
static void test_corners() {
    static const uint16_t corners[] = {0x0000, 0x0001, 0x7FFF, 0x8000,
                                       0x8001, 0xFFFF, 0x4000, 0xC000};
    const int NC = sizeof(corners) / sizeof(corners[0]);
    uint32_t cplx[NC * NC];
    for (int r = 0; r < NC; r++)
        for (int i = 0; i < NC; i++)
            cplx[r * NC + i] = ((uint32_t)corners[i] << 16) | corners[r];
    const int N = NC * NC;

    long err0 = g_errors, vec0 = g_vectors;
    for (unsigned ctrl = 0; ctrl < 16; ctrl++) {
        unsigned k = ctrl & 3, c = (ctrl >> 2) & 1, h = (ctrl >> 3) & 1;
        if (c && !TEST_2T) continue;          // c uzywa tylko CMLA - nie dubluj wektorow
        if (k && !TEST_1T && !TEST_2T) continue; // k nie wplywa na CCVT
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j += SWEEP_STRIDE) {
                unsigned n = (i + j) & 3;
                unsigned s = ((i >> 1) ^ j) & 1;
                run_vector(make_in(n, s, k, c, h, cplx[i], cplx[j], cplx[(i * 7 + j) % N]),
                           "corners");
            }
    }
    printf("[2] Sweep naroznikow:    %6ld wektorow, bledow: %ld\n",
           g_vectors - vec0, g_errors - err0);
}

// ---------------------------------------------------------------------------
// 3. Wyczerpujacy test CCVT (wszystkie 16-bitowe wartosci x n,s,h)
// ---------------------------------------------------------------------------
static void test_ccvt_exhaustive() {
    if (!fn_enabled(F_CCVT)) {
        printf("[3] CCVT wyczerpujacy:   pominiety (top_0t nie jest testowany)\n");
        return;
    }
    if (!CCVT_EXHAUSTIVE) {
        printf("[3] CCVT wyczerpujacy:   pominiety (TB_LIGHT)\n");
        return;
    }
    long err0 = g_errors, vec0 = g_vectors;
    for (unsigned ctrl = 0; ctrl < 8; ctrl++) {
        unsigned n = ctrl & 3, s = (ctrl >> 2) & 1;
        for (unsigned h = 0; h < 2; h++)
            for (uint32_t v = 0; v < 0x10000; v++) {
                uint32_t rs1 = v | 0xA5A50000u;          // smieci w gornej polowie
                uint32_t rs2 = (~v & 0xFFFF) | 0x5A5A0000u;
                dataIn in = make_in(n, s, v & 3, (v >> 2) & 1, h, rs1, rs2, v * 2654435761u);
                uint32_t got;
                dut_call(F_CCVT, in, got);
                g_vectors++;
                uint32_t exp = ref_fn(F_CCVT, in);
                if (got != exp) report("ccvt_exh", "CCVT", in, got, exp);
            }
    }
    printf("[3] CCVT wyczerpujacy:   %6ld wektorow, bledow: %ld\n",
           g_vectors - vec0, g_errors - err0);
}

// ---------------------------------------------------------------------------
// 4. Testy losowe (z domieszka wartosci naroznikowych)
// ---------------------------------------------------------------------------
static void test_random() {
    static const uint16_t corners[] = {0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFF};
    std::mt19937 rng(12345);   // staly seed -> powtarzalnosc
    auto half = [&]() -> uint16_t {
        uint32_t r = rng();
        return (r & 3) == 0 ? corners[(r >> 2) % 6] : (uint16_t)(r >> 16);
    };
    auto word = [&]() -> uint32_t { return ((uint32_t)half() << 16) | half(); };

    long err0 = g_errors, vec0 = g_vectors;
    for (long t = 0; t < N_RANDOM; t++) {
        uint32_t ctrl = rng();
        uint32_t a = word(), b = word(), c3 = word();   // kolejnosc wywolan rng ustalona
        run_vector(make_in(ctrl & 3, (ctrl >> 2) & 1, (ctrl >> 3) & 3,
                           (ctrl >> 5) & 1, (ctrl >> 6) & 1, a, b, c3),
                   "random");
    }
    printf("[4] Testy losowe:        %6ld wektorow, bledow: %ld\n",
           g_vectors - vec0, g_errors - err0);
}

// ---------------------------------------------------------------------------
int main() {
    printf("==============================================\n");
    printf(" Testbench DUT: %s\n", DUT_NAME);
#ifdef TB_LIGHT
    printf(" Tryb: TB_LIGHT\n");
#endif
    printf("==============================================\n");

    test_directed();
    test_corners();
    test_ccvt_exhaustive();
    test_random();

    printf("----------------------------------------------\n");
    printf("Wywolania: top_0t=%ld, top_1t=%ld, top_2t=%ld\n",
           g_calls[F_CCVT], g_calls[F_CADD], g_calls[F_CMLA]);
    printf("Lacznie wektorow: %ld, bledow: %ld\n", g_vectors, g_errors);
    if (g_errors == 0) {
        printf("*** TEST PASSED ***\n");
        return 0;
    }
    printf("*** TEST FAILED ***\n");
    return 1;   // Vitis HLS traktuje != 0 jako porazke C-sim / co-sim
}