import glob,os,json
M32=0xFFFFFFFF
def load(f):
    nb=None; pairs=[]
    for line in open(f):
        if line.startswith('#'):
            nb=int(line.strip().split('\t')[-1]); continue
        s,k=line.split(); pairs.append((int(s,16),int(k,16)))
    return nb,pairs
def inv_mod(a,m=1<<32):
    # a odd
    return pow(a,-1,m)
def fit_affine(pairs):
    # key = (A*x+B) mod 2^32 with x=seed or x=~seed; need A odd invertible
    for fn,xf in (('seed',lambda s:s),('~seed',lambda s:(~s)&M32)):
        # pick two seeds with x1-x2 odd-ish; use seeds 1 and 2
        d={s:k for s,k in pairs}
        # solve using x differences
        xs=[(xf(s),k) for s,k in pairs[:40]]
        (x1,k1),(x2,k2)=xs[1],xs[2]
        dx=(x1-x2)&M32
        if dx%2==0: 
            # try other pair
            found=False
            for i in range(len(xs)):
                for j in range(i+1,len(xs)):
                    if ((xs[i][0]-xs[j][0])&M32)%2==1:
                        (x1,k1),(x2,k2)=xs[i],xs[j]; dx=(x1-x2)&M32; found=True; break
                if found: break
            if not found: continue
        A=((k1-k2)*inv_mod(dx))&M32
        B=(k1-A*x1)&M32
        ok=all(((A*xf(s)+B)&M32)==k for s,k in pairs)
        if ok: return dict(fam='affine',x=fn,A=A,B=B)
    return None
def fit_xor(pairs):
    # key = (x ^ C) or rotate; quick checks
    for fn,xf in (('seed',lambda s:s),('~seed',lambda s:(~s)&M32)):
        C=pairs[0][1]^xf(pairs[0][0])
        if all((xf(s)^C)==k for s,k in pairs): return dict(fam='xor',x=fn,C=C)
    return None
res={}
for f in sorted(glob.glob('full/*.txt')):
    name=os.path.basename(f)[:-4]; nb,pairs=load(f)
    if not pairs: continue
    r=fit_xor(pairs) or fit_affine(pairs)
    if r is None: r=dict(fam='table' if nb==2 else 'complex')
    r['nb']=nb; res[name]=r
import collections
fams=collections.Counter(r['fam'] for r in res.values())
print('families:',dict(fams))
for fam in ('affine','xor','table','complex'):
    names=[n for n,r in res.items() if r['fam']==fam]
    print(fam,len(names),[n.replace('ASAM_seedkey_','').replace('ASAM_Seedkey_','') for n in names][:20])
json.dump(res,open('algos.json','w'),indent=0)
# show affine params
for n,r in sorted(res.items()):
    if r['fam']=='affine': print('  %-40s %s A=%08X B=%08X'%(n[:40],r['x'],r['A'],r['B']))
