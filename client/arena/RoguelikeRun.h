// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <iomanip>
#include <istream>
#include <ostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <boost/multiprecision/cpp_int.hpp>

namespace evolution
{
using Amount = boost::multiprecision::cpp_int;

inline int engineInt(const Amount & value)
{
	if(value < 0 || value > std::numeric_limits<int>::max())
		throw std::overflow_error("The next wave exceeds the engine's signed 32-bit range.");
	return value.convert_to<int>();
}

inline std::string displayAmount(const Amount & value)
{
	auto digits = value.str();
	if(value < 1000000) return digits;
	size_t exponent = digits.size()-1;
	if(exponent >= 12) return digits.substr(0,1) + "," + digits.substr(1,2) + "e" + std::to_string(exponent);
	size_t places = exponent >= 9 ? 9 : 6;
	size_t whole = digits.size()-places;
	return digits.substr(0,whole) + "," + digits.substr(whole,2) + (places == 9 ? " млрд" : " млн");
}

struct Encounter
{
	size_t bossIndex;
	int mutationRank;
	bool anomaly;
};

inline Encounter encounter(const Amount & wave, unsigned seed, size_t bosses)
{
	if(!bosses || wave < 1) throw std::invalid_argument("Invalid wave");
	Encounter plan{};
	plan.bossIndex = wave < 5 ? 0 : ((wave / 5 - 1) % bosses).convert_to<size_t>();
	plan.mutationRank = wave >= 26 ? engineInt(2 + (wave-26)/40) : 1;
	plan.anomaly = wave >= 12 && wave % 5 != 0 && seed % (wave >= 46 ? 3u : 6u) == 0;
	return plan;
}

struct Lineage
{
	uint32_t identity=1;
	int borrowedTrait=0;
	int creature = -1;
	Amount count = 0;
	std::map<std::string, unsigned> mutations;
	Amount harvest = 0;
	// Lasting keepsakes of won battles: "prop.N" an absorbed creature property,
	// "adapt" the number of adaptations, "arsenal.T" waves left for a taken mutation T.
	std::map<std::string, int> gifts;
};

enum class Phase { Draft, Battle, Reward, Defeat };

class RoguelikeRun
{
public:
	explicit RoguelikeRun(std::uint32_t seed) : seed(seed), random(seed) {}

	std::uint32_t seed;
	std::mt19937 random;
	Amount wave = 1;
	Amount kills = 0;
	int hero = -1;
	std::array<int, 4> primary{};
	Amount lastRecruitmentWave = 0;
	bool offersRecruitment() const
	{
		return phase == Phase::Battle && rosterSize() < 7 && (wave - 1) % 3 == 0 && lastRecruitmentWave != wave;
	}

	uint32_t nextIdentity=2;
	Lineage army;
	std::vector<Lineage> companions;
	std::vector<size_t> recipients;
	std::set<std::pair<size_t,std::string>> alternateForms;
	bool alternate(size_t slot,const std::string & id) const
	{
		return alternateForms.count({stack(slot).identity,id}) != 0;
	}
	std::vector<std::string> history;
	std::vector<std::string> synergies;
	std::vector<std::string> offers;
	Phase phase = Phase::Draft;
	// For each unit that is still in the army after the last battle: the slot it fought in.
	std::vector<size_t> lastSlots;
	// A unit that took a mutation after the previous wave rests for one wave.
	bool restRule = true;

	bool resting(size_t slot) const
	{
		return restRule && !history.empty() && !armyReward(history.back()) && recipients.back() == stack(slot).identity;
	}

	bool allResting() const
	{
		for(size_t slot = 0; slot < rosterSize(); ++slot)
			if(!resting(slot)) return false;
		return true;
	}

	bool boss() const { return wave % 5 == 0; }

	void begin(int type, const std::array<int, 4> & stats, int creature, int amount)
	{
		if(phase != Phase::Draft || type < 0 || creature < 0 || amount <= 0)
			throw std::logic_error("Invalid starting army");
		hero = type;
		primary = stats;
		army.creature = creature;
		army.count = amount;
		phase = Phase::Battle;
	}

	size_t rosterSize() const { return 1 + companions.size(); }
	const Lineage & stack(size_t slot) const { return slot ? companions.at(slot-1) : army; }
	Lineage & stack(size_t slot) { return slot ? companions.at(slot-1) : army; }

	void recruit(int creature, int count)
	{
		if(phase != Phase::Battle || !offersRecruitment() || creature < 0 || count <= 0)
			throw std::logic_error("Invalid recruitment");
		for(size_t i = 0; i < rosterSize(); ++i)
			if(stack(i).creature == creature) throw std::logic_error("Duplicate creature");
		Lineage line;
		if(nextIdentity==UINT32_MAX) throw std::overflow_error("Lineage identifier exhausted");
		line.identity=nextIdentity++;
		line.creature = creature;
		line.count = count;
		companions.push_back(line);
		lastRecruitmentWave = wave;
	}

	unsigned rank(const std::string & id, size_t slot = 0) const
	{
		const auto & mutations = stack(slot).mutations;
		auto it = mutations.find(id);
		return it == mutations.end() ? 0 : it->second;
	}

	bool hasCombination(const std::vector<std::string> & required, size_t slot = 0) const
	{
		return !required.empty() && std::all_of(required.begin(), required.end(),
			[this, slot](const std::string & id) { return rank(id, slot) != 0; });
	}

	bool smallest(size_t slot) const
	{
		if(rosterSize() < 2) return false;
		for(size_t other = 0; other < rosterSize(); ++other)
			if(stack(other).count < stack(slot).count) return false;
		return true;
	}

	Amount lineageDamage(size_t slot = 0) const
	{
		return stack(slot).harvest / 10 + stack(slot).count / 15 * rank("mass", slot) + (smallest(slot) ? 4u : 0u) * Amount(rank("underdog", slot));
	}

	// The battle callback reports actual surviving units (including permanent soul steals).
	// Recruitment supplies an army floor only on victory; defeat can never be converted into a win.
	void finish(bool won, const Amount & survivors, const Amount & casualties, std::vector<std::string> pool, const std::map<std::string, unsigned> & rarities = {}, bool elite = false, const std::vector<int> & companionSurvivors = {}, const std::array<int,7> & trophies = {})
	{
		if(phase != Phase::Battle)
			throw std::logic_error("Battle already settled");
		if(companionSurvivors.size() != companions.size() || std::any_of(companionSurvivors.begin(), companionSurvivors.end(), [](int n){ return n < 0; }) || survivors < 0 || casualties < 0)
			throw std::invalid_argument("Negative battle report");
		std::sort(pool.begin(), pool.end());
		pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
		if(won && pool.size() < 3)
			throw std::invalid_argument("Reward pool needs at least three distinct mutations");
		std::array<std::vector<std::string>, 4> groups;
		for(const auto & id : pool)
			groups.at(rarities.empty() ? 0 : rarities.at(id)).push_back(id);
		kills += casualties;
		if(!won)
		{
			phase = Phase::Defeat;
			return;
		}
		std::vector<Amount> remaining{survivors};
		remaining.insert(remaining.end(), companionSurvivors.begin(), companionSurvivors.end());
		std::vector<Amount> recruits(rosterSize()), growth(rosterSize());
		// Use one end-of-battle snapshot. Recruits cannot recursively produce recruits.
		for(size_t i = 0; i < rosterSize(); ++i)
		{
			Amount change = remaining[i] - stack(i).count;
			growth[i] += casualties * rank("harvest", i);
			if(change >= 0) recruits[i] += remaining[i] / 12 * rank("seedbank",i);
			if(remaining[i] == 0) recruits[i] += stack(i).count / 4 * rank("remnant",i);
			if(boss() || elite) recruits[i] += remaining[i] / 4 * rank("spoils",i);
			if(change > 0) recruits[i] += change * rank("incubator",i);
			if(remaining[i] * 2 <= stack(i).count) growth[i] -= change * 2 * rank("molting",i);
			Amount tithe = rank("tithe",i) && rosterSize() > 1 ? Amount(remaining[i] / 10) : Amount(0);
			recruits[i] -= tithe;
			for(size_t ally = 0; ally < rosterSize(); ++ally)
			{
				if(ally == i) continue;
				growth[ally] += tithe * 5 * rank("tithe",i);
				if(change < 0) recruits[ally] += -change / 6 * rank("graft",i);
				recruits[ally] += (remaining[i] / 8) * rank("nursery", i);
				if(change < 0) recruits[ally] += (-change / 3) * rank("ossuary", ally);
				if(change > 0) growth[ally] += change * 5 * rank("kinship", i);
			}
		}
		for(size_t i = 0; i < rosterSize(); ++i)
		{
			stack(i).harvest += growth[i];
			stack(i).borrowedTrait=trophies.at(i);
			if(remaining[i] == 0)
			{
				// A unit destroyed to the last fighter is gone for the rest of the run.
				stack(i).count = 0;
				continue;
			}
			stack(i).count = remaining[i] + recruits[i];
			Amount reinforcement = i==0 ? Amount(5+wave*2) : Amount(1+wave/(i+1));
			if(claimed("reward.edict.vanguard"))
				reinforcement = i==0 ? Amount(reinforcement*2) : Amount((reinforcement+1)/2);
			if(claimed("reward.edict.supply")) reinforcement+=1+wave/2;
			stack(i).count+=reinforcement;
		}
		std::vector<Lineage> standing;
		lastSlots.clear();
		for(size_t i = 0; i < rosterSize(); ++i)
			if(stack(i).count > 0)
			{
				standing.push_back(stack(i));
				lastSlots.push_back(i);
			}
		if(standing.empty())
		{
			// The battle was won by summoned bodies alone: nobody is left to continue.
			phase = Phase::Defeat;
			return;
		}
		army = standing.front();
		companions.assign(standing.begin()+1, standing.end());
		for(auto it = alternateForms.begin(); it != alternateForms.end();)
		{
			bool owned = false;
			for(size_t i = 0; i < rosterSize(); ++i) owned |= stack(i).identity == it->first;
			it = owned ? std::next(it) : alternateForms.erase(it);
		}
		offers.clear();
		for(int slot = 0; slot < 3; ++slot)
		{
			const std::array<int, 4> chances = boss() ? std::array<int,4>{45, 32, 18, 5} : std::array<int,4>{62, 26, 10, 2};
			std::array<int,4> weights = chances;
			for(size_t tier = 0; tier < groups.size(); ++tier)
				if(groups[tier].empty()) weights[tier] = 0;
			if(elite && slot == 0 && (!groups[2].empty() || !groups[3].empty()))
				weights[0] = weights[1] = 0;
			const auto tier = std::discrete_distribution<int>(weights.begin(), weights.end())(random);
			auto & group = groups[tier];
			auto index = std::uniform_int_distribution<size_t>(0, group.size()-1)(random);
			offers.push_back(group[index]);
			group.erase(group.begin()+index);
		}
		phase = Phase::Reward;
	}

	static bool armyReward(const std::string & id) {return id.rfind("reward.",0)==0;}
	bool claimed(const std::string & id) const {return std::find(history.begin(),history.end(),id)!=history.end();}

	void offerArmyReward(std::vector<std::string> extras)
	{
		if(phase!=Phase::Reward || offers.size()!=3) return;
		extras.erase(std::remove_if(extras.begin(),extras.end(),[&](const std::string & id){
			return !armyReward(id) || (id.rfind("reward.stat.",0)!=0 && claimed(id));
		}),extras.end());
		if(allResting() && extras.size() >= 3)
		{
			// Nobody may take a mutation this wave, so all three cards are rewards for the whole army.
			for(auto & offer : offers)
			{
				const auto index = std::uniform_int_distribution<size_t>(0,extras.size()-1)(random);
				offer = extras[index];
				extras.erase(extras.begin()+index);
			}
			return;
		}
		if(extras.empty() || (wave%3!=0 && std::uniform_int_distribution<int>(0,2)(random)!=0)) return;
		offers.back()=extras[std::uniform_int_distribution<size_t>(0,extras.size()-1)(random)];
	}

	void takeBossLaw(size_t slot,const std::string & law)
	{
		if(phase != Phase::Reward || !boss() || !rank("trait88",slot) || law.rfind("law.",0) != 0 || rank(law,slot))
			throw std::logic_error("Этому отряду недоступен закон босса");
		offers.at(0)=law;
		pick(0,slot);
	}

	void pick(size_t index, size_t slot = 0, bool alternateForm = false)
	{
		if(phase != Phase::Reward || index >= offers.size())
			throw std::logic_error("Invalid reward selection");
		const auto & id=offers[index];
		auto & line=stack(slot);
		if(armyReward(id))
		{
			if(alternateForm) throw std::logic_error("Invalid army reward form");
			if(id.rfind("reward.stat.",0)==0)
			{
				if(id.size()!=13 || id.back()<'0' || id.back()>'3') throw std::logic_error("Unknown primary skill");
				auto & stat=primary.at(id.back()-'0');
				if(stat>INT32_MAX-2) throw std::overflow_error("Hero stat exceeds engine range");
				stat+=2;
			}
		}
		else
		{
			if(resting(slot)) throw std::logic_error("Этот отряд получил мутацию после прошлой волны и сейчас отдыхает");
			const auto level=rank(id,slot);
			if(level==std::numeric_limits<unsigned>::max()) throw std::overflow_error("Mutation rank exceeds unsigned range");
			if(alternateForm && level!=1) throw std::logic_error("Evolution needs rank two");
			if(alternateForm) alternateForms.emplace(line.identity,id);
			gain(line,id);
		}
		history.push_back(offers[index]);
		recipients.push_back(line.identity);
		offers.clear();
		++wave;
		seasons(wave);
		phase = Phase::Battle;
	}

	// Mutations that reshape other mutations follow fixed rules, so that replaying the
	// ordered draft of a save file rebuilds exactly the same ranks.
	static void raiseWeakest(Lineage & line,const std::string & first,const std::string & second = {})
	{
		std::string weakest;
		for(const auto & entry : line.mutations)
			if(entry.first != first && entry.first != second && entry.first.rfind("law.",0) != 0
				&& entry.second < std::numeric_limits<unsigned>::max()
				&& (weakest.empty() || entry.second < line.mutations.at(weakest))) weakest = entry.first;
		if(!weakest.empty()) ++line.mutations[weakest];
	}

	// Biological Singularity: a mutation that is new to the unit also raises its weakest other mutation.
	static void gain(Lineage & line,const std::string & id)
	{
		const bool fresh = !line.mutations.count(id);
		++line.mutations[id];
		if(fresh && id != "trait92" && line.mutations.count("trait92")) raiseWeakest(line,id,"trait92");
	}

	// Mutation Rebirth: on every third wave the weakest other mutation of the bearer gains a rank.
	void seasons(const Amount & reached)
	{
		if(reached < 4 || (reached - 1) % 3 != 0) return;
		for(size_t slot = 0; slot < rosterSize(); ++slot)
			if(rank("trait83",slot)) raiseWeakest(stack(slot),"trait83");
	}

	// Lasting growth earned in the battle that was just won. `upgrade` is the improved
	// creature of the unit, or a negative number when it has none.
	void grow(size_t slot,int alliesLost,bool leaped,int upgrade)
	{
		if(phase != Phase::Reward || alliesLost < 0) throw std::logic_error("Growth belongs to a won battle");
		auto & line = stack(slot);
		line.harvest += Amount(alliesLost) * 10 * rank("trait80",slot);
		line.harvest += Amount(10) * rank("trait85",slot);
		if((leaped && rank("trait70",slot)) || (boss() && rank("trait91",slot)))
		{
			bool taken = upgrade < 0;
			for(size_t other = 0; other < rosterSize(); ++other)
				taken |= stack(other).creature == upgrade;
			if(taken) line.harvest += 50;
			else line.creature = upgrade;
		}
	}

	// What a unit keeps from the battle that was just won. `property` is the index of an absorbed
	// creature property or negative, `taken` the mutations of destroyed enemies, `absorbed` the
	// surviving fighters of the unit's own copies and descendants.
	void keep(size_t slot,int property,bool adapted,const std::array<int,3> & taken,int absorbed)
	{
		if(phase != Phase::Reward || absorbed < 0) throw std::logic_error("Keepsakes belong to a won battle");
		auto & line = stack(slot);
		for(auto it = line.gifts.begin(); it != line.gifts.end();)
		{
			if(it->first.rfind("arsenal.",0) == 0 && --it->second <= 0) it = line.gifts.erase(it);
			else ++it;
		}
		if(rank("trait96",slot) && property >= 0 && property < 6) line.gifts["prop." + std::to_string(property)] = 1;
		if(rank("trait98",slot) && adapted && line.gifts["adapt"] < 15) ++line.gifts["adapt"];
		if(rank("trait76",slot))
			for(int trait : taken)
				if(trait > 0) line.gifts["arsenal." + std::to_string(trait)] = 3;
		if(rank("trait86",slot) && line.mutations.size() >= 6) line.count += absorbed;
		if(line.gifts.count("adapt") && !line.gifts.at("adapt")) line.gifts.erase("adapt");
	}

	// World Rebirth: every defeated boss raises all four hero statistics.
	void rebirth()
	{
		if(phase != Phase::Reward || !boss()) return;
		Amount ranks = 0;
		for(size_t slot = 0; slot < rosterSize(); ++slot) ranks += rank("trait99",slot);
		for(auto & stat : primary)
			stat = engineInt(std::min<Amount>(Amount(stat) + ranks, std::numeric_limits<int>::max()));
	}

	// A battle checkpoint holds the RNG immediately before enemy generation.
	// Ranks are reconstructed from the ordered draft so they cannot disagree.
	void save(std::ostream & out) const
	{
		if(phase != Phase::Battle && phase != Phase::Reward)
			throw std::logic_error("No active run");
		out << "EA6 " << seed << ' ' << (phase == Phase::Reward) << ' ' << wave << ' ' << kills
			<< ' ' << hero << ' ' << army.creature << ' ' << army.count << ' ' << army.harvest << '\n';
		for(auto value : primary) out << value << ' ';
		out << '\n' << random << '\n';
		for(const auto * list : {&history, &offers, &synergies})
		{
			out << list->size() << '\n';
			for(const auto & item : *list) out << std::quoted(item) << '\n';
		}
		out << companions.size() << '\n';
		for(const auto & line : companions)
			out << line.creature << ' ' << line.count << ' ' << line.harvest << '\n';
		out << nextIdentity << '\n';
		for(size_t i=0;i<rosterSize();++i) out << stack(i).identity << ' ' << stack(i).borrowedTrait << '\n';
		for(auto identity : recipients) out << identity << ' ';
		out << '\n' << alternateForms.size() << '\n';
		for(const auto & form : alternateForms)
			out << form.first << ' ' << std::quoted(form.second) << '\n';
		for(size_t i=0;i<rosterSize();++i)
		{
			out << stack(i).gifts.size() << '\n';
			for(const auto & gift : stack(i).gifts) out << std::quoted(gift.first) << ' ' << gift.second << '\n';
		}
		if(!out) throw std::runtime_error("Не удалось записать забег");
	}

	static RoguelikeRun restore(std::istream & in)
	{
		const std::runtime_error invalid("Сохранение забега повреждено");
		std::string version;
		RoguelikeRun result(0);
		bool reward = false;
		if(!(in >> version) || (version != "EA2" && version != "EA3" && version != "EA4" && version != "EA5" && version != "EA6") || !(in >> result.seed >> reward >> result.wave >> result.kills
			>> result.hero >> result.army.creature >> result.army.count >> result.army.harvest))
			throw invalid;
		if(result.hero < 0 || result.army.creature < 0 || result.wave < 1 || result.kills < 0 || result.army.count < 1 || result.army.harvest < 0)
			throw invalid;
		for(auto & value : result.primary) if(!(in >> value)) throw invalid;
		if(!(in >> result.random)) throw invalid;
		for(auto * list : {&result.history, &result.offers, &result.synergies})
		{
			size_t count;
			if(!(in >> count)) throw invalid;
			for(size_t i = 0; i < count; ++i)
			{
				std::string item;
				if(!(in >> std::quoted(item)) || item.empty()) throw invalid;
				list->push_back(std::move(item));
			}
		}
		if(result.wave != Amount(result.history.size()) + 1 || result.offers.size() != (reward ? 3u : 0u)) throw invalid;
		if(version != "EA2")
		{
			size_t count;
			if(!(in >> count) || count > 6) throw invalid;
			for(size_t i = 0; i < count; ++i)
			{
				Lineage line;
				if(!(in >> line.creature >> line.count >> line.harvest) || line.creature < 0 || line.count < 1 || line.harvest < 0) throw invalid;
				for(size_t j = 0; j < result.rosterSize(); ++j)
					if(result.stack(j).creature == line.creature) throw invalid;
				result.companions.push_back(line);
			}
		}
		const bool identified = version=="EA5" || version=="EA6";
		if(identified)
		{
			if(!(in>>result.nextIdentity) || result.nextIdentity<2) throw invalid;
			std::set<uint32_t> ids;
			for(size_t i=0;i<result.rosterSize();++i)
			{
				auto & line=result.stack(i);
				if(!(in>>line.identity>>line.borrowedTrait) || !line.identity || line.identity>=result.nextIdentity
					|| line.borrowedTrait<0 || !ids.insert(line.identity).second) throw invalid;
			}
		}
		else
		{
			for(size_t i=0;i<result.rosterSize();++i) result.stack(i).identity=static_cast<uint32_t>(i+1);
			result.nextIdentity=static_cast<uint32_t>(result.rosterSize()+1);
		}
		// The owner of a past reward may have been destroyed since. Its slot is then `gone`.
		const size_t gone = std::numeric_limits<size_t>::max();
		size_t encoded=0;
		auto readOwner=[&](){
			encoded=0;
			if(version!="EA2" && !(in>>encoded)) throw invalid;
			for(size_t slot=0;slot<result.rosterSize();++slot)
				if(encoded==(identified?result.stack(slot).identity:slot)) return slot;
			if(!identified || !encoded || encoded>=result.nextIdentity) throw invalid;
			return gone;
		};
		Amount replayed = 1;
		for(const auto & id : result.history)
		{
			const auto slot=readOwner();
			result.recipients.push_back(slot==gone ? static_cast<uint32_t>(encoded) : result.stack(slot).identity);
			if(slot!=gone && !armyReward(id)) gain(result.stack(slot),id);
			result.seasons(++replayed);
		}
		if(version == "EA4" || identified)
		{
			size_t count;
			if(!(in >> count) || count > result.history.size()/2) throw invalid;
			for(size_t i=0;i<count;++i)
			{
				const auto slot=readOwner();
				std::string id;
				if(!(in >> std::quoted(id)) || slot==gone || result.rank(id,slot) < 2
					|| !result.alternateForms.emplace(result.stack(slot).identity,id).second) throw invalid;
			}
		}
		if(version == "EA6")
			for(size_t slot=0;slot<result.rosterSize();++slot)
			{
				size_t count;
				if(!(in >> count) || count > 64) throw invalid;
				for(size_t i=0;i<count;++i)
				{
					std::string key;
					int value;
					if(!(in >> std::quoted(key) >> value) || key.empty() || value < 1 || !result.stack(slot).gifts.emplace(key,value).second) throw invalid;
				}
			}
		result.phase = reward ? Phase::Reward : Phase::Battle;
		return result;
	}

	// The first ten waves ramp up from half strength so that a new army can find its build.
	static Amount introduction(const Amount & wave)
	{
		// Waves 1-5 start gently (50% to 70%), full strength comes on wave 8,
		// and every later wave adds another tenth on top of the growing budget.
		if(wave <= 5) return Amount(45 + 5 * wave);
		if(wave <= 8) return Amount(20 + 10 * wave);
		return Amount(100 + 10 * (wave - 8));
	}

	Amount threat() const
	{
		// AI value budget, not unit count. After the introductory waves the quadratic term dominates.
		return (600 + 250 * wave + 48 * wave * wave) * (boss() ? 3 : 2) / 2 * introduction(wave) / 100;
	}

	// Field hospital: share of a surviving unit's casualties that returns after a victory.
	static int hospitalShare(const Amount & wave)
	{
		return wave <= 5 ? 50 : wave <= 10 ? 35 : 25;
	}

	// A destroyed unit has nobody left to carry the wounded, and growth is never reduced.
	static Amount afterHospital(const Amount & before,const Amount & survivors,const Amount & wave)
	{
		if(survivors <= 0 || survivors >= before) return survivors;
		return survivors + (before - survivors) * hospitalShare(wave) / 100;
	}
};
}
