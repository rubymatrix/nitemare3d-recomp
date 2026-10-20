// Export a flat disassembly listing of all instructions grouped by function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportAsm extends GhidraScript {
    @Override
    public void run() throws Exception {
        try (PrintWriter w = new PrintWriter(new FileWriter(getScriptArgs()[0] + "/nite3w.asm"))) {
            Listing l = currentProgram.getListing();
            for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
                w.println();
                w.println("; ==== " + fn.getName() + " @ " + fn.getEntryPoint() + "  " + fn.getSignature().getPrototypeString());
                for (Instruction ins : l.getInstructions(fn.getBody(), true)) {
                    w.println(ins.getAddress() + "  " + ins);
                }
            }
        }
    }
}
