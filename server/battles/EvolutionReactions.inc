// SPDX-License-Identifier: GPL-2.0-or-later
// Included by BattleActionProcessor.cpp. All changes go through VCMI's battle packets.
namespace evolution_server
{
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

	void adapt(const CStack * unit)
	{
		// These traits can activate after a unit enters an ongoing battle.
		std::vector<int> pool{9,10,18,19,23,25,27,51};
		auto current = rules(unit);
		pool.erase(std::remove_if(pool.begin(), pool.end(), [&](int id){return current.count(id) != 0;}),pool.end());
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
	int64_t reserve(const CStack * unit) const
	{
		int64_t amount = 0;
		for(int i=0; i<3; ++i)
			amount |= int64_t(counter(unit,500247+i)) << (30*i);
		return amount;
	}

	void setReserve(const CStack * unit,int64_t amount)
	{
		for(int i=0; i<3; ++i)
		{
			setCounter(unit,500247+i,static_cast<int>(amount & 0x3fffffff));
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

	void fallen(const CStack * source, const CStack * corpse)
	{
		auto beforeSummons = living();
		const auto legacy=rules(corpse);
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
		for(auto * unit : beforeSummons)
			if(rules(unit).count(18) && BattleHex::getDistance(unit->getPosition(),corpse->getPosition()) == 1)
				summon(unit,corpse->creatureId(),1,corpse->getPosition(),false);
	}

	bool attach(const CStack * target, const CStack * owner, BonusType type)
	{
		auto marks = target->getBonusesOfType(type);
		for(const auto & mark : *marks)
			if(mark->val == static_cast<int>(owner->unitId())) return false;
		Bonus mark;
		mark.type = type;
		mark.val = static_cast<int>(owner->unitId());
		grant(target, mark);
		return true;
	}

	int64_t injure(const CStack * target, const CStack * owner, int64_t amount)
	{
		if(!target->alive() || amount <= 0) return 0;
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
			auto state = owner->acquireState();
			state->heal(amount,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
			BattleUnitsChanged packet;
			packet.battleID = battle.getBattle()->getBattleID();
			packet.changedStacks.emplace_back(owner->unitId(), UnitChanges::EOperation::RESET_STATE);
			state->save(packet.changedStacks.back().data);
			server.sendAndApply(packet);
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
			remember(closest,500062,root->val);
			if(auto * owner=battle.battleGetStackByID(root->val,false)) attach(closest,owner,BonusType::EVOLUTION_PARASITE);
		}
	}

	void spread(const CStack * host)
	{
		auto infected=host->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500026));
		if(infected->empty()) return;
		for(auto * other:living())
		{
			if(other->unitSide()!=host->unitSide() || other==host || BattleHex::getDistance(other->getPosition(),host->getPosition())!=1) continue;
			for(auto type:{BonusType::EVOLUTION_PARASITE,BonusType::EVOLUTION_WOUND})
			{
				const auto status=host->getBonusesOfType(type);
				for(const auto & mark:*status)
				{
					const auto * owner=battle.battleGetStackByID(mark->val,false);
					if(!owner || !attach(other,owner,type)) continue;
					remember(other,500026,(*infected->begin())->val);
					return;
				}
			}
		}
	}

	void moved(const CStack * mover)
	{
		spread(mover);
		epidemic(mover);
		const auto landing=mover->getPosition();
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
		return damage;
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
		for(auto * unit : living())
		{
			const auto active = rules(unit);
			if(!unit->alive()) continue;
			if(!first && (awakenHusk(unit) || hatch(unit))) continue;
			if(!first) { parasites(unit); if(unit->alive()) epidemic(unit); }
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
		// The packet may contain multiple victims. A spark is indirect damage, so it dispatches
		// death reactions but cannot recursively emit another primary attack.
		for(const auto & hit : packet.bsa)
		{
			const auto * victim = battle.battleGetStackByID(hit.stackAttacked,false);
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
			if(primary->alive() && active.count(62)) remember(primary,500062,source->unitId());
			if(primary->alive() && active.count(26)) remember(primary,500026,source->unitId());
		}
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
	for(auto * guard:battle.battleGetAllStacks())
		if(evolution_server::EvolutionReactions::threatens(mover,hex,guard))
		{
			const auto marks=guard->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500020));
			if(std::none_of(marks->begin(),marks->end(),[&](const auto & mark){return mark->val==battle.battleGetRound();})) return true;
		}
	return false;
}

void BattleActionProcessor::evolutionMoved(const CBattleInfoCallback & battle,const CStack * mover)
{
	withEvolution(battle,[&](auto & context){context.moved(mover);});
}
