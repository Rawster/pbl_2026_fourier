#include <stdint.h>
#include <cstdio>
#include <cmath>
#include <complex>

#include <ap_fixed.h>
#include <ap_int.h>

#define Q15   ap_fixed<16, 1, AP_RND, AP_SAT>
#define Q2_15 ap_fixed<17, 2>
#define Q2_16 ap_fixed<18, 2>
#define Q3_30 ap_fixed<33, 3>
#define Q4_31 ap_fixed<35, 4>

constexpr uint32_t BIT10 = 0;
constexpr uint32_t BIT12 = 1;
constexpr uint32_t BIT14 = 2;
constexpr uint32_t BIT16 = 3;

ap_uint<32> CCVT(ap_uint<2> n, ap_uint<1> s, ap_uint<1> h, ap_uint<16> rs1, ap_uint<16> rs2) {
    ap_uint<16> val_0;
    ap_uint<16> val_1;
    if (BIT10 == n) {
        val_0 = rs1 << 6;
        val_1 = rs2 << 6;
    } else if (BIT12 == n) {
        val_0 = rs1 << 4;
        val_1 = rs2 << 4;
    } else if (BIT14 == n) {
        val_0 = rs1 << 2;
        val_1 = rs2 << 2;
    } else {
        val_0 = rs1;
        val_1 = rs2;
    }

    if (!s) {
        val_0 ^= ap_uint<16>(0x8000);
        val_1 ^= ap_uint<16>(0x8000);
    }

    ap_uint<32> temp;
    if (h) {
        val_0 = ap_int<16>(val_0) >> 1;
        val_1 = ap_int<16>(val_1) >> 1;
    }
    temp.range(31, 16) = val_1.range(15, 0);
    temp.range(15, 0)  = val_0.range(15, 0);

    return temp;
}

uint32_t CADD(ap_uint<2> k, ap_uint<1> h, ap_uint<32> rs1, ap_uint<32> rs2) {
    Q15 rs1_Re, rs1_Im, rs2_Re, rs2_Im;

    rs1_Re.range(15, 0) = rs1.range(15, 0);
    rs1_Im.range(15, 0) = rs1.range(31,16);
    rs2_Re.range(15, 0) = rs2.range(15, 0);
    rs2_Im.range(15, 0) = rs2.range(31,16);

    Q2_15 rot_Re = rs2_Re;
    Q2_15 rot_Im = rs2_Im;

    if (k == 1) {
        rot_Re = -rs2_Im;
        rot_Im = rs2_Re;
    } else if (k == 2) {
        rot_Re = -rs2_Re;
        rot_Im = -rs2_Im;
    } else if (k == 3) {
        rot_Re = rs2_Im;
        rot_Im = -rs2_Re;
    }

    Q2_15 temp_Re = rs1_Re + rot_Re;
    Q2_15 temp_Im = rs1_Im + rot_Im;

    if (h) {
        temp_Re = static_cast<Q15>(static_cast<Q2_16>(temp_Re) >> 1);
        temp_Im = static_cast<Q15>(static_cast<Q2_16>(temp_Im) >> 1);
    }

    Q15 out_Re = static_cast<Q15>(temp_Re);
    Q15 out_Im = static_cast<Q15>(temp_Im);

    ap_uint<32> out;
    out.range(31, 16) = out_Im.range(15, 0);
    out.range(15, 0)  = out_Re.range(15, 0);

    return out;
}

uint32_t CMLA(ap_uint<2> k, ap_uint<1> c, ap_uint<1> h, ap_uint<32> rs1, ap_uint<32> rs2, ap_uint<32> rs3) {
    #pragma HLS inline
    
    Q15 rs1_Re, rs1_Im, rs2_Re, rs2_Im, rs3_Re, rs3_Im;

    rs1_Re.range(15, 0) = rs1.range(15, 0);
    rs1_Im.range(15, 0) = rs1.range(31,16);
    rs2_Re.range(15, 0) = rs2.range(15, 0);
    rs2_Im.range(15, 0) = rs2.range(31,16);
    rs3_Re.range(15, 0) = rs3.range(15, 0);
    rs3_Im.range(15, 0) = rs3.range(31,16);

    auto Re_Re = rs1_Re * rs2_Re; // Q2.30
    auto Im_Im = rs1_Im * rs2_Im; // Q2.30
    auto Re_Im = rs1_Re * rs2_Im; // Q2.30
    auto Im_Re = rs1_Im * rs2_Re; // Q2.30

    Q3_30 mult_Re, mult_Im;
    if (!c) {
        mult_Re = Re_Re - Im_Im;
        mult_Im = Re_Im + Im_Re;
    } else {
        mult_Re = Re_Re + Im_Im;
        mult_Im = Im_Re - Re_Im;
    }

    Q3_30 rot_Re = mult_Re;
    Q3_30 rot_Im = mult_Im;
    if (k == 1) {
        rot_Re = -mult_Im;
        rot_Im = mult_Re;
    } else if (k == 2) {
        rot_Re = -mult_Re;
        rot_Im = -mult_Im;
    } else if (k == 3) {
        rot_Re = mult_Im;
        rot_Im = -mult_Re;
    }

    auto temp_Re = static_cast<Q3_30>(rs3_Re) + rot_Re; // Q4.30
    auto temp_Im = static_cast<Q3_30>(rs3_Im) + rot_Im; // Q4.30

    Q15 out_Re;
    Q15 out_Im;
    if (h) {
        out_Re = static_cast<Q15>((static_cast<Q4_31>(temp_Re) >> 1));
        out_Im = static_cast<Q15>((static_cast<Q4_31>(temp_Im) >> 1));
    } else {
        out_Re = static_cast<Q15>(temp_Re);
        out_Im = static_cast<Q15>(temp_Im);
    }

    ap_uint<32> out;
    out.range(31, 16) = out_Im.range(15, 0);
    out.range(15, 0)  = out_Re.range(15, 0);

    return out;
}

struct dataIn {
    ap_uint<4> id;
    ap_uint<1> vld;
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
    ap_uint<4>  id;
    ap_uint<1> vld;
};


void top_ccvt (dataIn in, dataOut &out) {
    #pragma HLS INTERFACE port=return mode=ap_ctrl_none
    #pragma HLS INTERFACE mode=ap_none port=in 
    #pragma HLS INTERFACE mode=ap_none port=out

    #pragma HLS PIPELINE II=1
    #pragma HLS LATENCY max=0

    out.data = CCVT(in.n, in.s, in.h, in.rs1, in.rs2);
    out.id   = in.id;
    out.vld  = in.vld;
}

void top_cadd (dataIn in, dataOut &out) {
    #pragma HLS INTERFACE port=return mode=ap_ctrl_none
    #pragma HLS INTERFACE mode=ap_none port=in 
    #pragma HLS INTERFACE mode=ap_none port=out

    #pragma HLS PIPELINE II=1
    #pragma HLS LATENCY max=1

    out.data = CADD(in.k, in.h, in.rs1, in.rs2);
    out.id   = in.id;
    out.vld  = in.vld;
}


void top_cmla (dataIn in, dataOut &out) {
    #pragma HLS INTERFACE port=return mode=ap_ctrl_none
    #pragma HLS INTERFACE mode=ap_none port=in 
    #pragma HLS INTERFACE mode=ap_none port=out

    #pragma HLS PIPELINE II=1
    #pragma HLS LATENCY max=2

    out.data = CMLA(in.k, in.c, in.h, in.rs1, in.rs2, in.rs3);
    out.id   = in.id;
    out.vld  = in.vld;
}