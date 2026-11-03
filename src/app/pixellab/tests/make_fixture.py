# Generate synthetic data only. Requires Pillow for development tests, not at runtime.
import io, json, zipfile, sys
from PIL import Image
id='11111111-1111-4111-8111-111111111111'
f={'rotations':{},'animations':{'Walking':{'south':[],'south-east':[]}}}
with zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_DEFLATED) as z:
 for n,d in enumerate(['south','south-east','east','north-east','north','north-west','west','south-west']):
  name=f'rot/{d}.png'; im=Image.new('RGBA',(8,8),(150,n*20,20,255)); out=io.BytesIO(); im.save(out,'PNG');z.writestr(name,out.getvalue());f['rotations'][d]=name
 for direction in ['south','south-east']:
  for i in range(4):
   name=f'anim/{direction}/{i}.png';im=Image.new('RGBA',(8,8),(i*40,20,30,255));out=io.BytesIO();im.save(out,'PNG');z.writestr(name,out.getvalue());f['animations']['Walking'][direction].append(name)
 z.writestr('metadata.json',json.dumps({'states':[{'character':{'id':id},'frames':f}]}))
