import java.io.*; import java.lang.reflect.*; import java.util.*; import java.util.jar.*; import java.net.*;
public class Params {
  public static void main(String[] a) throws Exception {
    File dir=new File(a[0]); String stub=a[1];
    File[] jars=dir.listFiles((d,n)->n.endsWith(".jar")); Arrays.sort(jars);
    PrintStream out=new PrintStream(new FileOutputStream(a[2]),false,"UTF-8");
    for(File jf:jars){
      String cls=null;
      try(JarFile jar=new JarFile(jf)){ for(var e:Collections.list(jar.entries())){ String n=e.getName();
        if(n.endsWith(".class")&&!n.contains("$")&&!n.endsWith("ASAM_seedkey.class")){cls=n.substring(0,n.length()-6).replace('/','.');break;} } }
      if(cls==null) continue;
      try{
        URLClassLoader cl=new URLClassLoader(new URL[]{jf.toURI().toURL(),new File(stub).toURI().toURL()},Params.class.getClassLoader());
        Class<?> c=Class.forName(cls,true,cl); Object o=c.getDeclaredConstructor().newInstance();
        StringBuilder sb=new StringBuilder(); sb.append("JAR\t").append(jf.getName()).append('\n');
        for(Field f:c.getDeclaredFields()){ f.setAccessible(true); Object v=f.get(o);
          if(v instanceof byte[]){ byte[] b=(byte[])v; StringBuilder h=new StringBuilder(); for(byte x:b)h.append(String.format("%02X",x));
            sb.append("B\t").append(f.getName()).append('\t').append(h).append('\n'); }
          else if(v instanceof Integer||v instanceof Long){ sb.append("I\t").append(f.getName()).append('\t').append(v).append('\n'); } }
        Method mk=c.getMethod("SecM_ComputeKey",byte[].class); Method mb=c.getMethod("numberOfBytesForSeed");
        int nb=(Integer)mb.invoke(o); sb.append("NB\t").append(nb).append('\n');
        Random r=new Random(12345); int N=400;
        for(int t=0;t<N;t++){ byte[] seed=new byte[nb]; r.nextBytes(seed);
          byte[] key=(byte[])mk.invoke(o,(Object)seed);
          StringBuilder hs=new StringBuilder(),hk=new StringBuilder();
          for(byte x:seed)hs.append(String.format("%02X",x)); for(byte x:key)hk.append(String.format("%02X",x));
          sb.append("V\t").append(hs).append('\t').append(hk).append('\n'); }
        out.print(sb);
      }catch(Throwable t){ out.println("ERR\t"+jf.getName()+"\t"+t); }
    }
    out.close(); System.out.println("done");
  }
}
