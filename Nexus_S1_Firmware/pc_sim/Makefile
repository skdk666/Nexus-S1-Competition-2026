# Optional make entry points; build.py writes only to .pio/pc_sim.
PYTHON ?= python
SIM_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
.PHONY: all run test
all:
	$(PYTHON) "$(SIM_DIR)build.py"
run:
	$(PYTHON) "$(SIM_DIR)build.py" --run
test:
	$(PYTHON) "$(SIM_DIR)build.py" --test
