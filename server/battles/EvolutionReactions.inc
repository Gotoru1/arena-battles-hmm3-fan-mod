// SPDX-License-Identifier: GPL-2.0-or-later
// Included by BattleActionProcessor.cpp. All changes go through VCMI's battle packets.
namespace
{
class EvolutionReactions
{
	CGameHandler & server;
	const CBattleInfoCallback & battle;
	using Rules = std::map<int,int>;

	static Rules rules(const CStack * unit, bool temporaryOnly = false)
	{
		Rules result;
		auto markers = unit->getBonusesOfType(BonusType::EVOLUTION_TRAIT);
		for(const auto & marker : *markers)
		{
			if(temporaryOnly && marker->duration != BonusDuration::ONE_BATTLE) continue;
			int key = marker->val % 1024;
			result[key] = std::max(result[key], 1 + marker->val / 1024);
		}
		return result;
	}

	std::vector<const CStack *> living() const
	{
		std::vector<const CStack *> units;
		for(const auto * unit : battle.battleGetAllStacks())
			if(unit->alive() && unit->getPosition().isValid()) units.push_back(unit);
		std::sort(units.begin(), units.end(), [](auto * a, auto * b){ return a->unitId() < b->unitId(); });
		return units;
	}

	const CStack * nearest(const CStack * from, BattleSide side) const
	{
		const CStack * best = nullptr;
		for(auto * unit : living())
			if(unit != from && unit->unitSide() == side && (!best ||
				BattleHex::getDistance(from->getPosition(), unit->getPosition()) <
				BattleHex::getDistance(from->getPosition(), best->getPosition()))) best = unit;
		return best;
	}

	void grant(const CStack * unit, Bonus bonus)
	{
		bonus.duration = BonusDuration::ONE_BATTLE;
		SetStackEffect packet;
		packet.battleID = battle.getBattle()->getBattleID();
		packet.toAdd.emplace_back(unit->unitId(), std::vector<Bonus>{bonus});
		server.sendAndApply(packet);
	}

	void transfer(const CStack * unit, int id, int rank)
	{
		if(rules(unit)[id] >= rank) return;
		Bonus marker;
		marker.type = BonusType::EVOLUTION_TRAIT;
		marker.val = id + 1024 * (rank-1);
		grant(unit, marker);
	}

	void adapt(const CStack * unit)
	{
		// These traits can activate after a unit enters an ongoing battle.
		std::vector<int> pool{10,18,23,25,27,51};
		auto current = rules(unit);
		pool.erase(std::remove_if(pool.begin(), pool.end(), [&](int id){return current.count(id) != 0;}),pool.end());
		if(!pool.empty()) transfer(unit, pool[server.getRandomGenerator().nextInt(0,static_cast<int>(pool.size())-1)], 1);
	}

	void summon(const CStack * parent, CreatureID species, int count, BattleHex origin, bool clone)
	{
		auto hex = battle.getAvailableHex(species,parent->unitSide(),origin.toInt());
		if(!hex.isValid()) return;
		battle::UnitInfo info;
		info.id = battle.battleNextUnitId();
		info.type = species;
		info.count = count;
		info.side = parent->unitSide();
		info.position = hex;
		info.summoned = true;
		BattleUnitsChanged packet;
		packet.battleID = battle.getBattle()->getBattleID();
		packet.changedStacks.emplace_back(info.id,UnitChanges::EOperation::ADD);
		info.save(packet.changedStacks.back().data);
		server.sendAndApply(packet);
		auto * child = battle.battleGetStackByID(info.id);
		if(!child) return;
		const auto inherited = rules(parent);
		if(clone || inherited.count(87))
		{
			for(const auto & rule : inherited) transfer(child,rule.first,rule.second);
			adapt(child);
		}
	}

	void fallen(const CStack * source, const CStack * corpse)
	{
		auto beforeSummons = living();
		for(int id : {10,25})
		{
			const auto all = rules(corpse);
			if(!all.count(id)) continue;
			auto * heir = nearest(corpse,corpse->unitSide());
			if(!heir) continue;
			for(auto entry : rules(corpse,id == 25))
				if(entry.first != id && rules(heir)[entry.first] < entry.second)
				{
					transfer(heir,entry.first,entry.second);
					break;
				}
		}

		if(source && source->alive() && source->unitSide() != corpse->unitSide() && rules(source).count(27))
		{
			for(auto trait : {BonusType::FLYING, BonusType::BLOCKS_RETALIATION, BonusType::TWO_HEX_ATTACK_BREATH,
				BonusType::ATTACKS_ALL_ADJACENT, BonusType::NO_MELEE_PENALTY, BonusType::NO_DISTANCE_PENALTY})
			{
				if(source->hasBonusOfType(trait)) continue;
				auto traits = corpse->getBonusesOfType(trait);
				if(traits->empty()) continue;
				grant(source,**traits->begin());
				break;
			}
		}
		for(auto * unit : beforeSummons)
			if(rules(unit).count(18) && BattleHex::getDistance(unit->getPosition(),corpse->getPosition()) == 1)
				summon(unit,corpse->creatureId(),1,corpse->getPosition(),false);
	}

public:
	EvolutionReactions(CGameHandler & server, const CBattleInfoCallback & battle) : server(server), battle(battle) {}

	void round(bool first)
	{
		for(auto * unit : living())
		{
			const auto active = rules(unit);
			if(first && active.count(78)) summon(unit,unit->creatureId(),unit->getCount(),unit->getPosition(),true);
			if(active.count(57) && battle.battleGetRound() > 0 && battle.battleGetRound() % 3 == 0) adapt(unit);
		}
	}

	void attack(const CStack * source,const CStack * primary,const BattleAttack & packet)
	{
		const auto active = rules(source);
		// The packet may contain multiple victims. A spark is indirect damage, so it dispatches
		// death reactions but cannot recursively emit another primary attack.
		for(const auto & hit : packet.bsa)
		{
			auto * unit = battle.battleGetStackByID(hit.stackAttacked,false);
			if(unit && hit.killed() && !unit->alive()) fallen(source,unit);
		}
		if(!primary || !source->alive() || primary->unitSide() == source->unitSide()) return;
		if(active.count(23))
		{
			auto * target = nearest(primary,primary->unitSide());
			for(const auto & hit : packet.bsa)
				if(target && hit.stackAttacked == primary->unitId() && hit.damageAmount > 0)
				{
					BattleStackAttacked spark;
					spark.attackerID = source->unitId();
					spark.stackAttacked = target->unitId();
					spark.damageAmount = std::max<int64_t>(1, hit.damageAmount / 3);
					target->prepareAttacked(spark,server.getRandomGenerator());
					StacksInjured damage;
					damage.battleID = battle.getBattle()->getBattleID();
					damage.stacks.push_back(spark);
					server.sendAndApply(damage);
					if(spark.killed() && !target->alive()) fallen(source,target);
					break;
				}
		}
		if(primary->alive() && active.count(51))
		{
			const auto access = battle.getAccessibility(primary);
			auto best = primary->getPosition();
			for(auto hex : primary->getPosition().getNeighbouringTiles())
				if(access.accessible(hex,primary) && (best == primary->getPosition() ||
					BattleHex::getDistance(hex,source->getPosition()) > BattleHex::getDistance(best,source->getPosition()))) best = hex;
			if(best == primary->getPosition()) return;
			BattleStackMoved move;
			move.battleID = battle.getBattle()->getBattleID();
			move.stack = primary->unitId();
			move.tilesToMove.insert(best);
			move.teleporting = true;
			server.sendAndApply(move);
		}
	}
};
}

void BattleActionProcessor::evolutionRound(const CBattleInfoCallback & battle, bool first)
{
	EvolutionReactions(*gameHandler,battle).round(first);
}

void BattleActionProcessor::evolutionAttack(const CBattleInfoCallback & battle,const CStack * source,const CStack * target,const BattleAttack & attack)
{
	EvolutionReactions(*gameHandler,battle).attack(source,target,attack);
}
