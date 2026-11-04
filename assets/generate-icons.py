#!/usr/bin/env python3
"""Generate Windows multi-resolution ICO and preview from the canonical SVG. Requires rsvg-convert and ffmpeg."""
import pathlib, struct, subprocess
root=pathlib.Path(__file__).resolve().parent
sizes=(16,20,24,32,40,48,64,128,256)
images=[subprocess.check_output(['rsvg-convert','-w',str(n),'-h',str(n),str(root/'relay.svg')]) for n in sizes]
# Uncompressed 32-bit DIB entries also work in NSIS and older shell tools.
for i,(n,png) in enumerate(zip(sizes,images)):
 raw=subprocess.check_output(['ffmpeg','-v','error','-i','pipe:0','-f','rawvideo','-pix_fmt','bgra','pipe:1'],input=png)
 assert len(raw)==n*n*4
 pixels=b''.join(raw[y*n*4:(y+1)*n*4] for y in range(n-1,-1,-1))
 mask=b'\0'*(((n+31)//32)*4*n)
 images[i]=struct.pack('<IiiHHIIiiII',40,n,n*2,1,32,0,len(pixels)+len(mask),0,0,0,0)+pixels+mask
offset=6+16*len(sizes)
header=struct.pack('<HHH',0,1,len(sizes)); entries=[]
for n,data in zip(sizes,images):
 entries.append(struct.pack('<BBBBHHII',n%256,n%256,0,0,1,32,len(data),offset));offset+=len(data)
(root/'relay.ico').write_bytes(header+b''.join(entries)+b''.join(images))
(root/'relay.png').write_bytes(subprocess.check_output(['rsvg-convert','-w','512','-h','512',str(root/'relay.svg')]))
print('Generated relay.ico with sizes:',sizes)
