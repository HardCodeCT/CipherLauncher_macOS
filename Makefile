# ──────────────────────────────────────────────────────────────────────────────
#  CipherLauncher — macOS Makefile
#  Requires: clang++ (Xcode CLT), Homebrew curl
#
#  Usage:
#    make              — native binary for the current machine's arch
#    make universal    — fat binary (x86_64 + arm64) via lipo
#    make clean        — remove build artefacts
# ──────────────────────────────────────────────────────────────────────────────

CXX       = clang++
CXXFLAGS  = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
             -mmacosx-version-min=11.0
SRC       = CipherLauncher/cipher_launcher.cpp
CURL_INC  = $(shell brew --prefix curl)/include
CURL_LIB  = $(shell brew --prefix curl)/lib
LDFLAGS   = -L$(CURL_LIB) -lcurl -framework CoreFoundation \
             -rpath $(CURL_LIB)

# Detect current arch
NATIVE_ARCH := $(shell uname -m)

# Output names
OUT_NATIVE    = build/CipherLauncher
OUT_X86_64    = build/CipherLauncher_x86_64
OUT_ARM64     = build/CipherLauncher_arm64
OUT_UNIVERSAL = build/CipherLauncher_universal

.PHONY: all native universal clean

all: native

native: $(OUT_NATIVE)

$(OUT_NATIVE): $(SRC) | build
	$(CXX) $(CXXFLAGS) \
	    -arch $(NATIVE_ARCH) \
	    -I$(CURL_INC) \
	    $< \
	    $(LDFLAGS) \
	    -o $@
	strip $@
	@echo ""
	@echo "  Built: $@ ($(NATIVE_ARCH))"
	@file $@

universal: $(OUT_X86_64) $(OUT_ARM64)
	lipo -create $(OUT_X86_64) $(OUT_ARM64) -output $(OUT_UNIVERSAL)
	strip $(OUT_UNIVERSAL)
	@echo ""
	@echo "  Built: $(OUT_UNIVERSAL)"
	@lipo -info $(OUT_UNIVERSAL)

$(OUT_X86_64): $(SRC) | build
	$(CXX) $(CXXFLAGS) -arch x86_64 -I$(CURL_INC) $< $(LDFLAGS) -o $@

$(OUT_ARM64): $(SRC) | build
	$(CXX) $(CXXFLAGS) -arch arm64  -I$(CURL_INC) $< $(LDFLAGS) -o $@

build:
	mkdir -p build

clean:
	rm -rf build
