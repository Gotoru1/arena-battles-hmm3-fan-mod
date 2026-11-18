// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../client/arena/RoguelikeRun.h"
#include <iostream>
#include <set>

using namespace evolution;

static void require(bool ok, const char * reason)
{
	if(!ok) throw std::runtime_error(reason);
}

template<class F> static void mustFail(F action)
{
	try { action(); } catch(const std::exception &) { return; }
	throw std::runtime_error("Expected rejection");
}

int main()
{
	const std::vector<std::string> pool{"harvest", "fission", "flurry", "mass", "leech"};
	RoguelikeRun run(2048), replay(2048);
	for(auto * r : {&run, &replay})
		r->begin(2, {1,2,3,4}, 3, 24);
	for(int n = 1; n <= 16; ++n)
	{
		require(run.boss() == (n % 5 == 0), "Boss cadence");
		const auto threat = run.threat();
		for(auto * r : {&run, &replay})
		{
			const auto bank = r->army.harvest;
			const auto rank = r->rank("harvest");
			r->finish(true, 20, 12, pool);
			require(r->army.count == 25 + 2*n, "Recruitment must use actual survivors");
			require(r->army.harvest == bank + 12 * rank, "Harvest uses prior mutation rank");
			require(r->phase == Phase::Reward && r->offers.size() == 3, "Reward transition");
			require(std::set<std::string>(r->offers.begin(),r->offers.end()).size() == 3, "Distinct offers");
			mustFail([&]{r->finish(true, 20, 12, pool);});
		}
		require(run.offers == replay.offers, "Seed changed rewards");
		auto choice = std::find(run.offers.begin(), run.offers.end(), "harvest");
		size_t index = choice == run.offers.end() ? 0 : static_cast<size_t>(choice-run.offers.begin());
		const auto id = run.offers[index];
		auto prev = run.rank(id);
		run.pick(index);
		replay.pick(index);
		require(run.rank(id) == prev+1 && run.history.back() == id, "Mutation must stack");
		require(run.phase == Phase::Battle && run.wave == n+1, "Wave advance");
		if(n % 5 != 0) require(run.threat() > threat, "Rising regular threat");
		mustFail([&]{run.pick(0);});
	}
	run.finish(false, 0, 1, pool);
	require(run.phase == Phase::Defeat && run.offers.empty(), "Permadeath");
	mustFail([&]{run.pick(0);});
	RoguelikeRun fresh(2048);
	require(fresh.wave == 1 && fresh.history.empty() && fresh.kills == 0, "Fresh run reset");
	Amount enormous = Amount(1)<<160;
	require((enormous*enormous) > enormous, "Arbitrary precision");
	mustFail([&]{engineInt(enormous);});
	require(engineInt(std::numeric_limits<int>::max()) == std::numeric_limits<int>::max(), "Valid boundary rejected");
	std::cout << "16 wave transitions, replay, duplicate settlement, rewards, reset and range checks passed\n";
}
