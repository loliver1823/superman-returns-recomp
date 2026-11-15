// seed_missing.java — pipeline step 7: close the seed-coverage gap.
// seed_functions.java creates functions from ppc_func_mapping, but createFunction needs the bytes
// disassembled first. Where Ghidra auto-analysis mis-detected a code region as DATA (e.g. a false
// "string" over a real function entry, as at 0x82294C20), the seed silently skipped it -> the
// function is absent from the DB and can't be decompiled/emulated. This finds every ppc_func_mapping
// VA with no function, clears any blocking data, disassembles, and creates the function.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Listing;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class seed_missing extends GhidraScript {
    // adjust if the repo moves
    static final String MAPPING = System.getenv().getOrDefault("SR_REPO", ".") + "/guest/ppc/ppc_func_mapping.cpp";

    public void run() throws Exception {
        Listing l = currentProgram.getListing();
        Pattern p = Pattern.compile("\\{\\s*0x([0-9A-Fa-f]+)\\s*,\\s*sub_");
        int total = 0, missing = 0, fixed = 0, failed = 0;
        BufferedReader br = new BufferedReader(new FileReader(MAPPING));
        String line;
        while ((line = br.readLine()) != null) {
            Matcher m = p.matcher(line);
            if (!m.find()) continue;
            total++;
            long va = Long.parseLong(m.group(1), 16);
            Address a = toAddr(va);
            if (getFunctionAt(a) != null) continue;
            missing++;
            // clear any blocking data/instruction over the entry word, then disassemble
            Data d = l.getDefinedDataContaining(a);
            if (d != null) l.clearCodeUnits(d.getMinAddress(), d.getMaxAddress(), false);
            else l.clearCodeUnits(a, a.add(3), false);
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            Instruction ins = l.getInstructionAt(a);
            if (ins == null) { failed++; continue; }
            Function f = createFunction(a, null);
            if (f != null) fixed++; else failed++;
        }
        br.close();
        println("mapping entries=" + total + " missing=" + missing + " fixed=" + fixed + " still-failed=" + failed);
        println("function count now=" + currentProgram.getFunctionManager().getFunctionCount());
    }
}
