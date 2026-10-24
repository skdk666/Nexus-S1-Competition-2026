#include "xpt2046_live.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace xpt2046;
static uint32_t now=1000;
static WireTrace frame(uint16_t x=1000,uint16_t y=2000,uint16_t z1=400,uint16_t z2=3500) {
    WireTrace t;t.started=true;t.count=30;t.payload_zero=t.payload_ones=false;
    t.adc[0]=t.adc[8]=z1;t.adc[1]=t.adc[9]=z2;
    for(int i=2;i<=6;i+=2){t.adc[i]=x;t.adc[i+1]=y;}
    return t;
}
static LiveResult feed(LiveFilter& f,const WireTrace& t,unsigned count=1,unsigned ms=30,bool ok=true) {
    LiveResult r;for(unsigned i=0;i<count;++i){now+=ms;r=f.update(t,ok,now);}return r;
}
static void arm(LiveFilter& f) {assert(feed(f,frame(1000,2000,0,4095),12).state==State::Released);}
static void press(LiveFilter& f,WireTrace t=frame()) {assert(feed(f,t,3).state==State::Pressed);}
static void reviewRegression() {
    const auto mid=frame(1000,2000,100,3745); // 4095+100-3745 = 450, structurally valid Z.
    const auto release=frame(1000,2000,0,4095);
    LiveFilter f;f.begin(false);arm(f);
    auto r=feed(f,mid,20);
    assert(r.raw_available && r.rx_valid && r.xy_stable && r.mapped && r.z_valid);
    assert(r.pressure==LivePressure::Mid && r.pressure_before==450 && r.pressure_after==450);
    assert(!std::strcmp(livePressureName(r.pressure),"MID"));
    assert(r.state==State::Released && !std::strcmp(r.reason,"PRESSURE_MID"));
    assert(feed(f,frame(1000,2000,100,3596),3).state==State::Released); // 599 cannot start trusted press.
    assert(feed(f,frame(1000,2000,100,3595),3).state==State::Pressed); // 600 can.
    assert(feed(f,frame(1000,2000,100,3894)).state==State::Pressed); // 301 holds.
    r=feed(f,frame(1000,2000,100,3895),2); // 300 releases, unchanged two-frame rule.
    assert(r.state==State::Released && r.pressure==LivePressure::Release);

    f.begin(true);
    assert(feed(f,mid,20).state==State::Released); // MID must not self-arm after boot.
    arm(f);r=feed(f,mid,3);
    assert(r.state==State::Pressed && r.xy_only && r.z_valid && r.pressure==LivePressure::Mid);
    assert(!std::strcmp(r.reason,"XY_ONLY_EXPERIMENTAL"));
    r=feed(f,mid,6);assert(r.state==State::Released && !std::strcmp(r.reason,"XY_TAP_END"));
    for(int i=0;i<100;++i)assert(feed(f,mid).state==State::Released); // One tap, no cooldown retrigger.
    feed(f,release,5);assert(feed(f,mid,3).state==State::Released); // Short release cannot rearm.
    arm(f);assert(feed(f,mid,3).state==State::Pressed); // Sustained real release rearms.
    r=feed(f,release,2);assert(r.state==State::Released && r.pressure==LivePressure::Release);
    for(int i=0;i<30;++i)assert(feed(f,release).state==State::Released); // Clear release never infers press.

    f.begin(true);arm(f);
    const auto history=f.result();
    auto noisy=frame(3000,2000,100,3745);noisy.adc[4]=3021;
    r=feed(f,noisy);
    assert(r.raw_available && r.rx_valid && !r.xy_stable && !r.mapped);
    assert(r.x==3000 && r.y==2000 && r.dx==21 && r.dy==0); // Raw remains observable when rejected.
    assert(r.pixel.x==0 && r.pixel.y==0); // No stale current pixel.
    assert(r.history_valid && r.history_at==history.history_at);
    assert(r.history_pixel.x==history.history_pixel.x && r.history_pixel.y==history.history_pixel.y);
    assert(uint32_t(now-r.history_at)==30 && r.state==State::Released);
    for(int i=0;i<20;++i)assert(feed(f,noisy).state==State::Released); // History cannot create a tap.
    arm(f);press(f);noisy=release;noisy.adc[4]+=21;
    r=feed(f,noisy);assert(r.state==State::Cancel && !r.mapped); // Bad XY cannot finish a pressure click.
    assert(!std::strcmp(r.reason,"XY_SPREAD"));
    arm(f);press(f,mid);noisy=mid;noisy.adc[4]+=21;
    r=feed(f,noisy);
    assert(r.state==State::Cancel && r.raw_available && !r.mapped && r.dx==21);
    assert(feed(f,mid,30).state==State::Released); // A cancelled XY gesture cannot finish from history.

    for(int bad=0;bad<7;++bad) {
        f.begin(true);arm(f);press(f,mid);
        const auto saved=f.result();auto invalid=mid;bool ok=true;
        switch(bad) {
            case 0:invalid.payload_ones=true;break;
            case 1:invalid.payload_zero=true;break;
            case 2:invalid.started=false;break;
            case 3:invalid.count=29;break;
            case 4:invalid.count=63;break; // Wrong protocol length is not complete.
            case 5:invalid.adc[2]=5000;break;
            default:ok=false;break;
        }
        r=feed(f,invalid,1,30,ok);
        assert(r.state==State::Cancel && !r.rx_valid && !r.xy_stable && !r.mapped);
        assert(r.history_valid && r.history_at==saved.history_at);
        assert(r.pressure==LivePressure::Invalid);
        assert(feed(f,mid,30).state==State::Released); // Old coordinate cannot bypass neutral/latch.
    }
    f.begin(true);arm(f);
    assert(feed(f,frame(0,2000,100,3745),20).state==State::Released);
    assert(!std::strcmp(f.result().reason,"XY_RAIL"));
    f.begin(true,4);arm(f);r=feed(f,mid,20);
    assert(r.raw_available && r.xy_stable && !r.mapped && !r.history_valid && r.state==State::Released);
    now=0xFFFFFF00U;f.begin(true);arm(f);const auto at=f.result().history_at;
    feed(f,noisy,10);assert(uint32_t(now-at)==300 && f.result().history_at==at);
    std::puts("PASS review regression: raw/stable/map/Z separation, MID XY opt-in, 600/300 thresholds, history never clicks, bad-frame cancellation, release/rearm, single-tap latch and history age wrap; synthetic traces only");
}
int main() {
    for(unsigned rotation=0;rotation<4;++rotation)for(unsigned x=0;x<=4095;x+=63)for(unsigned y=0;y<=4095;y+=63){
        Point p;assert(liveMap(x,y,rotation,p));
        assert(p.x>=0 && p.x<(rotation%2?320:240) && p.y>=0 && p.y<(rotation%2?240:320));
    }
    Point p;assert(liveMap(0,0,1,p) && p.x==319 && p.y==0);
    assert(liveMap(4095,4095,1,p) && p.x==0 && p.y==239);
    assert(!liveMap(5000,1000,1,p) && !liveMap(1,1,4,p));
    LiveFilter f;f.begin(false);assert(feed(f,frame(),20).state==State::Released); // No boot press.
    arm(f);press(f);
    auto r=feed(f,frame(1005,2010,100,3745));assert(r.state==State::Pressed); // Existing 300/600 hysteresis.
    assert(feed(f,frame(1000,2000,0,4095)).state==State::Pressed);
    assert(feed(f,frame(1000,2000,0,4095)).state==State::Released);
    arm(f);press(f);
    auto bad=frame();bad.payload_ones=true;
    assert(feed(f,bad).state==State::Cancel);
    assert(feed(f,frame(),50).state==State::Released); // Bad frame cannot replay a click/press.
    arm(f);press(f);assert(feed(f,frame(),1,151).state==State::Cancel);
    arm(f);press(f);assert(feed(f,frame(),51).state==State::Cancel); // Hard maximum hold.
    f.begin(false);arm(f);
    assert(feed(f,frame(1000,2000,4,0),20).state==State::Released); // Bad Z not accepted in trusted mode.
    bad=frame();bad.adc[4]=3000;assert(feed(f,bad,20).state==State::Released);
    bad=frame(5000,5000);assert(feed(f,bad).state==State::Released);
    f.begin(true);bad=frame();bad.payload_zero=true;
    assert(feed(f,bad,12).state==State::Released);
    auto xy=frame(1000,2000,0,0);
    r=feed(f,xy,3);assert(r.state==State::Pressed && r.xy_only && r.mapped && !r.z_valid);
    assert(!std::strcmp(r.reason,"XY_ONLY_EXPERIMENTAL"));
    r=feed(f,xy,6);assert(r.state==State::Released && !std::strcmp(r.reason,"XY_TAP_END"));
    for(int i=0;i<100;++i)assert(feed(f,xy).state==State::Released); // Same stable signal cannot keep clicking.
    feed(f,bad,12);press(f,xy);bad.payload_zero=false;bad.payload_ones=true;
    assert(feed(f,bad).state==State::Cancel); // Invalid RX cancels, does not finish an experimental tap.
    feed(f,bad,12);press(f,xy);assert(feed(f,xy,1,151).state==State::Cancel);
    feed(f,bad,12);press(f,xy);assert(feed(f,xy,5,140).state==State::Cancel); // Sparse coherent frames hit max hold.
    feed(f,bad,12);
    for(unsigned i=0;i<30;++i)assert(feed(f,frame(i%2?1000:3000,2000,0,0)).state==State::Released);
    assert(feed(f,frame(0,4095,0,0),20).state==State::Released); // Rail-only XY cannot trigger.
    f.begin(true);arm(f);press(f);assert(!f.result().xy_only); // Trust pressure first when available.
    assert(feed(f,frame(2000,2000)).state==State::Cancel); // Large jump cancels.
    now=0xFFFFFF00U;f.begin(false);arm(f);press(f);
    assert(feed(f,frame(1000,2000,0,4095),2).state==State::Released);
    reviewRegression();
    std::puts("PASS live filter: real trace mapping/ranges, pressure first, debounce, normal release, FF/00/Z/noise/stale/jump cancellation, XY inferred tap latch, neutral rearm, max hold and timer wrap; synthetic ADC only");
}
