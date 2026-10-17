"""Append bounded Interact effect families from vanilla records; never rewrite prior records.
MGEF locals 060100..06014D, SPEL locals 060200..060247. Skyrim owns duration/expiry.
DATA offsets below are serialized 32-bit record fields, not runtime pointer layouts.
"""
from pathlib import Path
import argparse,struct,zlib,runpy
h=runpy.run_path(str(Path(__file__).with_name('build_interact_records.py')))
sub,record,walk,chunks=(h[k] for k in ('sub','record','walk','chunks'))
p=argparse.ArgumentParser();p.add_argument('--skyrim',type=Path,default=Path(r'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\Skyrim.esm'));args=p.parse_args()
# name, source, archetype, override AV, recover, detrimental; frost has a second slow component.
families=[('frost',0x1CEA2,5,None,False,True),('shock',0x1CEA8,5,None,False,True),
 ('drain_stamina',0x12F03,0,26,False,True),('drain_magicka',0x12F03,0,25,False,True),
 ('slow',0xEFC62,0,30,True,True),('haste',0xEFC62,0,30,True,False),
 ('weaken_armor',0xEFC62,0,39,True,True),('fortify_armor',0xEFC62,0,39,True,False),
 ('weaken_weapon',0xEFC62,0,154,True,True),('fortify_weapon',0xEFC62,0,154,True,False),
 ('absorb_health',0xAA155,4,24,False,True),('absorb_stamina',0xAA157,4,26,False,True),('absorb_magicka',0xAA156,4,25,False,True),
 ('ethereal',0x64D68,41,None,False,False),('soul_trap',0xF5D24,23,None,False,True),
 ('reanimate',0x16C3D,22,None,False,False),('turn_undead',0x5DD5C,24,None,False,True),('banish',0x6F951,42,None,False,True)]
raw=args.skyrim.read_bytes();sources={}
def scan(a,b):
 while a<b:
  tag=raw[a:a+4];n=struct.unpack_from('<I',raw,a+4)[0]
  if tag==b'GRUP':scan(a+24,a+n);a+=n;continue
  fid=struct.unpack_from('<I',raw,a+12)[0]
  if tag==b'MGEF' and fid in {v[1] for v in families}:
   body=raw[a+24:a+24+n]
   if struct.unpack_from('<I',raw,a+8)[0]&0x40000:body=zlib.decompress(body[4:])
   sources[fid]=list(chunks(body))
  a+=24+n
scan(0,len(raw));expected={};groups={b'MGEF':bytearray(),b'SPEL':bytearray()}
def effect_body(name,source,arch,av,recover,detrimental):
 fields=sources[source];d=bytearray(dict(fields)[b'DATA']);assert len(d)==152 and struct.unpack_from('<I',d,64)[0]==arch
 flags=struct.unpack_from('<I',d)[0]
 flags&=~((1<<9)|(1<<10)|(1<<17)|(1<<21)|(1<<22)|(1<<28));flags|=1<<11
 if recover:flags|=1<<1
 elif arch in (0,4,5):flags&=~(1<<1)
 flags=(flags|5) if detrimental else (flags&~5)
 if arch in (41,23):flags|=1<<10
 struct.pack_into('<I',d,0,flags)
 # Remove projectiles, area/explosion, skill scaling, taper, equip abilities and scripts.
 for off in (24,40,44,48,52,56,72,76,92,104,108,112,120,124,128,132,136):struct.pack_into('<I',d,off,0)
 struct.pack_into('<I',d,12,0xFFFFFFFF) # no skill
 struct.pack_into('<II',d,80,1,3)
 if av is not None:struct.pack_into('<I',d,68,av)
 if av is not None and arch==0:
  struct.pack_into('<I',d,16,44 if detrimental else 0xFFFFFFFF)
  for off in (28,32,36,96,100):struct.pack_into('<I',d,off,0) # no borrowed elemental art
 if arch==5:struct.pack_into('<f',d,60,1.0) # equal health and secondary resource damage
 if name=='FrostSlow':struct.pack_into('<I',d,16,43)
 if av is not None and arch==0:
  struct.pack_into('<I',d,88,0xFFFFFFFF);struct.pack_into('<f',d,60,0)
 body=sub(b'EDID',f'CHIMInteract{name}Effect\0'.encode())+sub(b'FULL',f'CHIM Interact {name}\0'.encode())+sub(b'DATA',d)
 # Keep actual vanilla target conditions (undead/summoned/etc) and keywords.
 for tag,value in fields:
  if not (av is not None and arch==0) and tag in (b'KSIZ',b'KWDA',b'CTDA',b'CIS1',b'CIS2'):body+=sub(tag,value)
 return body
for i,spec in enumerate(families):
 name,source,arch,av,recover,det=spec;fid=0x05060100+i*4
 body=effect_body(*spec);expected[fid]=(b'MGEF',body)
 effects=[fid]
 if name=='frost':
  fid2=fid+1;expected[fid2]=(b'MGEF',effect_body('FrostSlow',0xEFC62,0,30,True,True));effects.append(fid2)
 for variant,duration in enumerate((5,10,20,30)):
  spell=0x05060200+i*4+variant
  spit=struct.pack('<IIIfIIffI',0,1,0,0.,1,3,0.,0.,0)
  body=sub(b'EDID',f'CHIMInteract{name}{duration}\0'.encode())+sub(b'OBND',b'\0'*12)+sub(b'FULL',f'CHIM Interact {name}\0'.encode())+sub(b'SPIT',spit)
  for effect in effects:body+=sub(b'EFID',struct.pack('<I',effect))+sub(b'EFIT',struct.pack('<fII',1.,0,0 if name=='banish' else duration))
  expected[spell]=(b'SPEL',body)
path=h['PATH'];original=path.read_bytes();known={fid:(tag,body) for tag,fid,body in walk(original)}
for fid,(tag,body) in expected.items():
 if fid in known:assert known[fid]==(tag,body),f'Authored record collision {fid:08X}'
 else:groups[tag]+=record(tag,fid,body)
added=sum(fid not in known for fid in expected)
if not added:print('All expanded records identical; no changes');raise SystemExit
assert added==len(expected),'Partial expanded installation'
out=bytearray();pos=0
while pos<len(original):
 tag=original[pos:pos+4];n=struct.unpack_from('<I',original,pos+4)[0];total=n if tag==b'GRUP' else n+24;block=bytearray(original[pos:pos+total])
 if tag==b'TES4':
  count,nextid=struct.unpack_from('<II',block,34);struct.pack_into('<II',block,34,count+added,max(nextid,0x60248))
 elif tag==b'GRUP' and bytes(block[8:12]) in groups and struct.unpack_from('<I',block,12)[0]==0:
  block+=groups.pop(bytes(block[8:12]));struct.pack_into('<I',block,4,len(block))
 out+=block;pos+=total
assert not groups
assert all((tag,body)==known[fid] for tag,fid,body in walk(out) if fid in known and tag!=b'TES4')
path.write_bytes(out);print(f'Added {added} records; all existing nonheader record bodies preserved')
