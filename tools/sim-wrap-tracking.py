# Simulation of the HAL plug-in DMA wrap tracking
import random, math
random.seed(7)
RING=4096; RATE=48000*(1+80e-6)          # DSP clock 80 ppm fast
TPF=1e9/48000                            # host ticks per frame (ns timebase)
def dsp_frames(t_ns): return int(t_ns*1e-9*RATE)//48*48     # DPIB advances in 48-frame (1 ms) chunks
total=0; last_pos=None; last_host=0; errs=0; zts=[]
t=0.0
for i in range(20000):
    t += random.uniform(8e6,13e6)                     # ~10 ms IO cycles
    if i in (5000,12000): t += 150e6                  # two 150 ms stalls (> ring length 85 ms)
    true=dsp_frames(t); pos=true%RING; host=t
    if last_pos is None: total=pos; last_pos=pos; last_host=host; continue
    delta=(pos+RING-last_pos)%RING
    expected=(host-last_host)/TPF
    if expected>RING*0.75:
        wraps=math.floor((expected-delta)/RING+0.5)
        if wraps>0: delta+=wraps*RING
    total+=delta; last_pos=pos; last_host=host
    if total!=true: errs+=1
    zs=(total//RING)*RING; zh=host-(total-zs)*TPF
    zts.append((zs,zh))
# zero timestamps: sample times must step by exactly RING; host deltas ~ RING frames of DSP clock
steps=[(b[0]-a[0], b[1]-a[1]) for a,b in zip(zts,zts[1:]) if b[0]!=a[0]]
bad=[s for s in steps if s[0]%RING]
rate=[RING*s[0]//RING/(s[1]*1e-9) for s in steps if s[0]==RING]
print("tracking errors:",errs,"of",len(zts))
print("zero-ts steps:",len(steps),"non-multiple-of-period:",len(bad), "multi-period jumps:",sum(1 for s in steps if s[0]>RING))
print("rate implied by zero-ts: mean %.2f Hz (true %.2f), jitter sd %.2f Hz"%(sum(rate)/len(rate),RATE,(sum((r-sum(rate)/len(rate))**2 for r in rate)/len(rate))**.5))
# DoIO window check: HAL reads [now - safety - bufsize, now - safety); data must be <= written and > written-RING
SAFETY=256; BUF=512; ok=True
for now in range(10000, 2000000, 997):
    start=now-SAFETY-BUF; end=now-SAFETY
    written=now  # at 'now' the DSP has written up to 'now' frames (minus <=48 chunk)
    if end>written-48 or start<written-RING: ok=False
print("DoIO read window inside valid ring region:",ok)
