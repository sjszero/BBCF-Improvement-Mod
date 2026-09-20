# -*- coding: utf-8 -*-
# Phase 33: find who actually calls ThinkLogicStrategy vftable+0x1C (the tick),
# and settle whether anything writes a non-zero to the work manager's +0x20.
# Base vftable has +0x14/+0x18/+0x1C all pointing at the same stub FUN_00429DC0,
# i.e. they are the three overridable hooks; the dispatcher that invokes them is
# one of the other base entries. Decompile the whole base machinery and whoever
# calls it.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

SEEDS = [0x004293D0, 0x00429410, 0x004293C0, 0x00429430, 0x00429070,
         0x00429170, 0x00429DC0, 0x004293E0, 0x004293F0, 0x0046B560]

def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s %s -----%n", fn.getEntryPoint(), fn.getName(), note)
    r = ifc.decompileFunction(fn, 180, ConsoleTaskMonitor())
    if r.decompileCompleted():
        out.println(r.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", r.getErrorMessage())
    out.println()

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug33.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    seen = set()
    queue = list(SEEDS)
    out.println("===== CALLER MAP =====")
    for addr in SEEDS:
        fn = get_fn(toAddr(addr))
        if fn is None:
            continue
        out.printf("%08X %s callers:%n", addr, fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
            queue.append(c.getEntryPoint().getOffset())
    out.println()
    for addr in queue:
        if addr in seen:
            continue
        seen.add(addr)
        fn = get_fn(toAddr(addr))
        if fn is not None:
            dec(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
