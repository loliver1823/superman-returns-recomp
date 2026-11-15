// scan_table_refs.java — find EVERY reference into the RtamPackage_AssetHandlers table range
// [0x820DB940,0x820DC2B8], excluding the table's own outgoing handler pointers. Reports the
// external accessors (the registrar/dispatcher) with from-address + ref type + containing fn.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class scan_table_refs extends GhidraScript {
    public void run() throws Exception {
        long lo = 0x820DB940L, hi = 0x820DC2B8L;
        ReferenceManager rm = currentProgram.getReferenceManager();
        int ext = 0;
        for (long a = lo; a <= hi + 4; a += 4) {
            ReferenceIterator it = rm.getReferencesTo(toAddr(a));
            while (it.hasNext()) {
                Reference ref = it.next();
                long from = ref.getFromAddress().getOffset();
                if (from >= lo && from <= hi + 8) continue;   // skip table-internal
                Function f = getFunctionContaining(ref.getFromAddress());
                println("-> 0x" + Long.toHexString(a) + " from 0x" + Long.toHexString(from)
                        + " [" + ref.getReferenceType() + "] in "
                        + (f == null ? "(no fn)" : f.getName() + "@0x" + Long.toHexString(f.getEntryPoint().getOffset())));
                ext++;
                if (ext > 60) { println("... (capped)"); return; }
            }
        }
        println("external accessors into table = " + ext);
    }
}
