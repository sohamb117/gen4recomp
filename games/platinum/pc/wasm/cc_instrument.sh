#!/bin/sh
# pc/wasm/cc_instrument.sh CC... ARGS...
#
# -finstrument-functions for wasm, used only for pc/Makefile's VRAMCNT_SRCS
# (the SDK files that write a VRAM bank-control register; armrec_rt.c's
# __cyg_profile_func_exit applies the new bank mapping at every function
# exit there, see the comment above VRAMCNT_SRCS in pc/Makefile).
#
# clang's instrumentation passes __cyg_profile_func_exit(this_fn, call_site)
# with call_site = llvm.returnaddress(0), and the wasm backend refuses that
# intrinsic outside Emscripten ("Non-Emscripten WebAssembly hasn't
# implemented __builtin_return_address"), so the five files would not
# compile at all. armrec_rt.c ignores both arguments. So: compile to LLVM IR
# (the instrumentation pass has run by then; the calls are in the IR),
# replace each returnaddress call with a null pointer, and compile the IR to
# the object. The rewrite is checked: the IR must contain exit calls, every
# returnaddress use must be gone, and nothing else changes.
#
# Without -c and -o (preprocessing, unstatic.py probes), or without
# -finstrument-functions, this is the compiler unchanged.
set -e
# $1 is the compiler driver; every other argument is passed through, and a
# --target= among them is remembered for the final IR-to-object step.
cc=$1
shift

has_c=0 has_instr=0 out= target=
prev=
for a in "$@"; do
    case "$a" in
        -c) has_c=1 ;;
        -finstrument-functions) has_instr=1 ;;
        --target=*) target=$a ;;
    esac
    [ "$prev" = "-o" ] && out=$a
    prev=$a
done

if [ $has_c = 0 ] || [ $has_instr = 0 ] || [ -z "$out" ]; then
    exec $cc "$@"
fi

ll=$out.ll
# Same command, IR out instead of an object. "$@" is rotated in place: each
# original argument is shifted off the front and its rewrite appended.
prev=
for a in "$@"; do
    if [ "$prev" = "-o" ]; then
        set -- "$@" "$ll"
    elif [ "$a" = "-c" ]; then
        set -- "$@" -S -emit-llvm
    else
        set -- "$@" "$a"
    fi
    prev=$a
    shift
done
$cc "$@"

calls=$(grep -c 'call void @__cyg_profile_func_exit' "$ll" || true)
if [ "$calls" -eq 0 ]; then
    echo "cc_instrument: no __cyg_profile_func_exit calls in $ll" >&2
    exit 1
fi
sed -E 's/(tail |musttail |notail )?call ptr @llvm\.returnaddress(\.p0)?\(i32 0\)/getelementptr i8, ptr null, i32 0/' \
    "$ll" > "$ll.tmp"
left=$(grep '@llvm\.returnaddress' "$ll.tmp" | grep -vc '^declare' || true)
if [ "$left" -ne 0 ]; then
    echo "cc_instrument: $left llvm.returnaddress uses survived in $ll" >&2
    exit 1
fi
mv "$ll.tmp" "$ll"
# The optimizer already ran on the IR; -O2 here only drives codegen.
$cc $target -O2 -c -o "$out" "$ll"
rm -f "$ll"
