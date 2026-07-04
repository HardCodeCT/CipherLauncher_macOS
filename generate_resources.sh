#!/bin/bash
# Converts assets/* to C++ headers that are compiled into the launcher.
# Requires xxd (pre-installed on macOS).

OUT_DIR="."
mkdir -p "$OUT_DIR"

if [ -f assets/fairy-stockfish ]; then
    xxd -i assets/fairy-stockfish > "$OUT_DIR/res_fairy_stockfish.h"
    echo "Generated res_fairy_stockfish.h"
else
    echo "ERROR: assets/fairy-stockfish not found"
    exit 1
fi

if [ -f assets/nn-46832cfbead3.nnue.xz ]; then
    xxd -i assets/nn-46832cfbead3.nnue.xz > "$OUT_DIR/res_nnue_xz.h"
    echo "Generated res_nnue_xz.h"
else
    echo "ERROR: assets/nn-46832cfbead3.nnue.xz not found"
    exit 1
fi

if [ -f assets/cipher.png ]; then
    xxd -i assets/cipher.png > "$OUT_DIR/res_icon.h"
    echo "#define HAS_RES_ICON" >> "$OUT_DIR/res_icon.h"
    echo "Generated res_icon.h"
fi

echo "All resources generated."