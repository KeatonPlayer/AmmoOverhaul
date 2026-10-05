#include "ShotQueue.h"
#include <cassert>
#include <iostream>
int main() {
    dc::ShotQueue q;
    dc::Key player{0x1000, 0x14, 0x123}, npc{0x2000, 0xFE002000, 0x123};
    dc::Shot s;
    // Same base weapon, independent actor ammo, delayed mixed-ammo ejections.
    assert(q.push(player, {0x308, 1, 100}));
    assert(q.push(npc, {0x1673, 0, 101}));
    assert(q.push(player, {0x1673, 0, 102}));
    assert(q.pop(npc, 200, s) && s.ammoID == 0x1673);
    assert(q.pop(player, 201, s) && s.ammoID == 0x308);
    assert(q.pop(player, 202, s) && s.ammoID == 0x1673);
    assert(!q.pop(player, 203, s)); // duplicate native event must not repeat a shot
    assert(q.push(player, {0x999, -1, 204}));
    assert(q.pop(player, 205, s) && s.rule == -1); // unmatched falls back
    q.push(player, {0x308, 1, 300});
    assert(!q.pop(player, 30301, s)); // stale delayed events
    q.push(player, {0x308, 1, 31000});
    assert(!q.pop({0x1001,0x14,0x123},31001,s)); // reused ID, different actor
    assert(!q.pop({0x1000,0x14,0x456},31001,s)); // different weapon
    q.clear(); assert(!q.pop(player,31001,s));
    for (unsigned i=0;i<32;++i) assert(q.push(player,{0x308,1,40000+i}));
    assert(!q.push(player,{0x1673,0,40033}));
    assert(!q.pop(player,40034,s)); // overflow fails to original, not wrong casing
    assert(q.push(player,{0x1673,0,80000}));
    assert(q.pop(player,80001,s) && s.ammoID==0x1673);
    std::cout << "Shot queue: all behavioral checks passed\n";
}
