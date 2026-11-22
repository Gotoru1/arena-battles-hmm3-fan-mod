// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../client/arena/EvolutionSaves.h"
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
	run.restRule=false; replay.restRule=false; // these checks are not about the rest rule
	for(auto * r : {&run, &replay})
		r->begin(2, {1,2,3,4}, 3, 24);
	mustFail([&]{run.finish(true, 20, 12, {"a", "a", "b"});});
	require(run.phase == Phase::Battle && run.kills == 0, "Rejected report mutated run");
	require(run.threat() == 538, "The first wave starts at 60% strength");
	require(RoguelikeRun::afterHospital(24,12,1) == 18 && RoguelikeRun::afterHospital(24,12,6) == 16
		&& RoguelikeRun::afterHospital(24,12,11) == 15, "Hospital share by wave");
	require(RoguelikeRun::afterHospital(24,0,1) == 0 && RoguelikeRun::afterHospital(24,30,1) == 30
		&& RoguelikeRun::afterHospital(24,24,1) == 24, "Hospital never revives a unit or reduces growth");
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
		restored.restRule=false; // these checks are not about the rest rule
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
	fresh.restRule=false; // these checks are not about the rest rule
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
		hunt.restRule=false; // these checks are not about the rest rule
		hunt.begin(0,{1,1,1,1},0,24);
		hunt.finish(true,10,4,{"a","b","c","d"},{{"a",0},{"b",0},{"c",1},{"d",2}},true);
		require(hunt.offers.front()=="d", "Elite rarity guarantee");
	}
	RoguelikeRun team(55);
	team.restRule=false; // these checks are not about the rest rule
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
	loaded.restRule=false; // these checks are not about the rest rule
	// The first two units were destroyed to the last fighter: they are gone for good, the third leads the
	// army, and the save still opens although the owners of the earlier mutations no longer exist.
	require(team.rosterSize() == 1 && team.army.creature == 6 && team.army.count == 4 && team.lastSlots == std::vector<size_t>{2}
		&& team.army.mutations.empty(), "A destroyed unit does not return");
	require(loaded.rosterSize() == 1 && loaded.army.creature == 6 && loaded.army.count == 4
		&& loaded.recipients == team.recipients && loaded.army.identity == team.army.identity
		&& loaded.offers == team.offers && loaded.army.mutations.empty(), "Roster checkpoint");
	loaded.pick(0,0);
	loaded.finish(false, 0, 1, pool);
	require(loaded.phase == Phase::Defeat, "Roster wiped");
	std::stringstream legacy;
	legacy << "EA2 1 0 2 0 2 1 24 0\n1 1 1 1\n" << team.random << "\n1\n\"mass\"\n0\n0\n";
	auto migrated = RoguelikeRun::restore(legacy);
	migrated.restRule=false; // these checks are not about the rest rule
	require(migrated.rank("mass") == 1 && migrated.recipients == std::vector<size_t>{migrated.army.identity}, "EA2 migration");
	RoguelikeRun hive(83);
	hive.restRule=false; // these checks are not about the rest rule
	hive.begin(1,{1,1,1,1},1,24);
	hive.recruit(2,6);
	hive.army.mutations = {{"nursery",1},{"kinship",1},{"ossuary",1}};
	hive.stack(1).mutations = {{"underdog",2},{"nursery",1}};
	require(hive.lineageDamage(1) == 8 && !hive.smallest(0), "Outnumbered specialist");
	hive.finish(true,32,3,pool,{},false,{1});
	require(hive.army.count == 40 && hive.stack(1).count == 6, "Loss salvage and brood use battle counts only");
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
	colony.restRule=false; // these checks are not about the rest rule
	colony.begin(1,{0,0,0,0},1,24);
	colony.recruit(2,6);
	colony.army.mutations={{"incubator",2},{"seedbank",1},{"tithe",1},{"spoils",1}};
	colony.stack(1).mutations={{"graft",1},{"remnant",1},{"molting",1}};
	colony.finish(true,36,12,pool,{},true,{1});
	require(colony.army.count==76 && colony.stack(1).count==2 && colony.stack(1).harvest==25,
		"Brood, loss, tithe and elite settlement");
	for(int stop : {30,50,100})
	{
		RoguelikeRun simulation(2048);
		simulation.restRule=false; // these checks are not about the rest rule
		simulation.begin(1,{1,1,1,1},1,24);
		simulation.recruit(2,6);
		std::set<size_t> bosses;
		for(int wave=1;wave<=stop;++wave)
		{
			if(wave==4) simulation.recruit(3,4);
			auto plan=encounter(simulation.wave,wave,10);
			if(simulation.boss()) bosses.insert(plan.bossIndex);
			require(plan.mutationRank==(wave<26 ? 1 : 2+(wave-26)/40),"Enemy mutation ranks");
			std::vector<int> survivors(simulation.companions.size(),6);
			simulation.finish(true,20,10,pool,{},false,survivors);
			simulation.pick(0,wave%simulation.rosterSize());
			std::stringstream saved;
			simulation.save(saved);
			simulation=RoguelikeRun::restore(saved);
			simulation.restRule=false; // these checks are not about the rest rule
		}
		require(simulation.wave==stop+1 && bosses.size()==std::min(stop/5,10),"Long simulation with scripted survivors");
	}
	const auto folder = std::filesystem::temp_directory_path() / ("evolution-slots-" + std::to_string(std::random_device{}()));
	require(std::filesystem::create_directory(folder),"Isolated save test profile");
	SaveSlots slots(folder);
	{
		RoguelikeRun trophies(30);
		trophies.restRule=false; // these checks are not about the rest rule
		trophies.begin(0,{1,2,3,4},1,20);
		trophies.recruit(2,4);
		const auto id=trophies.stack(1).identity;
		std::array<int,7> gain{};gain[1]=23;
		trophies.finish(true,20,0,{"trait9","trait18","trait41"},{},false,{4},gain);
		slots.write(0,trophies);
		trophies=slots.read(0);
		trophies.restRule=false;
		require(trophies.stack(1).identity==id && id!=trophies.stack(0).identity && trophies.stack(1).borrowedTrait==23,"Saved trophy belongs to a persistent lineage");
		trophies.pick(0,1);
		std::swap(trophies.army,trophies.companions[0]);
		slots.write(0,trophies);
		trophies=slots.read(0);
		trophies.restRule=false;
		require(trophies.army.identity==id && trophies.rank(trophies.history.back(),0)==1,"Changing formation keeps draft ownership");
		trophies.finish(true,20,0,{"trait9","trait18","trait41"},{},false,{4});
		require(!trophies.army.borrowedTrait,"Trophy expires after its wave");
		std::filesystem::remove(slots.path(0));
	}
	{
		RoguelikeRun campaign(45);
		campaign.restRule=false; // these checks are not about the rest rule
		campaign.begin(0,{1,2,3,4},1,15);
		std::array<std::string,3> prizes{"reward.stat.0","reward.artifact.centaurAxe","reward.edict.supply"};
		for(const auto & id:prizes)
		{
			campaign.finish(true,10,0,{"trait9","trait18","trait23"});
			campaign.offers.back()=id;
			campaign.pick(2);
			slots.write(0,campaign);
			campaign=slots.read(0);
			campaign.restRule=false;
			require(campaign.claimed(id) && !campaign.rank(id),"Global reward survives without attaching to any lineage");
		}
		require(campaign.primary[0]==3,"Training applies exactly once across saves");
		campaign.finish(true,10,0,{"trait9","trait18","trait23"});
		require(campaign.army.count==26,"Supply doctrine increases actual reinforcement");
		std::filesystem::remove(slots.path(0));
	}
	{
		RoguelikeRun laws(913);
		laws.restRule=false; // these checks are not about the rest rule
		laws.begin(0,{1,2,3,4},1,20);
		for(int wave=1;wave<=5;++wave)
		{
			laws.finish(true,20,1,{"trait88","trait18","trait23"});
			if(wave < 5)
			{
				mustFail([&]{laws.takeBossLaw(0,"law.iron");});
				laws.pick(std::find(laws.offers.begin(),laws.offers.end(),"trait88")-laws.offers.begin());
			}
			else laws.takeBossLaw(0,"law.iron");
		}
		slots.write(0,laws);
		auto copy=slots.read(0);
		copy.restRule=false;
		require(copy.rank("law.iron",0)==1 && copy.wave==6,"Boss rule acquisition is permanent for its owner");
		std::filesystem::remove(slots.path(0));
	}
	{
		RoguelikeRun full(17);
		full.restRule=false; // these checks are not about the rest rule
		full.begin(0,{1,2,3,4},1,24);
		for(int wave=1;wave<=16;++wave)
		{
			if(full.offersRecruitment()) full.recruit(static_cast<int>(full.rosterSize())+1,5);
			slots.write(0,full);
			full=slots.read(0);
			full.restRule=false;
			require(!full.offersRecruitment(),"Reload preserves reinforcement choice");
			full.finish(true,24,1,{"trait9","trait23","trait51"},{},false,std::vector<int>(full.rosterSize()-1,5));
			full.pick(0,full.rosterSize()-1);
		}
		require(full.rosterSize()==7,"Seven independent lineages at wave sixteen");
		std::filesystem::remove(slots.path(0));
		RoguelikeRun solo(11);
		solo.restRule=false; // these checks are not about the rest rule
		solo.begin(0,{1,2,3,4},1,24);
		solo.lastRecruitmentWave=solo.wave;
		slots.write(0,solo);
		require(slots.read(0).rosterSize()==1 && !slots.read(0).offersRecruitment(),"Solo run remains solo after resuming");
		std::filesystem::remove(slots.path(0));
	}
	RoguelikeRun branching(12);
	branching.restRule=false; // these checks are not about the rest rule
	branching.begin(1,{1,2,3,4},1,24);
	branching.recruit(2,5);
	for(int wave=1;wave<=2;++wave)
	{
		branching.finish(true,20,4,{"wingbud","lens","quill"},{},false,{5});
		const size_t pick=std::find(branching.offers.begin(),branching.offers.end(),"wingbud")-branching.offers.begin();
		if(wave==2)
		{
			slots.write(1,branching);
			auto restored=slots.read(1);
			restored.restRule=false;
			require(restored.offers==branching.offers && restored.phase==Phase::Reward,"Stable reward checkpoint");
		}
		branching.pick(pick,1,wave==2);
		slots.write(0,branching);
	}
	auto resumed=slots.read(0);
	resumed.restRule=false;
	require(resumed.alternate(1,"wingbud") && !resumed.alternate(0,"wingbud") && resumed.rank("wingbud",1)==2
		&& resumed.random==branching.random && resumed.stack(1).count==branching.stack(1).count,"Branch ownership across restarts");
	slots.write(2,resumed);
	RoguelikeRun invalid(5);
	invalid.restRule=false; // these checks are not about the rest rule
	mustFail([&]{slots.write(0,invalid);});
	require(slots.read(0).wave==3,"Failed replacement preserves last good save");
	resumed.finish(false,0,0,pool,{},false,{0});
	slots.write(0,resumed);
	require(!slots.exists(0) && slots.read(1).phase==Phase::Reward && slots.read(2).alternate(1,"wingbud"),"Permadeath is isolated to one slot");
	std::istringstream previousFormat("EA3 12 0 1 0 1 2 24 0\n1 2 3 4\n");
	mustFail([&]{RoguelikeRun::restore(previousFormat);});
	std::ostringstream oldCheckpoint;
	oldCheckpoint << "EA3 12 0 1 0 1 2 24 0\n1 2 3 4\n" << branching.random << "\n0\n0\n0\n0\n";
	std::istringstream oldInput(oldCheckpoint.str());
	require(RoguelikeRun::restore(oldInput).alternateForms.empty(),"EA3 migration");
	std::filesystem::remove(slots.path(1));
	std::filesystem::remove(slots.path(2));
	require(std::filesystem::remove(folder),"No partial save files left behind");
	{
		// Biological Singularity and Mutation Rebirth reshape ranks by fixed rules that a save replay repeats.
		RoguelikeRun shaped(77);
		shaped.restRule=false; // these checks are not about the rest rule
		shaped.begin(2,{1,2,3,4},3,24);
		for(const std::string wanted : {"trait92","trait9","trait23","trait83","trait9","trait18","trait51"})
		{
			shaped.finish(true,20,1,{wanted,"filler1","filler2"});
			const auto at=std::find(shaped.offers.begin(),shaped.offers.end(),wanted);
			shaped.pick(static_cast<size_t>(at-shaped.offers.begin()));
		}
		const std::map<std::string,unsigned> expected{{"trait18",3},{"trait23",2},{"trait51",1},{"trait83",2},{"trait9",3},{"trait92",1}};
		require(shaped.army.mutations==expected && shaped.wave==8,"Singularity and rebirth ranks");
		std::stringstream stored;
		shaped.save(stored);
		require(RoguelikeRun::restore(stored).army.mutations==expected,"Replay rebuilds reshaped ranks");
	}
	{
		// Lasting growth: fallen allies, one point per victory, a leap into the improved creature, boss rebirth.
		RoguelikeRun grown(5);
		grown.restRule=false; // these checks are not about the rest rule
		grown.begin(2,{1,2,3,4},3,24);
		mustFail([&]{grown.grow(0,1,true,9);});
		for(const std::string wanted : {"trait80","trait85","trait70"})
		{
			grown.finish(true,20,1,{wanted,"filler1","filler2"});
			const auto at=std::find(grown.offers.begin(),grown.offers.end(),wanted);
			grown.pick(static_cast<size_t>(at-grown.offers.begin()));
		}
		grown.finish(true,20,1,{"trait99","filler1","filler2"});
		grown.grow(0,3,true,9);
		require(grown.army.harvest==40 && grown.army.creature==9,"Fallen allies, learning and the leap");
		grown.grow(0,0,true,9);
		require(grown.army.harvest==100 && grown.army.creature==9,"A unit already in its best form grows stronger instead");
		grown.grow(0,0,false,12);
		require(grown.army.harvest==110 && grown.army.creature==9,"No leap without its conditions");
		grown.rebirth();
		require(grown.primary==(std::array<int,4>{1,2,3,4}),"World Rebirth waits for a boss");
		const auto at=std::find(grown.offers.begin(),grown.offers.end(),"trait99");
		grown.pick(static_cast<size_t>(at-grown.offers.begin()));
		require(grown.boss(),"Wave five is a boss");
		grown.finish(true,20,1,{"filler0","filler1","filler2"});
		grown.rebirth();
		require(grown.primary==(std::array<int,4>{2,3,4,5}),"World Rebirth after the boss");
	}
	{
		// Keepsakes: an absorbed property, adaptations, a three-wave arsenal and absorbed descendants survive a save.
		RoguelikeRun keeper(9);
		keeper.restRule=false; // these checks are not about the rest rule
		keeper.begin(2,{1,2,3,4},3,24);
		mustFail([&]{keeper.keep(0,0,true,{0,0,0},0);});
		for(const std::string wanted : {"trait96","trait98","trait76","trait86","trait9","trait23"})
		{
			keeper.finish(true,20,1,{wanted,"filler1","filler2"});
			const auto at=std::find(keeper.offers.begin(),keeper.offers.end(),wanted);
			keeper.pick(static_cast<size_t>(at-keeper.offers.begin()));
		}
		keeper.finish(true,20,1,pool);
		const auto before=keeper.army.count;
		keeper.keep(0,2,true,{19,0,0},7);
		const std::map<std::string,int> expected{{"adapt",1},{"arsenal.19",3},{"prop.2",1}};
		require(keeper.army.gifts==expected && keeper.army.count==before+7,"Keepsakes of a won battle");
		std::stringstream stored;
		keeper.save(stored);
		auto loaded=RoguelikeRun::restore(stored);
		loaded.restRule=false; // these checks are not about the rest rule
		require(loaded.army.gifts==expected && loaded.army.count==keeper.army.count,"Keepsakes survive a save");
		// The arsenal lasts three waves; a property outside the list is ignored.
		loaded.keep(0,9,false,{0,0,0},0);
		loaded.keep(0,-1,false,{0,0,0},0);
		require(loaded.army.gifts.at("arsenal.19")==1 && !loaded.army.gifts.count("prop.9"),"Arsenal ages");
		loaded.keep(0,-1,true,{0,0,0},0);
		require(!loaded.army.gifts.count("arsenal.19") && loaded.army.gifts.at("adapt")==2,"Arsenal expires, adaptations add up");
		// A unit without these mutations keeps nothing.
		RoguelikeRun plain(9);
		plain.restRule=false; // these checks are not about the rest rule
		plain.begin(2,{1,2,3,4},3,24);
		plain.finish(true,20,1,pool);
		plain.keep(0,2,true,{19,0,0},7);
		require(plain.army.gifts.empty() && plain.army.count==27,"Keepsakes need their mutations");
		// A save from before keepsakes still opens.
		std::ostringstream old;
		old << "EA5 12 0 1 0 1 2 24 0\n1 2 3 4\n" << plain.random << "\n0\n0\n0\n0\n2\n1 0\n\n0\n";
		std::istringstream oldInput(old.str());
		require(RoguelikeRun::restore(oldInput).army.gifts.empty(),"EA5 migration");
	}
	{
		// Rest rule: the unit that has just taken a mutation cannot take the next one.
		RoguelikeRun rested(3);
		rested.begin(2,{1,2,3,4},3,24);
		rested.recruit(5,6);
		const std::vector<std::string> extras{"reward.stat.0","reward.stat.1","reward.stat.2","reward.stat.3","trait9"};
		rested.finish(true,20,1,pool,{},false,{6});
		require(!rested.resting(0) && !rested.resting(1),"Nobody rests before the first mutation");
		rested.pick(0,0);
		rested.finish(true,20,1,pool,{},false,{6});
		rested.offerArmyReward(extras);
		require(rested.resting(0) && !rested.resting(1) && !rested.allResting(),"The last recipient rests");
		mustFail([&]{rested.pick(0,0);});
		require(rested.phase==Phase::Reward && rested.wave==2,"A refused pick changes nothing");
		rested.pick(0,1);
		rested.finish(true,20,1,pool,{},false,{6});
		require(!rested.resting(0) && rested.resting(1),"Rest lasts one wave");
		rested.pick(0,0);
		// A reward for the whole army ends the rest of the last recipient as well.
		rested.finish(true,20,1,{"filler0","filler1","filler2"},{},false,{6});
		rested.offers.back()="reward.stat.1";
		rested.pick(2,0);
		rested.finish(true,20,1,pool,{},false,{6});
		require(!rested.resting(0) && !rested.resting(1),"An army reward is not a mutation");

		// A lone unit rests too: on that wave all three cards are army rewards.
		RoguelikeRun lone(4);
		lone.begin(2,{1,2,3,4},3,24);
		lone.finish(true,20,1,pool);
		lone.pick(0);
		lone.finish(true,20,1,pool);
		lone.offerArmyReward(extras);
		require(lone.allResting(),"A lone unit rests");
		for(const auto & offer : lone.offers) require(RoguelikeRun::armyReward(offer),"Only army rewards while everybody rests");
		require(std::set<std::string>(lone.offers.begin(),lone.offers.end()).size()==3,"Distinct army rewards");
		const auto before=lone.primary;
		lone.pick(0);
		require(lone.primary!=before && lone.wave==3,"The army reward is taken");
		lone.finish(true,20,1,pool);
		lone.offerArmyReward(extras);
		require(!lone.allResting(),"The unit may mutate again one wave later");
		// A save made while the unit rests restores the rest.
		RoguelikeRun saved(4);
		saved.begin(2,{1,2,3,4},3,24);
		saved.finish(true,20,1,pool);
		saved.pick(0);
		std::stringstream stream;
		saved.save(stream);
		require(RoguelikeRun::restore(stream).resting(0),"Rest survives a save");
	}
	{
		// Winning with every original unit destroyed ends the run.
		RoguelikeRun doomed(8);
		doomed.begin(2,{1,2,3,4},3,24);
		doomed.finish(true,0,5,pool);
		require(doomed.phase==Phase::Defeat,"No unit left to continue");
		// The leader may fall while a companion carries on with its own mutations.
		RoguelikeRun heir(8);
		heir.restRule=false;
		heir.begin(2,{1,2,3,4},3,24);
		heir.recruit(5,6);
		heir.finish(true,20,1,{"trait9","filler1","filler2"},{},false,{6});
		heir.pick(static_cast<size_t>(std::find(heir.offers.begin(),heir.offers.end(),"trait9")-heir.offers.begin()),1);
		heir.finish(true,0,1,pool,{},false,{4});
		require(heir.phase==Phase::Reward && heir.rosterSize()==1 && heir.army.creature==5 && heir.rank("trait9")==1
			&& heir.lastSlots==std::vector<size_t>{1},"The companion takes the lead");
	}
	{
		// Enemy strength: 60-80% on the first five waves, full budget on wave seven, then 15% more every wave.
		const auto ramp=[](int wave){return RoguelikeRun::introduction(wave);};
		require(ramp(1)==60 && ramp(5)==80 && ramp(6)==90 && ramp(8)==110 && ramp(9)==125 && ramp(20)==290 && ramp(50)==740,"Enemy ramp");
		// Rarity odds: a common card is far likelier than a rare one, and so on down to legendary.
		const std::vector<std::string> cards{"c1","c2","c3","c4","r1","r2","r3","r4","e1","e2","e3","e4","l1","l2","l3","l4"};
		std::map<std::string,unsigned> tiers;
		for(const auto & card : cards) tiers[card]=card[0]=='c' ? 0 : card[0]=='r' ? 1 : card[0]=='e' ? 2 : 3;
		std::array<int,4> seen{};
		for(unsigned seed=1;seed<=400;++seed)
		{
			RoguelikeRun odds(seed);
			odds.begin(2,{1,2,3,4},3,24);
			odds.finish(true,20,1,cards,tiers);
			for(const auto & offer : odds.offers) ++seen[tiers.at(offer)];
		}
		require(seen[0]>seen[1]*3/2 && seen[1]>seen[2]*3/2 && seen[2]>seen[3]*2 && seen[3]>0,"Rarity odds fall steeply");
	}
	{
		// Characters: each bends one rule for the whole run and survives a save.
		const std::vector<std::string> extras{"reward.stat.0","reward.stat.1","reward.stat.2","reward.stat.3"};
		RoguelikeRun surgeon(5);
		surgeon.begin(2,{1,2,3,4},3,24);
		mustFail([&]{surgeon.lead(0);});
		mustFail([&]{surgeon.lead(7);});
		surgeon.lead(1);
		mustFail([&]{surgeon.lead(2);});
		require(surgeon.hospital()==70 && surgeon.healed(24,14)==21 && surgeon.healed(24,0)==0 && surgeon.healed(24,30)==30,"The surgeon returns more of the fallen");
		std::stringstream stream;
		surgeon.save(stream);
		require(RoguelikeRun::restore(stream).leader==1,"The leader survives a save");
		RoguelikeRun nobody(5);
		nobody.begin(2,{1,2,3,4},3,24);
		require(nobody.hospital()==50 && nobody.healed(24,14)==19,"Without the surgeon");

		// The same battles with and without the Pack Mother differ by her 8%.
		RoguelikeRun mother(5), orphan(5);
		mother.begin(2,{1,2,3,4},3,24);
		orphan.begin(2,{1,2,3,4},3,24);
		mother.lead(3);
		mother.finish(true,4,1,pool);
		orphan.finish(true,4,1,pool);
		require(orphan.army.count<12 && mother.army.count==orphan.army.count+1,"The pack grows by at least one fighter");
		mother.army.count=100;
		orphan.army.count=100;
		mother.pick(0);
		orphan.pick(0);
		mother.finish(true,100,1,pool);
		orphan.finish(true,100,1,pool);
		require(mother.army.count==orphan.army.count+orphan.army.count*8/100,"The pack grows by 8%");

		RoguelikeRun gladiator(5);
		gladiator.restRule=false;
		gladiator.begin(2,{1,2,3,4},3,24);
		gladiator.lead(4);
		for(int wave=1;wave<=5;++wave)
		{
			gladiator.finish(true,24,1,pool);
			require(gladiator.primary[0]==(wave==5 ? 2 : 1) && gladiator.primary[1]==(wave==5 ? 3 : 2),"The gladiator grows on bosses only");
			gladiator.pick(0);
		}

		RoguelikeRun alchemist(5);
		alchemist.begin(2,{1,2,3,4},3,24);
		alchemist.lead(5);
		alchemist.finish(true,24,1,pool);
		alchemist.pick(0);
		alchemist.finish(true,24,1,pool);
		require(!alchemist.resting(0) && !alchemist.allResting(),"The alchemist's units do not rest");
		alchemist.pick(0);

		RoguelikeRun collector(5);
		collector.restRule=false;
		collector.begin(2,{1,2,3,4},3,24);
		collector.lead(6);
		for(int wave=1;wave<=4;++wave)
		{
			collector.finish(true,24,1,pool);
			collector.offerArmyReward(extras);
			require(RoguelikeRun::armyReward(collector.offers.back()),"The collector finds an army reward every wave");
			collector.pick(0);
		}

		std::array<int,4> plain{},rare{};
		const std::vector<std::string> cards{"c1","c2","c3","c4","r1","r2","r3","r4","e1","e2","e3","e4","l1","l2","l3","l4"};
		std::map<std::string,unsigned> tiers;
		for(const auto & card : cards) tiers[card]=card[0]=='c' ? 0 : card[0]=='r' ? 1 : card[0]=='e' ? 2 : 3;
		for(unsigned seed=1;seed<=300;++seed)
			for(int who:{0,2})
			{
				RoguelikeRun odds(seed);
				odds.begin(2,{1,2,3,4},3,24);
				if(who) odds.lead(who);
				odds.finish(true,20,1,cards,tiers);
				for(const auto & offer : odds.offers) ++(who ? rare : plain)[tiers.at(offer)];
			}
		require(rare[0]<plain[0] && rare[2]+rare[3]>plain[2]+plain[3],"The geneticist sees rarer cards");
	}
	Amount enormous = Amount(1)<<160;
	require((enormous*enormous) > enormous, "Arbitrary precision");
	mustFail([&]{engineInt(enormous);});
	require(engineInt(std::numeric_limits<int>::max()) == std::numeric_limits<int>::max(), "Valid boundary rejected");
	std::cout << "16 wave transitions, replay, duplicate settlement, rewards, reset and range checks passed\n";
}
