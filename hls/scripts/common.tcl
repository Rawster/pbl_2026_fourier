# common.tcl - wspolne ustawienia, dolaczany przez kazdy skrypt 0X_*.tcl

# ============================ USTAWIENIA ====================================
set top          "top_cmla"               ;# top_ccvt | top_cadd | top_cmla
set part_number  "xc7z020clg484-1"        ;# zu
set clock_period 10                       ;# [ns]
# ============================================================================

global env
proc env_or {name default} {
    global env
    if {[info exists env($name)]} { return $env($name) }
    return $default
}

set hls_dir   [file dirname [file dirname [file normalize [info script]]]]
set build_dir [file normalize [env_or HLS_BUILD_DIR "$hls_dir/build"]]
set out_dir   [file normalize [env_or HLS_OUT_DIR   "$hls_dir/out"]]

set tb_flags "-DTOP=$top"                ;# testbench wybiera DUT po nazwie topu
if {[env_or LIGHT 0]} { append tb_flags " -DTB_LIGHT" }

set src_file "$hls_dir/src/main.cpp"
set tb_file  "$hls_dir/tb/tb_main.cpp"
set solution "sol1"

puts "HLS: $top | $part_number | ${clock_period} ns | TB: $tb_flags"
file mkdir $build_dir
cd $build_dir