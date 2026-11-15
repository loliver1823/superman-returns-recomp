// run_constref.java — L5 step 3: run the PowerPC Constant Reference Analyzer with
// "Mark dual instruction references" ENABLED (off by default), so split lis+addi address
// loads become real references — this is what makes a dispatcher that loads a table base
// via `lis rX,hi; addi rX,rX,lo` show up under get_xrefs_to <table_base>. Runs ONLY this
// analyzer, so it won't re-trigger Non-Returning Functions / undo the save-restore flow fix.
import ghidra.app.script.GhidraScript;
import ghidra.app.services.Analyzer;
import ghidra.app.plugin.core.analysis.AutoAnalysisManager;
import ghidra.framework.options.Options;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Program;
import ghidra.util.classfinder.ClassSearcher;
import java.util.List;

public class run_constref extends GhidraScript {
    public void run() throws Exception {
        Analyzer target = null;
        for (Analyzer a : ClassSearcher.getInstances(Analyzer.class)) {
            if (a.getName().equals("PowerPC Constant Reference Analyzer")) { target = a; break; }
        }
        if (target == null) { println("ANALYZER NOT FOUND"); return; }

        Options opts = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
        Options sub = opts.getOptions("PowerPC Constant Reference Analyzer");
        sub.setBoolean("Mark dual instruction references", true);
        target.optionsChanged(sub, currentProgram);

        AddressSetView code = currentProgram.getMemory().getExecuteSet();
        println("running constref (dual-instruction refs ON) over " + code.getNumAddresses() + " addrs ...");
        int txn = currentProgram.startTransaction("constref-dual");
        boolean okk;
        try { okk = target.added(currentProgram, code, monitor, null); }
        finally { currentProgram.endTransaction(txn, true); }
        println("constref added()=" + okk + " DONE");
    }
}
