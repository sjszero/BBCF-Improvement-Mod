# -*- coding: utf-8 -*-
# Phase 35: FUN_00428260 is the work manager's Update -- it ticks BOTH strategy
# slots via vftable+0x0C:
#     (**(code **)(**(int **)(mgr + 0xe0) + 0xc))(mgr);
#     (**(code **)(**(int **)(mgr + 0xe4) + 0xc))(mgr);
# Walk its callers up until the thread of execution is obvious, so we can tell
# whether driving FUN_00428050 from our game-thread hook races the ticker.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

ROOT = 0x00428260
DEPTH = 5

def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), fn.getName())
    r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "decompile failed")
    out.println()

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug35.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%n%n", currentProgram.getName())
    mon = ConsoleTaskMonitor()
    level = {get_fn(toAddr(ROOT))}
    seen = set()
    for d in range(DEPTH):
        out.printf("===== CALLER LEVEL %d =====%n", d)
        nxt = set()
        for fn in level:
            if fn is None or fn.getEntryPoint().toString() in seen:
                continue
            seen.add(fn.getEntryPoint().toString())
            callers = list(fn.getCallingFunctions(mon))
            out.printf("  %s %s  <- %d caller(s)%n", fn.getEntryPoint(), fn.getName(), len(callers))
            for c in callers:
                out.printf("        %s %s%n", c.getEntryPoint(), c.getName())
                nxt.add(c)
        out.println()
        if not nxt:
            break
        level = nxt

    out.println("===== BODIES =====")
    for a in sorted(seen):
        fn = get_fn(toAddr(long(int(a, 16))))
        if fn is not None:
            dec(out, ifc, fn)

    out.println("===== THREAD-CREATION SITES (FUN_0042EFF0 callers, for naming) =====")
    tf = get_fn(toAddr(0x0042EFF0))
    if tf is not None:
        for c in tf.getCallingFunctions(mon):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
finally:
    ifc.dispose(); out.close()
print("done")
