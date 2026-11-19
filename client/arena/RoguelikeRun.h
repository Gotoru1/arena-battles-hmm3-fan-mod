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
	Lineage army;
	std::vector<Lineage> companions;
	std::vector<size_t> recipients;
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
		if(phase != Phase::Battle || rosterSize() >= (wave >= 4 ? 3u : 2u) || creature < 0 || count <= 0)
			throw std::logic_error("Invalid recruitment");
		for(size_t i = 0; i < rosterSize(); ++i)
			if(stack(i).creature == creature) throw std::logic_error("Duplicate creature");
		Lineage line;
		line.creature = creature;
		line.count = count;
		companions.push_back(line);
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

	Amount lineageDamage(size_t slot = 0) const
	{
		return stack(slot).harvest / 10 + stack(slot).count / 15 * rank("mass", slot);
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
		army.harvest += casualties * rank("harvest");
		army.count = survivors + 5 + wave * 2;
		for(size_t i = 0; i < companions.size(); ++i)
		{
			companions[i].count = wave / (i + 2) + 1 + companionSurvivors[i];
			companions[i].harvest += casualties * rank("harvest", i+1);
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

	void pick(size_t index, size_t slot = 0)
	{
		if(phase != Phase::Reward || index >= offers.size())
			throw std::logic_error("Invalid reward selection");
		auto & level = stack(slot).mutations[offers[index]];
		if(level == std::numeric_limits<unsigned>::max())
			throw std::overflow_error("Mutation rank exceeds unsigned range");
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
		out << "EA3 " << seed << ' ' << (phase == Phase::Reward) << ' ' << wave << ' ' << kills
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
		out << '\n';
		if(!out) throw std::runtime_error("Не удалось записать забег");
	}

	static RoguelikeRun restore(std::istream & in)
	{
		const std::runtime_error invalid("Сохранение забега повреждено");
		std::string version;
		RoguelikeRun result(0);
		bool reward = false;
		if(!(in >> version) || (version != "EA2" && version != "EA3") || !(in >> result.seed >> reward >> result.wave >> result.kills
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
		if(version == "EA3")
		{
			size_t count;
			if(!(in >> count) || count > 2) throw invalid;
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
			if(version == "EA3" && (!(in >> slot) || slot >= result.rosterSize())) throw invalid;
			result.recipients.push_back(slot);
			++result.stack(slot).mutations[id];
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
