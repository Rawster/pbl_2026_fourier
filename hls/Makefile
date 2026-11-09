# ============================================================================
# Makefile - Vitis HLS (flow z hls_config.cfg)
#
#   make                 - synteza + eksport IP           -> out/ip/
#   make csim            - C-sim (pelny testbench)
#   make csim LIGHT=1    - C-sim w trybie lekkim
#   make synth           - synteza C++ -> RTL
#   make cosim           - C/RTL co-sim (zawsze tryb lekki, wymaga synth)
#   make test            - csim + synth + cosim
#   make export_ip       - pakowanie IP -> out/ip/ (+ rozpakowanie)
#   make unzip_ip        - rozpakowuje wszystkie .zip w out/ip/
#   make ide             - otwiera workspace out/ w Vitis IDE
#   make clean           - usuwa out/
#
# Przed uzyciem: source <Vitis>/settings64.sh
# ============================================================================

# ============================ USTAWIENIA ====================================
# top_ccvt | top_cadd | top_cmla
TOP   ?= top_cmla
# zu9: xczu9eg-ffvc900-1L-i, zu15: xczu15eg-ffvc900-1L-i
PART  ?= xc7z020clg484-1
CLOCK ?= 100MHz
# ============================================================================

LIGHT ?= 0

# Struktura: out/ = workspace IDE, out/<top>/ = komponent HLS, out/ip/ = IP
OUT_DIR  := $(CURDIR)/out
COMP_DIR := $(OUT_DIR)/$(TOP)
WORK_DIR := $(COMP_DIR)/$(TOP)
CFG      := $(COMP_DIR)/hls_config.cfg
IP_DIR   := $(OUT_DIR)/ip

define CFG_TEXT
part=$(PART)

[hls]
flow_target=vivado
clock=$(CLOCK)

syn.file=$(CURDIR)/src/main.cpp
syn.top=$(TOP)

tb.file=$(CURDIR)/tb/tb_main.cpp
tb.cflags=-DTOP=$(TOP)
$(if $(filter 1,$(LIGHT)),csim.argv=light)

cosim.rtl=verilog
cosim.trace_level=none
cosim.argv=light

package.output.format=ip_catalog
package.output.syn=false
endef
export CFG_TEXT

# Tworzy komponent HLS w workspace (vitis-comp.json), zeby IDE go widzialo
define COMP_PY
import os, vitis
client = vitis.create_client()
client.set_workspace(path=os.environ["HLS_WS"])
client.create_hls_component(name=os.environ["HLS_TOP"],
                            cfg_file=["hls_config.cfg"],
                            template="empty_hls_component")
vitis.dispose()
endef
export COMP_PY

SHELL       := /bin/bash
.SHELLFLAGS := -o pipefail -c

# $(1) - polecenie, $(2) - nazwa logu
define run_hls
	cd $(COMP_DIR) && $(1) --config $(CFG) --work_dir $(WORK_DIR) \
		2>&1 | tee $(COMP_DIR)/$(2).log
endef

# Targets
.PHONY: all create_project csim synth cosim export_ip unzip_ip test ide clean

all: synth export_ip

test: csim synth cosim

# tworzy komponent IDE (raz) i generuje hls_config.cfg (przed kazdym krokiem)
create_project:
	@if [ ! -f $(COMP_DIR)/vitis-comp.json ]; then \
	   echo ">>> Tworze komponent $(TOP) w workspace $(OUT_DIR)"; \
	   mkdir -p $(OUT_DIR); \
	   py=$$(mktemp /tmp/create_comp_XXXX.py); \
	   printf '%s\n' "$$COMP_PY" > $$py; \
	   HLS_WS=$(OUT_DIR) HLS_TOP=$(TOP) vitis -s $$py \
	     || { echo "UWAGA: nie udalo sie utworzyc komponentu IDE (build dziala dalej)."; \
	          echo "       Zamknij Vitis IDE, zrob: rm -rf $(COMP_DIR) i uruchom make ponownie."; }; \
	   rm -f $$py; \
	 fi
	@mkdir -p $(COMP_DIR)
	@printf '%s\n' "$$CFG_TEXT" > $(CFG)
	@echo "HLS: $(TOP) | $(PART) | $(CLOCK) | cfg: $(CFG)"

csim: create_project
	$(call run_hls,vitis-run --mode hls --csim,csim)

synth: create_project
	$(call run_hls,v++ -c --mode hls,synth)

cosim: create_project
	$(call run_hls,vitis-run --mode hls --cosim,cosim)

export_ip: create_project
	$(call run_hls,vitis-run --mode hls --package,export)
	@mkdir -p $(IP_DIR)
	@zip=$$(find $(WORK_DIR) -type f -name '*.zip' -path '*/impl/*' | head -n 1); \
	 test -n "$$zip" || { echo "BLAD: nie znaleziono ZIP-a z IP"; exit 1; }; \
	 cp -f "$$zip" $(IP_DIR)/ && echo "IP:      $(IP_DIR)/$$(basename $$zip)"
	@vdir=$$(find $(WORK_DIR) -type d -path '*/syn/verilog' | head -n 1); \
	 rm -rf $(IP_DIR)/$(TOP)_verilog && cp -r "$$vdir" $(IP_DIR)/$(TOP)_verilog \
	 && echo "Verilog: $(IP_DIR)/$(TOP)_verilog"
	@$(MAKE) --no-print-directory unzip_ip

# kazdy out/ip/<nazwa>.zip -> out/ip/<nazwa>/
unzip_ip:
	@for z in $(IP_DIR)/*.zip; do \
	   [ -f "$$z" ] || { echo "Brak plikow .zip w $(IP_DIR)"; break; }; \
	   d="$${z%.zip}"; rm -rf "$$d"; \
	   if command -v unzip >/dev/null; then unzip -oq "$$z" -d "$$d"; \
	   else python3 -m zipfile -e "$$z" "$$d"; fi \
	   && echo "Rozpakowano: $$d"; \
	 done

ide:
	vitis -w $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR)