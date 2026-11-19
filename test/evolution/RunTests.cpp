// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../client/arena/RoguelikeRun.h"
#include "../../lib/battle/EvolutionSignals.h"
#include "../../lib/battle/EvolutionDamage.h"
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
	RoguelikeRun team(55);
	team.begin(2, {1,2,3,4}, 1, 24);
	mustFail([&]{team.recruit(1, 4);});
	team.recruit(3, 6);
	mustFail([&]{team.recruit(4, 3);});
	for(int wave = 1; wave <= 3; ++wave)
	{
		team.finish(true, 5, 8, {"mass","flurry","fission"}, {}, false, {wave});
		require(team.stack(1).count == wave + 1 + wave/2, "Independent survivor report");
		const std::string id = wave == 1 ? "mass" : wave == 2 ? "fission" : "flurry";
		auto choice = std::find(team.offers.begin(), team.offers.end(), id);
		team.pick(static_cast<size_t>(choice-team.offers.begin()), wave == 1 ? 0 : 1);
	}
	require(team.hasCombination({"fission","flurry"},1) && !team.hasCombination({"fission","flurry"},0), "Stack-specific synergy");
	team.recruit(6, 3);
	team.finish(true, 0, 8, pool, {}, false, {0,2});
	std::stringstream teamSave;
	team.save(teamSave);
	auto loaded = RoguelikeRun::restore(teamSave);
	require(loaded.rosterSize() == 3 && loaded.stack(2).creature == 6 && loaded.stack(2).count == 4
		&& loaded.stack(1).count == 3 && loaded.recipients == team.recipients
		&& loaded.offers == team.offers && loaded.hasCombination({"fission","flurry"},1), "Roster checkpoint");
	loaded.pick(0,2);
	loaded.finish(false, 0, 1, pool, {}, false, {0,0});
	require(loaded.phase == Phase::Defeat, "Roster wiped");
	std::stringstream legacy;
	legacy << "EA2 1 0 2 0 2 1 24 0\n1 1 1 1\n" << team.random << "\n1\n\"mass\"\n0\n0\n";
	auto migrated = RoguelikeRun::restore(legacy);
	require(migrated.rank("mass") == 1 && migrated.recipients == std::vector<size_t>{0}, "EA2 migration");
	RoguelikeRun hive(83);
	hive.begin(1,{1,1,1,1},1,24);
	hive.recruit(2,6);
	hive.army.mutations = {{"nursery",1},{"kinship",1},{"ossuary",1}};
	hive.stack(1).mutations = {{"underdog",2},{"nursery",1}};
	require(hive.lineageDamage(1) == 8 && !hive.smallest(0), "Outnumbered specialist");
	hive.finish(true,32,3,pool,{},false,{0});
	require(hive.army.count == 41 && hive.stack(1).count == 5, "Loss salvage and brood use battle counts only");
	require(hive.stack(1).harvest == 40 && hive.lineageDamage(1) == 12, "Permanent copy growth feeds allies");
	hive.pick(0);
	hive.finish(false,60,3,pool,{},false,{30});
	require(hive.phase == Phase::Defeat && hive.stack(1).harvest == 40, "No post-victory effects on defeat");
	std::map<std::pair<int,int>,int> deliveries;
	deliverSignal(0,AllyLost,{0,1,2},[&](int id,int event){
		++deliveries[{id,event}];
		return true;
	});
	require(deliveries.size()==5 && !deliveries.count({0,AllyLost}), "No self-delivery of ally losses");
	for(const auto & delivery:deliveries) require(delivery.second==1, "Once per listener and cause");
	require(damageProduct(INT64_MAX,100,100)==INT64_MAX && damageProduct(INT64_MAX,200,100)==INT64_MAX
		&& addDamage(INT64_MAX-2,3)==INT64_MAX, "Native damage boundary");
	require(displayAmount(1250000)=="1,25 млн" && displayAmount(Amount(10)*1000000000000)=="1,00e13","Large number display");
	RoguelikeRun colony(1);
	colony.begin(1,{0,0,0,0},1,24);
	colony.recruit(2,6);
	colony.army.mutations={{"incubator",2},{"seedbank",1},{"tithe",1},{"spoils",1}};
	colony.stack(1).mutations={{"graft",1},{"remnant",1},{"molting",1}};
	colony.finish(true,36,12,pool,{},true,{0});
	require(colony.army.count==77 && colony.stack(1).count==2 && colony.stack(1).harvest==27,
		"Brood, loss, tithe and elite settlement");
	for(int stop : {30,50,100})
	{
		RoguelikeRun simulation(2048);
		simulation.begin(1,{1,1,1,1},1,24);
		simulation.recruit(2,6);
		std::set<size_t> bosses;
		for(int wave=1;wave<=stop;++wave)
		{
			if(wave==4) simulation.recruit(3,4);
			auto plan=encounter(simulation.wave,wave,10);
			if(simulation.boss()) bosses.insert(plan.bossIndex);
			require(plan.pair==(wave>=16) && plan.recipe==(wave>=36) && plan.mutatedStacks==(wave>=56?4:wave>=36?3:wave>=16?2:wave>=6?1:0),"Encounter stages");
			std::vector<int> survivors(simulation.companions.size(),6);
			simulation.finish(true,20,10,pool,{},false,survivors);
			simulation.pick(0,wave%simulation.rosterSize());
			std::stringstream saved;
			simulation.save(saved);
			simulation=RoguelikeRun::restore(saved);
		}
		require(simulation.wave==stop+1 && bosses.size()==std::min(stop/5,10),"Long simulation with scripted survivors");
	}
	Amount enormous = Amount(1)<<160;
	require((enormous*enormous) > enormous, "Arbitrary precision");
	mustFail([&]{engineInt(enormous);});
	require(engineInt(std::numeric_limits<int>::max()) == std::numeric_limits<int>::max(), "Valid boundary rejected");
	std::cout << "16 wave transitions, replay, duplicate settlement, rewards, reset and range checks passed\n";
}
