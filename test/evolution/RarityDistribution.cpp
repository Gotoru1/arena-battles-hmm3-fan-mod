// SPDX-License-Identifier: GPL-2.0-or-later
#include "client/arena/RoguelikeRun.h"
#include <iostream>
#include <set>
int main()
{
    const std::vector<std::string> pool{"c","r","e","l"};
    const std::map<std::string,unsigned> tiers{{"c",0},{"r",1},{"e",2},{"l",3}};
    for(int wave : {1,5})
    {
        std::array<int,4> counts{};
        for(unsigned seed=0; seed<20000; ++seed)
        {
            evolution::RoguelikeRun run(seed);
            run.begin(0,{1,1,1,1},0,24);
            run.wave=wave;
            run.finish(true,24,2,pool,tiers);
            if(std::set<std::string>(run.offers.begin(),run.offers.end()).size()!=3) return 1;
            ++counts.at(tiers.at(run.offers.front()));
        }
        const std::array<int,4> expected = wave==5 ? std::array<int,4>{35,37,23,5} : std::array<int,4>{53,32,13,2};
        std::cout << "wave " << wave;
        for(size_t i=0;i<4;++i)
        {
            std::cout << ' ' << counts[i];
            if(std::abs(counts[i]-expected[i]*200)>250) return 1;
        }
        std::cout << '\n';
    }
}
