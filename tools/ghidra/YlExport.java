// Post-analysis: name functions from their own log calls, then export decompiled C by module.
//
// The firmware logs as log(fmt, module, ..., __func__, __LINE__) with fmt ending in "(%s,%d)",
// e.g. "[W] %s: sta offline timeout (%s,%d)". For each call whose args resolve to such a format we
// read the second-to-last argument (r1..r3, then [sp], [sp+4], ...) as the caller's real name and
// the first argument as its module. Majority vote per function; only FUN_* names are replaced.
//
// args: <outdir>
//@category Yeelight
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class YlExport extends GhidraScript {
    static final Pattern CONV = Pattern.compile("%[-+ #0]*[0-9*]*(?:\\.[0-9*]+)?(?:hh|h|ll|l|z|j|t)?[diouxXcspfu]");
    static final Pattern IDENT = Pattern.compile("[A-Za-z_][A-Za-z0-9_]{2,80}");
    Memory mem;

    String cstr(long addr) {
        try {
            Address a = toAddr(addr);
            MemoryBlock b = mem.getBlock(a);
            if (b == null || !b.isInitialized()) return null;
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 200; i++) {
                int c = mem.getByte(a.add(i)) & 0xFF;
                if (c == 0) return sb.length() >= 2 ? sb.toString() : null;
                if (c != '\r' && c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) return null;
                sb.append((char) c);
            }
        } catch (Exception e) { /* unmapped */ }
        return null;
    }

    /** Constant loaded by `ldr rX, [pc, #imm]` (Ghidra keeps a READ ref to the literal). */
    Long literal(Instruction ins) {
        if (!ins.getMnemonicString().toLowerCase().startsWith("ldr")) return null;
        for (Reference r : ins.getReferencesFrom()) {
            if (!r.getReferenceType().isRead()) continue;
            Address la = r.getToAddress();
            MemoryBlock b = mem.getBlock(la);
            if (b == null || !b.isInitialized() || !b.getName().equals("flash_app")) continue;
            try { return mem.getInt(la) & 0xFFFFFFFFL; } catch (Exception e) { return null; }
        }
        return null;
    }

    Register reg(Object[] objs) {
        return objs.length > 0 && objs[0] instanceof Register ? (Register) objs[0] : null;
    }

    @Override
    protected void run() throws Exception {
        mem = currentProgram.getMemory();
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : "decomp";
        new File(outDir).mkdirs();

        Map<Function, Map<String, Integer>> nameVotes = new HashMap<>();
        Map<Function, Map<String, Integer>> modVotes = new HashMap<>();
        Map<Function, Integer> loggers = new HashMap<>();

        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) return;
            Deque<Instruction> window = new ArrayDeque<>();
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                if (ins.getFlowType().isCall()) {
                    handleCall(f, ins, window, nameVotes, modVotes, loggers);
                    window.clear();
                    continue;
                }
                window.addLast(ins);
                if (window.size() > 20) window.removeFirst();
            }
        }

        // Apply names
        Set<String> used = new HashSet<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) used.add(f.getName());
        int renamed = 0;
        Map<Function, String> module = new HashMap<>();
        for (Map.Entry<Function, Map<String, Integer>> e : modVotes.entrySet())
            module.put(e.getKey(), best(e.getValue()));
        for (Map.Entry<Function, Map<String, Integer>> e : nameVotes.entrySet()) {
            Function f = e.getKey();
            if (!f.getName().startsWith("FUN_")) continue;
            String n = best(e.getValue()), cand = n;
            for (int i = 2; used.contains(cand); i++) cand = n + "_" + i;
            f.setName(cand, SourceType.ANALYSIS);
            used.add(cand);
            renamed++;
        }
        for (Map.Entry<Function, Integer> e : loggers.entrySet()) {
            Function f = e.getKey();
            if (f.getName().startsWith("FUN_") && e.getValue() >= 5) {
                String n = "log_printf";
                for (int i = 2; used.contains(n); i++) n = "log_printf_" + i;
                f.setName(n, SourceType.ANALYSIS); used.add(n);
            }
        }
        int thunks = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.isThunk()) continue;
            if (!f.getName().startsWith("thunk_FUN_") && !f.getName().startsWith("FUN_")) continue;
            Function tgt = f.getThunkedFunction(true);
            if (tgt == null || tgt.getName().startsWith("FUN_")) continue;
            String n = "thunk_" + tgt.getName(), cand = n;
            for (int i = 2; used.contains(cand); i++) cand = n + "_" + i;
            f.setName(cand, SourceType.ANALYSIS);
            used.add(cand);
            thunks++;
        }
        println("YlExport: renamed " + renamed + " functions from log calls, " + thunks + " thunks, " + loggers.size() + " logger candidates");

        // Export
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Map<String, PrintWriter> files = new TreeMap<>();
        PrintWriter csv = new PrintWriter(new File(outDir, "functions.csv"), StandardCharsets.UTF_8);
        csv.println("entry,name,module,size,file");
        int n = 0, failed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) break;
            String mod = module.get(f);
            String file;
            if (mod != null && IDENT.matcher(mod).matches()) file = mod + ".c";
            else file = String.format("unsorted/%s_%05x.c",
                f.getEntryPoint().getAddressSpace().getName(), (f.getEntryPoint().getOffset() >> 16) & 0xFFFFF);
            PrintWriter w = files.get(file);
            if (w == null) {
                File out = new File(outDir, file);
                out.getParentFile().mkdirs();
                w = new PrintWriter(out, StandardCharsets.UTF_8);
                w.println("// Ghidra 12.2 pseudo-C from yeelink.light.ceiling3 2.0.2_0048 (segment 0). Not original source.");
                files.put(file, w);
            }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            w.println();
            w.printf("// ---- %s @ %s (size %d) ----%n", f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses());
            if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
            else { w.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage())); failed++; }
            csv.printf("%s,%s,%s,%d,%s%n", f.getEntryPoint(), f.getName(), mod == null ? "" : mod,
                f.getBody().getNumAddresses(), file);
            if (++n % 250 == 0) println("YlExport: decompiled " + n);
        }
        for (PrintWriter w : files.values()) w.close();
        csv.close();
        di.dispose();
        println("YlExport: " + n + " functions -> " + files.size() + " files in " + outDir + ", failed " + failed);
    }

    void handleCall(Function f, Instruction call, Deque<Instruction> window,
                    Map<Function, Map<String, Integer>> nameVotes,
                    Map<Function, Map<String, Integer>> modVotes, Map<Function, Integer> loggers) {
        Map<String, Long> regs = new HashMap<>();
        Map<Long, Long> stack = new HashMap<>();
        for (Instruction ins : window) {
            String m = ins.getMnemonicString().toLowerCase();
            Register d = reg(ins.getOpObjects(0));
            Long lit = literal(ins);
            if (lit != null && d != null) { regs.put(d.getName(), lit); continue; }
            if ((m.equals("str") || m.equals("str.w")) && d != null && ins.getNumOperands() > 1) {
                Object[] o = ins.getOpObjects(1);
                if (o.length >= 1 && o[0] instanceof Register && ((Register) o[0]).getName().equals("sp")) {
                    long k = 0;
                    for (Object x : o) if (x instanceof Scalar) k = ((Scalar) x).getSignedValue();
                    Long v = regs.get(d.getName());
                    if (v != null) stack.put(k, v); else stack.remove(k);
                }
                continue;
            }
            if (m.startsWith("mov") && d != null && ins.getNumOperands() > 1) {
                Register s = reg(ins.getOpObjects(1));
                if (s != null && regs.containsKey(s.getName())) regs.put(d.getName(), regs.get(s.getName()));
                else regs.remove(d.getName());
                continue;
            }
            if (d != null) regs.remove(d.getName());  // any other write clobbers the tracked value
        }
        Long fmtAddr = regs.get("r0");
        String fmt = fmtAddr == null ? null : cstr(fmtAddr);
        if (fmt == null) return;

        // Collect all % conversions in fmt
        List<String> convTypes = new ArrayList<>();
        Matcher mc = CONV.matcher(fmt.replace("%%", ""));
        while (mc.find()) convTypes.add(mc.group());
        int nconv = convTypes.size();

        // FreeRTOS / lwIP / HAL assert(cond, file, line) or assert-like calls
        for (int i = 0; i <= 3 && i < Math.max(nconv, 3); i++) {
            Long a = i < 3 ? regs.get("r" + i) : null; // handle r0/r1/r2 assert args even without fmt
            if (a == null) continue;
            String s = cstr(a);
            if (s != null && s.endsWith(".c")) {
                int sl = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
                String base = (sl >= 0 ? s.substring(sl + 1) : s);
                base = base.substring(0, base.length() - 2);
                if (IDENT.matcher(base).matches())
                    modVotes.computeIfAbsent(f, k -> new HashMap<>()).merge(base, 1, Integer::sum);
            }
        }

        if (nconv == 0) return;
        List<Long> args = new ArrayList<>();
        for (int i = 0; i < nconv; i++)
            args.add(i < 3 ? regs.get("r" + (i + 1)) : stack.get((long) 4 * (i - 3)));

        if (fmt.matches("(?s).*\\(%s,\\s*%d\\)\\s*$") && nconv >= 2) {
            Long fn = args.get(nconv - 2);
            String name = fn == null ? null : cstr(fn);
            if (name != null && IDENT.matcher(name).matches())
                nameVotes.computeIfAbsent(f, k -> new HashMap<>()).merge(name, 1, Integer::sum);
            if (fmt.matches("(?s)^\\[[A-Z]\\] %s:.*") && args.get(0) != null) {
                String mod = cstr(args.get(0));
                if (mod != null) modVotes.computeIfAbsent(f, k -> new HashMap<>()).merge(mod, 1, Integer::sum);
            }
            for (Reference r : call.getReferencesFrom()) {
                Function callee = getFunctionAt(r.getToAddress());
                if (callee != null) loggers.merge(callee, 1, Integer::sum);
            }
            return;
        }

        // Pattern: "[I] %s: func:%s, ..." or "[I] @@@file:[%s] func:[%s] ..."
        if (fmt.matches("(?s).*func:\\[?%s.*")) {
            Matcher fm = Pattern.compile("func:\\[?%s").matcher(fmt);
            if (fm.find()) {
                String prefix = fmt.substring(0, fm.start()).replace("%%", "");
                Matcher pm = CONV.matcher(prefix);
                int idx = 0;
                while (pm.find()) idx++;
                if (idx < args.size() && args.get(idx) != null) {
                    String name = cstr(args.get(idx));
                    if (name != null && IDENT.matcher(name).matches())
                        nameVotes.computeIfAbsent(f, k -> new HashMap<>()).merge(name, 1, Integer::sum);
                }
            }
            if (fmt.matches("(?s)^\\[[A-Z]\\] %s:.*") && args.get(0) != null) {
                String mod = cstr(args.get(0));
                if (mod != null) modVotes.computeIfAbsent(f, k -> new HashMap<>()).merge(mod, 1, Integer::sum);
            }
        }
    }

    static String best(Map<String, Integer> votes) {
        return Collections.max(votes.entrySet(), Map.Entry.comparingByValue()).getKey();
    }
}
