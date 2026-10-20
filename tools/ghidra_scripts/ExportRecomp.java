// Export everything the static recompiler needs:
//   functions.json  - functions with instructions and raw p-code
//   memory.json + mem_*.bin - relocated memory image of every initialized block
//   lang.json       - register names and user-defined op names
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportRecomp extends GhidraScript {
    private String vn(Varnode v) {
        if (v == null) return "null";
        return "[\"" + v.getAddress().getAddressSpace().getName() + "\"," + v.getOffset() + "," + v.getSize() + "]";
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        Language lang = currentProgram.getLanguage();
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/lang.json"))) {
            w.println("{\"registers\":{");
            boolean first = true;
            for (Register r : lang.getRegisters()) {
                if (!first) w.println(",");
                first = false;
                w.print("\"" + r.getName() + "\":[" + r.getOffset() + "," + r.getMinimumByteSize() + "]");
            }
            w.println("},\"userops\":[");
            for (int i = 0; i < lang.getNumberOfUserDefinedOpNames(); i++)
                w.println((i > 0 ? "," : "") + "\"" + lang.getUserDefinedOpName(i) + "\"");
            w.println("]}");
        }
        Memory mem = currentProgram.getMemory();
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/memory.json"))) {
            w.println("[");
            boolean first = true;
            for (MemoryBlock b : mem.getBlocks()) {
                String file = "";
                if (b.isInitialized()) {
                    byte[] data = new byte[(int) b.getSize()];
                    b.getBytes(b.getStart(), data);
                    file = "mem_" + b.getName() + ".bin";
                    try (FileOutputStream fo = new FileOutputStream(out + "/" + file)) { fo.write(data); }
                }
                if (!first) w.println(",");
                first = false;
                w.print("{\"name\":\"" + b.getName() + "\",\"start\":" + b.getStart().getOffset() + ",\"size\":" + b.getSize()
                        + ",\"exec\":" + b.isExecute() + ",\"file\":\"" + file + "\"}");
            }
            w.println("]");
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/imports.json"))) {
            w.println("[");
            boolean first = true;
            for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
                MemoryBlock blk = mem.getBlock(fn.getEntryPoint());
                if (blk == null || !blk.getName().equals("EXTERNAL")) continue;
                String lib = "", orig = "";
                Function t = fn.isThunk() ? fn.getThunkedFunction(true) : fn;
                if (t != null && t.isExternal()) {
                    ghidra.program.model.symbol.ExternalLocation el = t.getExternalLocation();
                    lib = el.getLibraryName();
                    orig = el.getOriginalImportedName() == null ? "" : el.getOriginalImportedName();
                }
                for (Symbol sym : currentProgram.getSymbolTable().getSymbols(fn.getEntryPoint())) {
                    if (sym.isExternalEntryPoint()) orig = orig;
                }
                if (!first) w.println(",");
                first = false;
                w.print("{\"addr\":" + fn.getEntryPoint().getOffset() + ",\"name\":\"" + fn.getName() + "\",\"lib\":\"" + lib + "\",\"orig\":\"" + orig + "\"}");
            }
            w.println("]");
        }
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out + "/functions.json")))) {
            w.println("[");
            boolean firstF = true;
            for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
                if (fn.isExternal() || fn.isThunk() && fn.getBody().isEmpty()) continue;
                MemoryBlock blk = mem.getBlock(fn.getEntryPoint());
                if (blk == null || !blk.isExecute() || !blk.isInitialized()) continue;
                if (!firstF) w.println(",");
                firstF = false;
                w.println("{\"name\":\"" + fn.getName() + "\",\"entry\":" + fn.getEntryPoint().getOffset() + ",\"insns\":[");
                boolean firstI = true;
                for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
                    if (!firstI) w.println(",");
                    firstI = false;
                    StringBuilder sb = new StringBuilder();
                    sb.append("{\"a\":").append(ins.getAddress().getOffset());
                    sb.append(",\"n\":").append(ins.getLength());
                    sb.append(",\"t\":\"").append(ins.toString().replace("\\", "\\\\").replace("\"", "\\\"")).append("\"");
                    Address ft = ins.getFallThrough();
                    sb.append(",\"ft\":").append(ft == null ? "null" : Long.toString(ft.getOffset()));
                    sb.append(",\"refs\":[");
                    boolean fr = true;
                    for (Reference r : ins.getReferencesFrom()) {
                        RefType rt = r.getReferenceType();
                        if (!rt.isFlow()) continue;
                        if (!fr) sb.append(",");
                        fr = false;
                        sb.append(r.getToAddress().getOffset());
                    }
                    sb.append("],\"p\":[");
                    boolean fp = true;
                    for (PcodeOp op : ins.getPcode()) {
                        if (!fp) sb.append(",");
                        fp = false;
                        sb.append("[\"").append(op.getMnemonic()).append("\",").append(vn(op.getOutput()));
                        for (Varnode in : op.getInputs()) sb.append(",").append(vn(in));
                        sb.append("]");
                    }
                    sb.append("]}");
                    w.print(sb);
                }
                w.print("]}");
            }
            w.println("]");
        }
    }
}
