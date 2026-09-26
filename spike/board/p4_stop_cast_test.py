import sys, time
sys.path.insert(0,'/cast')
from board_config import fb, display_drv
from roku_cast import RokuScreen
display_drv.fill(0x001F); display_drv.fill_rect(200,200,320,320,0xF800)
class Scene:
    def __init__(self): self.n=0
    def step(self):
        self.n+=1; x=20+(self.n*9)%600
        display_drv.fill_rect(0,100,720,70,0x001F); display_drv.fill_rect(x,100,70,70,0xFFFF)
LOG=open('/cast/stop_test2.log','w')
def log(*a):
    s=' '.join(str(x) for x in a); print(s); LOG.write(s+'\n'); LOG.flush()
def frames(tv):
    try: return tv._session.streamer.frames
    except: return -1
tv=RokuScreen('192.168.1.129', name='PyDevices P4', log=lambda *a: None)
tv.on()
lat=[]; starts=0
for c in range(8):
    ok=tv.start_cast(fb, scene=Scene(), seconds=300, audio=(1000,600))
    if ok: starts+=1
    time.sleep(8)
    f0=frames(tv)
    t=time.ticks_ms(); tv.stop_cast(); d=time.ticks_diff(time.ticks_ms(),t)
    lat.append(d)
    time.sleep(1); f1=frames(tv)
    log('cycle',c,'start',ok,'stop_ms',d,'casting',tv.is_casting(),'frames',f0,'->',f1,'ceased',f1==f0)
    time.sleep(2)
lat.sort()
log('RESULT starts',starts,'/8  stop_ms sorted',lat,'max',max(lat),'median',lat[len(lat)//2])
LOG.close(); print('STOP2_DONE')
