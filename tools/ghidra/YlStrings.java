// Type literal-pool words used by `ldr rX, [pc, #imm]` as pointers, and define C strings at
// their targets, so the decompiler prints "text" / &DAT_2000xxxx instead of DAT_<pool address>.
//@category Yeelight
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class YlStrings extends GhidraScript {
    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        int ptrs = 0, strs = 0;
        for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
            if (monitor.isCancelled()) return;
            if (!ins.getMnemonicString().toLowerCase().startsWith("ldr")) continue;
            for (Reference r : ins.getReferencesFrom()) {
                if (!r.getReferenceType().isRead()) continue;
                Address lit = r.getToAddress();
                MemoryBlock lb = mem.getBlock(lit);
                if (lb == null || !lb.getName().equals("flash_app") || lit.getOffset() % 4 != 0) continue;
                long v = mem.getInt(lit) & 0xFFFFFFFFL;
                Address t;
                try { t = toAddr(v); } catch (Exception e) { continue; }
                MemoryBlock tb = mem.getBlock(t);
                if (tb == null || getInstructionContaining(lit) != null) continue;
                if (getDataAt(lit) == null || !getDataAt(lit).isPointer()) {
                    try { clearListing(lit, lit.add(3)); createData(lit, PointerDataType.dataType); ptrs++; }
                    catch (Exception e) { continue; }
                }
                if (tb.isInitialized() && !tb.isExecute() || tb.getName().equals("flash_app")) {
                    if (getInstructionContaining(t) != null || getDataContaining(t) != null && getDataContaining(t).isDefined()) continue;
                    int len = 0;
                    try {
                        while (len < 512) {
                            int c = mem.getByte(t.add(len)) & 0xFF;
                            if (c == 0) break;
                            if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c > 0x7E) { len = -1; break; }
                            len++;
                        }
                    } catch (Exception e) { len = -1; }
                    if (len >= 2 && len < 512) {
                        try { createAsciiString(t); strs++; } catch (Exception e) { /* overlap */ }
                    }
                }
            }
        }
        println("YlStrings: typed " + ptrs + " literal pointers, defined " + strs + " strings");
    }
}
