import java.io.*; import java.lang.reflect.*; import java.net.*; import java.util.*;
public class DumpFull {
  // args: <jar> <stubdir> <out> <count>  (count seeds: for 2-byte dumps all 65536; else <count> pseudo-random + sequential)
  public static void main(String[] a) throws Exception {
    File jf = new File(a[0]); String stub = a[1];
    String cls=null;
    try (var jar = new java.util.jar.JarFile(jf)) {
      for (var e : Collections.list(jar.entries())) { String n=e.getName();
        if (n.endsWith(".class") && !n.contains("$")) { cls=n.substring(0,n.length()-6).replace('/','.'); break; } }
    }
    URLClassLoader cl = new URLClassLoader(new URL[]{ jf.toURI().toURL(), new File(stub).toURI().toURL() }, DumpFull.class.getClassLoader());
    Class<?> c = Class.forName(cls, true, cl); Object o = c.getDeclaredConstructor().newInstance();
    Method mk=c.getMethod("SecM_ComputeKey", byte[].class); int nb=(Integer)c.getMethod("numberOfBytesForSeed").invoke(o);
    PrintStream out=new PrintStream(new BufferedOutputStream(new FileOutputStream(a[2]),1<<20),false,"UTF-8");
    out.println("# "+c.getMethod("getName").invoke(o)+"\t"+c.getMethod("getAccessMode").invoke(o)+"\t"+nb);
    java.util.function.LongConsumer emit = s -> {
      byte[] seed=new byte[nb]; for(int i=0;i<nb;i++) seed[nb-1-i]=(byte)((s>>(8*i))&0xFF);
      try { byte[] key=(byte[])mk.invoke(o,(Object)seed); long k=0; for(byte b:key) k=(k<<8)|(b&0xFF);
        out.println(Long.toHexString(s)+" "+Long.toHexString(k)); } catch(Exception e){ throw new RuntimeException(e);} };
    if (nb==2) { for(long s=0;s<65536;s++) emit.accept(s); }
    else { long n=Long.parseLong(a[3]); java.util.Random r=new java.util.Random(42);
      for(long s=0;s<2048;s++) emit.accept(s);
      for(long i=0;i<n;i++) emit.accept(r.nextLong()&0xFFFFFFFFL);
      for(long s=0xFFFFF000L;s<=0xFFFFFFFFL;s++) emit.accept(s); }
    out.close(); System.out.println("ok "+nb);
  }
}
