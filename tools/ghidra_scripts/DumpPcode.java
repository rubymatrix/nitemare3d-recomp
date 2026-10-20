// Dump raw p-code for given function addresses (debug aid).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;

public class DumpPcode extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Function fn = getFunctionAt(toAddr(a));
            println("==== " + fn.getName());
            for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
                println(ins.getAddress() + "  " + ins);
                for (PcodeOp op : ins.getPcode()) println("      " + op);
            }
        }
    }
}
