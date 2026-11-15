// recover_disasm.java — reclaim post-call code that CALL_TERMINATOR flow cleared. Blanket-
// disassemble every undefined address inside each seeded function body, then report.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;

public class recover_disasm extends GhidraScript {
    public void run() throws Exception {
        Listing l = currentProgram.getListing();
        AddressSet undef = new AddressSet();
        FunctionIterator fit = currentProgram.getFunctionManager().getFunctions(true);
        int fns = 0;
        while (fit.hasNext()) {
            Function f = fit.next();
            long s = f.getEntryPoint().getOffset();
            if (s < 0x820F0000L || s >= 0x82900000L) continue;
            // collect undefined addresses inside the body
            ghidra.program.model.address.AddressIterator ai = f.getBody().getAddresses(true);
            while (ai.hasNext()) {
                Address a = ai.next();
                if (l.getInstructionAt(a) == null && l.getUndefinedDataAt(a) != null) undef.add(a);
            }
            fns++;
        }
        println("scanned fns=" + fns + " undefined-in-body addrs=" + undef.getNumAddresses());
        if (!undef.isEmpty()) {
            DisassembleCommand dc = new DisassembleCommand(undef, null, true);
            dc.applyTo(currentProgram, monitor);
            println("disassembled.");
        }
    }
}
