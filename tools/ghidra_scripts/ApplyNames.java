// Apply function names and optional C prototypes from re/names.txt.
// Line format:  seg:off name [| prototype]
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.util.parser.FunctionSignatureParser;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.nio.file.*;

public class ApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String raw : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            String line = raw.split("#", 2)[0].trim();
            if (line.isEmpty()) continue;
            String proto = null;
            int bar = line.indexOf('|');
            if (bar >= 0) { proto = line.substring(bar + 1).trim(); line = line.substring(0, bar).trim(); }
            String[] parts = line.split("\\s+");
            Address a = toAddr(parts[0]);
            Function f = getFunctionAt(a);
            if (f == null) f = createFunction(a, parts[1]);
            if (f == null) { println("no function at " + parts[0]); continue; }
            f.setName(parts[1], SourceType.USER_DEFINED);
            if (proto != null && !proto.isEmpty()) {
                FunctionDefinitionDataType sig;
                try { sig = new FunctionSignatureParser(currentProgram.getDataTypeManager(), null).parse(null, proto); }
                catch (Exception e) { println("bad proto: " + proto + " : " + e.getMessage()); continue; }
                if (sig == null) { println("bad proto: " + proto); continue; }
                sig.setCallingConvention("__cdecl16far");
                new ApplyFunctionSignatureCmd(a, sig, SourceType.USER_DEFINED).applyTo(currentProgram, monitor);
            }
        }
    }
}
