// Pre-analysis: pin DS to the NE automatic data segment so near pointers resolve.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.MemoryBlock;
import java.math.BigInteger;

public class SetDS extends GhidraScript {
    @Override
    public void run() throws Exception {
        long seg = Long.decode(getScriptArgs().length > 0 ? getScriptArgs()[0] : "0x1048");
        ProgramContext ctx = currentProgram.getProgramContext();
        Register ds = ctx.getRegister("DS");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isExecute()) {
                ctx.setValue(ds, b.getStart(), b.getEnd(), BigInteger.valueOf(seg));
                println("DS=" + Long.toHexString(seg) + " for " + b.getName());
            }
        }
    }
}
