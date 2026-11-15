// define_handler_table.java — L5: turn the RTAM asset-handler dispatch table (RtamPackage_-
// AssetHandlers) at ~0x820DB...-0x820DC0Cx into real typed data so every (handler,id) entry
// creates a genuine xref to its handler. Each entry = { void* handler (4B); u32 id (4B) }.
// Auto-detects the table extent by the entry pattern: handler in code range, id in [0x40000000,
// 0x40010000). Then defines each handler slot as a 4-byte pointer (creates the xref).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.mem.Memory;

public class define_handler_table extends GhidraScript {
    boolean validEntry(long a) {
        try {
            long w0 = Integer.toUnsignedLong(getInt(toAddr(a)));
            long w1 = Integer.toUnsignedLong(getInt(toAddr(a + 4)));
            return w0 >= 0x820F0000L && w0 < 0x82900000L && w1 >= 0x40000000L && w1 < 0x40010000L;
        } catch (Exception e) { return false; }
    }

    public void run() throws Exception {
        long seed = 0x820DC0C8L;            // known entry: {sub_826D8A10, 0x40008203}
        if (!validEntry(seed)) { println("seed entry invalid — aborting"); return; }
        long start = seed, end = seed;
        while (validEntry(start - 8)) start -= 8;
        while (validEntry(end + 8))   end += 8;
        long count = (end - start) / 8 + 1;
        println("handler table: start=0x" + Long.toHexString(start) + " end=0x" + Long.toHexString(end)
                + " entries=" + count);

        DataType ptr4 = new PointerDataType(null, 4, currentProgram.getDataTypeManager());
        int defined = 0, failed = 0;
        for (long a = start; a <= end; a += 8) {
            try {
                clearListing(toAddr(a), toAddr(a + 3));
                createData(toAddr(a), ptr4);           // handler pointer -> creates xref
                defined++;
            } catch (Exception e) { failed++; }
        }
        println("handler pointers defined=" + defined + " failed=" + failed);
        // report the table base label target for the dispatcher hunt
        createLabel(toAddr(start), "RtamPackage_AssetHandlers_table", true);
        println("labeled table base 0x" + Long.toHexString(start));
    }
}
