ARCH ?= $(shell uname -m)

ifeq ($(ARCH),x86_64)
    DEPLOY_TARGET  ?= 10.15
    HOMEBREW_PREFIX ?= /usr/local
else
    DEPLOY_TARGET  ?= 11.0
    HOMEBREW_PREFIX ?= /opt/homebrew
endif

CXX = clang++
CXXFLAGS = -std=c++17 -O3 -flto \
           -fvisibility=hidden \
           -ffunction-sections \
           -fdata-sections \
           -fstack-protector-strong \
           -Wall -Wextra -Wno-unused-parameter \
           -arch $(ARCH) \
           -mmacosx-version-min=$(DEPLOY_TARGET)

SRC = cipher_launcher.cpp

CURL_INC = $(HOMEBREW_PREFIX)/opt/curl/include
CURL_LIB = $(HOMEBREW_PREFIX)/opt/curl/lib
LZMA_INC = $(HOMEBREW_PREFIX)/opt/xz/include
LZMA_LIB = $(HOMEBREW_PREFIX)/opt/xz/lib

INCLUDES = -I$(CURL_INC) -I$(LZMA_INC)

LDFLAGS  = -L$(CURL_LIB) -L$(LZMA_LIB) -lcurl -llzma \
           -framework CoreFoundation \
           -rpath $(CURL_LIB) \
           -Wl,-dead_strip \
           -Wl,-S

OUT = CipherLauncher

$(OUT): $(SRC) res_fairy_stockfish.h res_nnue_xz.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) $(LDFLAGS) -o $(OUT)
	strip -x -S -T $(OUT)
	@echo ""
	@echo "  Built: $(OUT)  (arch=$(ARCH), min=$(DEPLOY_TARGET), prefix=$(HOMEBREW_PREFIX))"
	@file $(OUT)

clean:
	rm -f $(OUT) CipherLauncher-arm64 CipherLauncher-x86_64 res_*.h
