#!/bin/bash
# Converts assets/* to C++ headers that are compiled into the launcher.
# Requires xxd (pre-installed on macOS).
OUT_DIR="."
mkdir -p "$OUT_DIR"

if [ -f assets/fairy-stockfish ]; then
    xxd -i assets/fairy-stockfish > "$OUT_DIR/res_fairy_stockfish.h"
    sed -i.bak 's/assets_fairy_stockfish/res_fairy_stockfish/g' "$OUT_DIR/res_fairy_stockfish.h" && rm -f "$OUT_DIR/res_fairy_stockfish.h".bak
    echo "Generated res_fairy_stockfish.h"
else
    echo "ERROR: assets/fairy-stockfish not found"
    exit 1
fi

if [ -f assets/nn-46832cfbead3.nnue.xz ]; then
    xxd -i assets/nn-46832cfbead3.nnue.xz > "$OUT_DIR/res_nnue_xz.h"
    sed -i.bak 's/assets_nn_46832cfbead3_nnue_xz/res_nn_46832cfbead3_nnue_xz/g' "$OUT_DIR/res_nnue_xz.h" && rm -f "$OUT_DIR/res_nnue_xz.h".bak
    echo "Generated res_nnue_xz.h"
else
    echo "ERROR: assets/nn-46832cfbead3.nnue.xz not found"
    exit 1
fi

if [ -f assets/cipher.png ]; then
    xxd -i assets/cipher.png > "$OUT_DIR/res_icon.h"
    sed -i.bak 's/assets_cipher_png/res_cipher_png/g' "$OUT_DIR/res_icon.h" && rm -f "$OUT_DIR/res_icon.h".bak
    echo "#define HAS_RES_ICON" >> "$OUT_DIR/res_icon.h"
    echo "Generated res_icon.h"
fi

if [ -f assets/firefox.png ]; then
    xxd -i assets/firefox.png > "$OUT_DIR/res_firefox_icon.h"
    sed -i.bak 's/assets_firefox_png/res_firefox_png/g' "$OUT_DIR/res_firefox_icon.h" && rm -f "$OUT_DIR/res_firefox_icon.h".bak
    echo "#define HAS_RES_FIREFOX_ICON" >> "$OUT_DIR/res_firefox_icon.h"
    echo "Generated res_firefox_icon.h"
else
    echo "WARNING: assets/firefox.png not found — stealth icon will be unavailable"
fi

echo "All resources generated."
