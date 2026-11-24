import sys, struct
sys.path.insert(0,'tools')
from ttp_dump import urbs
def items(p):
    i=2; out=[]
    while i+5<=len(p):
        ty=p[i]; ln=struct.unpack_from(">I",p,i+1)[0]; v=p[i+5:i+5+ln]; i+=5+ln
        out.append((ty,v))
    return out
def show(ty,v):
    if ty==4: return "fx %.3f"%(struct.unpack(">q",v)[0]/2**42)
    if ty==5: return "struct n=%d id=%d"%(struct.unpack(">HH",v))
    if ty in(1,2,3): return "u32 %d"%struct.unpack(">I",v)[0] if len(v)==4 else v.hex()
    if ty==6: return "u64 %d"%struct.unpack(">Q",v)[0]
    if ty==0x14: return "str %r"%v
    return "t%d len=%d %s"%(ty,len(v),v[:24].hex())
if __name__=="__main__":
    pk=[(t,d) for t,ep,d in urbs(sys.argv[1]) if ep==0x83 and len(d)>32 and d[8:12]==b"\0\0\0\x53" and d[20:24]==b"\0\0\x05\0"]
    n=int(sys.argv[2])
    for j,(ty,v) in enumerate(items(pk[n][1][32:])): print(j,ty,show(ty,v))
