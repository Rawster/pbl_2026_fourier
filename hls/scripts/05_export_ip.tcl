source [file join [file dirname [info script]] common.tcl]

open_project $top
open_solution $solution

file mkdir $out_dir
set ip_zip "$out_dir/xilinx_com_hls_${top}_1_0.zip"
export_design -format ip_catalog -rtl verilog -output $ip_zip
puts "IP exported to: $ip_zip"

set rtl_dir "$out_dir/${top}_verilog"
file delete -force $rtl_dir
file copy "$build_dir/$top/$solution/syn/verilog" $rtl_dir
puts "Verilog copied to: $rtl_dir"

exit
