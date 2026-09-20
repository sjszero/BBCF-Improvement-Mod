# -*- coding: utf-8 -*-
# Phase 32: close the two blockers that gate the forced-re-login repair.
#   (1) WHICH THREAD ticks the strategies? The work manager is the static
#       DAT_00A5A050, so its two strategy slots are plain globals:
#           mgr+0xE0 == DAT_00A5A130   (Login / types 0-6)
#           mgr+0xE4 == DAT_00A5A134   (DownloadTUS / UploadTUS, types 7-8)
#       Every read of those is a site that may call vftable+0x1C. If the only
#       caller of the +0xE0 tick is the game thread, FUN_00428050 is safe to
#       drive from our hook.
#   (2) Can mgr+0x20 (DAT_00A5A070, the guard in FUN_00428050) ever be 1?
#       Only one direct xref exists (the read), but the manager is passed by
#       pointer, so a write could be hiding as `+0x20` inside any function that
#       obtains it from the getter FUN_00427CD0. Decompile all of those and let
#       the report be grepped for it.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

SLOTS = [(0x00A5A130, "mgr+0xE0 strategy slot (Login / types 0-6)"),
         (0x00A5A134, "mgr+0xE4 strategy slot (TUS download/upload)"),
         (0x00A5A070, "mgr+0x20 == DAT_00A5A070, the FUN_00428050 guard")]
GETTER = 0x00427CD0

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
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug32.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    rm = currentProgram.getReferenceManager()
    targets = []

    for addr, note in SLOTS:
        out.printf("===== XREFS TO %08X (%s) =====%n", addr, note)
        for ref in rm.getReferencesTo(toAddr(addr)):
            fn = get_fn(ref.getFromAddress())
            out.printf("  %-6s from %s in %s%n", ref.getReferenceType(),
                       ref.getFromAddress(), fn.getName() if fn else "?")
            if fn is not None:
                targets.append(fn.getEntryPoint().getOffset())
        out.println()

    getter = get_fn(toAddr(GETTER))
    out.printf("===== ALL CALLERS OF THE MANAGER GETTER %08X =====%n", GETTER)
    for c in getter.getCallingFunctions(ConsoleTaskMonitor()):
        out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
        targets.append(c.getEntryPoint().getOffset())
    out.println()

    out.println("===== DECOMPILES (grep these for vftable+0x1c ticks and +0x20 writes) =====")
    seen = set()
    for addr in targets:
        if addr in seen:
            continue
        seen.add(addr)
        fn = get_fn(toAddr(addr))
        if fn is not None:
            dec(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
