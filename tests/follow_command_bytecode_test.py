"""Run mock engine scenarios against disassembled Papyrus follow scripts.

Usage: python tests/follow_command_bytecode_test.py <disassembly-directory>
The directory must contain AIAgentAIMind.pex.txt and AIAgentFollowSafety.pex.txt
produced by the Papyrus compiler's `disassembly` command. Native engine effects
are modeled as package/faction state changes; no Skyrim process is started.
"""
from pathlib import Path
import re
import sys
root=Path(sys.argv[1])
def read(script,fn):
 s=(root/(script+'.pex.txt')).read_text(encoding='utf-8-sig')
 body=re.search(r"\n\t\t\tname: '"+fn+r"'\n(.*?)(?=\n\t\t\tname:|\Z)",s,re.S)[1]
 types={name.lower():typ.lower() for typ,name in re.findall(r'^\s+(\w+(?:\[\])?) (\S+)\s*$',body,re.M)}
 code=[]
 for op,raw in re.findall(r"opcode: '(\w+)', args: \[(.*?)\]\s*$",body,re.M):
  args=[]
  for m in re.finditer(r"ident\(([^)]+)\)|integer\((-?\d+)\)|string\('(.*?)'\)|boolean\((0[01])\)|(none)",raw):
   args.append(('id',m[1].lower()) if m[1] else ('v',int(m[2]) if m[2] else m[3] if m[3] is not None else m[4]=='01' if m[4] else None))
  code.append((op,args))
 return types,code
def run(script,fn,env,world,calls):
 types,code=read(script,fn);get=lambda a:env.get(a[1]) if a[0]=='id' else a[1];pc=0
 for _ in range(300):
  if pc>=len(code):return None
  op,a=code[pc];step=1
  if op=='ret':return get(a[0]) if a else None
  if op=='assign':env[a[0][1]]=get(a[1])
  elif op=='cast':
   v=get(a[1]);t=types.get(a[0][1]);env[a[0][1]]=bool(v) if t=='bool' else str(v) if t=='string' else v
  elif op=='not':env[a[0][1]]=not get(a[1])
  elif op=='cmp_eq':env[a[0][1]]=get(a[1])==get(a[2])
  elif op=='strcat':env[a[0][1]]=str(get(a[1]))+str(get(a[2]))
  elif op=='jmp':step=get(a[0])
  elif op=='jmpf':step=1 if get(a[0]) else get(a[1])
  elif op=='jmpt':step=get(a[1]) if get(a[0]) else 1
  elif op=='callmethod':
   method=a[0][1];actor=get(a[1]);params=[get(x) for x in a[4:]]
   if method=='getcurrentpackage':value=world['package']
   elif method=='getlinkedref':value=world['linked']
   elif method=='getdisplayname':value=actor
   else:
    calls.append((method,actor,*params));value=None
    if world.get('deep'):
     if method=='removefromfaction':world['factions'].discard(params[0])
     elif method=='setfactionrank':world['factions'].add(params[0])
   env[a[2][1]]=value
  elif op=='callstatic':
   cls,method=a[0][1],a[1][1];params=[get(x) for x in a[4:]];value=None
   if cls=='game' and method=='getformfromfile':value=None if world.get('missing')==params[0] else params[0]
   elif cls=='game' and method=='getplayer':value=world.get('player','Player')
   elif cls=='game' and method=='getform':value=params[0]
   elif cls=='aiagentfollowsafety':
    fn={'isvalidfollowtarget':'IsValidFollowTarget','repairselffollow':'RepairSelfFollow'}[method]
    value=run('AIAgentFollowSafety',fn,dict(zip(['npc','target'],params)),world,calls)
   elif cls=='aiagentaimind' and world.get('deep') and method in ['stayatplace','resetpackages']:
    fn,keys=('stayAtPlace',['npc','followplayer','taskid']) if method=='stayatplace' else ('ResetPackages',['npc'])
    value=run('AIAgentAIMind',fn,dict(zip(keys,params)),world,calls)
   elif cls=='debug':pass
   else:
    calls.append((cls+'.'+method,*params))
    if world.get('deep'):
     if cls=='actorutil' and method=='removepackageoverride':world['packages'].discard(params[1])
     elif cls=='actorutil' and method=='addpackageoverride':world['packages'].add(params[1])
     elif cls=='storageutil' and method=='setintvalue':world['storage'][params[1]]=params[2]
     elif cls=='po3_sksefunctions' and method=='setlinkedref':world['linked']=params[1]
   env[a[2][1]]=value
  else:raise AssertionError(op)
  pc+=step
 raise AssertionError('did not terminate')
checks=0
for npc,target,expected in [(None,'Other',False),('NPC',None,False),('NPC','NPC',False),('NPC','Player',True),('NPC','Other',True)]:
 assert run('AIAgentFollowSafety','IsValidFollowTarget',{'npc':npc,'target':target},{},[])==expected;checks+=1
for package,linked,expected in [(0x1bc25,'NPC',True),(0x1bc25,'Other',False),(0x2226d,'NPC',False),('NFF','NPC',False),(0x1bc25,None,False)]:
 calls=[];w={'package':package,'linked':linked}
 assert run('AIAgentFollowSafety','RepairSelfFollow',{'npc':'NPC'},w,calls)==expected
 removed=[c for c in calls if c[0]=='actorutil.removepackageoverride']
 assert removed==([('actorutil.removepackageoverride','NPC',0x1bc25)] if expected else [])
 assert all('clearpackageoverride' not in c[0] for c in calls);checks+=1
for npc,target in [(None,'Other'),('NPC',None),('NPC','NPC'),('NPC','Player'),('NPC','Other')]:
 calls=[];w={'package':'NFF','linked':None}
 run('AIAgentAIMind','Follow',{'npc':npc,'aktarget':target},w,calls)
 if not npc or not target or npc==target:assert not calls,calls
 elif target=='Player':assert calls==[('aiagentaimind.stayatplace','NPC',1,'')],calls
 else:
  assert ('aiagentaimind.resetpackages','NPC') in calls,calls
  assert ('po3_sksefunctions.setlinkedref','NPC','Other',0x21245) in calls,calls
 checks+=1
print(f'Compiled follow safety: {checks} scenarios passed')

# Execute the compiled Follow -> stayAtPlace -> ResetPackages bytecode chain.
# External engine/native effects are modeled as package/faction state changes.
for npc, owned in [('NFF Regular', {'NFF Follow'}), ('NFF Expendable', {'NFF Follow','NFF Travel'}),
                   ('Vanilla Follower', {'Vanilla Follow'}), ('Custom Companion', {'Custom Follow'}),
                   ('Unrecruited NPC', {'Default Sandbox'})]:
 for player in ['Player','Custom Hero']:
  factions={'NFF Membership',0x5c84d,0x5c84e}
  w={'deep':True,'player':player,'packages':owned|{0x1abfe,0x2021f},'factions':set(factions),'storage':{},'linked':None,'package':'NFF'}
  calls=[]
  run('AIAgentAIMind','Follow',{'npc':npc,'aktarget':player},w,calls)
  assert w['packages']==owned|{0x2226d},w
  assert factions<=w['factions'] and w['storage']['CHIM_FollowPlayerActive']==1
  assert w['linked'] is None and not any('clearpackageoverride' in c[0] for c in calls)
  checks+=1
  run('AIAgentAIMind','Follow',{'npc':npc,'aktarget':'Other NPC'},w,calls)
  assert w['packages']==owned|{0x1bc25} and w['linked']=='Other NPC'
  assert factions<=w['factions']
  checks+=1
  run('AIAgentAIMind','stayAtPlace',{'npc':npc,'followplayer':0,'taskid':''},w,calls)
  assert w['packages']==owned|{0x20ce2} and factions<=w['factions']
  checks+=1
  run('AIAgentAIMind','stayAtPlace',{'npc':npc,'followplayer':1,'taskid':''},w,calls)
  assert w['packages']==owned|{0x2226d} and w['storage']['CHIM_FollowPlayerActive']==1
  assert factions<=w['factions']
  checks+=1
print(f'Follow command validation: {checks} compiled scenarios passed; player/actor follow, wait/resume and external package ownership preserved.')
