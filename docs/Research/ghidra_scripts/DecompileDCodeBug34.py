# -*- coding: utf-8 -*-
# Phase 34: find the strategy TICKER definitively.
# Every ThinkLogicStrategy vtable has +0x0C == FUN_00429430, a thin forwarder
# that calls vftable+0x1C (the tick). So the ticker is whatever invokes slot
# +0x0C on the work manager's strategy slots (mgr+0xE0 / mgr+0xE4). It is not
# among the getter's callers (phase 32), so scan the whole uei/network module
# and report every function whose decompiled body both touches +0xE0/+0xE4 and
# makes an indirect call, plus every function that calls slot +0x0C at all.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

LO, HI = 0x00400000, 0x00500000

def dec_text(ifc, fn):
    r = ifc.decompileFunction(fn, 60, ConsoleTaskMonitor())
    if r.decompileCompleted():
        return r.getDecompiledFunction().getC()
    return ""

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug34.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nScanned range: %08X-%08X%n%n", currentProgram.getName(), LO, HI)
    fm = currentProgram.getFunctionManager()
    hits_slot = []
    hits_e0 = []
    scanned = 0
    for fn in fm.getFunctions(True):
        ep = fn.getEntryPoint().getOffset()
        if ep < LO or ep >= HI:
            continue
        scanned += 1
        c = dec_text(ifc, fn)
        if not c:
            continue
        slot = "+ 0xc))" in c or "+ 0xc)(" in c
        e0 = "0xe0)" in c or "0xe4)" in c
        if slot:
            hits_slot.append((ep, fn.getName(), e0, c))
        elif e0 and "))(" in c:
            hits_e0.append((ep, fn.getName(), c))
    out.printf("scanned %d functions%n%n", scanned)

    out.println("===== CALLS A VTABLE SLOT +0x0C (the tick forwarder) =====")
    for ep, name, e0, c in hits_slot:
        out.printf("  %08X %s   touches_e0/e4=%s%n", ep, name, e0)
    out.println()
    out.println("===== TOUCHES +0xE0/+0xE4 AND MAKES AN INDIRECT CALL =====")
    for ep, name, c in hits_e0:
        out.printf("  %08X %s%n", ep, name)
    out.println()

    out.println("===== BODIES =====")
    for ep, name, e0, c in hits_slot:
        out.printf("----- %08X %s -----%n%s%n", ep, name, c)
    for ep, name, c in hits_e0:
        out.printf("----- %08X %s -----%n%s%n", ep, name, c)
finally:
    ifc.dispose(); out.close()
print("done")
