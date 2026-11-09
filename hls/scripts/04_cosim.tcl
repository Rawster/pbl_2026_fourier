source [file join [file dirname [info script]] common.tcl]

open_project $top
open_solution $solution

# co-sim zawsze z lekkim testbenchem (pelny trwalby godzinami)
cosim_design -rtl verilog -trace_level none -argv light

exit