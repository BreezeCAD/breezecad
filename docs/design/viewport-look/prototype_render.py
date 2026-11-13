import numpy as np, sys, os
DARK=os.environ.get('BG')=='dark'
from PIL import Image
W,H=int(sys.argv[2]),int(sys.argv[3]); mode=sys.argv[1]
def sdbox(p,c,h):
    q=np.abs(p-c)-h
    return np.linalg.norm(np.maximum(q,0),axis=-1)+np.minimum(q.max(-1),0)
def sdcylz(p,c,r):
    return np.hypot(p[...,0]-c[0],p[...,1]-c[1])-r
def sdslot(p,a,b,r):
    x=np.clip(p[...,0],a,b); return np.hypot(p[...,0]-x,p[...,1]-20.0)-r
def scene(p):
    d=sdbox(p,np.array([30,20,5.]),np.array([30,20,5.]))
    d=np.maximum(d,-sdcylz(p,(12,25),5.0))
    d=np.maximum(d,-sdslot(p,28,46,3.6))
    d=np.maximum(d,-sdbox(p,np.array([57,30,10.]),np.array([5,4.5,5.0])))
    return d
def ground(p): return p[...,2]+0.0
def normal(p):
    e=0.01;n=np.zeros_like(p)
    for i in range(3):
        o=np.zeros(3);o[i]=e;n[...,i]=scene(p+o)-scene(p-o)
    return n/np.linalg.norm(n,axis=-1,keepdims=True)
eye=np.array([171.8,-121.8,146.8]);ctr=np.array([30,20,5.])
f=ctr-eye;f/=np.linalg.norm(f);up=np.array([-0.408248,0.408248,0.816497])
r=np.cross(f,up);r/=np.linalg.norm(r);u=np.cross(r,f)
vh=56.34*float(os.environ.get("ZOOM","1.08"));vw=vh*W/H
ys,xs=np.mgrid[0:H,0:W]
sx=(xs+0.5)/W-0.5;sy=0.5-(ys+0.5)/H
ro=eye+ (sx[...,None]*vw)*r+(sy[...,None]*vh)*u
rd=np.broadcast_to(f,ro.shape).copy()
t=np.zeros((H,W));hit=np.zeros((H,W),bool)
for i in range(160):
    p=ro+rd*t[...,None];d=scene(p)
    hit|=d<1e-3; t=np.where(hit,t,t+np.maximum(d,1e-3))
    if t.max()>600:pass
p=ro+rd*t[...,None];hit&=t<500
n=normal(p)
# ground plane intersection z=0 slightly below (floor at z=-0.02)
tg=(-0.02-ro[...,2])/rd[...,2]; pg=ro+rd*tg[...,None]
def bg_grad(top,bot):
    k=(ys/H)[...,None]; return np.array(top)*(1-k)+np.array(bot)*k
L=np.array([-0.65,0.15,0.74]) if DARK else np.array([-0.35,-0.55,0.76]);L/=np.linalg.norm(L)
V=-rd
# edges: normal/depth discontinuity
def edges(n,t,hit):
    e=np.zeros(t.shape)
    for dy,dx in((0,1),(1,0)):
        n2=np.roll(n,(-dy,-dx),(0,1));t2=np.roll(t,(-dy,-dx),(0,1));h2=np.roll(hit,(-dy,-dx),(0,1))
        crease=(1-(n*n2).sum(-1))>0.05
        sil=(hit!=h2)|(hit&h2&(np.abs(t-t2)>2.0))
        e=np.maximum(e,np.where(sil,2,np.where(crease&hit&h2,1,0)))
    return e
E=edges(n,t,hit)
def soft_shadow(po,Ld,k=10):
    res=np.ones(po.shape[:2]);tt=np.full(po.shape[:2],0.05)
    for i in range(48):
        d=scene(po+Ld*tt[...,None]);res=np.minimum(res,k*np.maximum(d,0)/tt);tt+=np.clip(d,0.05,3)
    return np.clip(res,0,1)
def ao(po,nn):
    occ=np.zeros(po.shape[:2]);s=1.0
    for i in range(1,6):
        h=0.6*i;d=scene(po+nn*h);occ+=(h-np.maximum(d,0))*s;s*=0.6
    return np.clip(1-0.22*occ,0,1)
if mode=='now':
    img=bg_grad([0.91,0.93,0.96],[0.70,0.77,0.86])
    base=np.array([0.79,0.81,0.84])
    lam=np.clip((n*V).sum(-1),0,1)*0.45+0.55
    col=base*lam[...,None]
    img=np.where(hit[...,None],col,img)
    edge=np.array([0.24,0.27,0.32]);m=(E>0)[...,None]
    img=np.where(m,img*0.25+edge*0.75,img)
else:
    beauty=(mode=='render')
    img=bg_grad([0.22,0.24,0.28],[0.10,0.11,0.13]) if DARK else bg_grad([0.97,0.98,0.99],[0.84,0.88,0.93])
    # ground: shadow + grid fade, only where ray hits ground and not body
    gm=(~hit)&(tg>0)
    shp=pg+np.array([0,0,0.03])
    sh=soft_shadow(shp,L,k=3.5 if beauty else 8)
    aog=ao(shp,np.array([0,0,1.0]))
    dist=np.hypot(pg[...,0]-30,pg[...,1]-20)
    fade=np.clip(1-dist/140,0,1)**1.5
    gx=np.abs((pg[...,0]+1000)%10-5);gy=np.abs((pg[...,1]+1000)%10-5)
    grid=np.clip(1-np.minimum(5-gx,5-gy)/0.18,0,1)*0.10*fade
    gcol=img*(1-grid[...,None]*(0 if DARK else 1))+np.array([0.35,0.45,0.6])*grid[...,None]*(0 if DARK else 1)
    shade=1-(1-sh*aog)*(0.32 if beauty else 0.26)*fade
    gcol=gcol*shade[...,None]
    img=np.where(gm[...,None],gcol,img)
    # body: PBR-ish
    pc=p+n*0.02
    S=soft_shadow(pc,L,k=10); A=ao(pc,n)
    base=np.array([0.78,0.80,0.84])
    ndl=np.clip((n*L).sum(-1),0,1)
    hemi=0.5+0.5*n[...,2]
    sky=np.array([0.90,0.94,1.0]);gnd=np.array([0.62,0.62,0.64])
    amb=(sky*hemi[...,None]+gnd*(1-hemi[...,None]))*0.55
    Hh=L+V;Hh/=np.linalg.norm(Hh,axis=-1,keepdims=True)
    spec=np.clip((n*Hh).sum(-1),0,1)**(60 if beauty else 40)*(0.35 if beauty else 0.22)
    fres=(1-np.clip((n*V).sum(-1),0,1))**5
    L2=np.array([0.7,-0.2,0.35]);L2/=np.linalg.norm(L2);fill=np.clip((n*L2).sum(-1),0,1)*0.16
    col=base*(amb*A[...,None]+ (0.62*ndl*S+fill)[...,None]) + (spec*S)[...,None] + (fres*0.18*A)[...,None]*sky
    if beauty:
        # env reflection: blend vertical gradient by reflected z
        R=rd-2*(rd*n).sum(-1,keepdims=True)*n
        env=np.where((R[...,2]>0)[...,None],sky*(0.9+0.1*R[...,2,None]),gnd*0.9)
        col=col*0.84+env*0.16*A[...,None]
    col=col/(1+col*0.25)*(0.98 if DARK else 1.12)  # gentle tonemap
    if mode=='glass':
        bgimg=bg_grad([0.97,0.98,0.99],[0.84,0.88,0.93])
        gl=bgimg*(1-grid[...,None])+np.array([0.35,0.45,0.6])*grid[...,None]
        gl=gl*(1-(1-sh*aog)*0.12*fade)[...,None]
        behind=np.where((tg>0)[...,None],gl,bgimg)
        th=np.zeros((H,W));q=p+rd*0.05;tt=np.full((H,W),0.05)
        inside=hit.copy()
        for i in range(60):
            qq=p+rd*tt[...,None];d=-scene(qq);inside&=d>1e-3;tt=np.where(inside,tt+np.maximum(d,0.05),tt)
        th=np.where(hit,tt,0)
        tint=np.array([0.80,0.90,1.0])
        trans=np.exp(-th*0.010)[...,None]*tint**(np.clip(th,0,60)[...,None]/45)
        fr=0.06+0.94*(1-np.clip((n*V).sum(-1),0,1))**5
        R=rd-2*(rd*n).sum(-1,keepdims=True)*n
        env=np.where((R[...,2]>0)[...,None],sky*(0.95+0.05*R[...,2,None]),gnd)
        col=behind*trans*(1-fr[...,None])+env*fr[...,None]*0.9+(spec*S*2.2)[...,None]
        col=col*0.92+np.array([0.62,0.72,0.86])*0.08*(1-A[...,None])*3
    img=np.where(hit[...,None],col,img)
    edge=np.array([0.16,0.19,0.25])
    sil=(E==2)[...,None];cr=(E==1)[...,None]
    img=np.where(sil,img*0.15+edge*0.85,img)
    img=np.where(cr,img*0.55+edge*0.45,img)
if mode in('glass','render'):
    pass
if mode=='render':
    vg=1-0.12*(((xs/W-0.5)**2+(ys/H-0.5)**2)*2.2)
    img=img*vg[...,None]
if mode=='preview':
    def gboss(q):
        h=np.abs(q[...,2]-15.0)-5.0; c=np.hypot(q[...,0]-46,q[...,1]-33)-5.0
        return np.linalg.norm(np.stack([np.maximum(c,0),np.maximum(h,0)],-1),axis=-1)+np.minimum(np.maximum(c,h),0)
    def gcut(q):
        return sdbox(q,np.array([20,8,7.]),np.array([12,4,3.2]))
    for fn,colr in((gboss,np.array([0.15,0.42,0.95])),(gcut,np.array([0.92,0.25,0.22]))):
        tq=np.zeros((H,W));hq=np.zeros((H,W),bool)
        for i in range(120):
            q=ro+rd*tq[...,None];d=fn(q);hq|=d<1e-3;tq=np.where(hq,tq,tq+np.maximum(d,1e-3))
        allv=hq&(tq<500);front=allv&((~hit)|(tq<=t+0.6));vis=allv
        img=np.where((allv&~front)[...,None],img*0.78+colr*0.22,img)
        e=np.zeros((H,W),bool)
        for dy,dx in((0,1),(1,0),(0,-1),(-1,0)):
            e|=vis!=np.roll(vis,(dy,dx),(0,1))
        img=np.where(front[...,None],img*0.62+colr*0.38,img)
        img=np.where((e&(vis|np.roll(vis,(1,0),(0,1))))[...,None],img*0.3+colr*0.7,img)
img=np.clip(img,0,1)
Image.fromarray((img*255).astype(np.uint8)).resize((W//2,H//2),Image.LANCZOS).save(sys.argv[4])
print('ok',hit.mean())
