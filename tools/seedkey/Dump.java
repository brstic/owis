import java.io.*; import java.lang.reflect.*; import java.net.*; import java.util.*;
public class Dump {
  public static void main(String[] a) throws Exception {
    File dir = new File(a[0]); String stub = a[1];
    File[] jars = dir.listFiles((d,n)->n.endsWith(".jar"));
    Arrays.sort(jars);
    PrintStream out = new PrintStream(new FileOutputStream(a[2]), false, "UTF-8");
    for (File jf : jars) {
      String cls = null;
      try (java.util.jar.JarFile jar = new java.util.jar.JarFile(jf)) {
        for (var e : Collections.list(jar.entries())) {
          String n = e.getName();
          if (n.endsWith(".class") && !n.contains("$")) { cls = n.substring(0,n.length()-6).replace('/','.'); break; }
        }
      }
      if (cls == null) continue;
      try {
        URLClassLoader cl = new URLClassLoader(new URL[]{ jf.toURI().toURL(), new File(stub).toURI().toURL() }, Dump.class.getClassLoader());
        Class<?> c = Class.forName(cls, true, cl);
        Object o = c.getDeclaredConstructor().newInstance();
        Method mk = c.getMethod("SecM_ComputeKey", byte[].class);
        Method mn = c.getMethod("getName"); Method ma = c.getMethod("getAccessMode"); Method mb = c.getMethod("numberOfBytesForSeed");
        int nb = (Integer)mb.invoke(o);
        StringBuilder sb = new StringBuilder();
        sb.append(jf.getName()).append('\t').append(mn.invoke(o)).append('\t').append(ma.invoke(o)).append('\t').append(nb).append('\t');
        // sample seeds
        long[] seeds = {0,1,2,3,0x1234,0xABCD,0xFFFF,0x00010203L,0xDEADBEEFL,0x7F3C91A2L,0x80000000L,0xFFFFFFFFL,0x55555555L,0xAAAAAAAAL,0x01020304L};
        for (long s : seeds) {
          byte[] seed = new byte[nb];
          for (int i=0;i<nb;i++) seed[nb-1-i] = (byte)((s>>(8*i))&0xFF);
          byte[] key = (byte[])mk.invoke(o, (Object)seed);
          StringBuilder h = new StringBuilder();
          for (byte b : key) h.append(String.format("%02X", b));
          sb.append(Long.toHexString(s)).append('=').append(h).append(' ');
        }
        out.println(sb.toString());
      } catch (Throwable t) { out.println(jf.getName()+"\tERROR\t"+t); }
    }
    out.close();
    System.out.println("done");
  }
}
