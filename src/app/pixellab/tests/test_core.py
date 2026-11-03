import base64,ctypes,io,json,os,tempfile,zipfile
from pathlib import Path
from PIL import Image
id='11111111-1111-4111-8111-111111111111';gid='22222222-2222-4222-8222-222222222222'
with tempfile.TemporaryDirectory() as d:
 path=os.path.join(d,'fixture.zip');capture=os.path.join(d,'capture.jsonl')
 f={'rotations':{},'animations':{'Walking':{'south':[],'south-east':[]}}}
 with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED) as z:
  for direction in ['south','south-east','east','north-east','north','north-west','west','south-west']:
   name=f'rot/{direction}.png';img=Image.new('RGBA',(8,8),(150,50,20,255));out=io.BytesIO();img.save(out,'PNG');z.writestr(name,out.getvalue());f['rotations'][direction]=name
  for direction in ['south','south-east']:
   for i in range(4):
    name=f'anim/{direction}/{i}.png';img=Image.new('RGBA',(8,8),(i*40,20,30,255));out=io.BytesIO();img.save(out,'PNG');z.writestr(name,out.getvalue());f['animations']['Walking'][direction].append(name)
  z.writestr('metadata.json',json.dumps({'states':[{'character':{'id':id},'frames':f}]}))
 os.environ['PIXELLAB_FIXTURE']=path;os.environ['PIXELLAB_CAPTURE']=capture
 lib=ctypes.CDLL(str(Path(__file__).resolve().parent/'libmock.so'));lib.make_client.restype=ctypes.c_void_p;lib.run_job.argtypes=[ctypes.c_void_p,ctypes.c_char_p];lib.run_job.restype=ctypes.c_char_p;lib.free_client.argtypes=[ctypes.c_void_p];c=lib.make_client()
 def call(act,**kw):
  r=json.loads(lib.run_job(c,json.dumps(dict(action=act,**kw)).encode()));assert r['ok'],(act,r);return r
 try:
  assert call('auth',token='synthetic')['info']=='42 generations left';assert call('list')['characters'][0]['id']==id
  r=call('select',character_id=id);assert len(r['previews'])==8
  r=call('open_rotations',character_id=id); assert len(r['frames'])==8
  rotation=r['frames'][0]; rotation_image=rotation['image']['hex']
  bad=json.loads(lib.run_job(c,json.dumps({'action':'upload','character_id':id,'items':[{'direction':rotation['direction'],'w':8,'h':8,'hex':rotation_image}]}).encode())); assert not bad['ok'] and 'original' in bad['error'].lower()
  a=call('animations',character_id=id)['animations'];assert a[0]['group_id']==gid
  o=call('open_animation',character_id=id,group_id=gid,name='Walking',directions=['south','south-east']);assert len(o['clips'])==2
  fr=[x['image']['hex'] for x in o['clips'][0]['frames']]
  assert call('upload_animation',character_id=id,group_id=gid,name='Walking',items=[{'direction':'south','w':8,'h':8,'frames':fr,'original_frames':fr,'delay_cs':10}])['saved']==['south']
  sent=json.loads(open(capture).readline());assert sent['target']==f'character:{id}:south:{gid}'
  gif=Image.open(io.BytesIO(base64.b64decode(sent['image'].split(',')[1])));assert gif.n_frames==4
  stale=fr.copy(); stale[0]='00'*len(bytes.fromhex(stale[0]))
  bad=json.loads(lib.run_job(c,json.dumps({'action':'upload_animation','character_id':id,'group_id':gid,'items':[{'direction':'south','w':8,'h':8,'frames':fr,'original_frames':stale}]}).encode())); assert not bad['ok'] and 'changed' in bad['error'].lower(); assert len(open(capture).readlines())==1
  bad=json.loads(lib.run_job(c,json.dumps({'action':'upload','character_id':id,'items':[{'direction':rotation['direction'],'w':8,'h':8,'hex':rotation_image,'original_hex':'00'*(8*8*4)}]}).encode())); assert not bad['ok'] and 'changed' in bad['error'].lower(); assert len(open(capture).readlines())==1
  bad=json.loads(lib.run_job(c,json.dumps({'action':'upload_animation','character_id':id,'group_id':gid,'items':[{'direction':'west','w':8,'h':8,'frames':fr}]}).encode()));assert not bad['ok'];assert len(open(capture).readlines())==1
  # Every direction is validated before any network write (all-or-nothing preflight).
  second=o['clips'][1]; fr2=[x['image']['hex'] for x in second['frames']]
  item1={'direction':'south','w':8,'h':8,'frames':fr,'original_frames':fr}
  item2={'direction':'south-east','w':8,'h':8,'frames':fr2,'original_frames':['00'*(8*8*4)]*len(fr2)}
  bad=json.loads(lib.run_job(c,json.dumps({'action':'upload_animation','character_id':id,'group_id':gid,'items':[item1,item2]}).encode()))
  assert not bad['ok'] and len(open(capture).readlines())==1
  rotitem={'direction':rotation['direction'],'w':8,'h':8,'hex':rotation_image,'original_hex':rotation_image}
  assert call('upload',character_id=id,items=[rotitem])['saved']==[rotation['direction']]
  assert json.loads(open(capture).readlines()[1])['target']==f"character:{id}:{rotation['direction']}"
  assert call('undo')['restored']==[rotation['direction']]
  assert len(open(capture).readlines())==3
  assert not json.loads(lib.run_job(c,b'{bad json'))['ok']
  print('PASS in-process API: auth, list, ZIP fallback, open 2 directions, correct targets, GIF, conflicts, preflight, rotation undo and malformed request')
 finally:lib.free_client(c)
