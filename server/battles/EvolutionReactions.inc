// SPDX-License-Identifier: GPL-2.0-or-later
// Included by BattleActionProcessor.cpp. All changes go through VCMI's battle packets.
namespace evolution_server
{
// Ground marks are real passable battlefield obstacles, so both players see them.
// spellLevel holds the mutation id, casterSpellPower the unit that left the mark.
constexpr int groundTag=500900;
struct Ground { int id; BattleHex hex; int kind; int owner; BattleSide side; };
static std::vector<Ground> groundMarks(const CBattleInfoCallback & battle)
{
	std::vector<Ground> result;
	for(const auto & obstacle:battle.battleGetAllObstacles())
	{
		const auto * mark=dynamic_cast<const SpellCreatedObstacle *>(obstacle.get());
		if(mark && mark->minimalDamage==groundTag)
			result.push_back({mark->uniqueID,mark->pos,mark->spellLevel,mark->casterSpellPower,mark->casterSide});
	}
	return result;
}

static bool touches(const CStack * unit,BattleHex place,BattleHex centre,int reach)
{
	for(auto hex:unit->getHexes(place))
		if(BattleHex::getDistance(hex,centre)<=reach) return true;
	return false;
}

class EvolutionReactions
{
	CGameHandler & server;
	const CBattleInfoCallback & battle;
	using Rules = std::map<int,int>;
	evolution::BattleChain chain;
	std::function<void(const CStack *,const CStack *)> strike;
	std::map<uint32_t,uint32_t> origins;
	mutable std::map<std::pair<uint32_t,bool>,Rules> ruleCache;

	uint32_t lineageRoot(uint32_t id) const
	{
		auto it=origins.find(id);
		return it==origins.end()?id:it->second;
	}

	static Rules readRules(const CStack * unit, bool temporaryOnly = false)
	{
		Rules result;
		auto markers = unit->getBonusesOfType(BonusType::EVOLUTION_TRAIT);
		for(const auto & marker : *markers)
		{
			if(temporaryOnly && marker->duration != BonusDuration::ONE_BATTLE) continue;
			const int key=marker->subtype.getNum();
			if(key>0 && marker->val>0) result[key]=std::max(result[key],marker->val);
		}
		const auto muted=unit->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500050));
		for(const auto & mark:*muted) result.erase(mark->val);
		return result;
	}

	Rules rules(const CStack * unit,bool temporaryOnly=false) const
	{
		auto key=std::make_pair(unit->unitId(),temporaryOnly);
		auto it=ruleCache.find(key);
		if(it==ruleCache.end()) it=ruleCache.emplace(key,readRules(unit,temporaryOnly)).first;
		return it->second;
	}

	void invalidate(const CStack * unit)
	{
		ruleCache.erase({unit->unitId(),false});
		ruleCache.erase({unit->unitId(),true});
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
		if(bonus.type==BonusType::EVOLUTION_TRAIT || (bonus.type==BonusType::NONE && bonus.subtype==BonusCustomSubtype(500050))) invalidate(unit);
		bonus.duration = BonusDuration::ONE_BATTLE;
		SetStackEffect packet;
		packet.battleID = battle.getBattle()->getBattleID();
		packet.toAdd.emplace_back(unit->unitId(), std::vector<Bonus>{bonus});
		server.sendAndApply(packet);
	}

	void discovery(const CStack * actor,int combo)
	{
		// Record on an original body so that dismissing an egg or copy cannot erase the discovery.
		const CStack * anchor=actor;
		for(auto * unit:battle.battleGetAllStacks())
			if(unit->unitSide()==actor->unitSide() && unit->base && (!anchor->base || unit->unitId()<anchor->unitId())) anchor=unit;
		const auto key=BonusCustomSubtype(500501);
		auto known=anchor->getBonusesOfType(BonusType::NONE,key);
		if(std::any_of(known->begin(),known->end(),[&](const auto & mark){return mark->val==combo;})) return;
		Bonus mark;
		mark.type=BonusType::NONE;
		mark.subtype=key;
		mark.val=combo;
		grant(anchor,mark);
	}

	bool marked(const CStack * unit,int subtype,int value) const
	{
		const auto marks=unit->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(subtype));
		return std::any_of(marks->begin(),marks->end(),[&](const auto & mark){return mark->val==value;});
	}

	void remember(const CStack * unit,int subtype,int value)
	{
		if(marked(unit,subtype,value)) return;
		Bonus mark;
		mark.type=BonusType::NONE;
		mark.subtype=BonusCustomSubtype(subtype);
		mark.val=value;
		grant(unit,mark);
	}

	void exchange()
	{
		std::set<uint32_t> paired;
		const auto initial=living();
		for(auto * bearer:initial)
		{
			const auto left=rules(bearer);
			if(!left.count(50) || paired.count(bearer->unitId())) continue;
			for(auto * ally:initial)
			{
				if(ally==bearer || ally->unitSide()!=bearer->unitSide() || paired.count(ally->unitId())) continue;
				const auto right=rules(ally);
				auto offered=std::find_if(left.begin(),left.end(),[&](const auto & entry){return entry.first!=50 && entry.first!=88 && !right.count(entry.first);});
				auto returned=std::find_if(right.begin(),right.end(),[&](const auto & entry){return entry.first!=50 && entry.first!=88 && !left.count(entry.first);});
				if(offered==left.end() || returned==right.end()) continue;
				remember(bearer,500050,offered->first);
				remember(ally,500050,returned->first);
				transfer(bearer,returned->first,returned->second);
				transfer(ally,offered->first,offered->second);
				paired.insert(ally->unitId());
				paired.insert(bearer->unitId());
				break;
			}
		}
	}

	void descend(const CStack * parent,const CStack * child)
	{
		origins[child->unitId()]=lineageRoot(parent->unitId());
		auto roots=parent->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500095));
		for(const auto & root:*roots) remember(child,500095,root->val);
		if(rules(parent).count(95)) remember(child,500095,parent->unitId());
		if(rules(parent).count(97))
		{
			remember(child,500097,parent->unitId());
			transfer(child,97,1);
			adapt(child,rules(parent));
		}
	}

	void putTrait(const CStack * unit, int id, int rank)
	{
		if(rules(unit)[id] >= rank) return;
		Bonus marker;
		marker.type = BonusType::EVOLUTION_TRAIT;
		marker.subtype=BonusCustomSubtype(id);
		marker.val=rank;
		grant(unit, marker);
	}

	void transfer(const CStack * unit, int id, int rank)
	{
		const auto before = rules(unit);
		auto owned = before.find(id);
		if(owned != before.end() && owned->second >= rank) return;
		std::vector<const CStack *> receivers{unit};
		auto family = unit->getBonusesOfType(BonusType::EVOLUTION_FAMILY);
		if(!family->empty())
		{
			const int root = (*family->begin())->val;
			std::vector<const CStack *> siblings;
			bool shared = before.count(56) || id == 56;
			for(auto * other : living())
			{
				if(other == unit || other->unitSide() != unit->unitSide()) continue;
				auto marks = other->getBonusesOfType(BonusType::EVOLUTION_FAMILY);
				if(marks->empty() || (*marks->begin())->val != root) continue;
				siblings.push_back(other);
				shared |= rules(other).count(56) != 0;
			}
			if(shared) receivers.insert(receivers.end(),siblings.begin(),siblings.end());
		}
		for(auto * target : receivers)
		{
			auto prior = rules(target);
			if(prior[id] >= rank) continue;
			putTrait(target,id,rank);
			if(target!=unit) discovery(unit,1);
			if(!prior.count(61) || prior.at(61) <= 0 || prior[id] != 0) continue;
			// A rank increase is not an acquisition. It cannot reactivate the catalyst.
			for(auto entry : prior)
				if(entry.first != id && entry.first != 61 && entry.second < INT32_MAX)
				{
					putTrait(target,entry.first,entry.second+1);
					break;
				}
		}
	}

	void adapt(const CStack * unit,const Rules & excluded = {})
	{
		// These traits can activate after a unit enters an ongoing battle.
		std::vector<int> pool{9,10,18,19,23,25,27,51};
		auto current = rules(unit);
		pool.erase(std::remove_if(pool.begin(), pool.end(), [&](int id){return current.count(id) || excluded.count(id);}),pool.end());
		if(!pool.empty()) transfer(unit, pool[server.getRandomGenerator().nextInt(0,static_cast<int>(pool.size())-1)], 1);
	}

	const CStack * spawn(BattleSide side, CreatureID species, int count, BattleHex origin)
	{
		auto hex = battle.getAvailableHex(species,side,origin.toInt());
		if(!hex.isValid()) return nullptr;
		battle::UnitInfo info;
		info.id = battle.battleNextUnitId();
		info.type = species;
		info.count = count;
		info.side = side;
		info.position = hex;
		info.summoned = true;
		BattleUnitsChanged packet;
		packet.battleID = battle.getBattle()->getBattleID();
		packet.changedStacks.emplace_back(info.id,UnitChanges::EOperation::ADD);
		info.save(packet.changedStacks.back().data);
		server.sendAndApply(packet);
		return battle.battleGetStackByID(info.id);
	}

	std::vector<Bonus> spellCharges(const CStack * unit) const
	{
		auto held=unit->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500021));
		std::vector<Bonus> result;
		for(const auto & charge:*held) result.push_back(*charge);
		return result;
	}

	void spend(const CStack * unit,const std::vector<Bonus> & charges)
	{
		if(charges.empty()) return;
		SetStackEffect packet;
		packet.battleID=battle.getBattle()->getBattleID();
		packet.toRemove.emplace_back(unit->unitId(),charges);
		server.sendAndApply(packet);
	}

	const CStack * summon(const CStack * parent, CreatureID species, int count, BattleHex origin, bool clone)
	{
		auto charges=spellCharges(parent);
		const int extra=static_cast<int>(std::min<size_t>(charges.size(),INT32_MAX-count));
		auto * child = spawn(parent->unitSide(),species,count+extra,origin);
		if(!child) return nullptr;
		spend(parent,charges);
		descend(parent,child);
		const auto inherited = rules(parent);
		if(clone)
		{
			auto family = parent->getBonusesOfType(BonusType::EVOLUTION_FAMILY);
			Bonus mark;
			mark.type = BonusType::EVOLUTION_FAMILY;
			mark.val = family->empty() ? int(parent->unitId()) : (*family->begin())->val;
			if(family->empty()) grant(parent,mark);
			grant(child,mark);
		}
		if(clone || inherited.count(87))
		{
			for(const auto & rule : inherited) transfer(child,rule.first,rule.second);
			adapt(child);
		}
		observe(child);
		return child;
	}

	void copyProperty(const CStack * recipient,const CStack * donor)
	{
		for(auto trait : {BonusType::FLYING, BonusType::BLOCKS_RETALIATION, BonusType::TWO_HEX_ATTACK_BREATH,
			BonusType::ATTACKS_ALL_ADJACENT, BonusType::NO_MELEE_PENALTY, BonusType::NO_DISTANCE_PENALTY})
		{
			if(recipient->hasBonusOfType(trait)) continue;
			auto traits = donor->getBonusesOfType(trait);
			if(traits->empty()) continue;
			grant(recipient,**traits->begin());
			break;
		}
	}

	void observe(const CStack * newcomer)
	{
		for(auto * witness : living())
		{
			if(witness->unitSide() == newcomer->unitSide() || !rules(witness).count(67)) continue;
			const int species = newcomer->creatureId().getNum();
			const auto seen = witness->getBonusesOfType(BonusType::EVOLUTION_SEEN);
			if(std::any_of(seen->begin(),seen->end(),[&](const auto & mark){return mark->val == species;})) continue;
			Bonus record;
			record.type = BonusType::EVOLUTION_SEEN;
			record.val = species;
			grant(witness,record);
			copyProperty(witness,newcomer);
		}
	}

	static CreatureID eggType()
	{
		static const CreatureID id(CreatureID::decode("arenabattles:evolutionEgg"));
		return id;
	}

	static CreatureID larvaType()
	{
		static const CreatureID id(CreatureID::decode("arenabattles:evolutionLarva"));
		return id;
	}

	int counter(const CStack * unit,int subtype) const
	{
		const auto stored=unit->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(subtype));
		return stored->empty()?0:(*stored->begin())->val;
	}

	void setCounter(const CStack * unit,int subtype,int value)
	{
		const auto stored=unit->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(subtype));
		std::vector<Bonus> old;
		for(const auto & mark:*stored)old.push_back(*mark);
		spend(unit,old);
		if(value) remember(unit,subtype,value);
	}

	// VCMI serializes each bonus value as a signed 32-bit integer.
	// Three nonnegative limbs preserve the full native damage range across packets.
	int64_t reserve(const CStack * unit,int key=500247) const
	{
		int64_t amount = 0;
		for(int i=0; i<3; ++i)
			amount |= int64_t(counter(unit,key+i)) << (30*i);
		return amount;
	}

	void setReserve(const CStack * unit,int64_t amount,int key=500247)
	{
		for(int i=0; i<3; ++i)
		{
			setCounter(unit,key+i,static_cast<int>(amount & 0x3fffffff));
			amount >>= 30;
		}
	}

	static int64_t combineDamage(int64_t amount,int64_t extra)
	{
		return amount + std::min(extra,INT64_MAX-amount);
	}

	void lostLarvae(const CStack * larva,uint32_t count)
	{
		if(!count) return;
		const auto mothers=larva->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500075));
		for(const auto & mother:*mothers)
			if(const auto * queen=battle.battleGetStackByID(mother->val,false))
				setCounter(queen,500175,static_cast<int>(std::min<int64_t>(INT32_MAX-1,int64_t(counter(queen,500175))+count)));
	}

	bool lay(const CStack * parent, BattleHex hex)
	{
		auto * egg = spawn(parent->unitSide(),eggType(),1,hex);
		if(!egg) return false;
		origins[egg->unitId()]=lineageRoot(parent->unitId());
		Bonus source;
		source.type = BonusType::EVOLUTION_BROOD;
		source.val = static_cast<int>(parent->unitId());
		grant(egg,source);
		if(const auto stored=counter(parent,500175))
		{
			setCounter(egg,500275,stored);
			setCounter(parent,500175,0);
		}
		return true;
	}

	bool hatch(const CStack * egg)
	{
		auto brood = egg->getBonusesOfType(BonusType::EVOLUTION_BROOD);
		if(brood->empty()) return false;
		const auto parentId = (*brood->begin())->val;
		const auto side = egg->unitSide();
		const auto hex = egg->getPosition();
		const auto id = egg->unitId();
		const int clutch=1+counter(egg,500275);
		BattleUnitsChanged remove;
		remove.battleID = battle.getBattle()->getBattleID();
		remove.changedStacks.emplace_back(id,UnitChanges::EOperation::REMOVE);
		server.sendAndApply(remove);
		auto * parent = battle.battleGetStackByID(parentId,false);
		if(parent)
		{
			const auto * larva=summon(parent,larvaType(),clutch,hex,false);
			if(larva && rules(parent).count(75))
			{
				remember(larva,500075,parent->unitId());
				for(auto trait:rules(parent))
					if(trait.first!=75)
					{
						transfer(larva,trait.first,trait.second);
						break;
					}
			}
			if(rules(parent).count(60) && lay(parent,hex)) discovery(parent,2);
		}
		else spawn(side,larvaType(),clutch,hex);
		return true;
	}

	void recycle(const CStack * carrier, uint32_t casualties)
	{
		// Eight bodies yield one skeleton. A blocked summon stays banked in battle bonuses.
		const auto key = BonusCustomSubtype(500032);
		auto saved = carrier->getBonusesOfType(BonusType::NONE,key);
		uint64_t total = casualties;
		std::vector<Bonus> old;
		for(const auto & mark : *saved)
		{
			total += static_cast<uint32_t>(mark->val);
			old.push_back(*mark);
		}
		if(!casualties && total < 8) return;
		const int bones = static_cast<int>(std::min<uint64_t>(total/8,INT32_MAX));
		if(bones)
		{
			static const CreatureID skeleton(CreatureID::decode("core:skeleton"));
			if(auto * child = spawn(carrier->unitSide(),skeleton,bones,carrier->getPosition()))
			{
				descend(carrier,child);
				total -= uint64_t(bones)*8;
				if(rules(carrier).count(87))
				{
					for(auto entry : rules(carrier)) transfer(child,entry.first,entry.second);
					adapt(child);
					discovery(carrier,3);
				}
				observe(child);
			}
			else if(!casualties) return;
		}
		SetStackEffect packet;
		packet.battleID = battle.getBattle()->getBattleID();
		if(!old.empty()) packet.toRemove.emplace_back(carrier->unitId(),old);
		std::vector<Bonus> reserve;
		while(total)
		{
			Bonus mark;
			mark.duration=BonusDuration::ONE_BATTLE;
			mark.type=BonusType::NONE;
			mark.subtype=key;
			mark.val=static_cast<int>(std::min<uint64_t>(total,INT32_MAX));
			total-=mark.val;
			reserve.push_back(mark);
		}
		if(!reserve.empty()) packet.toAdd.emplace_back(carrier->unitId(),reserve);
		if(!old.empty() || !reserve.empty()) server.sendAndApply(packet);
	}

	void split(const CStack * unit)
	{
		if(!unit || !unit->alive() || unit->getCount()<2 || int64_t(unit->getCount())*2>unit->unitBaseAmount() || !rules(unit).count(3)) return;
		const auto key = BonusCustomSubtype(500003);
		if(!unit->getBonusesOfType(BonusType::NONE,key)->empty()) return;
		const int count = unit->getCount()/2;
		auto * child=spawn(unit->unitSide(),unit->creatureId(),count,unit->getPosition());
		if(!child) return;
		descend(unit,child);
		// Reserve full individuals for the new body. The wounded front individual stays here.
		const int temporary=std::min(count,unit->health.getResurrected());
		auto state=unit->acquireState();
		auto health=int64_t(count)*unit->getMaxHealth();
		state->damage(health);
		BattleUnitsChanged packet;
		packet.battleID=battle.getBattle()->getBattleID();
		packet.changedStacks.emplace_back(unit->unitId(),UnitChanges::EOperation::RESET_STATE);
		state->save(packet.changedStacks.back().data);
		if(temporary)
		{
			auto detached=child->acquireState();
			auto temporaryHP=int64_t(temporary)*child->getMaxHealth();
			detached->damage(temporaryHP);
			detached->heal(temporaryHP,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
			packet.changedStacks.emplace_back(child->unitId(),UnitChanges::EOperation::RESET_STATE);
			detached->save(packet.changedStacks.back().data);
		}
		server.sendAndApply(packet);
		Bonus origin;
		origin.type=BonusType::NONE;
		origin.subtype=key;
		origin.val=static_cast<int>(unit->unitId());
		grant(unit,origin);
		grant(child,origin);
		for(auto trait:rules(unit)) transfer(child,trait.first,trait.second);
		observe(child);
	}

	void casualties(uint32_t count)
	{
		if(!count) return;
		for(auto * carrier : living())
			if(rules(carrier).count(32)) recycle(carrier,count);
	}

	static CreatureID huskType()
	{
		static const CreatureID id(CreatureID::decode("arenabattles:evolutionHusk"));
		return id;
	}

	void shed(const CStack * unit,unsigned losses)
	{
		if(!unit || !unit->alive() || !losses || uint64_t(losses)*4 < uint64_t(unit->unitBaseAmount()) || !rules(unit).count(7) || marked(unit,500007,1)) return;
		const auto old=unit->getPosition();
		const auto access=battle.getAccessibility(unit);
		for(auto hex:old.getNeighbouringTiles())
		{
			if(!access.accessible(hex,unit)) continue;
			// Keep the old front cell free for the shell of a double-wide creature.
			const auto & occupied=unit->getHexes(hex);
			if(std::find(occupied.begin(),occupied.end(),old)!=occupied.end()) continue;
			BattleStackMoved move;
			move.battleID=battle.getBattle()->getBattleID();
			move.stack=unit->unitId();
			move.teleporting=true;
			move.tilesToMove.insert(hex);
			server.sendAndApply(move);
			remember(unit,500007,1);
			spawn(unit->unitSide(),huskType(),1,old);
			return;
		}
	}

	bool awakenHusk(const CStack * shell)
	{
		auto incubating=shell->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500122));
		if(incubating->empty()) return false;
		auto * original=battle.battleGetStackByID((*incubating->begin())->val,false);
		if(!original) return true;
		if(!original->alive())
		{
			for(auto occupied:original->getHexes())
				for(auto * body:living())
					if(body!=shell)
					{
						const auto & cells=body->getHexes();
						if(std::find(cells.begin(),cells.end(),occupied)!=cells.end()) return true;
					}
		}
		BattleUnitsChanged packet;
		packet.battleID=battle.getBattle()->getBattleID();
		packet.changedStacks.emplace_back(shell->unitId(),UnitChanges::EOperation::REMOVE);
		if(!original->alive())
		{
			auto restored=original->acquireState();
			int64_t hp=int64_t(std::max(1,original->unitBaseAmount()/4))*original->getMaxHealth();
			restored->heal(hp,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
			packet.changedStacks.emplace_back(original->unitId(),UnitChanges::EOperation::RESET_STATE);
			packet.changedStacks.back().healthDelta=restored->getAvailableHealth();
			restored->save(packet.changedStacks.back().data);
		}
		server.sendAndApply(packet);
		return true;
	}

	void burst(const CStack * killer,const CStack * corpse)
	{
		bool spawned = false;
		for(auto hex : corpse->getPosition().getNeighbouringTiles())
		{
			if(!hex.isValid() || battle.getAvailableHex(larvaType(),killer->unitSide(),hex.toInt()) != hex) continue;
			if(summon(killer,larvaType(),1,hex,false)) spawned = true;
		}
		if(spawned) return;
		for(auto * neighbour : living())
			if(neighbour->unitSide()!=killer->unitSide() && BattleHex::getDistance(neighbour->getPosition(),corpse->getPosition())==1)
				injure(neighbour,killer,std::max<int64_t>(1,corpse->getMaxHealth()));
	}

	void passGeneration(const CStack * corpse,const Rules & legacy,const std::vector<const CStack *> & survivors)
	{
		for(const auto * child : survivors)
		{
			if(child->unitSide()!=corpse->unitSide() || !marked(child,500097,corpse->unitId())) continue;
			const auto owned = rules(child);
			int best=0, rank=0;
			for(auto entry : legacy)
			{
				auto existing=owned.find(entry.first);
				if(entry.first==97 || (existing!=owned.end() && existing->second>=entry.second)) continue;
				if(entry.second>rank || (entry.second==rank && entry.first>best))
				{
					best=entry.first;
					rank=entry.second;
				}
			}
			if(best) transfer(child,best,rank);
		}
	}

	void fallen(const CStack * source, const CStack * corpse)
	{
		auto beforeSummons = living();
		const auto legacy=rules(corpse);
		if(legacy.count(97)) passGeneration(corpse,legacy,beforeSummons);
		if(source && source->alive() && source->unitSide()!=corpse->unitSide() && rules(source).count(41)
			&& source->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500041))->empty())
		{
			const auto owned=rules(source);
			for(auto trait:legacy)
				if(trait.first!=41 && trait.first!=88 && !owned.count(trait.first))
				{
					remember(source,500041,trait.first);
					break;
				}
		}
		const auto pathogens=corpse->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500062));
		for(const auto & germ:*pathogens)
			if(auto * owner=battle.battleGetStackByID(germ->val,false))
				if(auto * carrier=summon(owner,larvaType(),1,corpse->getPosition(),false))
					remember(carrier,500162,owner->unitId());
		if(legacy.count(22) && !marked(corpse,500022,1))
		{
			if(auto * shell=spawn(corpse->unitSide(),huskType(),1,corpse->getPosition()))
			{
				remember(corpse,500022,1);
				remember(shell,500122,corpse->unitId());
			}
		}
		if(legacy.count(95))
		{
			auto previous=corpse->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500095));
			for(auto * heir:beforeSummons)
			{
				if(heir->unitSide()!=corpse->unitSide()) continue;
				bool related=marked(heir,500095,corpse->unitId());
				for(const auto & root:*previous) related |= marked(heir,500095,root->val);
				if(!related) continue;
				for(auto rule:legacy) transfer(heir,rule.first,rule.second);
				break;
			}
		}
		if(source && source->unitSide()!=corpse->unitSide())
			for(auto * witness:beforeSummons)
				if(witness->unitSide()==corpse->unitSide() && rules(witness).count(8)) remember(witness,500008,source->unitId());
		if(source && source->alive() && source->unitSide()!=corpse->unitSide() && rules(source).count(68))
		{
			static const CreatureID ghost(CreatureID::decode("core:wight"));
			if(auto * hunter=spawn(source->unitSide(),ghost,1,corpse->getPosition()))
			{
				descend(source,hunter);
				transfer(hunter,68,1);
				if(rules(source).count(87))
				{
					for(auto trait:rules(source)) transfer(hunter,trait.first,trait.second);
					adapt(hunter);
				}
				observe(hunter);
				chain.post(evolution::ReactionKind::Hunt,hunter->unitId(),corpse->unitId());
			}
		}
		const auto infection = corpse->getBonusesOfType(BonusType::EVOLUTION_PARASITE);
		if(source && source->unitSide()!=corpse->unitSide() && rules(source).count(54) && !infection->empty())
			burst(source,corpse);
		for(const auto & mark : *infection)
		{
			auto * carrier = battle.battleGetStackByID(mark->val,false);
			if(!carrier || carrier->unitSide()==corpse->unitSide()) continue;
			const auto traits=rules(carrier);
			if(traits.count(89) && !marked(corpse,500089,1))
			{
				if(auto * converted=summon(carrier,corpse->creatureId(),std::max(1,corpse->unitBaseAmount()/2),corpse->getPosition(),false))
				{
					remember(corpse,500089,1);
					copyProperty(converted,corpse);
					if(!legacy.empty()) transfer(converted,legacy.begin()->first,legacy.begin()->second);
				}
			}
			else if(traits.count(39) && !traits.count(89)) lay(carrier,corpse->getPosition());
		}
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
			copyProperty(source,corpse);
		if(source && source->alive() && source->unitSide() != corpse->unitSide() && rules(source).count(24))
			stain(corpse->getPosition(),24,source,-1);
		for(auto * unit : beforeSummons)
			if(rules(unit).count(18) && BattleHex::getDistance(unit->getPosition(),corpse->getPosition()) == 1)
				summon(unit,corpse->creatureId(),1,corpse->getPosition(),false);
	}

	// Negative marks that can travel between bodies. The value of a mark is the unit id of its source.
	// Order: parasite, wound, breeding germ, viral tag, poison, slowing spore.
	struct Ailment { BonusType type; int subtype; };
	static const std::vector<Ailment> & ailments()
	{
		static const std::vector<Ailment> all{{BonusType::EVOLUTION_PARASITE,0},{BonusType::EVOLUTION_WOUND,0},
			{BonusType::NONE,500062},{BonusType::NONE,500026},{BonusType::NONE,500077},{BonusType::NONE,500177}};
		return all;
	}

	std::vector<int> carried(const CStack * unit,size_t kind) const
	{
		const auto & def=ailments()[kind];
		const auto held=def.subtype ? unit->getBonusesOfType(def.type,BonusCustomSubtype(def.subtype)) : unit->getBonusesOfType(def.type);
		std::vector<int> owners;
		for(const auto & mark:*held) owners.push_back(mark->val);
		return owners;
	}

	bool suffers(const CStack * unit,size_t kind,int owner) const
	{
		const auto owners=carried(unit,kind);
		return std::find(owners.begin(),owners.end(),owner)!=owners.end();
	}

	struct Contagion { uint32_t target; size_t kind; int owner; };
	std::deque<Contagion> contagion;
	bool spreading=false;

	// Every new mark is queued. Memory Infection and Domino Law read the queue, and each
	// unit/kind/source triple can be added only once, so a cascade always terminates.
	bool afflict(const CStack * target,size_t kind,int owner)
	{
		if(!target || !target->alive() || suffers(target,kind,owner)) return false;
		const auto & def=ailments()[kind];
		const bool firstSlow=def.subtype==500177 && carried(target,kind).empty();
		Bonus mark;
		mark.type=def.type;
		if(def.subtype) mark.subtype=BonusCustomSubtype(def.subtype);
		mark.val=owner;
		grant(target,mark);
		if(firstSlow)
		{
			// One penalty per body keeps the slowest native creatures mobile.
			Bonus slow;
			slow.type=BonusType::STACKS_SPEED;
			slow.val=-2;
			grant(target,slow);
		}
		contagion.push_back({static_cast<uint32_t>(target->unitId()),kind,owner});
		if(spreading) return true;
		spreading=true;
		try
		{
			while(!contagion.empty())
			{
				const auto event=contagion.front();
				contagion.pop_front();
				resonate(event);
			}
		}
		catch(...) { spreading=false; contagion.clear(); throw; }
		spreading=false;
		return true;
	}

	void resonate(const Contagion & event)
	{
		const auto * host=battle.battleGetStackByID(event.target);
		if(!host) return;
		const CStack * memory=nullptr;
		const CStack * domino=nullptr;
		for(auto * unit:living())
		{
			if(unit->unitSide()==host->unitSide()) continue;
			const auto active=rules(unit);
			if(!memory && active.count(48)) memory=unit;
			if(!domino && active.count(82)) domino=unit;
		}
		if(!memory && !domino) return;
		std::vector<std::pair<size_t,int>> load;
		std::set<size_t> kinds;
		for(size_t kind=0;kind<ailments().size();++kind)
			for(int owner:carried(host,kind))
			{
				load.emplace_back(kind,owner);
				kinds.insert(kind);
			}
		if(memory)
			for(auto * other:living())
			{
				if(other==host || other->unitSide()!=host->unitSide()) continue;
				const bool linked=std::any_of(load.begin(),load.end(),[&](const auto & mark){
					return (mark.first!=event.kind || mark.second!=event.owner) && suffers(other,mark.first,mark.second);});
				if(!linked || !afflict(other,event.kind,event.owner)) continue;
				if(marked(other,500182,1) || marked(host,500182,1)) discovery(memory,4);
			}
		if(!domino || kinds.size()<2) return;
		const CStack * next=nullptr;
		std::pair<size_t,int> passed;
		for(auto * other:living())
		{
			if(other==host || other->unitSide()!=host->unitSide()) continue;
			if(next && BattleHex::getDistance(host->getPosition(),other->getPosition())>=BattleHex::getDistance(host->getPosition(),next->getPosition())) continue;
			for(const auto & mark:load)
				if(!suffers(other,mark.first,mark.second))
				{
					next=other;
					passed=mark;
					break;
				}
		}
		if(!next) return;
		remember(next,500182,1);
		afflict(next,passed.first,passed.second);
	}

	bool attach(const CStack * target, const CStack * owner, BonusType type)
	{
		return afflict(target,type==BonusType::EVOLUTION_WOUND ? 1 : 0,static_cast<int>(owner->unitId()));
	}

	bool stain(BattleHex hex,int kind,const CStack * owner,int turns)
	{
		if(!hex.isValid()) return false;
		for(const auto & mark:groundMarks(battle))
			if(mark.hex==hex && mark.kind==kind && mark.side==owner->unitSide()) return false;
		const bool pit=kind==6 || kind==13 || kind==17;
		SpellCreatedObstacle mark;
		mark.uniqueID=battle.nextObstacleId();
		mark.pos=hex;
		mark.ID=SpellID(pit ? SpellID::QUICKSAND : SpellID::LAND_MINE).getNum();
		mark.turnsRemaining=turns;
		mark.casterSpellPower=static_cast<int>(owner->unitId());
		mark.spellLevel=kind;
		mark.minimalDamage=groundTag;
		mark.casterSide=owner->unitSide();
		mark.trigger=SpellID::NONE;
		mark.hidden=false;
		mark.revealed=true;
		mark.passable=true;
		mark.trap=false;
		mark.removeOnTrigger=false;
		mark.appearSound=AudioPath::builtin(pit ? "QUIKSAND" : "LANDMINE");
		mark.appearAnimation=AnimationPath::builtin(pit ? "C17SPE0" : "C09SPF0");
		mark.animation=AnimationPath::builtin(pit ? "C17SPE1" : "C09SPF1");
		mark.animationYOffset=pit ? -42 : 0;
		mark.customSize.insert(hex);
		BattleObstaclesChanged packet;
		packet.battleID=battle.getBattle()->getBattleID();
		packet.changes.emplace_back();
		mark.toInfo(packet.changes.back());
		server.sendAndApply(packet);
		return true;
	}

	void wipe(const Ground & mark)
	{
		BattleObstaclesChanged packet;
		packet.battleID=battle.getBattle()->getBattleID();
		packet.changes.emplace_back(mark.id,ObstacleChanges::EOperation::REMOVE);
		server.sendAndApply(packet);
	}

	void mend(const CStack * unit,int64_t amount)
	{
		if(!unit->alive() || amount<=0) return;
		auto state = unit->acquireState();
		state->heal(amount,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
		BattleUnitsChanged packet;
		packet.battleID = battle.getBattle()->getBattleID();
		packet.changedStacks.emplace_back(unit->unitId(), UnitChanges::EOperation::RESET_STATE);
		state->save(packet.changedStacks.back().data);
		server.sendAndApply(packet);
	}

	int rankOf(const CStack * unit,int trait) const
	{
		const auto active=rules(unit);
		const auto found=active.find(trait);
		return found==active.end() ? 1 : std::max(1,found->second);
	}

	// Blood Trail and Living Trap fire when an enemy body enters the marked cell.
	bool tread(const CStack * mover)
	{
		bool halted=false;
		for(const auto & mark:groundMarks(battle))
		{
			if(!mover->alive()) break;
			if(mark.side==mover->unitSide() || (mark.kind!=4 && mark.kind!=6) || !touches(mover,mover->getPosition(),mark.hex,0)) continue;
			wipe(mark);
			const auto * owner=battle.battleGetStackByID(mark.owner,false);
			if(!owner) continue;
			const auto size=std::max<int64_t>(1,owner->unitBaseAmount());
			if(mark.kind==6)
			{
				halted=true;
				injure(mover,owner,evolution::damageProduct(size,rankOf(owner,6)));
			}
			else
			{
				afflict(mover,1,mark.owner);
				injure(mover,owner,evolution::damageProduct(std::max<int64_t>(1,size/2),rankOf(owner,4)));
			}
		}
		return halted;
	}

	// Spore Cloud: an enemy standing in the cloud receives the bearer's parasite.
	void inhale(const CStack * unit)
	{
		if(!unit->alive()) return;
		for(const auto & mark:groundMarks(battle))
			if(mark.kind==13 && mark.side!=unit->unitSide() && touches(unit,unit->getPosition(),mark.hex,0)
				&& battle.battleGetStackByID(mark.owner,false)) afflict(unit,0,mark.owner);
	}

	// Mycelium: at the start of a round allies on it recover, enemies are poisoned.
	void mycelium(const CStack * unit)
	{
		for(const auto & mark:groundMarks(battle))
		{
			if(!unit->alive()) return;
			if(mark.kind!=17 || !touches(unit,unit->getPosition(),mark.hex,0)) continue;
			const auto * owner=battle.battleGetStackByID(mark.owner,false);
			if(!owner) continue;
			if(mark.side!=unit->unitSide()) afflict(unit,4,mark.owner);
			else mend(unit,evolution::damageProduct(std::max<int64_t>(1,owner->unitBaseAmount()),rankOf(owner,17)));
		}
	}

	// Territorial Predator: once per round the bearer strikes the first enemy that enters
	// its marked ground while the bearer itself stands on that ground.
	void territory(const CStack * mover)
	{
		const int round=battle.battleGetRound();
		for(const auto & mark:groundMarks(battle))
		{
			if(!mover->alive()) return;
			if(mark.kind!=24 || mark.side==mover->unitSide() || !touches(mover,mover->getPosition(),mark.hex,1)) continue;
			const auto * owner=battle.battleGetStackByID(mark.owner);
			if(!owner || !touches(owner,owner->getPosition(),mark.hex,1) || counter(owner,500024)==~round) continue;
			setCounter(owner,500024,~round);
			strike(owner,mover);
		}
	}

	const CStack * nearestFoe(const CStack * from) const
	{
		const CStack * best=nullptr;
		for(auto * unit:living())
			if(unit->unitSide()!=from->unitSide() && (!best ||
				BattleHex::getDistance(from->getPosition(),unit->getPosition()) <
				BattleHex::getDistance(from->getPosition(),best->getPosition()))) best=unit;
		return best;
	}

	// Death Strike: a lethal creature hit was held back by incoming(). The bearer attacks
	// natively at full strength, then the stored damage lands and the usual kill reactions run.
	void lastStand(const CStack * victim)
	{
		if(!victim || !victim->alive()) return;
		const auto debt=reserve(victim,500402);
		if(!debt) return;
		const auto * killer=battle.battleGetStackByID(counter(victim,500102)-1,false);
		setReserve(victim,0,500402);
		setCounter(victim,500102,0);
		const auto * prey=killer && killer->alive() && killer->unitSide()!=victim->unitSide() ? killer : nearestFoe(victim);
		if(prey) strike(victim,prey);
		if(victim->alive()) injure(victim,killer ? killer : victim,debt);
	}

	// Blood Reactor: three periodic ticks anywhere on the field release the stored damage.
	void reactor(int64_t amount)
	{
		if(amount<=0) return;
		for(auto * bearer:living())
		{
			if(!bearer->alive()) continue;
			const auto active=rules(bearer);
			if(!active.count(71)) continue;
			const auto energy=combineDamage(reserve(bearer,500471),amount);
			const int ticks=counter(bearer,500071)+1;
			if(ticks<3)
			{
				setCounter(bearer,500071,ticks);
				setReserve(bearer,energy,500471);
				continue;
			}
			setCounter(bearer,500071,0);
			setReserve(bearer,0,500471);
			const auto wave=combineDamage(evolution::damageProduct(energy,active.at(71)),evolution::damageProduct(energy,active.at(71)));
			for(auto * foe:living())
				if(bearer->alive() && foe->unitSide()!=bearer->unitSide()) injure(foe,bearer,wave);
		}
	}

	// Pack Hunt: the third allied hit on one target in a round gives the nearest other ally a free attack.
	void packHunt(const CStack * source,const CStack * primary)
	{
		bool led=false;
		for(auto * unit:living())
			if(unit->unitSide()==source->unitSide() && rules(unit).count(35)) led=true;
		if(!led) return;
		const int round=battle.battleGetRound();
		const int key=500135+static_cast<int>(source->unitSide());
		if(counter(primary,500035)!=~round)
		{
			setCounter(primary,500035,~round);
			setCounter(primary,500135,0);
			setCounter(primary,500136,0);
		}
		const int hits=counter(primary,key);
		if(hits<0) return;
		if(hits<2)
		{
			setCounter(primary,key,hits+1);
			return;
		}
		setCounter(primary,key,-1);
		const CStack * ally=nullptr;
		for(auto * unit:living())
			if(unit!=source && unit->unitSide()==source->unitSide() && (!ally ||
				BattleHex::getDistance(primary->getPosition(),unit->getPosition()) <
				BattleHex::getDistance(primary->getPosition(),ally->getPosition()))) ally=unit;
		if(ally) strike(ally,primary);
	}

	int64_t injure(const CStack * target, const CStack * owner, int64_t amount)
	{
		if(!target->alive() || amount <= 0) return 0;
		if(amount>=target->getAvailableHealth() && owner!=target && owner->alive() && owner->unitSide()!=target->unitSide()
			&& rules(target).count(2) && !marked(target,500002,1))
		{
			remember(target,500002,1);
			strike(target,owner);
			if(!target->alive()) return 0;
		}
		amount = std::min<int64_t>(amount, target->getAvailableHealth());
		BattleStackAttacked hit;
		hit.attackerID = owner->unitId();
		hit.stackAttacked = target->unitId();
		hit.damageAmount = amount;
		target->prepareAttacked(hit,server.getRandomGenerator());
		StacksInjured packet;
		packet.battleID = battle.getBattle()->getBattleID();
		packet.stacks.push_back(hit);
		server.sendAndApply(packet);
		chain.post(evolution::ReactionKind::Loss,owner->unitId(),target->unitId(),hit.killedAmount,hit.killed() && !target->alive());
		flush();
		return amount;
	}

	void parasites(const CStack * target)
	{
		const auto marks = target->getBonusesOfType(BonusType::EVOLUTION_PARASITE);
		for(const auto & mark : *marks)
		{
			auto * owner = battle.battleGetStackByID(mark->val);
			if(!owner || !owner->alive() || !target->alive() || owner->unitSide() == target->unitSide()) continue;
			auto amount = injure(target, owner, std::max<int64_t>(1,owner->getCount()/3));
			if(owner->alive())
			{
				auto state = owner->acquireState();
				state->heal(amount,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
				BattleUnitsChanged packet;
				packet.battleID = battle.getBattle()->getBattleID();
				packet.changedStacks.emplace_back(owner->unitId(), UnitChanges::EOperation::RESET_STATE);
				state->save(packet.changedStacks.back().data);
				server.sendAndApply(packet);
			}
			reactor(amount);
		}
		// Poison spores keep working after their source dies, as long as its body is still known.
		for(int id : carried(target,4))
		{
			const auto * owner = battle.battleGetStackByID(id,false);
			if(!owner || !target->alive() || owner->unitSide() == target->unitSide()) continue;
			reactor(injure(target,owner,std::max<int64_t>(1,owner->unitBaseAmount()/4)));
		}
	}
public:
	EvolutionReactions(CGameHandler & server,const CBattleInfoCallback & battle,uint64_t id,std::function<void(const CStack *,const CStack *)> strike)
		:server(server),battle(battle),chain(id),strike(std::move(strike)){}

	void flush()
	{
		chain.drain([&](const evolution::Reaction & event){
			auto * source=event.source==UINT32_MAX ? nullptr : battle.battleGetStackByID(event.source,false);
			auto * target=battle.battleGetStackByID(event.target,false);
			if(!target) return;
			switch(event.kind)
			{
			case evolution::ReactionKind::Loss:
				lostLarvae(target,event.amount);
				casualties(event.amount);
				if(event.amount) { shed(target,event.amount); split(target); }
				if(event.amount && source && source->alive() && source->unitSide()!=target->unitSide() && rules(source).count(1))
					lay(source,target->getPosition());
				if(event.destroyed && !target->alive()) chain.post(evolution::ReactionKind::Death,event.source,event.target);
				break;
			case evolution::ReactionKind::Death:
				if(!target->alive()) fallen(source,target);
				break;
			case evolution::ReactionKind::Hunt:
				if(!source || !source->alive()) break;
				{
					auto * prey=nearest(source,target->unitSide());
					if(prey && chain.allowHunt(lineageRoot(event.source),lineageRoot(prey->unitId()),origins.count(prey->unitId())!=0))
						strike(source,prey);
				}
				break;
			}
		});
	}

	static bool threatens(const CStack * mover,BattleHex hex,const CStack * guard)
	{
		if(!guard->alive() || guard->unitSide()==mover->unitSide() || !readRules(guard).count(20)) return false;
		for(auto a:guard->getHexes())
			for(auto b:mover->getHexes(hex))
				if(BattleHex::getDistance(a,b)==1) return true;
		return false;
	}

	void epidemic(const CStack * carrier)
	{
		const auto roots=carrier->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500162));
		for(const auto & root:*roots)
		{
			const CStack * closest=nullptr;
			for(auto * unit:living())
			{
				if(unit->unitSide()==carrier->unitSide() || marked(unit,500062,root->val)) continue;
				const auto distance=BattleHex::getDistance(carrier->getPosition(),unit->getPosition());
				if(distance<=3 && (!closest || distance<BattleHex::getDistance(carrier->getPosition(),closest->getPosition()))) closest=unit;
			}
			if(!closest) continue;
			const int germ=root->val;
			afflict(closest,2,germ);
			if(auto * owner=battle.battleGetStackByID(germ,false)) attach(closest,owner,BonusType::EVOLUTION_PARASITE);
		}
	}

	void spread(const CStack * host)
	{
		const auto infected=carried(host,3);
		if(infected.empty()) return;
		for(auto * other:living())
		{
			if(other->unitSide()!=host->unitSide() || other==host || BattleHex::getDistance(other->getPosition(),host->getPosition())!=1) continue;
			for(size_t kind:{size_t(0),size_t(1),size_t(4),size_t(5),size_t(2)})
				for(int owner:carried(host,kind))
				{
					if(!battle.battleGetStackByID(owner,false) || !afflict(other,kind,owner)) continue;
					afflict(other,3,infected.front());
					return;
				}
		}
	}

	bool moved(const CStack * mover,BattleHex origin)
	{
		spread(mover);
		epidemic(mover);
		const auto landing=mover->getPosition();
		if(origin.isValid() && origin!=landing)
		{
			const auto active=rules(mover);
			if(active.count(4)) stain(origin,4,mover,5);
			if(active.count(17)) stain(origin,17,mover,3);
		}
		const bool halted=tread(mover);
		if(mover->alive() && mover->getPosition()==landing) territory(mover);
		for(auto * guard:living())
		{
			if(!mover->alive() || mover->getPosition()!=landing) break;
			if(!threatens(mover,landing,guard) || marked(guard,500020,battle.battleGetRound())) continue;
			auto old=guard->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500020));
			std::vector<Bonus> previous;
			for(const auto & mark:*old)previous.push_back(*mark);
			spend(guard,previous);
			remember(guard,500020,battle.battleGetRound());
			strike(guard,mover);
		}
		return halted;
	}

	void defended(const CStack * unit)
	{
		if(!unit->alive() || !rules(unit).count(6)) return;
		const auto * foe=nearestFoe(unit);
		const auto access=battle.getAccessibility(unit);
		const auto taken=groundMarks(battle);
		BattleHex best;
		for(auto hex:unit->getPosition().getNeighbouringTiles())
		{
			if(!hex.isValid() || !access.accessible(hex,unit)) continue;
			if(std::any_of(taken.begin(),taken.end(),[&](const Ground & mark){return mark.hex==hex && mark.kind==6 && mark.side==unit->unitSide();})) continue;
			if(!best.isValid() || (foe && BattleHex::getDistance(hex,foe->getPosition())<BattleHex::getDistance(best,foe->getPosition()))) best=hex;
		}
		if(best.isValid()) stain(best,6,unit,-1);
	}

	void spellCast(const BattleSpellCast & spell)
	{
		for(auto id:spell.affectedCres)
		{
			const auto * unit=battle.battleGetStackByID(id);
			if(!unit || !unit->alive() || unit->unitSide()==spell.side || spell.resistedCres.count(id) || !rules(unit).count(21)) continue;
			Bonus charge;
			charge.type=BonusType::NONE;
			charge.subtype=BonusCustomSubtype(500021);
			charge.val=1;
			grant(unit,charge);
		}
	}

	int64_t incoming(const CStack * source,const CStack * target,int64_t damage,bool ranged,bool secondary)
	{
		if(damage<=0 || !source || source->unitSide()==target->unitSide()) return damage;
		if(!secondary && rules(source).count(47))
		{
			const auto stored = reserve(source);
			if(stored)
			{
				setReserve(source,0);
				damage = combineDamage(damage,stored);
			}
		}
		if(!secondary && rules(source).count(66))
		{
			const auto charge=reserve(source,500366);
			if(charge)
			{
				damage=combineDamage(damage,charge);
				setReserve(source,0,500366);
			}
		}
		const auto active = rules(target);
		const int round = battle.battleGetRound();
		if(active.count(47) && ranged && counter(target,500047) != ~round)
		{
			setCounter(target,500047,~round);
			setReserve(target,combineDamage(reserve(target),damage));
			return 0;
		}
		if(active.count(14))
		{
			if(counter(target,500014) != ~round)
			{
				setCounter(target,500014,~round);
				setCounter(target,500114,3);
			}
			const int charges = counter(target,500114);
			if(charges>0)
			{
				setCounter(target,500114,charges-1);
				damage -= std::min(damage,std::max<int64_t>(1,target->getAvailableHealth()/8));
				if(charges==1) setCounter(target,500214,1);
			}
		}
		const int channel=ranged ? 2 : 1;
		if(damage>0 && active.count(53))
		{
			const int last=counter(target,500053);
			if(last==channel) damage-=damage/4;
			else if(last) damage=combineDamage(damage,damage/4);
		}
		if(active.count(73) && damage>=target->getAvailableHealth() && counter(target,500073)!=~round && !reserve(target,500373))
		{
			setCounter(target,500073,~round);
			setCounter(target,500173,static_cast<int>(source->unitId()));
			setReserve(target,target->getAvailableHealth(),500373);
			return 0;
		}
		if(active.count(2) && damage>=target->getAvailableHealth() && !marked(target,500002,1) && !reserve(target,500402))
		{
			// Held until the attack packet is applied. See lastStand().
			remember(target,500002,1);
			setCounter(target,500102,static_cast<int>(source->unitId())+1);
			setReserve(target,damage,500402);
			return 0;
		}
		if(damage>0 && active.count(53)) setCounter(target,500053,channel);
		return damage;
	}

	void settleDeferred()
	{
		std::vector<uint32_t> due;
		for(auto * unit:living())
			if(reserve(unit,500373)) due.push_back(unit->unitId());
		for(auto id:due)
		{
			auto * unit=battle.battleGetStackByID(id);
			if(!unit) continue;
			const auto amount=reserve(unit,500373);
			const auto * owner=battle.battleGetStackByID(counter(unit,500173),false);
			setReserve(unit,0,500373);
			if(owner && owner->alive()) injure(unit,owner,amount);
			else if(amount/2) injure(unit,owner ? owner : unit,amount/2);
		}
	}

	void externalDamage(const StacksInjured & injuries)
	{
		for(const auto & hit:injuries.stacks)
			if(hit.killedAmount || hit.killed()) chain.post(evolution::ReactionKind::Loss,hit.attackerID,hit.stackAttacked,hit.killedAmount,hit.killed());
		flush();
	}

	void round(bool first)
	{
		if(first) exchange();
		else
		{
			settleDeferred();
			for(auto * unit:living()) lastStand(unit);
		}
		for(auto * unit : living())
		{
			const auto active = rules(unit);
			if(!unit->alive()) continue;
			if(!first && (awakenHusk(unit) || hatch(unit))) continue;
			if(!first) { inhale(unit); mycelium(unit); }
			if(!first && unit->alive()) { parasites(unit); if(unit->alive()) epidemic(unit); }
			if(!unit->alive()) continue;
			if(active.count(32)) recycle(unit,0);
			if(first) observe(unit);
			if(first && active.count(59))
				for(auto * ally : living())
					if(ally->unitSide() == unit->unitSide() && ally->creatureId() != unit->creatureId())
					{
						copyProperty(unit,ally);
						copyProperty(ally,unit);
						break;
					}
			if(first && active.count(78)) summon(unit,unit->creatureId(),unit->getCount(),unit->getPosition(),true);
			if(active.count(57) && battle.battleGetRound() > 0 && battle.battleGetRound() % 3 == 0) adapt(unit);
		}
	}

	BattleHex pursue(const CStack * source,const CStack * victim) const
	{
		const BattleHex start = source->getPosition();
		if(!source->alive() || !victim || victim->alive() || victim->unitSide()==source->unitSide() || !rules(source).count(15)) return start;
		auto * prey=nearest(source,victim->unitSide());
		if(!prey) return start;
		BattleHex best=start;
		for(auto hex : battle.battleGetAvailableHexes(source,true))
			if(BattleHex::getDistance(hex,prey->getPosition()) < BattleHex::getDistance(best,prey->getPosition())) best=hex;
		return best;
	}

	void attack(const CStack * source,const CStack * primary,const BattleAttack & packet)
	{
		const auto active = rules(source);
		spread(source);
		inhale(source);
		for(const auto & hit : packet.bsa)
		{
			const auto * struck=battle.battleGetStackByID(hit.stackAttacked,false);
			if(struck && struck->alive() && hit.damageAmount>0 && struck->unitSide()!=source->unitSide() && rules(struck).count(13))
			{
				const auto access=battle.getAccessibility(struck);
				for(auto hex:struck->getPosition().getNeighbouringTiles())
					if(hex.isValid() && access.accessible(hex,struck)) stain(hex,13,struck,2);
			}
			lastStand(struck);
		}
		// The packet may contain multiple victims. A spark is indirect damage, so it dispatches
		// death reactions but cannot recursively emit another primary attack.
		for(const auto & hit : packet.bsa)
		{
			const auto * victim = battle.battleGetStackByID(hit.stackAttacked,false);
			if(victim && hit.damageAmount>0 && source->unitSide()!=victim->unitSide())
			{
				const bool offensive=battle.battleGetRound()%2!=0;
				const auto * bearer=offensive ? source : victim;
				if(bearer->alive() && rules(bearer).count(66))
				{
					const auto gained=std::max<int64_t>(1,hit.damageAmount/4);
					setReserve(bearer,combineDamage(reserve(bearer,500366),gained),500366);
				}
			}
			if(victim && counter(victim,500214))
			{
				setCounter(victim,500214,0);
				spawn(victim->unitSide(),huskType(),1,victim->getPosition());
			}
			chain.post(evolution::ReactionKind::Loss,hit.attackerID,hit.stackAttacked,hit.killedAmount,hit.killed());
		}
		flush();
		if(!primary || !source->alive() || primary->unitSide() == source->unitSide()) return;
		if(primary->alive())
		{
			const int side=static_cast<int>(source->unitSide());
			if(marked(primary,500108,side))
				injure(primary,source,std::max<int64_t>(1,source->getCount()/2));
			else if(active.count(8) && marked(source,500008,primary->unitId()))
				remember(primary,500108,side);
			auto charges=spellCharges(source);
			if(!charges.empty())
			{
				spend(source,charges);
				injure(primary,source,evolution::damageProduct(source->getCount(),static_cast<int64_t>(charges.size())));
			}
			const auto wounds = primary->getBonusesOfType(BonusType::EVOLUTION_WOUND);
			for(const auto & wound : *wounds)
			{
				auto * owner = battle.battleGetStackByID(wound->val, false);
				if(owner && owner != source && owner->unitSide() == source->unitSide())
					injure(primary,source,std::max<int64_t>(1,source->getCount()/4));
			}
			if(primary->alive() && (active.count(9) || active.count(89))) attach(primary,source,BonusType::EVOLUTION_PARASITE);
			if(primary->alive() && active.count(19)) attach(primary,source,BonusType::EVOLUTION_WOUND);
			const int self=static_cast<int>(source->unitId());
			if(active.count(62)) afflict(primary,2,self);
			if(active.count(26)) afflict(primary,3,self);
			if(primary->alive() && active.count(77))
			{
				// Poison, slowing, bleeding or a breeding mark. Each is an ordinary mark for other mutations.
				static const size_t spores[]={4,5,1,2};
				afflict(primary,spores[server.getRandomGenerator().nextInt(0,3)],self);
			}
		}
		if(primary->alive() && source->alive())
			for(const auto & hit : packet.bsa)
				if(hit.stackAttacked == primary->unitId() && hit.attackerID == source->unitId() && hit.damageAmount > 0)
				{
					packHunt(source,primary);
					break;
				}
		if(!source->alive()) return;
		if(active.count(23))
		{
			auto * target = nearest(primary,primary->unitSide());
			for(const auto & hit : packet.bsa)
				if(target && hit.stackAttacked == primary->unitId() && hit.damageAmount > 0)
				{
					injure(target,source,std::max<int64_t>(1,evolution::damageProduct(hit.damageAmount,active.at(23),3)));
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

void BattleActionProcessor::withEvolution(const CBattleInfoCallback & battle,const std::function<void(evolution_server::EvolutionReactions &)> & work)
{
	if(activeReaction) { work(*activeReaction); return; }
	evolution_server::EvolutionReactions context(*gameHandler,battle,++reactionSerial,[&](const CStack * hunter,const CStack * prey){
		makeAttack(battle,hunter,prey,0,prey->getPosition(),true,false,false);
	});
	struct Reset { evolution_server::EvolutionReactions * & active; ~Reset(){active=nullptr;} } reset{activeReaction};
	activeReaction=&context;
	work(context);
	context.flush();
}

int64_t BattleActionProcessor::evolutionIncoming(const CBattleInfoCallback & battle, uint32_t source, const CStack * target, int64_t damage, bool ranged, bool secondary)
{
	withEvolution(battle,[&](auto & context){
		damage = context.incoming(battle.battleGetStackByID(source),target,damage,ranged,secondary);
	});
	return damage;
}

void BattleActionProcessor::evolutionRound(const CBattleInfoCallback & battle, bool first)
{
	if(evolutionEnabled()) withEvolution(battle,[&](auto & context){context.round(first);});
}

void BattleActionProcessor::evolutionAttack(const CBattleInfoCallback & battle,const CStack * source,const CStack * target,const BattleAttack & attack)
{
	if(!evolutionEnabled()) return;
	withEvolution(battle,[&](auto & reactions){
		reactions.attack(source,target,attack);
		const auto destination=reactions.pursue(source,target);
		if(destination!=source->getPosition()) moveStack(battle,source->unitId(),destination);
	});
}

void BattleActionProcessor::evolutionDamage(const CBattleInfoCallback & battle,const StacksInjured & packet)
{
	withEvolution(battle,[&](auto & context){context.externalDamage(packet);});
}

void BattleActionProcessor::evolutionSpell(const CBattleInfoCallback & battle,const BattleSpellCast & packet)
{
	withEvolution(battle,[&](auto & context){context.spellCast(packet);});
}

bool BattleActionProcessor::evolutionThreatened(const CBattleInfoCallback & battle,const CStack * mover,BattleHex hex)
{
	// A walker pauses on every cell of hostile marked territory so that its owner can react.
	for(const auto & mark:evolution_server::groundMarks(battle))
		if(mark.kind==24 && mark.side!=mover->unitSide() && evolution_server::touches(mover,hex,mark.hex,1)) return true;
	for(auto * guard:battle.battleGetAllStacks())
		if(evolution_server::EvolutionReactions::threatens(mover,hex,guard))
		{
			const auto marks=guard->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500020));
			if(std::none_of(marks->begin(),marks->end(),[&](const auto & mark){return mark->val==battle.battleGetRound();})) return true;
		}
	return false;
}

bool BattleActionProcessor::evolutionMoved(const CBattleInfoCallback & battle,const CStack * mover,BattleHex origin)
{
	bool halted=false;
	withEvolution(battle,[&](auto & context){halted=context.moved(mover,origin);});
	return halted;
}

void BattleActionProcessor::evolutionDefended(const CBattleInfoCallback & battle,const CStack * unit)
{
	if(evolutionEnabled()) withEvolution(battle,[&](auto & context){context.defended(unit);});
}
