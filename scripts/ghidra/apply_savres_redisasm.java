// apply_savres_redisasm.java — DEFINITIVE save/restore decompile fix (run after seed_functions +
// recover_disasm, with ppc_savres_millicode loaded in the cspec). Two passes:
//  (1) every millicode call-TARGET: ensure a function, set call-fixup ppc_savres_millicode, clear noReturn.
//  (2) every call-SITE bl: clear + re-disassemble it so its (stale) flowType recomputes against the
//      now-returning target -> becomes UNCONDITIONAL_CALL with a fallthrough -> caller no longer truncates.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import java.util.ArrayList;
import java.util.Arrays;

public class apply_savres_redisasm extends GhidraScript {
    static final String FIXUP = "ppc_savres_millicode";

    public void run() throws Exception {
        if (!Arrays.asList(currentProgram.getCompilerSpec().getPcodeInjectLibrary().getCallFixupNames()).contains(FIXUP)) {
            println("FIXUP not loaded — restart Ghidra so it re-reads ppc_64_32.cspec, then re-run."); return;
        }
        long[][] ranges = {
            {0x8279E3F0L, 0x8279E43CL}, {0x8279E440L, 0x8279E490L},
            {0x8279E730L, 0x8279E7C0L}, {0x8279E9C8L, 0x8279EA58L},
            {0x8279F1F0L, 0x8279F238L}, {0x8279F23CL, 0x8279F284L},
        };
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing l = currentProgram.getListing();
        ArrayList<Address> sites = new ArrayList<Address>();
        int fixed = 0;
        // Pass 1: fix the call-targets.
        for (long[] r : ranges) {
            for (long a = r[0]; a <= r[1]; a += 4) {
                boolean isTarget = false;
                ReferenceIterator it = rm.getReferencesTo(toAddr(a));
                while (it.hasNext()) {
                    Reference ref = it.next();
                    Instruction ins = l.getInstructionAt(ref.getFromAddress());
                    if (ins != null && ins.getMnemonicString().toLowerCase().startsWith("bl")) {
                        isTarget = true;
                        if (!sites.contains(ref.getFromAddress())) sites.add(ref.getFromAddress());
                    }
                }
                if (!isTarget) continue;
                Function f = getFunctionAt(toAddr(a));
                if (f == null) f = createFunction(toAddr(a), "savres_" + Long.toHexString(a));
                if (f != null) { f.setCallFixup(FIXUP); f.setNoReturn(false); fixed++; }
            }
        }
        // Pass 2: clear each call-site bl individually, collect into a set, re-disassemble the set.
        AddressSet set = new AddressSet();
        for (Address a : sites) {
            Instruction ins = l.getInstructionAt(a);
            int len = (ins != null) ? ins.getLength() : 4;
            l.clearCodeUnits(a, a.add(len - 1), false);
            set.addRange(a, a.add(len - 1));
        }
        new DisassembleCommand(set, null, true).applyTo(currentProgram, monitor);
        println("targets fixed=" + fixed + " call-site bls re-disassembled=" + sites.size());
    }
}
