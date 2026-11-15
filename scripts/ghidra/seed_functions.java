// seed_functions.java — Tier 0a Ghidra seed (docs/TOOLING_PLAN.md).
//
// Creates the recomp's ~18.7k missing functions in Ghidra from the recompiler's own
// authoritative list (guest/ppc/ppc_func_mapping.cpp). Run on the default.xex program.
//
// WHY explicit bodies: XenonRecomp function prologues start with `bl <savegprlr>`, and
// Ghidra's flow analysis treats that as the function end — so `create_function` with
// auto-body truncates every function to 8 bytes (`FUN_xxxx(); return;`). We instead give
// each function an explicit body [thisVA, nextVA) derived from the sorted mapping, so the
// whole body is included and decompiles. (Trailing alignment padding may be swept into the
// last function's body; harmless for decompilation/xref.)
//
// ENABLE FIRST: the Ghidra MCP server must be started with GHIDRA_MCP_ALLOW_SCRIPTS=1
// (loopback-only, so no auth token needed), then reconnect the `ghidra` MCP server.
// Run via run_ghidra_script(script_name="seed_functions", program="default.xex") after
// placing this file in a Ghidra script directory, or paste the body into run_script_inline.
//
// After it finishes, run the program's "PowerPC Constant Reference Analyzer" (Analysis ▸
// One Shot) so lis/addi address pairs become data xrefs — that's what makes get_xrefs_to
// resolve the handler-table dispatcher for route 1.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.SourceType;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class seed_functions extends GhidraScript {
    public void run() throws Exception {
        String path = System.getenv().getOrDefault("SR_REPO", ".") + "/guest/ppc/ppc_func_mapping.cpp";
        Pattern p = Pattern.compile("\\{\\s*0x([0-9A-Fa-f]{6,8})\\s*,");
        ArrayList<Long> vas = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = br.readLine()) != null) {
                Matcher m = p.matcher(line);
                if (m.find()) vas.add(Long.parseLong(m.group(1), 16));
            }
        }
        Collections.sort(vas);
        println("seed: parsed " + vas.size() + " function VAs from ppc_func_mapping.cpp");

        FunctionManager fm = currentProgram.getFunctionManager();
        int created = 0, existed = 0, failed = 0, recreated = 0;
        for (int i = 0; i < vas.size(); i++) {
            if (monitor.isCancelled()) break;
            long va = vas.get(i);
            long next = (i + 1 < vas.size()) ? vas.get(i + 1) : va + 0x40;
            if (next <= va) next = va + 0x40;
            Address entry = toAddr(va);

            Function f = fm.getFunctionAt(entry);
            if (f != null && f.getBody().getNumAddresses() > 8) { existed++; continue; }
            if (f != null) { removeFunction(f); recreated++; }

            AddressSet body = new AddressSet(entry, toAddr(next - 1));
            new DisassembleCommand(entry, body, true).applyTo(currentProgram, monitor);
            CreateFunctionCmd cmd = new CreateFunctionCmd(
                "sub_" + Long.toHexString(va).toUpperCase(), entry, body, SourceType.USER_DEFINED);
            if (cmd.applyTo(currentProgram, monitor)) created++; else failed++;

            if (created > 0 && created % 2000 == 0) println("seed: created " + created + " ...");
        }
        println("SEED DONE: created=" + created + " (recreated=" + recreated +
                ") existed=" + existed + " failed=" + failed + " total=" + vas.size());
        println("NEXT: run the PowerPC Constant Reference Analyzer so lis/addi -> data xrefs.");
    }
}
