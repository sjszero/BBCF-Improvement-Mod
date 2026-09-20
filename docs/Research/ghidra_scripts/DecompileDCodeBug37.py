# -*- coding: utf-8 -*-
# Phase 37: last two questions.
#  (1) Which function calls AASTEAM_CNetworker vftable+0x14 (== FUN_0041D410,
#      the strategy ticker)? Scan the 45 callers of the singleton getter
#      FUN_0041C900 for an indirect call through +0x14, then climb to a thread.
#  (2) Confirm the two "+0x20" writes in FUN_0046B560 are NOT on the work
#      manager, i.e. mgr+0x20 (the FUN_00428050 guard) is never set non-zero.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

GETTER = 0x0041C900

def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def body(ifc, fn):
    r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    return r.getDecompiledFunction().getC() if r.decompileCompleted() else ""

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug37.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    mon = ConsoleTaskMonitor()
    out.printf("Program: %s%n%n", currentProgram.getName())

    getter = get_fn(toAddr(GETTER))
    hits = []
    out.println("===== CALLERS OF THE CNetworker GETTER THAT INVOKE +0x14 =====")
    for c in getter.getCallingFunctions(mon):
        t = body(ifc, c)
        if "+ 0x14))" in t:
            hits.append((c, t))
            out.printf("  HIT %s %s%n", c.getEntryPoint(), c.getName())
    out.println()

    out.println("===== THEIR BODIES AND CALLERS =====")
    for c, t in hits:
        out.printf("----- %s %s -----%n%s%n", c.getEntryPoint(), c.getName(), t)
        out.printf("  callers:%n")
        for up in c.getCallingFunctions(mon):
            out.printf("      %s %s%n", up.getEntryPoint(), up.getName())
            t2 = body(ifc, up)
            out.printf("----- caller %s %s -----%n%s%n", up.getEntryPoint(), up.getName(), t2)
        out.println()

    out.println("===== FUN_0046B560 (does its +0x20 write touch the work manager?) =====")
    fn = get_fn(toAddr(0x0046B560))
    if fn is not None:
        out.println(body(ifc, fn)[:6000])
finally:
    ifc.dispose(); out.close()
print("done")
