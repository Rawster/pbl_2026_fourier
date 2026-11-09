source [file join [file dirname [info script]] common.tcl]

open_project $top
open_solution $solution

if {$light} { csim_design -argv light } else { csim_design }

exit