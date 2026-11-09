source [file join [file dirname [info script]] common.tcl]

open_project -reset $top
set_top $top
add_files $src_file
add_files -tb $tb_file -cflags $tb_flags

open_solution -reset $solution -flow_target vivado
set_part $part_number
create_clock -period $clock_period -name default
config_export -format ip_catalog -rtl verilog

exit
