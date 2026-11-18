// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
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

	unsigned rank(const std::string & id) const
	{
		auto it = army.mutations.find(id);
		return it == army.mutations.end() ? 0 : it->second;
	}

	bool hasCombination(const std::vector<std::string> & required) const
	{
		return !required.empty() && std::all_of(required.begin(), required.end(),
			[this](const std::string & id) { return rank(id) != 0; });
	}

	Amount lineageDamage() const
	{
		return army.harvest / 10 + army.count / 15 * rank("mass");
	}

	// The battle callback reports actual surviving units (including permanent soul steals).
	// Recruitment supplies an army floor only on victory; defeat can never be converted into a win.
	void finish(bool won, const Amount & survivors, const Amount & casualties, std::vector<std::string> pool)
	{
		if(phase != Phase::Battle)
			throw std::logic_error("Battle already settled");
		if(survivors < 0 || casualties < 0)
			throw std::invalid_argument("Negative battle report");
		std::sort(pool.begin(), pool.end());
		pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
		if(won && pool.size() < 3)
			throw std::invalid_argument("Reward pool needs at least three distinct mutations");
		kills += casualties;
		if(!won)
		{
			phase = Phase::Defeat;
			return;
		}
		army.harvest += casualties * rank("harvest");
		army.count = survivors + 5 + wave * 2;
		std::shuffle(pool.begin(), pool.end(), random);
		offers.assign(pool.begin(), pool.begin() + 3);
		phase = Phase::Reward;
	}

	void pick(size_t index)
	{
		if(phase != Phase::Reward || index >= offers.size())
			throw std::logic_error("Invalid reward selection");
		auto & level = army.mutations[offers[index]];
		if(level == std::numeric_limits<unsigned>::max())
			throw std::overflow_error("Mutation rank exceeds unsigned range");
		++level;
		history.push_back(offers[index]);
		offers.clear();
		++wave;
		phase = Phase::Battle;
	}

	Amount threat() const
	{
		// AI value budget, not unit count. After the introductory waves the quadratic term dominates.
		return (210 + 75 * wave + 8 * wave * wave) * (boss() ? 3 : 2) / 2;
	}
};
}
