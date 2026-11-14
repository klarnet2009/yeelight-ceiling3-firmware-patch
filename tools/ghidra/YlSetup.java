// Pre-analysis setup for the ceiling3 Cortex-M4 app (segment 0 loaded raw at 0x10071000).
// Recreates what the startup code at 0x10071000 does (see disassembly-start.txt): copies
// .data to SYSRAM/TCM and zeroes .bss, so references into RAM resolve. Forces Thumb mode and
// seeds functions from code pointers stored in the image.
//@category Yeelight
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.io.ByteArrayInputStream;
import java.math.BigInteger;

public class YlSetup extends GhidraScript {
    static final long FLASH = 0x10071000L;

    Address a(long v) { return toAddr(v); }

    void copyBlock(String name, long dst, long src, int len, boolean exec) throws Exception {
        byte[] buf = new byte[len];
        currentProgram.getMemory().getBytes(a(src), buf);
        MemoryBlock b = currentProgram.getMemory().createInitializedBlock(
            name, a(dst), new ByteArrayInputStream(buf), len, monitor, false);
        b.setRead(true); b.setWrite(true); b.setExecute(exec);
    }

    void bssBlock(String name, long start, long end) throws Exception {
        MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, a(start), end - start, false);
        b.setRead(true); b.setWrite(true); b.setExecute(false);
    }

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock flash = mem.getBlock(a(FLASH));
        flash.setName("flash_app"); flash.setWrite(false); flash.setExecute(true);
        long flashEnd = flash.getEnd().getOffset() + 1;

        // literals at 0x10071058.. : sp, (src,dst,end) x2, bss ranges x2
        copyBlock("sysram_data", 0x20000000L, 0x1014C4E4L, 0x1548, false);
        bssBlock("sysram_bss", 0x20001548L, 0x2003AA44L);
        bssBlock("sysram_stack", 0x2003AA44L, 0x20040000L);
        copyBlock("tcm_text_data", 0x00100000L, 0x1014DA2CL, 0x2C18, true);  // ramfuncs + data
        bssBlock("tcm_bss", 0x00102C18L, 0x0010F4B8L);

        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        BigInteger one = BigInteger.ONE;
        currentProgram.getProgramContext().setValue(tmode, a(FLASH), a(flashEnd - 1), one);
        currentProgram.getProgramContext().setValue(tmode, a(0x00100000L), a(0x00102C17L), one);

        long[][] named = { {FLASH, 0}, {0x100BAC68L, 1}, {0x100A7694L, 2} };
        String[] names = { "reset_startup", "startup_init", "startup_main" };  // blx / bx targets of startup
        for (long[] n : named) { disassemble(a(n[0])); createFunction(a(n[0]), names[(int) n[1]]); }

        // Seed: odd words pointing into code whose target starts with PUSH (16-bit 0xB5xx / 0xB4xx
        // or 32-bit 0xE92D) -> almost certainly a function pointer (tables, callbacks, timers).
        int seeded = 0;
        long[][] ranges = { {FLASH, flashEnd}, {0x00100000L, 0x00102C18L} };
        for (long off = FLASH; off + 4 <= flashEnd; off += 4) {
            long v = mem.getInt(a(off)) & 0xFFFFFFFFL;
            if ((v & 1) == 0) continue;
            long t = v & ~1L;
            boolean inCode = false;
            for (long[] r : ranges) inCode |= (t >= r[0] && t + 2 <= r[1]);
            if (!inCode) continue;
            int hw = mem.getShort(a(t)) & 0xFFFF;
            if ((hw & 0xFE00) == 0xB400 || hw == 0xE92D) {
                if (getFunctionAt(a(t)) == null && disassemble(a(t)) && createFunction(a(t), null) != null) seeded++;
            }
        }
        println("YlSetup: memory map rebuilt, seeded " + seeded + " functions from code pointers");
    }
}
