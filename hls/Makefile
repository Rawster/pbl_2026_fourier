# Narzedzie HLS
# vitis-run --mode hls --tcl  - Vitis 2023.2+ (domyslnie)
# vitis_hls -f                - starsze wersje
HLS ?= vitis-run --mode hls --tcl

# Top i part ustawiasz w scripts/common.tcl
# Lekki testbench w C-sim: make csim LIGHT=1
LIGHT ?= 0

# Build variables
REPO_ROOT     ?= $(shell cd $(CURDIR)/.. && pwd)
HLS_BUILD_DIR ?= $(REPO_ROOT)/build/hls
HLS_OUT_DIR   ?= $(CURDIR)/out

export LIGHT HLS_BUILD_DIR HLS_OUT_DIR

SHELL       := /bin/bash
.SHELLFLAGS := -o pipefail -c

# $(1) - skrypt TCL, $(2) - nazwa logu
define run_hls
	@mkdir -p $(HLS_BUILD_DIR)
	cd $(HLS_BUILD_DIR) && $(HLS) $(CURDIR)/scripts/$(1) \
		2>&1 | tee $(HLS_BUILD_DIR)/$(2).log
endef

# Targets
.PHONY: all create_project csim synth cosim export_ip test clean clean_out clean_all

all: create_project synth export_ip

# pelna weryfikacja: C-sim + synteza + C/RTL co-sim
test: create_project csim synth cosim

create_project:
	$(call run_hls,01_create_project.tcl,create)

csim:
	$(call run_hls,02_csim.tcl,csim)

synth:
	$(call run_hls,03_synth.tcl,synth)

cosim:
	$(call run_hls,04_cosim.tcl,cosim)

export_ip:
	$(call run_hls,05_export_ip.tcl,export)

clean:
	rm -rf $(HLS_BUILD_DIR)

clean_out:
	rm -rf $(HLS_OUT_DIR)

clean_all: clean clean_out