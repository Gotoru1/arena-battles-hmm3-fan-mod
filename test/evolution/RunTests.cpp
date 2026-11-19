// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../client/arena/RoguelikeRun.h"
#include <iostream>
#include <sstream>
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
	mustFail([&]{run.finish(true, 20, 12, {"a", "a", "b"});});
	require(run.phase == Phase::Battle && run.kills == 0, "Rejected report mutated run");
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
	for(int phase = 0; phase < 2; ++phase)
	{
		auto snapshot = run;
		if(phase) snapshot.finish(true, 23, 5, pool);
		std::stringstream stream;
		snapshot.save(stream);
		auto restored = RoguelikeRun::restore(stream);
		require(restored.phase == snapshot.phase && restored.random == snapshot.random && restored.offers == snapshot.offers
			&& restored.history == snapshot.history && restored.army.mutations == snapshot.army.mutations && restored.army.count == snapshot.army.count
			&& restored.wave == snapshot.wave && restored.army.harvest == snapshot.army.harvest, "Checkpoint round trip");
		std::istringstream truncated("EA2 42 1");
		mustFail([&]{ RoguelikeRun::restore(truncated); });
	}
	run.finish(false, 0, 1, pool);
	require(run.phase == Phase::Defeat && run.offers.empty(), "Permadeath");
	mustFail([&]{run.pick(0);});
	RoguelikeRun fresh(2048);
	require(fresh.wave == 1 && fresh.history.empty() && fresh.kills == 0, "Fresh run reset");
	fresh.begin(0, {1, 1, 1, 1}, 1, 24);
	for(const auto & id : {"flurry", "fission", "mass", "harvest"})
	{
		std::vector<std::string> choices{id, "vigor", "haste"};
		fresh.finish(true, 40, 10, choices);
		auto i = std::find(fresh.offers.begin(), fresh.offers.end(), id);
		fresh.pick(static_cast<size_t>(i-fresh.offers.begin()));
	}
	require(fresh.hasCombination({"fission", "flurry"}), "Feeding Frenzy missing");
	require(!fresh.hasCombination({"leech", "barbs"}), "Blood Mirror unlocked too early");
	require(!fresh.hasCombination({"carapace", "riposte"}), "Living Fortress unlocked too early");
	fresh.finish(true, 95, 40, pool);
	require(fresh.army.count == 110 && fresh.army.harvest == 40 && fresh.lineageDamage() == 11,
		"Soul-steal survivors and Harvest must both feed lineage damage");
	for(unsigned seed=0; seed<200; ++seed)
	{
		RoguelikeRun hunt(seed);
		hunt.begin(0,{1,1,1,1},0,24);
		hunt.finish(true,10,4,{"a","b","c","d"},{{"a",0},{"b",0},{"c",1},{"d",2}},true);
		require(hunt.offers.front()=="d", "Elite rarity guarantee");
	}
	Amount enormous = Amount(1)<<160;
	require((enormous*enormous) > enormous, "Arbitrary precision");
	mustFail([&]{engineInt(enormous);});
	require(engineInt(std::numeric_limits<int>::max()) == std::numeric_limits<int>::max(), "Valid boundary rejected");
	std::cout << "16 wave transitions, replay, duplicate settlement, rewards, reset and range checks passed\n";
}
