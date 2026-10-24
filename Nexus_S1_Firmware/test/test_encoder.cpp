#include "encoder_core.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
using namespace nexus_encoder;
static void cw(Quadrature& q) { for(auto s:{2,0,1,3})q.edge(uint8_t(s)); }
static void ccw(Quadrature& q) { for(auto s:{1,0,2,3})q.edge(uint8_t(s)); }
int main() {
    Quadrature q(4);q.reset(3);
    q.edge(2);assert(q.take()==0);q.edge(3);assert(q.take()==0); // Contact bounce cancels.
    for(int i=0;i<100;++i)cw(q);
    assert(q.take()==100 && q.take()==0);
    for(int i=0;i<100;++i)ccw(q);
    assert(q.take()==-100);
    q.edge(0);assert(q.take()==0 && q.invalid()==1);q.reset(3);
    q.edge(2);q.edge(0);q.edge(2);q.edge(3);assert(q.take()==0); // Partial reversal.
    for(int initial=0;initial<4;++initial) { q.reset(initial);q.edge(initial);assert(!q.take()); }
    Quadrature half(2);half.reset(3);cw(half);assert(half.take()==2);
    Button b;b.begin(false,0);b.update(false,29);assert(!b.armed());
    b.update(false,30);assert(b.armed() && b.take()==Action::None);
    b.update(true,40);b.update(false,45);b.update(true,50);b.update(true,79);
    assert(!b.pressed() && b.pressCount()==0); // Sub-30ms bounce never counts.
    b.update(true,80);assert(b.pressed() && b.pressCount()==1 && b.take()==Action::None);
    b.update(true,150);assert(b.pressCount()==1); // Count on press, not on release or every poll.
    b.update(false,160);b.update(true,170);b.update(false,180);b.update(false,209);
    assert(b.pressed() && b.pressCount()==1 && b.take()==Action::None);
    b.update(false,210);assert(!b.pressed() && b.armed() && b.pressCount()==1);
    assert(b.take()==Action::Click && b.take()==Action::None);
    b.update(true,240);b.update(true,270);assert(b.pressCount()==2);
    b.update(false,300);b.update(false,330);assert(b.take()==Action::Click && b.pressCount()==2);
    // A/B decoding remains independent while the separate switch is held.
    q.reset(3);b.update(true,400);b.update(true,430);
    for(int i=0;i<5;++i) { cw(q);b.update(true,440+i*10); }
    assert(q.take()==5 && b.pressed() && b.pressCount()==3 && b.take()==Action::None);
    for(int i=0;i<3;++i) { ccw(q);b.update(true,500+i*10); }
    assert(q.take()==-3 && b.pressCount()==3);
    b.update(true,1199);assert(b.take()==Action::None);
    b.update(true,1200);assert(b.take()==Action::Back);b.update(true,3000);assert(b.take()==Action::None && b.pressCount()==3);
    b.update(false,3010);b.update(false,3040);assert(b.take()==Action::None && b.pressCount()==3);
    b.update(true,3100);b.update(true,3130);b.update(false,3150);b.update(false,3180);
    assert(b.take()==Action::Click && b.pressCount()==4); // Release re-arms after long hold.
    b.begin(true,4000);b.update(true,6000);assert(b.take()==Action::None && b.pressCount()==4); // Rebind-held must not act or reset count.
    b.update(false,6010);b.update(false,6040);b.update(true,6050);b.update(true,6080);
    assert(b.pressCount()==5);b.cancel();b.update(true,8000);assert(b.take()==Action::None && b.pressCount()==5);
    b.update(false,8010);b.update(false,8040);assert(b.take()==Action::None);
    Button boot;boot.begin(true,0);boot.update(true,2000);
    assert(boot.pressed() && !boot.armed() && boot.pressCount()==0 && boot.take()==Action::None);
    boot.update(false,2010);boot.update(false,2039);assert(!boot.armed());
    boot.update(true,2040);boot.update(true,3000);assert(boot.pressCount()==0); // Release bounce cannot re-arm.
    boot.update(false,3010);boot.update(false,3040);assert(boot.armed());
    boot.update(true,3050);boot.update(true,3080);assert(boot.pressCount()==1);
    // Diagnostic reads neither arm a boot-held input nor consume a pending click.
    b.begin(true,0);
    assert(b.pressed() && !b.armed() && !b.held());
    b.update(true,1000);assert(b.take()==Action::None && !b.armed());
    b.update(false,1010);b.update(false,1040);
    assert(!b.pressed() && b.armed() && !b.held());
    b.update(true,1050);b.update(true,1080);
    assert(b.pressed() && b.armed() && b.held() && b.pressCount()==6);
    b.update(false,1100);b.update(false,1130);
    for(int i=0;i<100;++i)assert(!b.pressed() && b.armed() && !b.held() && b.pressCount()==6);
    assert(b.take()==Action::Click && b.take()==Action::None);
    b.begin(false,0);b.update(false,30);b.update(true,100);b.update(true,130);
    b.update(false,899);b.update(false,929);assert(b.take()==Action::Click); // 799ms, not debounce-delayed Back.
    b.update(true,1000);b.update(true,1030);b.update(false,1800);b.update(false,1830);
    assert(b.take()==Action::Back); // Release at exactly 800ms.
    b.begin(false,UINT32_MAX-100);b.update(false,UINT32_MAX-70);
    b.update(true,UINT32_MAX-50);b.update(true,UINT32_MAX-20);b.update(true,749);
    assert(b.take()==Action::Back); // Millisecond wrap.
    std::puts("PASS encoder core: both directions, 100 steps, bounce/illegal/reversal, configurable 2/4 edges; independent 30ms SW press counting, no held repeats, release bounce/rearm, boot-held/rebind/cancel, short/800ms exclusion and wrap; synthetic levels only");
}
