# -*- coding: utf-8 -*-
# Phase 36: AASTEAM_CNetworker::vftable is 0084FF54 and its slot +0x14 is
# FUN_0041D410 == { mgr = FUN_00427CD0(); FUN_00428260(mgr); }, i.e. the
# strategy ticker. The ONLY thing still gating the forced-re-login repair is
# which thread invokes that Update. Vtable is stored in objects at 0041B1E9 /
# 0041BC3B (ctors) and 008480DA; find the singleton and whoever calls +0x14 on
# it, then walk up to a thread entry or the frame loop.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

SEEDS = [0x0041B1E9, 0x0041BC3B, 0x008480DA, 0x0041BC10, 0x0041CA20]
DEPTH = 6

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
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug36.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    mon = ConsoleTaskMonitor()
    out.printf("Program: %s%n%n", currentProgram.getName())

    # 1. the ctor / storage sites
    out.println("===== VTABLE STORAGE SITES =====")
    seeds = set()
    for a in SEEDS:
        fn = get_fn(toAddr(a))
        if fn is not None:
            out.printf("  %08X -> %s %s%n", a, fn.getEntryPoint(), fn.getName())
            seeds.add(fn)
    out.println()

    # 2. climb callers until we run out
    out.println("===== CALLER CHAIN =====")
    seen = set()
    level = set(seeds)
    for depth in range(DEPTH):
        out.printf("--- level %d ---%n", depth)
        nxt = set()
        for fn in level:
            key = fn.getEntryPoint().toString()
            if key in seen:
                continue
            seen.add(key)
            callers = list(fn.getCallingFunctions(mon))
            out.printf("  %s %s <- %d%n", fn.getEntryPoint(), fn.getName(), len(callers))
            for c in callers:
                out.printf("      %s %s%n", c.getEntryPoint(), c.getName())
                nxt.add(c)
        if not nxt:
            break
        level = nxt
    out.println()

    # 3. thread entry points, so a caller landing in one is obvious
    out.println("===== THREAD CREATION SITES =====")
    for tf_addr in (0x0042EFF0,):
        tf = get_fn(toAddr(tf_addr))
        if tf is None:
            continue
        out.printf("  callers of %08X:%n", tf_addr)
        for c in tf.getCallingFunctions(mon):
            out.printf("      %s %s%n", c.getEntryPoint(), c.getName())
    out.println()

    out.println("===== BODIES =====")
    for key in sorted(seen):
        fn = get_fn(toAddr(long(int(key, 16))))
        if fn is not None:
            dec(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
