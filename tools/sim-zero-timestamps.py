# Simulation of the HAL plug-in zero-timestamp fit (DSP clock +80 ppm, 1 ms DPIB chunks, jitter, 150 ms stalls)
import random, math, collections
random.seed(7)
RING=4096; RATE=48000*(1+80e-6); TPF=1e9/48000
def dsp_frames(t): return int(t*1e-9*RATE)//48*48
N=128
win=collections.deque(maxlen=N)
total=0; last_pos=None; last_host=0; zts=[]; t=0.0
for i in range(20000):
    t += random.uniform(8e6,13e6)
    if i in (5000,12000): t += 150e6
    true=dsp_frames(t); pos=true%RING; host=t
    if last_pos is None: total=pos; last_pos=pos; last_host=host; continue
    delta=(pos+RING-last_pos)%RING
    exp=(host-last_host)/TPF
    if exp>RING*0.75:
        w=math.floor((exp-delta)/RING+0.5)
        if w>0: delta+=w*RING
    total+=delta; last_pos=pos; last_host=host
    # +24 frames: DPIB is a lower bound, the true position is on average half a chunk ahead
    win.append((host, total+24))
    if len(win)>=16:
        h0=win[0][0]
        xs=[h-h0 for h,_ in win]; ys=[f for _,f in win]
        mx=sum(xs)/len(xs); my=sum(ys)/len(ys)
        sxx=sum((x-mx)**2 for x in xs); sxy=sum((x-mx)*(y-my) for x,y in zip(xs,ys))
        b=sxy/sxx; a=my-b*mx            # frames = a + b*(host-h0)
        zs=(total//RING)*RING
        zh=h0+(zs-a)/b
    else:
        zs=(total//RING)*RING; zh=host-(total-zs)*TPF
    if zts and zts[-1][0]==zs: zh=zts[-1][1]
    zts.append((zs,zh))
steps=[(b[0]-a[0], b[1]-a[1]) for a,b in zip(zts,zts[1:]) if b[0]!=a[0]]
rate=[RING/(s[1]*1e-9) for s in steps[20:] if s[0]==RING]
m=sum(rate)/len(rate); sd=(sum((r-m)**2 for r in rate)/len(rate))**.5
print("LSQ window %d: rate mean %.2f Hz (true %.2f), jitter sd %.3f Hz (%.1f ppm)"%(N,m,RATE,sd,sd/48000*1e6))
# monotonic host times for successive zero ts?
mono=all(b[1]>a[1] for a,b in zip(zts,zts[1:]) if b[0]>a[0])
print("zero-ts host times monotonic:",mono)
# consistency: same sample time reported repeatedly should not move host time a lot
same=[abs(b[1]-a[1]) for a,b in zip(zts,zts[1:]) if b[0]==a[0]]
print("host time wobble for same zero sample: max %.1f us"%(max(same)/1e3))
