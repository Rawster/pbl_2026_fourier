source [file join [file dirname [info script]] common.tcl]

open_project $top
remove_files -tb $tb_file
add_files -tb $tb_file -cflags $tb_flags
open_solution $solution

csim_design

exit
