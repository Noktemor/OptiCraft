#include "mobs/Boss_Choreography.h"
#include "mobs/Knight_Adaptive_Tactics.h"
#include "mobs/Celestial_Boss_Rules.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace Boss_Choreography;
int main(){
  // Every technique has positive warning, a finite strike window and recovery.
  for (int i=1;i<=static_cast<int>(Sun_Move::Delayed_Palm);++i){
    const Sun_Move technique=static_cast<Sun_Move>(i);
    const Move_Spec spec=sun_spec(technique);
    assert(spec.tell>=.4f && spec.active>.3f && spec.recovery>.3f);
    assert(spec.range>0 && spec.damage>0);
    int count=0;
    for (int j=0;j<3;++j) if(spec.strikes[j]>=0){
      ++count;
      assert(spec.strikes[j]<=spec.active);
      const float at=spec.tell+spec.strikes[j];
      assert(in_strike_window(spec,j,at-.03f,at+.03f));
      assert(!in_strike_window(spec,j,at+.01f,at+.04f));
      assert(!in_strike_window(spec,j,at-1.0f,at-.01f));
    }
    assert(count>=1);
  }
  // No forced removal of hands. The phase is determined ONLY by hand HP,
  // not by remaining core HP. Every hand configuration has valid attacks.
  for (int hands=0;hands<=4;++hands){
    for(unsigned n=0;n<50;n++){
      const Sun_Move move=choose_sun_move(hands,4.0f,Sun_Move::Idle,n);
      assert(move!=Sun_Move::Idle);
      assert(has_hitbox(move,hands));
      if(hands==0) assert(!uses_hands(move));
    }
  }
  std::array<float,4> alive{70,70,70,70};
  assert(!Celestial_Boss_Rules::sun_second_phase(alive));
  assert(Celestial_Boss_Rules::MoonHealth==300 && Celestial_Boss_Rules::SunHealth==250);
  // Observing a single dodge doesn't cause a cheating immediate counter.
  Knight_Adaptive_Tactics tactics;
  tactics.begin_windup();
  tactics.observe_dodge(.25f,5.0f,0.0f);
  tactics.observe_dodge(.3f,5.0f,0.0f);
  assert(tactics.dodge_events()==1);
  assert(tactics.intentional_delay(1)==0.0f);
  tactics.begin_windup();tactics.observe_dodge(.28f,5.0f,0.0f);
  tactics.begin_windup();tactics.observe_dodge(.30f,-5.0f,0.0f);
  assert(tactics.dodge_events()==3);
  assert(tactics.intentional_delay(1)>.20f);
  assert(tactics.intentional_delay(3)==0.0f); // not always the same feint
  tactics.complete_attack(1,false);tactics.complete_attack(1,false);
  tactics.complete_attack(2,true);tactics.complete_attack(2,true);
  assert(tactics.preference(2)>tactics.preference(1));
  assert(tactics.propose_followup(1,4,false,2)==4); // kite -> charge
  assert(tactics.propose_followup(1,2,false,3)==7); // orbit -> spin
  assert(tactics.propose_followup(1,5,false,1)==3); // jump -> ground slam
  assert(tactics.propose_followup(1,0,true,1)==2); // swipe -> double-swipe
  tactics.decay(200.0f);
  assert(tactics.preference(1)>0.0f);
  std::cout << "boss choreography: attack timing, hand freedom, knight adaptation OK\n";
}
