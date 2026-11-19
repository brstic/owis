import glob,os,re,subprocess,json
JAVAP='/opt/homebrew/opt/openjdk@21/bin/javap'
res=json.load(open('algos.json'))
def load(f):
    d={}
    for line in open(f):
        if line.startswith('#'): continue
        s,k=line.split(); d[int(s,16)]=int(k,16)
    return d
# locate the jar on disk
jars={}
import os
for P in ['G1','9x1','918S','CS1','Cajun']:
    D=os.path.expanduser('~/Projects/porsche/piwis_data/%s/Allgemein/DLL'%P)
    if os.path.isdir(D):
        for f in os.listdir(D):
            if f.endswith('.jar'): jars.setdefault(f[:-4], os.path.join(D,f))
def extract(name):
    jar=jars.get(name)
    if not jar: return None
    import zipfile
    z=zipfile.ZipFile(jar); cls=[n for n in z.namelist() if n.endswith('.class') and '$' not in n][0]
    tmp='/tmp/_sk'; os.makedirs(tmp,exist_ok=True); z.extract(cls,tmp)
    bc=subprocess.run([JAVAP,'-p','-c','-constants',os.path.join(tmp,cls)],capture_output=True,text=True).stdout
    m=re.search(r'SecM_ComputeKey.*?(?=\n  public|\n  private|\Z)',bc,re.S)
    if not m: return None
    body=m.group(0)
    # XOR operand: iconst_m1 (=-1) or ldc #x // int N  preceding ixor; multiply: ldc //int N before imul; add: before iadd
    def op_before(mnem):
        vals=[]
        for mm in re.finditer(r'(iconst_m1|iconst_(\d)|ldc\s+#\d+\s+//\s+int (-?\d+))\s*\n\s*\d+:\s*'+mnem, body):
            if mm.group(1)=='iconst_m1': vals.append(-1)
            elif mm.group(2) is not None: vals.append(int(mm.group(2)))
            else: vals.append(int(mm.group(3)))
        return vals
    xs=op_before('ixor'); ms=op_before('imul'); ad=op_before('iadd')
    # require exactly the keyed-multiply shape: one ixor, one imul, one iadd, no arrays/loops beyond the 4-byte assemble
    if 'newarray' in body or 'getstatic' in body: return None
    if len(ms)!=1 or len(ad)==0: return None
    K=(xs[0] if xs else 0)&0xFFFFFFFF; A=ms[0]&0xFFFFFFFF; B=ad[-1]&0xFFFFFFFF
    return K,A,B
out={}; fail=[]
for name,r in res.items():
    if r.get('nb')!=4: continue
    full='full/%s.txt'%name
    if not os.path.exists(full): continue
    d=load(full); 
    e=extract(name)
    if e:
        K,A,B=e
        ok=all((((s^K)*A+B)&0xFFFFFFFF)==k for s,k in d.items())
        if ok: out[name]=dict(fam='kmul',K=K,A=A,B=B); continue
        else: fail.append((name,'const-mismatch'))
    # affine already known
    if r['fam']=='affine': out[name]=dict(fam='kmul',K=0,A=r['A'],B=r['B']); continue
    if r['fam']=='xor': out[name]=dict(fam='kmul',K=r['C'],A=1,B=0) if r['x']=='seed' else dict(fam='kmul',K=(r['C']^0xFFFFFFFF),A=0xFFFFFFFF,B=0); continue
    fail.append((name, r['fam']))
print('keyed-multiply solved:',len(out),'/ 4-byte')
print('remaining 4-byte (true array hashes):',len(fail))
for n,why in fail: print('   ',n.replace('ASAM_seedkey_','').replace('ASAM_Seedkey_','').replace('ASAM_seed_',''),why)
json.dump(out,open('kmul.json','w'),indent=0)
