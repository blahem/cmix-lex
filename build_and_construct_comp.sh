#!/bin/bash -x

#SEED="$1"
#UPDATE_LIMIT="$2"

SEED="923"
UPDATE_LIMIT="3000"

CFLAGS_DEFINES="-DSEED=$SEED -DUPDATE_LIMIT=$UPDATE_LIMIT"

# building with PGO
if [[ "${REUSE_PGO:-0}" == "1" && -s ./pgo_data/default.profdata ]]; then
  echo "Reusing existing PGO profile ./pgo_data/default.profdata"
else
  rm -rf pgo_data
  mkdir -p pgo_data
  make CFLAGS_DEFINES="$CFLAGS_DEFINES" prof_gen -j

  ./cmix -c ./prof_input/input ./prof_comp > ./prof_output
  # The real -e run enters compression through the dictionary/preprocessor path;
  # include a small sample of that route so PGO does not overfit to nodict only.
  ./cmix -c ./dictionary/english.dic ./prof_input/input ./prof_comp_dict > ./prof_output_dict
  rm ./prof_comp ./prof_output ./prof_comp_dict ./prof_output_dict
  llvm-profdata-17 merge -output=default.profdata ./pgo_data/*
  mv default.profdata pgo_data/
fi

make CFLAGS_DEFINES="$CFLAGS_DEFINES" prof_use -j
# Drop non-runtime ELF metadata before UPX. The binary is already linked with
# -s; these sections are extra loader/comment notes and do not affect behavior.
llvm-strip-17 --strip-all cmix || true
objcopy --remove-section=.comment \
  --remove-section=.note.gnu.property \
  --remove-section=.note.gnu.build-id \
  --remove-section=.note.ABI-tag \
  cmix 2>/dev/null || true
# LZMA-packed UPX is ~23 KB smaller than the default UCL mode on this binary.
# UPX 3.94's LZMA loader crashes on current WSL2 kernels, so use the local
# modern UPX installed by install_tools/install_upx.sh.
UPX_BIN="${UPX_BIN:-./tools/upx}"
if [[ ! -x "$UPX_BIN" ]]; then
  echo "Missing executable UPX at $UPX_BIN. Run ./install_tools/install_upx.sh or set UPX_BIN." >&2
  exit 1
fi
UPX_VERSION_TEXT="$("$UPX_BIN" --version | head -n 1)"
echo "Using $UPX_VERSION_TEXT"
if [[ "$UPX_VERSION_TEXT" == upx\ 3.* ]]; then
  echo "Refusing UPX 3.x for LZMA packing; its loader crashes under current WSL2." >&2
  exit 1
fi
"$UPX_BIN" --ultra-brute cmix 

# this is a directory where the compressor binary will be placed 
DIR=run
mkdir -p ./$DIR
ROOT=$(pwd)
cp ./cmix $DIR/cmix_orig
# git diff > $DIR/patch
# exit
# building a selfextracting binary 
pushd $DIR
# Reuse the embedded dictionary/order streams by default. PPM mmap/RSS-only
# changes do not alter compression behavior, and recompressing these payloads
# costs time without changing the final self-extract contents. Set
# FORCE_SELFEXTRACT_REBUILD=1 when the dictionary, order, or coder changes.
if [[ "${FORCE_SELFEXTRACT_REBUILD:-0}" == "1" || ! -s ./comp_dict ]]; then
  ./cmix_orig -c $ROOT/dictionary/english.dic ./comp_dict
else
  echo "Reusing cached ./comp_dict ($(wc -c < ./comp_dict) bytes)"
fi
if [[ "${FORCE_SELFEXTRACT_REBUILD:-0}" == "1" || ! -s ./comp_order ]]; then
  ./cmix_orig -c $ROOT/src/readalike_prepr/data/new_article_order ./comp_order
else
  echo "Reusing cached ./comp_order ($(wc -c < ./comp_order) bytes)"
fi
# creating a header with size of the above files
./cmix_orig -h $(wc -c < ./comp_dict) $(wc -c < ./comp_order) 0

# merging the above files and setting permissions for the final executable file
cat ./cmix_orig ./comp_dict ./comp_order header.dat > ./cmix
chmod +x ./cmix
