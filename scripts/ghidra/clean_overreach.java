// clean_overreach.java — pipeline step 6 (run after apply_savres_redisasm).
// recover_disasm.java (step 2) disassembles every undefined-in-body address, which over-reaches
// into tail/inline DATA padding (words with primary opcode 0 or 22 = illegal/data, e.g. zero
// padding) and marks them as "bad instructions". This clears those data over-reach units back to
// undefined, removing the cosmetic Error-bookmark noise. It deliberately LEAVES primary-opcode
// 4/5/6 units in place: those are real Xenon VMX128 vector instructions that stock Ghidra's
// PowerPC decoder can't decode — honest "undecodable code here" markers, not garbage. (To actually
// DECODE them, a VMX128 SLEIGH spec would be needed — separate, larger task; the recomp source via
// va_to_source is authoritative for vector-heavy functions.)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import java.util.ArrayList;
import java.util.Iterator;

public class clean_overreach extends GhidraScript {
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing l = currentProgram.getListing();
        BookmarkManager bm = currentProgram.getBookmarkManager();
        ArrayList<Address> toClear = new ArrayList<Address>();
        Iterator<Bookmark> it = bm.getBookmarksIterator("Error");
        while (it.hasNext()) {
            Address a = it.next().getAddress();
            try {
                int w = mem.getInt(a);
                int primary = (w >>> 26) & 0x3F;
                if (primary == 0 || primary == 22) toClear.add(a);   // data over-reach, not code
            } catch (Exception e) {}
        }
        int cleared = 0;
        for (Address a : toClear) {
            l.clearCodeUnits(a, a, false);
            bm.removeBookmarks(new AddressSet(a, a), "Error", monitor);
            cleared++;
        }
        int rem = 0;
        Iterator<Bookmark> it2 = bm.getBookmarksIterator("Error");
        while (it2.hasNext()) { it2.next(); rem++; }
        println("cleared data-overreach units=" + cleared + "  remaining (VMX128 op4/5/6)=" + rem);
    }
}
