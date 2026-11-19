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
	int mutatedStacks;
	int mutationRank;
	bool pair;
	bool recipe;
	bool anomaly;
};

inline Encounter encounter(const Amount & wave, unsigned seed, size_t bosses)
{
	if(!bosses || wave < 1) throw std::invalid_argument("Invalid wave");
	Encounter plan{};
	plan.bossIndex = wave < 5 ? 0 : ((wave / 5 - 1) % bosses).convert_to<size_t>();
	plan.mutatedStacks = wave >= 56 ? 4 : wave >= 36 ? 3 : wave >= 16 ? 2 : wave >= 6 ? 1 : 0;
	plan.mutationRank = wave >= 26 ? engineInt(2 + (wave-26)/40) : 1;
	plan.pair = wave >= 16;
	plan.recipe = wave >= 36;
	plan.anomaly = wave >= 12 && wave % 5 != 0 && seed % (wave >= 46 ? 3u : 6u) == 0;
	return plan;
}

struct Lineage
{
	int creature = -1;
	Amount count = 0;
	std::map<std::string, unsigned> mutations;
	Amount harvest = 0;
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

	Lineage army;
	std::vector<Lineage> companions;
	std::vector<size_t> recipients;
	std::set<std::pair<size_t,std::string>> alternateForms;
	bool alternate(size_t slot,const std::string & id) const
	{
		return alternateForms.count({slot,id}) != 0;
	}
	std::vector<std::string> history;
	std::vector<std::string> synergies;
	std::vector<std::string> offers;
	Phase phase = Phase::Draft;

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
	void finish(bool won, const Amount & survivors, const Amount & casualties, std::vector<std::string> pool, const std::map<std::string, unsigned> & rarities = {}, bool elite = false, const std::vector<int> & companionSurvivors = {})
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
			stack(i).count = remaining[i] + recruits[i];
			if(i == 0) stack(i).count += 5 + wave * 2;
			else stack(i).count += 1 + wave / (i + 1);
		}
		offers.clear();
		for(int slot = 0; slot < 3; ++slot)
		{
			const std::array<int, 4> chances = boss() ? std::array<int,4>{35, 37, 23, 5} : std::array<int,4>{53, 32, 13, 2};
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
		auto & level = stack(slot).mutations[offers[index]];
		if(level == std::numeric_limits<unsigned>::max())
			throw std::overflow_error("Mutation rank exceeds unsigned range");
		if(alternateForm && level != 1) throw std::logic_error("Evolution needs rank two");
		if(alternateForm) alternateForms.emplace(slot,offers[index]);
		++level;
		history.push_back(offers[index]);
		recipients.push_back(slot);
		offers.clear();
		++wave;
		phase = Phase::Battle;
	}

	// A battle checkpoint holds the RNG immediately before enemy generation.
	// Ranks are reconstructed from the ordered draft so they cannot disagree.
	void save(std::ostream & out) const
	{
		if(phase != Phase::Battle && phase != Phase::Reward)
			throw std::logic_error("No active run");
		out << "EA4 " << seed << ' ' << (phase == Phase::Reward) << ' ' << wave << ' ' << kills
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
		for(auto slot : recipients) out << slot << ' ';
		out << '\n' << alternateForms.size() << '\n';
		for(const auto & form : alternateForms)
			out << form.first << ' ' << std::quoted(form.second) << '\n';
		if(!out) throw std::runtime_error("Не удалось записать забег");
	}

	static RoguelikeRun restore(std::istream & in)
	{
		const std::runtime_error invalid("Сохранение забега повреждено");
		std::string version;
		RoguelikeRun result(0);
		bool reward = false;
		if(!(in >> version) || (version != "EA2" && version != "EA3" && version != "EA4") || !(in >> result.seed >> reward >> result.wave >> result.kills
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
		for(const auto & id : result.history)
		{
			size_t slot = 0;
			if(version != "EA2" && (!(in >> slot) || slot >= result.rosterSize())) throw invalid;
			result.recipients.push_back(slot);
			++result.stack(slot).mutations[id];
		}
		if(version == "EA4")
		{
			size_t count;
			if(!(in >> count) || count > result.history.size()/2) throw invalid;
			for(size_t i=0;i<count;++i)
			{
				size_t slot;
				std::string id;
				if(!(in >> slot >> std::quoted(id)) || slot >= result.rosterSize() || result.rank(id,slot) < 2
					|| !result.alternateForms.emplace(slot,id).second) throw invalid;
			}
		}
		result.phase = reward ? Phase::Reward : Phase::Battle;
		return result;
	}

	Amount threat() const
	{
		// AI value budget, not unit count. After the introductory waves the quadratic term dominates.
		return (600 + 230 * wave + 40 * wave * wave) * (boss() ? 3 : 2) / 2;
	}
};
}
