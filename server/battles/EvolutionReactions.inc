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

	uint32_t lineageRoot(uint32_t id) const
	{
		auto it=origins.find(id);
		return it==origins.end()?id:it->second;
	}

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
		const auto muted=unit->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500050));
		for(const auto & mark:*muted) result.erase(mark->val);
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

	void putTrait(const CStack * unit, int id, int rank)
	{
		if(rules(unit)[id] >= rank) return;
		Bonus marker;
		marker.type = BonusType::EVOLUTION_TRAIT;
		marker.val = id + 1024 * (rank-1);
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
				if(entry.first != id && entry.first != 61 && entry.second < INT32_MAX/1024)
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

	void summon(const CStack * parent, CreatureID species, int count, BattleHex origin, bool clone)
	{
		auto charges=spellCharges(parent);
		const int extra=static_cast<int>(std::min<size_t>(charges.size(),INT32_MAX-count));
		auto * child = spawn(parent->unitSide(),species,count+extra,origin);
		if(!child) return;
		spend(parent,charges);
		origins[child->unitId()]=lineageRoot(parent->unitId());
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

	bool lay(const CStack * parent, BattleHex hex)
	{
		auto * egg = spawn(parent->unitSide(),eggType(),1,hex);
		if(!egg) return false;
		origins[egg->unitId()]=lineageRoot(parent->unitId());
		Bonus source;
		source.type = BonusType::EVOLUTION_BROOD;
		source.val = static_cast<int>(parent->unitId());
		grant(egg,source);
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
		BattleUnitsChanged remove;
		remove.battleID = battle.getBattle()->getBattleID();
		remove.changedStacks.emplace_back(id,UnitChanges::EOperation::REMOVE);
		server.sendAndApply(remove);
		auto * parent = battle.battleGetStackByID(parentId,false);
		if(parent)
		{
			summon(parent,larvaType(),1,hex,false);
			if(rules(parent).count(60) && lay(parent,hex)) discovery(parent,2);
		}
		else spawn(side,larvaType(),1,hex);
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
				origins[child->unitId()]=lineageRoot(carrier->unitId());
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
		origins[child->unitId()]=lineageRoot(unit->unitId());
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

	void fallen(const CStack * source, const CStack * corpse)
	{
		auto beforeSummons = living();
		if(source && source->unitSide()!=corpse->unitSide())
			for(auto * witness:beforeSummons)
				if(witness->unitSide()==corpse->unitSide() && rules(witness).count(8)) remember(witness,500008,source->unitId());
		if(source && source->alive() && source->unitSide()!=corpse->unitSide() && rules(source).count(68))
		{
			static const CreatureID ghost(CreatureID::decode("core:wight"));
			if(auto * hunter=spawn(source->unitSide(),ghost,1,corpse->getPosition()))
			{
				origins[hunter->unitId()]=lineageRoot(source->unitId());
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
			if(carrier && rules(carrier).count(39)) lay(carrier,corpse->getPosition());
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
				casualties(event.amount);
				if(event.amount) split(target);
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
			if(!first && hatch(unit)) continue;
			if(!first) parasites(unit);
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
		// The packet may contain multiple victims. A spark is indirect damage, so it dispatches
		// death reactions but cannot recursively emit another primary attack.
		for(const auto & hit : packet.bsa)
		{
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
			if(primary->alive() && active.count(9)) attach(primary,source,BonusType::EVOLUTION_PARASITE);
			if(primary->alive() && active.count(19)) attach(primary,source,BonusType::EVOLUTION_WOUND);
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

void BattleActionProcessor::evolutionRound(const CBattleInfoCallback & battle, bool first)
{
	withEvolution(battle,[&](auto & context){context.round(first);});
}

void BattleActionProcessor::evolutionAttack(const CBattleInfoCallback & battle,const CStack * source,const CStack * target,const BattleAttack & attack)
{
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
