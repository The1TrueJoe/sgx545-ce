import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpDecomp extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    long lo = Long.decode(a[1]), hi = Long.decode(a[2]);
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    PrintWriter w = new PrintWriter(new FileWriter(a[0]));
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      long e = f.getEntryPoint().getOffset() - currentProgram.getImageBase().getOffset();
      if (e < lo || e >= hi) continue;
      DecompileResults r = d.decompileFunction(f, 120, monitor);
      w.println("// ===== " + f.getName() + " @ " + Long.toHexString(e));
      if (r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
      else w.println("// failed: " + r.getErrorMessage());
    }
    w.close();
  }
}
