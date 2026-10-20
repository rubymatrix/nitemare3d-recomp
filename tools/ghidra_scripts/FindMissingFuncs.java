// Create functions for code Ghidra's analysis missed (vtable/message-map targets).
// Scans bytes not covered by any instruction in executable blocks for typical
// MSVC Win16 prologues and disassembles/creates functions there. Repeats until stable.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.app.cmd.function.CreateFunctionCmd;

public class FindMissingFuncs extends GhidraScript {
    private boolean match(byte[] b, int i, int... pat) {
        if (i + pat.length > b.length) return false;
        for (int k = 0; k < pat.length; k++) if (pat[k] >= 0 && (b[i + k] & 0xff) != pat[k]) return false;
        return true;
    }

    // Resolve near jump tables of the form JMP word ptr CS:[BX + table] that analysis left
    // without targets (e.g. the C runtime's printf state machine).
    private int resolveJumpTables() throws Exception {
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        int fixed = 0;
        for (Instruction ins : listing.getInstructions(true)) {
            String t = ins.toString();
            if (!t.startsWith("JMP word ptr CS:[BX + 0x")) continue;
            boolean hasFlow = false;
            for (Reference r : ins.getReferencesFrom()) if (r.getReferenceType().isFlow()) hasFlow = true;
            if (hasFlow) continue;
            long table = Long.decode(t.substring(t.indexOf("0x"), t.indexOf("]")));
            Address seg = ins.getAddress();
            long base = seg.getOffset() & ~0xffffL;
            long here = seg.getOffset() & 0xffff;
            java.util.List<Address> targets = new java.util.ArrayList<>();
            for (int k = 0; k < 64; k++) {
                Address ta = seg.getNewAddress(base + table + 2 * k);
                int w = mem.getShort(ta) & 0xffff;
                if (w < here - 0x400 || w > here + 0x1000) break;
                targets.add(seg.getNewAddress(base + w));
            }
            for (Address to : targets) {
                new DisassembleCommand(to, null, true).applyTo(currentProgram, monitor);
                currentProgram.getReferenceManager().addMemoryReference(ins.getAddress(), to, RefType.COMPUTED_JUMP, SourceType.ANALYSIS, 0);
            }
            Function f = getFunctionContaining(ins.getAddress());
            if (f != null) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            println("resolved jump table at " + ins.getAddress() + ": " + targets.size() + " targets");
            fixed++;
        }
        return fixed;
    }

    @Override
    public void run() throws Exception {
        resolveJumpTables();
        Listing listing = currentProgram.getListing();
        int total = 0;
        for (int pass = 0; pass < 8; pass++) {
            int created = 0;
            for (MemoryBlock blk : currentProgram.getMemory().getBlocks()) {
                if (!blk.isExecute() || !blk.isInitialized()) continue;
                byte[] b = new byte[(int) blk.getSize()];
                blk.getBytes(blk.getStart(), b);
                for (int i = 0; i < b.length; i++) {
                    Address a = blk.getStart().add(i);
                    if (listing.getInstructionContaining(a) != null) continue;
                    if (listing.getDefinedDataContaining(a) != null) continue;
                    boolean prologue =
                        match(b, i, 0x55, 0x8b, 0xec) ||                       // push bp; mov bp,sp
                        match(b, i, 0xc8, -1, -1, 0x00) ||                     // enter n,0
                        match(b, i, 0x8c, 0xd8, 0x90, 0x45, 0x55, 0x8b, 0xec) || // mov ax,ds; nop; inc bp; push bp; mov bp,sp
                        match(b, i, 0x1e, 0x58, 0x90, 0x45, 0x55, 0x8b, 0xec) || // push ds; pop ax; nop; inc bp; ...
                        match(b, i, 0x45, 0x55, 0x8b, 0xec);                   // inc bp; push bp; mov bp,sp
                    if (!prologue) continue;
                    // Prefer the start of an exported-style prologue if we are inside one.
                    DisassembleCommand cmd = new DisassembleCommand(a, null, true);
                    cmd.applyTo(currentProgram, monitor);
                    if (listing.getInstructionAt(a) == null) continue;
                    if (getFunctionAt(a) == null && getFunctionContaining(a) == null) {
                        if (createFunction(a, null) != null) created++;
                    }
                }
            }
            total += created;
            println("pass " + pass + ": created " + created);
            if (created == 0) break;
            analyzeChanges(currentProgram);
        }
        println("total created " + total);
    }
}
