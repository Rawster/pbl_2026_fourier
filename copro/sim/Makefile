# ---------------------------------------------------------------------------
# hdl/sim/Makefile - symulacja koprocesora w Verilatorze, testbench w C++
#
#   make              kompilacja + uruchomienie
#   make run SEED=7   inne ziarno losowania
#   make run N=100000 dłuższy test losowy
#   make TRACE=1      dodatkowo wave.vcd
#   make lint         sam lint RTL (bez symulacji)
#
# Układ katalogów:
#   copro/src/cv32e40x_if_xif.sv
#   copro/src/coprocesor.sv
#   copro/sim/{Makefile, copro_top.sv, tb_main.cpp}
#   hls/out/xilinx_com_hls_top_{ccvt,cadd,cmla}_1_0/hdl/verilog/*.v
# ---------------------------------------------------------------------------
VERILATOR ?= verilator
SEED      ?= 1
N         ?= 20000

TOP    = copro_top
HLS_V ?= $(wildcard ../../hls/out/xilinx_com_hls_top_ccvt_1_0/hdl/verilog/*.v) \
         $(wildcard ../../hls/out/xilinx_com_hls_top_cadd_1_0/hdl/verilog/*.v) \
         $(wildcard ../../hls/out/xilinx_com_hls_top_cmla_1_0/hdl/verilog/*.v)
RTL    = ../src/cv32e40x_if_xif.sv $(HLS_V) ../src/coprocesor.sv copro_top.sv
TB     = tb_main.cpp
OBJ    = obj_dir
BIN    = $(OBJ)/V$(TOP)

# --x-initial unique + randReset(2) w tb: losowy stan początkowy rejestrów
# (rejestry danych w HLS nie mają resetu - test sprawdza, że to nie szkodzi)
VFLAGS   = --cc --exe --build -j 0 --top-module $(TOP) --no-timing \
           -Wno-fatal -Wno-lint -Wno-style -Werror-USERFATAL \
           -O3 --x-assign unique --x-initial unique
CXXFLAGS = -O2 -std=c++17

ifeq ($(TRACE),1)
VFLAGS   += --trace
CXXFLAGS += -DTRACE
endif

.PHONY: all build run lint clean
all: run

build: $(BIN)

$(BIN): $(RTL) $(TB) Makefile
	$(VERILATOR) $(VFLAGS) $(RTL) $(TB) -CFLAGS "$(CXXFLAGS)" -o V$(TOP)

run: $(BIN)
	./$(BIN) +seed=$(SEED) +n=$(N)

lint:
	$(VERILATOR) --lint-only -Wall --no-timing -Werror-USERFATAL \
	  -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM --top-module $(TOP) $(RTL) \
	  2>&1 | grep -E "^%(Warning|Error)" | grep -E "src/coprocesor\.sv|USERFATAL" \
	  || echo "coprocesor.sv: brak ostrzezen"

clean:
	rm -rf $(OBJ) wave.vcd
