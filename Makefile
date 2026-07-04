CXX = clang++
CXXFLAGS = -std=c++17 -O3 -flto \
           -fvisibility=hidden \
           -ffunction-sections \
           -fdata-sections \
           -fstack-protector-strong \
           -Wall -Wextra -Wno-unused-parameter \
           -mmacosx-version-min=11.0

SRC = cipher_launcher.cpp

HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
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
	@echo "  Built: $(OUT)"
	@file $(OUT)

clean:
	rm -f $(OUT) res_*.h