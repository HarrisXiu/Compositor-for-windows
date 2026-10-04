# SPDX-License-Identifier: MIT
"""Independent NumPy verification of the soft BiRefNet mask and guided matte."""
from pathlib import Path
import argparse, hashlib, json, zlib
import numpy as np
from PIL import Image

def box(source,radius):
    padded=np.pad(source.astype(np.float64),((0,0),(radius,radius)),mode='edge')
    sums=np.cumsum(padded,axis=1,dtype=np.float64)
    sums=np.concatenate((np.zeros((sums.shape[0],1)),sums),axis=1)
    span=2*radius+1
    horizontal=(sums[:,span:]-sums[:,:-span])/span
    padded=np.pad(horizontal,((radius,radius),(0,0)),mode='edge')
    sums=np.cumsum(padded,axis=0,dtype=np.float64)
    sums=np.concatenate((np.zeros((1,sums.shape[1])),sums),axis=0)
    return ((sums[span:]-sums[:-span])/span).astype(np.float32)

def quantize(plane):return np.floor(np.clip(plane,0,1)*255+.5).astype(np.uint8)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--references',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    catalog=json.loads((args.references/'references.json').read_text(encoding='utf-8'))
    model=next(m for m in catalog['models'] if m['id']=='birefnet-lite')
    checks=[]
    for record in model['images']:
        image=np.asarray(Image.open(args.references/record['file']).convert('RGBA'))
        tensor=record['encoder_outputs']['logits']
        compressed=(args.references/tensor['file']).read_bytes()
        raw_bytes=zlib.decompress(compressed[4:])
        if int.from_bytes(compressed[:4],'big') != len(raw_bytes) or hashlib.sha256(raw_bytes).hexdigest() != tensor['sha256']:
            raise ValueError('Reference tensor size or SHA256 mismatch')
        raw=np.frombuffer(raw_bytes,dtype='<f4').reshape(tensor['shape'])[0,0]
        probabilities=1/(1+np.exp(-np.clip(raw,-80,80)))
        h,w=image.shape[:2];mh,mw=probabilities.shape
        sx=np.clip((np.arange(w)+.5)*mw/w-.5,0,mw-1);sy=np.clip((np.arange(h)+.5)*mh/h-.5,0,mh-1)
        x0=sx.astype(int);x1=np.minimum(x0+1,mw-1);y0=sy.astype(int);y1=np.minimum(y0+1,mh-1)
        horizontal=probabilities[:,x0]*(1-(sx-x0))+probabilities[:,x1]*(sx-x0)
        soft=horizontal[y0,:]*(1-(sy-y0))[:,None]+horizontal[y1,:]*(sy-y0)[:,None]
        basic=quantize(soft);basic[image[:,:,3]==0]=0
        Image.fromarray(basic).save(args.output/('basic-'+record['file']))
        guide=(image[:,:,:3].astype(np.float64)@np.array([.2126,.7152,.0722])/255).astype(np.float32)
        p=basic.astype(np.float32)/255
        mg=box(guide,12);mp=box(p,12);ms=box(guide*guide,12);mc=box(guide*p,12)
        a=(mc-mg*mp)/(np.maximum(0,ms-mg*mg)+np.float32(1e-4));b=mp-a*mg
        guided=quantize(box(a,12)*guide+box(b,12))
        slope=1/(1-.25*.98)
        advanced=quantize((guided.astype(np.float64)/255-.5)*slope+.5)
        Image.fromarray(advanced).save(args.output/('advanced-'+record['file']))
        checks.append({'image':record['file'],'size':[w,h],'soft_levels':int(len(np.unique(basic))),'settings':{'refine':12,'contrast':25,'shift':0}})
    (args.output/'reference-report.json').write_text(json.dumps({'source':'Python ONNX AI1 logits; independent NumPy float planes and clipped-border cumulative box means','checks':checks},indent=2)+'\n',encoding='utf-8')
    print(json.dumps(checks))
if __name__=='__main__':main()
