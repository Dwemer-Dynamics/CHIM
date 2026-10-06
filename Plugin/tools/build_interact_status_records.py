"""Author persistent single-target timed Interact effects; preserve all existing ESP records.
Requires the installed Skyrim.esm as the documented vanilla effect source. No game process is touched.
Stable locals: MGEF 060000..060005; SPEL 060010..060027 (four durations per family).
"""
from pathlib import Path
import argparse, struct, zlib, runpy
helpers=runpy.run_path(str(Path(__file__).with_name('build_interact_records.py')))
sub,record,walk,chunks=(helpers[k] for k in ['sub','record','walk','chunks'])
p=argparse.ArgumentParser();p.add_argument('--skyrim',type=Path,default=Path(r'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\Skyrim.esm'));args=p.parse_args()
path=helpers['PATH'];original=path.read_bytes();existing=list(walk(original));vanilla={}
# walk() preserves compressed payloads; expand only the six explicitly selected vanilla records.
sources=[('poison',0x10AA4A,0),('burning',0x12F03,0),('paralysis',0x1EA6E,21),('calm',0x4DEE7,6),('fear',0x1EA77,7),('frenzy',0x4DEE6,8)]
raw=args.skyrim.read_bytes()
def scan(start,end):
 while start<end:
  tag=raw[start:start+4];n=struct.unpack_from('<I',raw,start+4)[0]
  if tag==b'GRUP':scan(start+24,start+n);start+=n;continue
  fid=struct.unpack_from('<I',raw,start+12)[0]
  if tag==b'MGEF' and fid in [x[1] for x in sources]:
   body=raw[start+24:start+24+n]
   if struct.unpack_from('<I',raw,start+8)[0]&0x40000:body=zlib.decompress(body[4:])
   vanilla[fid]=list(chunks(body))
  start+=24+n
scan(0,len(raw));add={b'MGEF':bytearray(),b'SPEL':bytearray()};expected={}
for index,(name,source,archetype) in enumerate(sources):
 fields=vanilla[source];data=bytearray(dict(fields)[b'DATA']);assert len(data)==152
 assert struct.unpack_from('<I',data,64)[0]==archetype
 flags=struct.unpack_from('<I',data)[0];flags &= ~((1<<9)|(1<<17)|(1<<21)|(1<<22));flags |= 1<<11
 struct.pack_into('<I',data,0,flags)
 # No area, projectile, explosion, casting art, skill scaling, taper or secondary effects.
 for off in [24,28,40,44,48,52,56,60,72,76,92,104,108,112,120,124,128,132,136]:struct.pack_into('<I',data,off,0)
 struct.pack_into('<II',data,80,1,2) # FireAndForget, Aimed (direct exact-target native cast).
 effect=0x05060000+index
 body=sub(b'EDID',f'CHIMInteract{name.title()}Effect\0'.encode())+sub(b'FULL',f'CHIM Interact {name}\0'.encode())+sub(b'DATA',data)
 # Retain authored keywords and target conditions, never vanilla attached scripts.
 for tag,value in fields:
  if tag in [b'KSIZ',b'KWDA',b'CTDA',b'CIS1',b'CIS2']:body+=sub(tag,value)
 expected[effect]=(b'MGEF',body);add[b'MGEF']+=record(b'MGEF',effect,body)
 for variant,duration in enumerate([5,10,20,30]):
  fid=0x05060010+index*4+variant
  spit=struct.pack('<IIIfIIffI',0,1,0,0.0,1,2,0.0,0.0,0)
  body=sub(b'EDID',f'CHIMInteract{name.title()}{duration}\0'.encode())+sub(b'OBND',b'\0'*12)+sub(b'FULL',f'CHIM Interact {name}\0'.encode())+sub(b'SPIT',spit)+sub(b'EFID',struct.pack('<I',effect))+sub(b'EFIT',struct.pack('<fII',1.0,0,duration))
  expected[fid]=(b'SPEL',body);add[b'SPEL']+=record(b'SPEL',fid,body)
known={fid:(tag,body) for tag,fid,body in existing}
if any(fid in known for fid in expected):
 assert all(known.get(fid)==value for fid,value in expected.items()),'Collision or changed authored status records'
 print('Timed Interact records already present and identical');raise SystemExit
out=bytearray();pos=0
while pos<len(original):
 tag=original[pos:pos+4];n=struct.unpack_from('<I',original,pos+4)[0];total=n if tag==b'GRUP' else n+24;block=bytearray(original[pos:pos+total])
 if tag==b'TES4':
  count,nextid=struct.unpack_from('<II',block,34);struct.pack_into('<II',block,34,count+len(expected),max(nextid,0x60028))
 elif tag==b'GRUP' and block[8:12] in (b'MGEF',b'SPEL') and struct.unpack_from('<I',block,12)[0]==0:
  block+=add.pop(bytes(block[8:12]));struct.pack_into('<I',block,4,len(block))
 out+=block;pos+=total
assert not add
updated=list(walk(out));assert len(updated)==len(existing)+30
assert all((tag,body)==known[fid] for tag,fid,body in updated if fid in known and tag!=b'TES4')
path.write_bytes(out);print('Added6MGEF+24SPEL; every prior record body preserved. Engine-owned expiry; no dynamic forms.')
