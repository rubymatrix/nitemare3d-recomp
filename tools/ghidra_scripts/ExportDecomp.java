// Export decompiled C for every function, plus strings and function list.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter c = new PrintWriter(new FileWriter(outDir + "/decomp.c"));
             PrintWriter f = new PrintWriter(new FileWriter(outDir + "/functions.txt"));
             PrintWriter s = new PrintWriter(new FileWriter(outDir + "/strings.txt"))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function fn = it.next();
                f.println(fn.getEntryPoint() + " " + fn.getName() + " " + fn.getBody().getNumAddresses());
                DecompileResults r = ifc.decompileFunction(fn, 120, monitor);
                c.println("// ==== " + fn.getName() + " @ " + fn.getEntryPoint());
                if (r != null && r.decompileCompleted()) c.println(r.getDecompiledFunction().getC());
                else c.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
            }
            DataIterator di = currentProgram.getListing().getDefinedData(true);
            while (di.hasNext()) {
                Data d = di.next();
                if (d.hasStringValue()) s.println(d.getAddress() + " " + d.getDefaultValueRepresentation());
            }
        }
    }
}
