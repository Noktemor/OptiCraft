#include "mobs/Celestial_Boss_Rules.h"
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace Celestial_Boss_Rules;
int main() {
    static_assert(SatelliteCount == 4);
    static_assert(MoonHealth == 300 && SunHealth == 250);
    static_assert(MoonShieldHealth == 45 && SunHandHealth == 70);
    std::array<float,4> moon{MoonShieldHealth,MoonShieldHealth,MoonShieldHealth,MoonShieldHealth};
    assert(!moon_swooping(0.0f));
    assert(!moon_swooping(4.9f));
    assert(moon_swooping(5.0f));
    assert(moon_swooping(7.5f));
    assert(!moon_swooping(8.01f));
    assert(!moon_vulnerable(moon));
    assert(!damage_satellite(moon,-1,100));
    assert(!damage_satellite(moon,4,100));
    assert(!damage_satellite(moon,0,0));
    for (int i=0;i<4;++i) {
        assert(damage_satellite(moon,i,44));
        assert(!moon_vulnerable(moon));
        assert(damage_satellite(moon,i,1));
        assert(moon[i]==0);
        assert(alive_count(moon)==3-i);
    }
    assert(moon_vulnerable(moon));
    assert(!damage_satellite(moon,0,999));
    std::array<float,4> sun{70,70,70,70};
    assert(!sun_second_phase(sun));
    float old_speed=sun_speed(sun);
    float old_interval=sun_attack_interval(sun);
    for(int i=0;i<4;++i) {
        assert(damage_satellite(sun,i,100));
        assert(sun[i]==0.0f);
        assert(sun_speed(sun)>old_speed);
        assert(sun_attack_interval(sun)<old_interval);
        old_speed=sun_speed(sun); old_interval=sun_attack_interval(sun);
    }
    assert(sun_second_phase(sun));
    std::cout << "celestial boss rules passed (HP, 4 parts, gates, rage, phases)\n";
}
