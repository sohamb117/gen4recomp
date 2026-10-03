# pc/mk/bridge.mk: see pc/Makefile.wasm and the BRIDGE design. Owned by the
# IRBridge slice.
#
# BRIDGE_COMPILE: the one way a C TU of this module becomes an object.
#   $(call BRIDGE_COMPILE,<cflags>)  in a recipe whose $< is the .c, $@ the .o
# Seed definition: a plain compile, so the other fragments can be developed
# before the IR rewrite exists. IRBridge replaces it with
# clang -S -emit-llvm -> irbridge.py -> clang -c.
define BRIDGE_COMPILE
@mkdir -p $(dir $@)
$(CC) $(1) -c -o $@ $<
endef
