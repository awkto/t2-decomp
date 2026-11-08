/* The two bare `rts` instructions that the C object runner and level-script interpreter
 * (objects.c, levelscript.c) hle_jump to when they leave their loops: $00B3C6 (ObjRunAll / ObjNext
 * with no objects left) and $009356 (ScriptTick not due). Registered so those 16 cycles are charged as
 * C instead of running the real 68k `rts`. */
#include "hle.h"

#include <stdint.h>

HLE_FUNC_PURE(0x00B3C6, ObjLoopRts, 16) { hle_insn(ctx, 16); /* rts */ }
HLE_FUNC_PURE(0x009356, ScriptRts, 16) { hle_insn(ctx, 16); /* rts */ }
