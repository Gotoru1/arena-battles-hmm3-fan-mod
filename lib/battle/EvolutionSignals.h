// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <deque>
#include <set>
#include <utility>
#include <vector>

namespace evolution
{
// Custom ON_COMBAT_EVENT subtypes, kept separate from VCMI's native event IDs.
enum Signal { AllyMoved=200, AllyWaited, AllyGuarded, AllyKilled, AllyLost, AllyHealed, AllyGrew, AllyFell, Echo };

template<class Apply>
void deliverSignal(int source, int signal, const std::vector<int> & allies, Apply apply)
{
	std::deque<std::pair<int,int>> queue{{source,signal}};
	std::set<std::pair<int,int>> delivered;
	while(!queue.empty())
	{
		const auto current = queue.front();
		queue.pop_front();
		for(int receiver : allies)
		{
			if(receiver == current.first || !delivered.emplace(receiver,current.second).second)
				continue;
			if(apply(receiver,current.second))
				queue.emplace_back(receiver,Echo);
		}
	}
}
}
