source [file join [file dirname [info script]] common.tcl]

open_project $top
open_solution $solution

csynth_design

exit
