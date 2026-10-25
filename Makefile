# Checks and builds. CI runs the same targets. Needs uv and a C++ compiler.
#
#   make            same as make check
#   make check      host test, linters, `esphome config` of both variants
#   make test       host unit test of include/*.h
#   make lint       yamllint, clang-format (check only), clang-tidy
#   make format     apply clang-format
#   make config     validate both firmware variants
#   make compile    full firmware build of both variants (slow)
#   make run        compile and flash heatpump.yaml (USB or OTA)
#   make logs       stream logs from DEVICE (default heatpump.local)
#
# config and compile take CONFIGS=... to pick one variant. heatpump-lease.tmp.yaml
# is heatpump.yaml with the opt-in lease package enabled.

ESPHOME_VERSION  := 2026.6.5
YAMLLINT_VERSION := 1.38.0
LLVM_VERSION     := 23.1.3

ESPHOME      := uvx -q --from esphome==$(ESPHOME_VERSION) esphome
YAMLLINT     := uvx -q --from yamllint==$(YAMLLINT_VERSION) yamllint
CLANG_FORMAT := uvx -q --from clang-format==$(LLVM_VERSION) clang-format
CLANG_TIDY   := uvx -q --from clang-tidy==$(LLVM_VERSION) clang-tidy

CXXFLAGS  := -std=c++17 -I tests/stubs -I include
CPP_FILES := $(shell git ls-files -co --exclude-standard '*.h' '*.cpp')
CONFIGS   := heatpump.yaml heatpump-lease.tmp.yaml
DEVICE    := heatpump.local

# clang-tidy can't find the macOS C++ headers on its own.
ifeq ($(shell uname -s),Darwin)
TIDY_FLAGS := -isysroot $(shell xcrun --show-sdk-path)
endif

.PHONY: check test lint format config compile run logs clean need-secrets
.DELETE_ON_ERROR:

check: test lint config

test:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Wall -Wextra -Werror tests/test_helpers.cpp -o build/test_helpers
	build/test_helpers

lint:
	$(YAMLLINT) --strict .
	$(CLANG_FORMAT) --dry-run -Werror $(CPP_FILES)
	$(CLANG_TIDY) --quiet tests/test_helpers.cpp -- $(CXXFLAGS) $(TIDY_FLAGS)

format:
	$(CLANG_FORMAT) -i $(CPP_FILES)

# Prints the merged config only when it is invalid.
config: need-secrets $(CONFIGS)
	@mkdir -p build
	@for c in $(CONFIGS); do \
	  echo "esphome config $$c"; \
	  $(ESPHOME) config $$c > build/config.log 2>&1 || { cat build/config.log; exit 1; }; \
	done

compile: need-secrets $(CONFIGS)
	@for c in $(CONFIGS); do echo "esphome compile $$c"; $(ESPHOME) compile $$c || exit 1; done

run: need-secrets
	$(ESPHOME) run heatpump.yaml

logs: need-secrets
	$(ESPHOME) logs heatpump.yaml --device $(DEVICE)

heatpump-lease.tmp.yaml: heatpump.yaml
	sed 's|^  # \(- !include packages/t1s-lease.yaml\)$$|  \1|' $< > $@
	grep -q '^  - !include packages/t1s-lease.yaml$$' $@

need-secrets:
	@test -f secrets.yaml || { echo "secrets.yaml missing: see README (for checks only: cp tests/secrets.ci.yaml secrets.yaml)" >&2; exit 1; }

clean:
	rm -rf build heatpump-lease.tmp.yaml
