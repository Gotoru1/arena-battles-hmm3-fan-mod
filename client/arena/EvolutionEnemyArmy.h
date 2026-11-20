// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace evolution
{
struct EnemyTrait
{
	int id;
	unsigned rarity;
};

inline int enemyCompanies(int wave)
{
	return 3 + std::min(27,(wave-1)/2);
}

// Draw carriers without replacement for each layer and traits without replacement
// on each individual company. Use raw mt19937 output for identical Windows/Linux deals.
inline std::vector<std::vector<int>> enemyMutations(int wave,uint32_t seed,const std::vector<EnemyTrait> & pool)
{
	const int slots=enemyCompanies(wave);
	std::vector<std::vector<int>> result(slots);
	if(wave<2 || pool.empty()) return result;
	size_t remaining=std::min<size_t>(slots*pool.size(),1u+unsigned(wave-2)/2);
	std::mt19937 rng(seed);
	std::vector<int> order(slots);
	std::iota(order.begin(),order.end(),0);
	constexpr std::array<unsigned,4> weights{54,29,14,3};
	while(remaining)
	{
		for(int i=slots-1;i>0;--i) std::swap(order[i],order[rng()%(i+1)]);
		for(int slot:order)
		{
			auto & owned=result[slot];
			std::array<std::vector<int>,4> buckets;
			for(const auto & trait:pool)
				if(std::find(owned.begin(),owned.end(),trait.id)==owned.end())
					buckets.at(trait.rarity).push_back(trait.id);
			unsigned total=0;
			for(size_t i=0;i<buckets.size();++i) if(!buckets[i].empty()) total+=weights[i];
			unsigned roll=rng()%total;
			for(size_t i=0;i<buckets.size();++i)
			{
				if(buckets[i].empty()) continue;
				if(roll<weights[i])
				{
					owned.push_back(buckets[i][rng()%buckets[i].size()]);
					break;
				}
				roll-=weights[i];
			}
			if(--remaining==0) break;
		}
	}
	return result;
}
}
