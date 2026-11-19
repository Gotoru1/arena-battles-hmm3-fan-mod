// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <deque>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
namespace evolution
{
enum class ReactionKind { Loss, Death, Hunt };
struct Reaction
{
	uint64_t serial, cause;
	ReactionKind kind;
	uint32_t source, target, amount;
	bool destroyed;
};

// One causal chain owns its FIFO. Nested native attacks append to the active chain.
// A hunter may revisit a lineage after a new action, or hit another original body.
// A repeated hunter/newborn lineage pair in the same chain is a feedback edge.
class BattleChain
{
	std::deque<Reaction> pending;
	std::set<std::pair<uint32_t,uint32_t>> newbornHunts;
	uint64_t next=0, parent=0;
	bool dispatching=false;
public:
	const uint64_t id;
	explicit BattleChain(uint64_t id):id(id){}
	void post(ReactionKind kind,uint32_t source,uint32_t target,uint32_t amount=0,bool destroyed=false)
	{
		if(next==std::numeric_limits<uint64_t>::max()) throw std::overflow_error("Evolution event id exhausted");
		pending.push_back({++next,parent,kind,source,target,amount,destroyed});
	}
	bool allowHunt(uint32_t sourceRoot,uint32_t targetRoot,bool newborn)
	{
		return !newborn || newbornHunts.emplace(sourceRoot,targetRoot).second;
	}
	template<class Handle> void drain(Handle handle)
	{
		if(dispatching) return;
		dispatching=true;
		try
		{
			while(!pending.empty())
			{
				auto event=pending.front();
				pending.pop_front();
				parent=event.serial;
				handle(event);
			}
		}
		catch(...) {dispatching=false; parent=0; throw;}
		dispatching=false;
		parent=0;
	}
};
}
