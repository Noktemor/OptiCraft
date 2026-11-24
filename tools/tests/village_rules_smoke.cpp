#include "mobs/village/Village_Rules.h"
#include <cassert>
#include <iostream>
#include <unordered_map>

using Village::Cell;
using Village::Cell_Data;
using Village::Cell_Hash;

int main() {
    std::unordered_map<Cell,Cell_Data,Cell_Hash> blocks;
    const auto place=[&](int x,int y,int z,bool solid=false,bool bed=false,bool door=false,bool torch=false) {
        blocks[{x,y,z}]={true,solid,door,bed,torch,false};
    };
    for(int x=-1;x<=7;++x)for(int z=-1;z<=7;++z) {
        for(int y=0;y<=7;++y)place(x,y,z);
        place(x,0,z,true);
    }
    for(int x=0;x<7;++x)for(int z=0;z<7;++z) {
        place(x,3,z,true);
        if(x==0||x==6||z==0||z==6) {
            place(x,1,z,true);place(x,2,z,true);
        }
    }
    place(3,1,0,false,false,true);place(3,2,0,false,false,true);
    place(2,1,3,false,true);place(1,2,1,false,false,false,true);
    auto read=[&](Cell p)->Cell_Data {
        const auto it=blocks.find(p);
        return it==blocks.end()?Cell_Data{}:it->second;
    };
    const Cell bed{2,1,3};
    const auto home=Village::find_home(bed,read);
    assert((home && home->floor_area>=20 && home->door == Cell{3,1,0}));
    place(1,2,1);
    assert(!Village::find_home(bed,read));
    place(1,2,1,false,false,false,true);
    place(3,1,0,true);place(3,2,0,true);
    assert(!Village::find_home(bed,read));
    place(3,1,0,false,false,true);place(3,2,0,false,false,true);
    place(4,3,4);
    assert(!Village::find_home(bed,read));
    place(4,3,4,true);
    place(6,1,3);place(6,2,3);
    assert(!Village::find_home(bed,read));
    for(int n=0;n<=110;++n) {
        const int expected = n>=92?7:n>=70?6:n>=51?5:n>=35?4:n>=22?3:n>=12?2:n>=5?1:0;
        assert(Village::golem_limit(n)==expected);
    }
    assert(Village::job_for_block(533)==Village::Profession::Farmer);
    assert(Village::job_for_block(530)==Village::Profession::Archer);
    assert(Village::job_for_block(427)==Village::Profession::Librarian);
    assert(Village::job_for_block(531)==Village::Profession::Hunter);
    assert(Village::job_for_block(532)==Village::Profession::Cook);
    assert(Village::job_for_block(534)==Village::Profession::Smith);
    for(auto p:{Village::Profession::Farmer,Village::Profession::Archer,Village::Profession::Librarian,
                Village::Profession::Hunter,Village::Profession::Cook,Village::Profession::Smith}) {
        for(const auto& offer:Village::offers(p)) {
            assert(offer.cost&&offer.result&&offer.cost_count>0&&offer.result_count>0);
        }
    }
    std::cout<<"Village rules: houses, invalid rooms, workstations, trades, 111 golem thresholds passed\n";
}
