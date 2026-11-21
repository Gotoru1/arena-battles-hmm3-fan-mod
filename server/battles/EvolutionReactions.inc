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
	// Bodies created by this causal chain. Killing one of them cannot extend the chain,
	// otherwise a summon-on-death and a strike-on-kill would feed each other forever.
	std::set<uint32_t> newborn;
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
			// Nobody to trade with: the bearer mutates on its own for this battle.
			if(!paired.count(bearer->unitId())) adapt(bearer);
		}
	}

	int brood(const CStack * unit) const
	{
		const int inherited=counter(unit,500190);
		return inherited>0 ? inherited : static_cast<int>(unit->unitId())+1;
	}

	const CStack * broodMother(const CStack * unit,int trait) const
	{
		const auto * mother=battle.battleGetStackByID(brood(unit)-1);
		return mother && mother->unitSide()==unit->unitSide() && rules(mother).count(trait) ? mother : nullptr;
	}

	// Parasitic Mind: a larva hunts the nearest infected enemy on its own at the start of a round.
	void seek(const CStack * larva)
	{
		if(!larva->alive() || counter(larva,500190)<=0 || !broodMother(larva,29)) return;
		const CStack * prey=nullptr;
		for(auto * unit:living())
			if(unit->unitSide()!=larva->unitSide() && unit->hasBonusOfType(BonusType::EVOLUTION_PARASITE) && (!prey ||
				BattleHex::getDistance(larva->getPosition(),unit->getPosition())<BattleHex::getDistance(larva->getPosition(),prey->getPosition()))) prey=unit;
		if(prey) strike(larva,prey);
	}

	// Hungry Colony: the bearer's larva stacks eat each other into one, keeping every mutation.
	void gather(const CStack * mother)
	{
		std::vector<const CStack *> larvae;
		for(auto * unit:living())
			if(unit->unitSide()==mother->unitSide() && unit->creatureId()==larvaType() && counter(unit,500190)==static_cast<int>(mother->unitId())+1)
				larvae.push_back(unit);
		if(larvae.size()<2) return;
		int64_t total=0;
		Rules pooled;
		for(auto * larva:larvae)
		{
			total+=larva->getCount();
			for(auto rule:rules(larva)) pooled[rule.first]=std::max(pooled[rule.first],rule.second);
		}
		const auto place=larvae.front()->getPosition();
		BattleUnitsChanged merge;
		merge.battleID=battle.getBattle()->getBattleID();
		for(auto * larva:larvae) merge.changedStacks.emplace_back(larva->unitId(),UnitChanges::EOperation::REMOVE);
		server.sendAndApply(merge);
		if(auto * colony=spawn(mother->unitSide(),larvaType(),headcount(total),place))
		{
			descend(mother,colony);
			for(auto rule:pooled) transfer(colony,rule.first,rule.second);
			observe(colony);
		}
	}

	// Sentient Plague: each round the bearer's infection moves to the healthiest clean enemy near an infected one.
	void plague(const CStack * bearer,int reach)
	{
		const int self=static_cast<int>(bearer->unitId());
		for(int spread=0;spread<reach;++spread)
		{
			const CStack * best=nullptr;
			for(auto * candidate:living())
			{
				if(candidate->unitSide()==bearer->unitSide() || suffers(candidate,0,self)) continue;
				bool near=false;
				for(auto * host:living())
					if(host->unitSide()==candidate->unitSide() && suffers(host,0,self) && BattleHex::getDistance(host->getPosition(),candidate->getPosition())<=2) near=true;
				if(near && (!best || candidate->getAvailableHealth()>best->getAvailableHealth())) best=candidate;
			}
			if(!best) return;
			afflict(best,0,self);
		}
	}

	// Swarm Mind: when one body of the brood strikes, every other body of that brood next to the target joins in.
	void swarm(const CStack * source,const CStack * primary)
	{
		const int family=brood(source);
		const auto * mother=battle.battleGetStackByID(family-1);
		if(!mother || mother->unitSide()!=source->unitSide() || !rules(mother).count(90)) return;
		const int round=battle.battleGetRound();
		// The body that struck has made its move for this round and does not join again.
		if(counter(source,500090)!=~round) setCounter(source,500090,~round);
		for(auto * member:living())
		{
			if(!primary->alive()) return;
			if(member==source || !member->alive() || member->unitSide()!=source->unitSide() || brood(member)!=family) continue;
			if(member->creatureId()==eggType() || !touches(member,member->getPosition(),primary->getPosition(),1) || counter(member,500090)==~round) continue;
			setCounter(member,500090,~round);
			strike(member,primary);
		}
	}

	void descend(const CStack * parent,const CStack * child)
	{
		origins[child->unitId()]=lineageRoot(parent->unitId());
		// The brood mark outlives this chain: it names the first ancestor for Swarm Mind and the colony.
		remember(child,500190,brood(parent));
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
		labelTrait(marker,id);
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
		newborn.insert(info.id);
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

	// Returns the index of the copied property in this list, or -1. The run stores that index.
	int copyProperty(const CStack * recipient,const CStack * donor)
	{
		int index=0;
		for(auto trait : {BonusType::FLYING, BonusType::BLOCKS_RETALIATION, BonusType::TWO_HEX_ATTACK_BREATH,
			BonusType::ATTACKS_ALL_ADJACENT, BonusType::NO_MELEE_PENALTY, BonusType::NO_DISTANCE_PENALTY})
		{
			auto traits = donor->getBonusesOfType(trait);
			if(!recipient->hasBonusOfType(trait) && !traits->empty())
			{
				grant(recipient,**traits->begin());
				return index;
			}
			++index;
		}
		return -1;
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

	// Colony mutations are playable alone: each of them lets its bearer lay eggs on a kill.
	bool brooder(const CStack * unit) const
	{
		const auto active=rules(unit);
		for(int id:{1,29,45,60,75,84,87,90,93,95,97}) if(active.count(id)) return true;
		return false;
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
			// Emptying a summoned body flags it for removal. The heal does not clear the flag.
			detached->ghostPending=false;
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

	static CreatureID skeletonType()
	{
		static const CreatureID id(CreatureID::decode("core:skeleton"));
		return id;
	}

	// Remains are the bodies of original units that still lie on the field. A summoned body
	// is removed by the engine when it dies, and each body can be used up only once.
	std::vector<const CStack *> remains() const
	{
		std::vector<const CStack *> result;
		for(const auto * unit:battle.battleGetAllStacks())
			if(!unit->alive() && !unit->isGhost() && !unit->summoned && unit->getPosition().isValid() && !marked(unit,500905,1)
				&& unit->creatureId()!=eggType() && unit->creatureId()!=huskType()) result.push_back(unit);
		std::sort(result.begin(),result.end(),[](auto * a,auto * b){return a->unitId()<b->unitId();});
		return result;
	}

	bool fresh(const CStack * corpse) const
	{
		return !corpse->alive() && !corpse->isGhost() && !corpse->summoned && !marked(corpse,500905,1);
	}

	void consume(const CStack * corpse)
	{
		remember(corpse,500905,1);
	}

	static int headcount(int64_t amount)
	{
		return static_cast<int>(std::max<int64_t>(1,std::min<int64_t>(amount,INT32_MAX)));
	}

	// Evolutionary Leap: destroy an enemy unit (1), lose fighters (2) and reach the third round (4)
	// in one battle. The marker 500270 tells the run to improve the creature after a victory.
	void leap(const CStack * unit,int deed)
	{
		if(!unit->alive() || !rules(unit).count(70)) return;
		const int deeds=counter(unit,500170);
		if((deeds|deed)==deeds) return;
		setCounter(unit,500170,deeds|deed);
		if((deeds|deed)==7) remember(unit,500270,1);
	}

	// Predatory Rebirth: the dead bearer returns at the start of the next round by using up
	// the untouched remains of an enemy it destroyed.
	void rebirths()
	{
		for(const auto * dead:battle.battleGetAllStacks())
		{
			if(dead->alive() || dead->isGhost() || !marked(dead,500249,1)) continue;
			setCounter(dead,500249,0);
			const CStack * fuel=nullptr;
			for(const auto * prey:remains())
				if(prey!=dead && marked(prey,500049,dead->unitId())) { fuel=prey; break; }
			if(!fuel) continue;
			bool blocked=false;
			for(auto occupied:dead->getHexes())
				for(auto * body:living())
				{
					const auto & cells=body->getHexes();
					if(std::find(cells.begin(),cells.end(),occupied)!=cells.end()) blocked=true;
				}
			if(blocked) continue;
			consume(fuel);
			remember(dead,500149,1);
			auto restored=dead->acquireState();
			int64_t hp=int64_t(std::max(1,dead->unitBaseAmount()/3))*dead->getMaxHealth();
			restored->heal(hp,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
			restored->ghostPending=false;
			BattleUnitsChanged packet;
			packet.battleID=battle.getBattle()->getBattleID();
			packet.changedStacks.emplace_back(dead->unitId(),UnitChanges::EOperation::RESET_STATE);
			packet.changedStacks.back().healthDelta=restored->getAvailableHealth();
			restored->save(packet.changedStacks.back().data);
			server.sendAndApply(packet);
		}
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
		// A body the engine already removed from the field cannot come back.
		if(!original->alive() && !original->isGhost())
		{
			auto restored=original->acquireState();
			int64_t hp=int64_t(std::max(1,original->unitBaseAmount()/4))*original->getMaxHealth();
			restored->heal(hp,EHealLevel::RESURRECT,EHealPower::ONE_BATTLE);
			restored->ghostPending=false;
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
		if(source && source->alive() && source->unitSide()!=corpse->unitSide() && (counter(source,500169)>0 || counter(source,500069)>0))
		{
			// Entwined Fates: the twin's kill brings back a third of what the bearer has lost.
			const auto * bearer=counter(source,500169)>0 ? partner(source,500169) : source;
			if(bearer && bearer->alive() && (bearer!=source || partner(source,500069)==source))
				mend(bearer,std::max<int64_t>(1,missing(bearer)/3));
		}
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
		const bool original=!corpse->summoned && !corpse->isClone();
		if(legacy.count(64) && original && corpse->unitBaseAmount()>=6 && !marked(corpse,500064,1))
		{
			// Fractal Army: two independent copies, a sixth of the unit each.
			remember(corpse,500064,1);
			for(int i=0;i<2;++i)
				summon(corpse,corpse->creatureId(),std::max(1,corpse->unitBaseAmount()/6),corpse->getPosition(),true);
		}
		if(legacy.count(28) && original && !marked(corpse,500028,1))
		{
			// Reverse Evolution: the unit returns one level lower and strikes twice.
			remember(corpse,500028,1);
			const auto lesser=lesserForm(corpse->creatureId());
			const int count=lesser==corpse->creatureId() ? std::max(1,corpse->unitBaseAmount()/2) : corpse->unitBaseAmount();
			if(auto * reborn=spawn(corpse->unitSide(),lesser,count,corpse->getPosition()))
			{
				descend(corpse,reborn);
				for(auto rule:legacy) if(rule.first!=28) transfer(reborn,rule.first,rule.second);
				Bonus extra;
				extra.type=BonusType::ADDITIONAL_ATTACK;
				extra.val=1;
				grant(reborn,extra);
				observe(reborn);
			}
		}
		if(legacy.count(42) && original && !marked(corpse,500042,1))
		{
			// Soul Split: a third of the unit stays as flying ghosts.
			remember(corpse,500042,1);
			if(auto * ghost=spawn(corpse->unitSide(),corpse->creatureId(),std::max(1,corpse->unitBaseAmount()/3),corpse->getPosition()))
			{
				descend(corpse,ghost);
				for(auto rule:legacy) if(rule.first!=42) transfer(ghost,rule.first,rule.second);
				if(!ghost->hasBonusOfType(BonusType::FLYING))
				{
					Bonus wings;
					wings.type=BonusType::FLYING;
					wings.val=1;
					grant(ghost,wings);
				}
				observe(ghost);
			}
		}
		if(legacy.count(55))
			if(auto * heir=nearest(corpse,corpse->unitSide()))
			{
				// Mind Transfer: every mutation and half of the unit's health pool as an attack charge.
				for(auto rule:legacy) if(rule.first!=55) transfer(heir,rule.first,rule.second);
				setReserve(heir,combineDamage(reserve(heir,500312),int64_t(corpse->unitBaseAmount())*corpse->getMaxHealth()/2),500312);
			}
		if(counter(corpse,500158)>0)
		{
			// A defeated echo returns to the side that lost the original, with one more mutation.
			if(const auto * owner=battle.battleGetStackByID(counter(corpse,500158)-1))
				if(auto * reborn=spawn(owner->unitSide(),corpse->creatureId(),std::max(1,corpse->unitBaseAmount()),corpse->getPosition()))
				{
					descend(owner,reborn);
					adapt(reborn);
					observe(reborn);
				}
			setCounter(corpse,500158,0);
		}
		else if(original)
			for(auto * witness:beforeSummons)
			{
				// Rewritten Death: the first fallen ally comes back as a hostile echo of half its size.
				if(witness->unitSide()!=corpse->unitSide() || !rules(witness).count(58) || marked(witness,500058,1)) continue;
				const auto * foe=nearestFoe(witness);
				if(!foe) break;
				if(auto * echo=spawn(foe->unitSide(),corpse->creatureId(),std::max(1,corpse->unitBaseAmount()/2),corpse->getPosition()))
				{
					remember(witness,500058,1);
					setCounter(echo,500158,static_cast<int>(witness->unitId())+1);
				}
				break;
			}
		// Summoned bodies and clones are removed by the engine when they die, so they cannot feign death.
		if(legacy.count(22) && !marked(corpse,500022,1) && !corpse->summoned && !corpse->isClone())
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
		if(corpse->summoned && counter(corpse,500190)>0 && !marked(corpse,500184,1))
			if(const auto * hive=broodMother(corpse,84))
				if(hive->alive() && hive!=corpse)
					setCounter(hive,500084,std::min(INT32_MAX-1,counter(hive,500084))+1);
		// Vengeance Machine counts every allied body destroyed in this battle; the run turns the count into lasting damage.
		if(corpse->creatureId()!=eggType() && corpse->creatureId()!=huskType())
			for(auto * witness:beforeSummons)
				if(witness!=corpse && witness->unitSide()==corpse->unitSide() && rules(witness).count(80))
					setCounter(witness,500080,std::min(INT32_MAX-1,counter(witness,500080))+1);
		if(source && source->alive() && source->unitSide()!=corpse->unitSide()) leap(source,1);
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
				bool passed=false;
			for(auto entry : rules(corpse,id == 25))
				if(entry.first != id && rules(heir)[entry.first] < entry.second)
				{
					transfer(heir,entry.first,entry.second);
					passed=true;
					break;
				}
			// With nothing of its own to leave, the dying unit still mutates its heir.
			if(!passed) adapt(heir);
		}

		if(source && source->alive() && source->unitSide() != corpse->unitSide() && rules(source).count(27))
			copyProperty(source,corpse);
		if(source && source->alive() && source->unitSide() != corpse->unitSide() && rules(source).count(24))
			stain(corpse->getPosition(),24,source,-1);
		if(source && source->unitSide() != corpse->unitSide() && rules(source).count(49))
			remember(corpse,500049,source->unitId());
		if(legacy.count(49) && !marked(corpse,500149,1) && !corpse->summoned && !corpse->isClone())
			remember(corpse,500249,1);
		if(source && source->alive() && source->unitSide() != corpse->unitSide())
		{
			const auto hunter=rules(source);
			if(hunter.count(12)) stain(corpse->getPosition(),12,source,-1);
			if(hunter.count(37)) stain(corpse->getPosition(),37,source,3);
			if(hunter.count(96) && !counter(source,500096))
			{
				// Species Rewrite: the first property absorbed in a wave is reported to the run for good.
				const int property=copyProperty(source,corpse);
				if(property>=0) setCounter(source,500096,property+1);
			}
			if(hunter.count(76) && source->getBonusesOfType(BonusType::NONE,BonusCustomSubtype(500076))->size()<3)
				for(auto taken:legacy)
					if(taken.first!=88 && !hunter.count(taken.first) && !marked(source,500076,taken.first))
					{
						// Living Arsenal: one mutation of a destroyed enemy, kept for the next waves.
						remember(source,500076,taken.first);
						break;
					}
			if(hunter.count(74) && !counter(source,500174))
			{
				// Evolutionary Fork: the third destroyed unit fixes the form for the rest of the battle.
				const int kills=counter(source,500074)+1;
				setCounter(source,500074,kills);
				if(kills>=3)
					setCounter(source,500174,source->getAvailableHealth()*2<int64_t(source->unitBaseAmount())*source->getMaxHealth() ? 1 : 2);
			}
			if(hunter.count(52) && fresh(corpse))
			{
				// Bone Puppet: a quarter of the dead unit fights for the killer for three rounds.
				if(auto * puppet=spawn(source->unitSide(),corpse->creatureId(),std::max(1,corpse->unitBaseAmount()/4),corpse->getPosition()))
				{
					consume(corpse);
					descend(source,puppet);
					setCounter(puppet,500052,battle.battleGetRound()+3);
					observe(puppet);
				}
			}
			if(hunter.count(65) && fresh(corpse))
			{
				// Predatory Assimilation: part of the remains becomes a unit of the killer's own kind.
				const auto flesh=int64_t(corpse->unitBaseAmount())*corpse->getMaxHealth();
				const int grown=headcount(evolution::damageProduct(flesh,hunter.at(65),4)/std::max(1,int(source->getMaxHealth())));
				if(auto * kin=summon(source,source->creatureId(),grown,corpse->getPosition(),false))
				{
					consume(corpse);
					copyProperty(kin,corpse);
				}
			}
		}
		for(auto * unit : beforeSummons)
			if(rules(unit).count(18) && BattleHex::getDistance(unit->getPosition(),corpse->getPosition()) == 1)
				summon(unit,corpse->creatureId(),1,corpse->getPosition(),false);
		// Echo of Infinity: every destroyed enemy unit repeats the bearer's attack on the nearest enemy.
		if(source && source->alive() && source->unitSide()!=corpse->unitSide() && rules(source).count(94) && !newborn.count(corpse->unitId()))
			if(const auto * prey=nearestFoe(source))
				strike(source,prey);
	}

	// Negative marks that can travel between bodies. The value of a mark is the unit id of its source.
	// Order: parasite, wound, breeding germ, viral tag, poison, slowing spore, memory mark.
	struct Ailment { BonusType type; int subtype; };
	static const std::vector<Ailment> & ailments()
	{
		static const std::vector<Ailment> all{{BonusType::EVOLUTION_PARASITE,0},{BonusType::EVOLUTION_WOUND,0},
			{BonusType::NONE,500062},{BonusType::NONE,500026},{BonusType::NONE,500077},{BonusType::NONE,500177},{BonusType::NONE,500148}};
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
		static const char * const names[]={"parasite","wound","germ","viral","poison","slow","memory"};
		labelStatus(mark,names[kind]);
		grant(target,mark);
		if(firstSlow)
		{
			// One penalty per body keeps the slowest native creatures mobile.
			Bonus slow;
			slow.type=BonusType::STACKS_SPEED;
			slow.val=-2;
			grant(target,slow);
		}
		hexed(target);
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

	// An enemy spell or a negative mark has just landed on the unit.
	void hexed(const CStack * unit)
	{
		if(!unit->alive()) return;
		const auto active=rules(unit);
		// Mirror Shard: the first one of the battle forms a shard that strikes at the next round.
		if(active.count(16) && !marked(unit,500016,1))
		{
			remember(unit,500016,1);
			setCounter(unit,500116,1);
		}
		// Magic Eater: every one becomes a charge for the next attack.
		if(active.count(31)) setCounter(unit,500031,std::min(INT32_MAX-1,counter(unit,500031))+1);
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
		const std::string look=kind==4 ? "blood" : kind==6 ? "trap" : kind==11 ? "shadow" : kind==12 ? "ash" : kind==13 ? "spores"
			: kind==17 ? "mycelium" : kind==37 ? "portal" : kind==46 ? "curse" : "territory";
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
		// Own 45x52 pictures drawn over the whole cell.
		mark.appearAnimation=AnimationPath::builtin("evoMarkIn_"+look);
		mark.animation=AnimationPath::builtin("evoMark_"+look);
		mark.animationYOffset=0;
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

	int64_t missing(const CStack * unit) const
	{
		return std::max<int64_t>(0,int64_t(unit->unitBaseAmount())*unit->getMaxHealth()-unit->getAvailableHealth());
	}

	// Every heal of the reactions goes through here, so that healing beyond full strength
	// reaches Blood Cloning and Blood Alchemy.
	void mend(const CStack * unit,int64_t amount,bool lasting=false)
	{
		if(!unit->alive() || amount<=0) return;
		const auto useful=std::min(amount,missing(unit));
		if(useful>0)
		{
			auto state = unit->acquireState();
			auto portion=useful;
			state->heal(portion,EHealLevel::RESURRECT,lasting ? EHealPower::PERMANENT : EHealPower::ONE_BATTLE);
			BattleUnitsChanged packet;
			packet.battleID = battle.getBattle()->getBattleID();
			packet.changedStacks.emplace_back(unit->unitId(), UnitChanges::EOperation::RESET_STATE);
			state->save(packet.changedStacks.back().data);
			server.sendAndApply(packet);
		}
		surplus(unit,amount-useful);
	}

	// Grow a copy once enough spare material has gathered: at least a tenth of the unit.
	void bud(const CStack * unit,int key)
	{
		const auto stock=reserve(unit,key);
		const auto body=std::max<int64_t>(1,unit->getMaxHealth());
		const auto fighters=stock/body;
		if(fighters<std::max(1,unit->unitBaseAmount()/10)) return;
		if(summon(unit,unit->creatureId(),headcount(fighters),unit->getPosition(),true))
			setReserve(unit,stock-std::min<int64_t>(fighters,INT32_MAX)*body,key);
	}

	void surplus(const CStack * unit,int64_t extra)
	{
		if(extra<=0 || !unit->alive()) return;
		const auto active=rules(unit);
		if(active.count(43)) setReserve(unit,combineDamage(reserve(unit,500343),extra),500343);
		if(active.count(30))
		{
			setReserve(unit,combineDamage(reserve(unit,500330),evolution::damageProduct(extra,active.at(30))),500330);
			bud(unit,500330);
		}
	}

	// Living Chain and Entwined Fates pick the nearest ally at the start of the battle.
	const CStack * partner(const CStack * unit,int key) const
	{
		const int id=counter(unit,key);
		if(id<=0) return nullptr;
		const auto * other=battle.battleGetStackByID(id-1);
		return other && other->unitSide()==unit->unitSide() ? other : nullptr;
	}

	void bind(const CStack * unit,int key,int reverseKey)
	{
		if(counter(unit,key)) return;
		const CStack * best=nullptr;
		for(auto * other:living())
			if(other!=unit && other->unitSide()==unit->unitSide() && !counter(other,reverseKey) && other->creatureId()!=eggType() && other->creatureId()!=huskType()
				&& (!best || BattleHex::getDistance(unit->getPosition(),other->getPosition())<BattleHex::getDistance(unit->getPosition(),best->getPosition()))) best=other;
		// Alone, the bearer is bound to itself.
		if(!best) best=unit;
		setCounter(unit,key,static_cast<int>(best->unitId())+1);
		if(best!=unit) setCounter(best,reverseKey,static_cast<int>(unit->unitId())+1);
	}

	// The part of a hit that Living Chain moved to the partner lands after the attack packet.
	void settleShared(const CStack * source)
	{
		for(auto * unit:living())
		{
			const auto share=reserve(unit,500433);
			if(!share) continue;
			setReserve(unit,0,500433);
			if(unit->alive()) injure(unit,source && source->unitSide()!=unit->unitSide() ? source : unit,share);
		}
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
			if(mark.kind==37 && mark.side==mover->unitSide() && touches(mover,mover->getPosition(),mark.hex,0))
			{
				// Wormhole: the first ally to step in comes out next to the nearest enemy.
				const auto * foe=nearestFoe(mover);
				const auto exit=foe ? battle.getAvailableHex(mover->creatureId(),mover->unitSide(),foe->getPosition().toInt()) : BattleHex();
				if(!exit.isValid() || exit==mover->getPosition()) continue;
				wipe(mark);
				BattleStackMoved jump;
				jump.battleID=battle.getBattle()->getBattleID();
				jump.stack=mover->unitId();
				jump.teleporting=true;
				jump.tilesToMove.insert(exit);
				server.sendAndApply(jump);
				return true;
			}
			if(mark.kind==46 && mark.side!=mover->unitSide() && touches(mover,mover->getPosition(),mark.hex,0))
			{
				// Cursed Geometry: crossing the line hurts.
				wipe(mark);
				if(const auto * owner=battle.battleGetStackByID(mark.owner,false))
					injure(mover,owner,evolution::damageProduct(std::max(1,owner->unitBaseAmount()),rankOf(owner,46)));
				continue;
			}
			if(mark.kind==12 && touches(mover,mover->getPosition(),mark.hex,0))
			{
				// Ash: an ally takes it in for its next attack, an enemy scatters it.
				wipe(mark);
				const auto * owner=battle.battleGetStackByID(mark.owner,false);
				if(mark.side==mover->unitSide() && owner)
					setReserve(mover,combineDamage(reserve(mover,500312),evolution::damageProduct(int64_t(std::max(1,owner->unitBaseAmount()))*2,rankOf(owner,12))),500312);
				continue;
			}
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

	// Pack Hunt: every third allied hit on one target gives the nearest other ally a free attack,
	// at most once per round for that target. A lone bearer takes the free attack itself.
	void packHunt(const CStack * source,const CStack * primary)
	{
		bool led=false;
		for(auto * unit:living())
			if(unit->unitSide()==source->unitSide() && rules(unit).count(35)) led=true;
		if(!led) return;
		const int round=battle.battleGetRound();
		const int key=500135+static_cast<int>(source->unitSide());
		const int roundKey=500235+static_cast<int>(source->unitSide());
		const int hits=counter(primary,key)+1;
		if(hits<3 || counter(primary,roundKey)==~round)
		{
			setCounter(primary,key,std::min(hits,3));
			return;
		}
		setCounter(primary,key,0);
		setCounter(primary,roundKey,~round);
		const CStack * ally=nullptr;
		for(auto * unit:living())
			if(unit!=source && unit->unitSide()==source->unitSide() && (!ally ||
				BattleHex::getDistance(primary->getPosition(),unit->getPosition()) <
				BattleHex::getDistance(primary->getPosition(),ally->getPosition()))) ally=unit;
		strike(ally ? ally : source,primary);
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
			// The mark is an infection for every colony mutation. Only leeches feed through it.
			const auto feeding=rules(owner);
			if(!feeding.count(9) && !feeding.count(89) && !feeding.count(13) && !feeding.count(81)) continue;
			auto amount = injure(target, owner, std::max<int64_t>(1,owner->getCount()/3));
			mend(owner,amount);
			reactor(amount);
		}
		// An open wound bleeds every round while the unit that opened it lives.
		for(int id : carried(target,1))
		{
			const auto * owner = battle.battleGetStackByID(id);
			if(!owner || !target->alive() || owner->unitSide() == target->unitSide()) continue;
			reactor(injure(target,owner,std::max<int64_t>(1,owner->getCount()/4)));
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
				if(event.amount && target->alive()) leap(target,2);
				if(event.amount && counter(target,500069)>0)
					if(const auto * twin=partner(target,500069))
						if(twin->alive())
						{
							// Entwined Fates: the fallen fighters arm the twin's next attack.
							const auto gift=evolution::damageProduct(int64_t(event.amount)*target->getMaxHealth(),rankOf(target,69));
							setReserve(twin,combineDamage(reserve(twin,500312),gift),500312);
						}
				if(event.amount) { shed(target,event.amount); split(target); }
				if(event.amount && source && source->alive() && source->unitSide()!=target->unitSide() && brooder(source))
					lay(source,target->getPosition());
				// Blood Debt also remembers whoever kills the bearer's own fighters.
				if(event.amount && source && target->alive() && source->unitSide()!=target->unitSide() && rules(target).count(8))
					remember(target,500008,source->unitId());
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
			if(active.count(11))
			{
				// Predatory Shadow: one shadow per bearer stays on the cell it left.
				for(const auto & old:groundMarks(battle))
					if(old.kind==11 && old.owner==static_cast<int>(mover->unitId())) wipe(old);
				stain(origin,11,mover,-1);
			}
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
		if(!unit->alive()) return;
		const auto active=rules(unit);
		if(active.count(36))
		{
			// Symbiotic Parasite: defending next to an ally feeds that ally.
			const CStack * host=nullptr;
			for(auto * ally:living())
				if(ally!=unit && ally->unitSide()==unit->unitSide() && ally->creatureId()!=eggType() && ally->creatureId()!=huskType()
					&& touches(unit,unit->getPosition(),ally->getPosition(),1) && (!host || ally->unitId()<host->unitId())) host=ally;
			if(host)
			{
				for(auto rule:active) if(rule.first!=36 && rule.first!=88) transfer(host,rule.first,rule.second);
				setReserve(host,combineDamage(reserve(host,500312),static_cast<int64_t>(unit->getAvailableHealth())/2),500312);
			}
		}
		if(active.count(93))
		{
			// Army Devourer: defending swallows the summoned allies standing next to the bearer.
			std::vector<const CStack *> meal;
			for(auto * ally:living())
				if(ally!=unit && ally->summoned && ally->unitSide()==unit->unitSide() && ally->creatureId()!=eggType() && ally->creatureId()!=huskType()
					&& touches(unit,unit->getPosition(),ally->getPosition(),1)) meal.push_back(ally);
			for(auto * ally:meal)
			{
				const auto flesh=static_cast<int64_t>(ally->getAvailableHealth());
				for(auto rule:rules(ally)) if(rule.first!=88) transfer(unit,rule.first,rule.second);
				BattleUnitsChanged swallow;
				swallow.battleID=battle.getBattle()->getBattleID();
				swallow.changedStacks.emplace_back(ally->unitId(),UnitChanges::EOperation::REMOVE);
				server.sendAndApply(swallow);
				// What does not fit into the bearer's own ranks arms its next attack.
				const auto healed=std::min(flesh,missing(unit));
				mend(unit,healed);
				if(flesh>healed) setReserve(unit,combineDamage(reserve(unit,500312),flesh-healed),500312);
			}
		}
		if(active.count(63) && unit->getCount()>=2)
		{
			// Blood Economy: a tenth of the bearer's own fighters pays for an immediate attack.
			const auto * foe=nearestFoe(unit);
			if(foe)
			{
				injure(unit,unit,int64_t(std::max(1,unit->getCount()/10))*unit->getMaxHealth());
				if(unit->alive() && foe->alive()) strike(unit,foe);
			}
			if(!unit->alive()) return;
		}
		if(active.count(5))
			for(const auto * corpse:remains())
			{
				// Corpse Eater: defending next to remains restores fallen fighters for good.
				if(!touches(unit,unit->getPosition(),corpse->getPosition(),1)) continue;
				consume(corpse);
				mend(unit,evolution::damageProduct(int64_t(corpse->unitBaseAmount())*corpse->getMaxHealth(),active.at(5),3),true);
				break;
			}
		if(!active.count(6)) return;
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
			if(!unit || !unit->alive() || unit->unitSide()==spell.side || spell.resistedCres.count(id)) continue;
			hexed(unit);
			if(!rules(unit).count(21)) continue;
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
		if(!secondary)
		{
			const auto ash=reserve(source,500312);
			if(ash)
			{
				setReserve(source,0,500312);
				damage=combineDamage(damage,ash);
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
		if(!secondary && rules(source).count(44))
		{
			// Mirror Hunter: the last blow it took comes back with its next attack.
			const auto recorded=reserve(source,500344);
			if(recorded)
			{
				setReserve(source,0,500344);
				damage=combineDamage(damage,evolution::damageProduct(recorded,rules(source).at(44)));
			}
		}
		// Evolutionary Fork: the predator form hits half as hard again, the shell form takes a quarter less.
		if(!secondary && counter(source,500174)==2) damage=combineDamage(damage,damage/2);
		if(counter(target,500174)==1) damage-=damage/4;
		if(!secondary && rules(source).count(43) && source->getAvailableHealth()*2>=int64_t(source->unitBaseAmount())*source->getMaxHealth())
		{
			// Blood Alchemy: a healthy bearer puts its charges into the strike.
			const auto charges=reserve(source,500343);
			if(charges)
			{
				setReserve(source,0,500343);
				damage=combineDamage(damage,charges);
			}
		}
		const auto active = rules(target);
		const int round = battle.battleGetRound();
		if(active.count(43))
		{
			// The charges of a hard-pressed bearer soak up to half of a hit.
			const auto charges=reserve(target,500343);
			const auto soaked=std::min(charges,damage/2);
			if(soaked>0)
			{
				setReserve(target,charges-soaked,500343);
				damage-=soaked;
			}
		}
		if(counter(target,500033)>0 || counter(target,500133)>0)
		{
			const auto * other=partner(target,counter(target,500033)>0 ? 500033 : 500133);
			if(other==target) damage-=damage/5;
			else if(other && other->alive() && damage>1)
			{
				// Living Chain: half of the hit is carried by the bound ally after this attack.
				const auto share=damage/2;
				setReserve(other,combineDamage(reserve(other,500433),share),500433);
				damage-=share;
			}
		}
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
		if(first)
		{
			exchange();
			for(auto * unit:living())
			{
				const auto bound=rules(unit);
				if(bound.count(33)) bind(unit,500033,500133);
				if(bound.count(69)) bind(unit,500069,500169);
			}
		}
		else
		{
			settleShared(nullptr);
			settleDeferred();
			for(auto * unit:living()) lastStand(unit);
			rebirths();
		}
		for(auto * unit : living())
		{
			const auto active = rules(unit);
			if(!unit->alive()) continue;
			if(!first && counter(unit,500052) && counter(unit,500052)<=battle.battleGetRound())
			{
				// Puppets and shadows fall apart when their time is up.
				BattleUnitsChanged crumble;
				crumble.battleID=battle.getBattle()->getBattleID();
				crumble.changedStacks.emplace_back(unit->unitId(),UnitChanges::EOperation::REMOVE);
				server.sendAndApply(crumble);
				continue;
			}
			if(!first && active.count(72))
			{
				// Lord of Remains: one untouched body per round rises as skeletons.
				const CStack * nearestBody=nullptr;
				for(const auto * corpse:remains())
					if(!nearestBody || BattleHex::getDistance(unit->getPosition(),corpse->getPosition())<BattleHex::getDistance(unit->getPosition(),nearestBody->getPosition())) nearestBody=corpse;
				if(nearestBody && summon(unit,skeletonType(),headcount(evolution::damageProduct(nearestBody->unitBaseAmount(),active.at(72),2)),nearestBody->getPosition(),false))
					consume(nearestBody);
			}
			if(!first && (awakenHusk(unit) || hatch(unit))) continue;
			if(!first) { inhale(unit); mycelium(unit); }
			if(!first && unit->alive()) { parasites(unit); if(unit->alive()) epidemic(unit); }
			if(!unit->alive()) continue;
			if(active.count(32)) recycle(unit,0);
			if(battle.battleGetRound()>=3) leap(unit,4);
			if(first && active.count(100) && active.size()>=8)
			{
				// Final Evolution: with eight different mutations every one of them works a rank higher.
				for(auto rule:active)
					if(rule.first!=100 && rule.second<INT32_MAX) putTrait(unit,rule.first,rule.second+1);
			}
			if(first) observe(unit);
			if(first && active.count(79))
			{
				// Predator Merge: the bearer and its nearest ally fight this battle with each other's mutations.
				const CStack * mate=nullptr;
				for(auto * ally:living())
					if(ally!=unit && ally->unitSide()==unit->unitSide() && ally->creatureId()!=eggType() && ally->creatureId()!=huskType() && (!mate ||
						BattleHex::getDistance(unit->getPosition(),ally->getPosition())<BattleHex::getDistance(unit->getPosition(),mate->getPosition()))) mate=ally;
				if(!mate) adapt(unit);
				else
				{
					const auto theirs=rules(mate);
					for(auto rule:active) if(rule.first!=79 && rule.first!=88) transfer(mate,rule.first,rule.second);
					for(auto rule:theirs) if(rule.first!=79 && rule.first!=88) transfer(unit,rule.first,rule.second);
				}
			}
			if(first && active.count(59))
				for(auto * ally : living())
					if(ally->unitSide() == unit->unitSide() && ally->creatureId() != unit->creatureId())
					{
						copyProperty(unit,ally);
						copyProperty(ally,unit);
						break;
					}
			if(first && active.count(78)) summon(unit,unit->creatureId(),unit->getCount(),unit->getPosition(),true);
			// Hive Mind alone grows a small offshoot to share its boosts with.
			else if(first && active.count(56) && unit->getBonusesOfType(BonusType::EVOLUTION_FAMILY)->empty())
				summon(unit,unit->creatureId(),std::max(1,unit->getCount()/10),unit->getPosition(),true);
			// The Catalyst starts every battle with one new temporary mutation to react to.
			if(first && active.count(61)) adapt(unit);
			if(active.count(57) && battle.battleGetRound() > 0 && battle.battleGetRound() % 3 == 0) adapt(unit);
			if(!first && unit->alive() && active.count(81)) plague(unit,active.at(81));
			if(!first && unit->alive() && counter(unit,500116))
			{
				setCounter(unit,500116,0);
				if(const auto * foe=nearestFoe(unit))
					injure(foe,unit,evolution::damageProduct(int64_t(std::max(1,unit->getCount()))*5,rankOf(unit,16)));
			}
			if(!first && unit->alive() && active.count(84) && counter(unit,500084))
			{
				// Dimensional Hive: the fallen descendants come back as larvae beside the bearer.
				const int charges=counter(unit,500084);
				if(auto * returned=summon(unit,larvaType(),charges,unit->getPosition(),false))
				{
					setCounter(unit,500084,0);
					remember(returned,500184,1);
				}
			}
			if(!first && unit->alive() && unit->creatureId()==larvaType()) seek(unit);
		}
		if(!first)
			for(auto * unit : living())
				if(unit->alive() && rules(unit).count(45)) gather(unit);
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
			if(struck && struck->alive() && hit.damageAmount>0 && struck->unitSide()!=source->unitSide() && rules(struck).count(44))
				setReserve(struck,hit.damageAmount,500344);
			// Absolute Adaptation: outliving a blow of a mutated enemy is what the run remembers.
			if(struck && struck->alive() && hit.damageAmount>0 && struck->unitSide()!=source->unitSide() && rules(struck).count(98) && !active.empty())
				remember(struck,500098,1);
			lastStand(struck);
		}
		settleShared(source);
		for(const auto & hit : packet.bsa)
		{
			if(hit.attackerID!=source->unitId() || hit.damageAmount<=0) continue;
			const auto * struck=battle.battleGetStackByID(hit.stackAttacked,false);
			if(!struck || struck->unitSide()==source->unitSide()) continue;
			// Blood Cloning and Blood Alchemy drink a quarter of the damage their bearer deals.
			if(source->alive() && (active.count(30) || active.count(43))) mend(source,std::max<int64_t>(1,hit.damageAmount/4));
			const auto spikes=rules(struck);
			if(struck->alive() && source->alive() && spikes.count(40) && !packet.shot())
			{
				// Shard Breeding: a quarter of a melee hit is thrown back and the shards gather into a copy.
				const auto thrown=injure(source,struck,std::max<int64_t>(1,evolution::damageProduct(hit.damageAmount,spikes.at(40),4)));
				if(struck->alive() && thrown>0)
				{
					setReserve(struck,combineDamage(reserve(struck,500340),thrown),500340);
					bud(struck,500340);
				}
			}
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
			if(primary->alive() && (active.count(9) || active.count(89) || active.count(39) || active.count(54) || active.count(29) || active.count(81))) attach(primary,source,BonusType::EVOLUTION_PARASITE);
			if(primary->alive() && active.count(19)) attach(primary,source,BonusType::EVOLUTION_WOUND);
			const int self=static_cast<int>(source->unitId());
			if(active.count(62)) afflict(primary,2,self);
			// Each of these brings its own mark, so it works without any other mutation.
			if(active.count(26)) { afflict(primary,5,self); afflict(primary,3,self); }
			if(active.count(71)) afflict(primary,4,self);
			if(active.count(48)) afflict(primary,6,self);
			if(active.count(82) && !afflict(primary,5,self)) afflict(primary,4,self);
			if(primary->alive() && active.count(77))
			{
				// Poison, slowing, bleeding or a breeding mark. Each is an ordinary mark for other mutations.
				static const size_t spores[]={4,5,1,2};
				afflict(primary,spores[server.getRandomGenerator().nextInt(0,3)],self);
			}
		}
		if(source->alive() && active.count(48))
			for(const auto & hit : packet.bsa)
			{
				if(hit.stackAttacked != primary->unitId() || hit.attackerID != source->unitId() || hit.damageAmount <= 0) continue;
				const auto echo=std::max<int64_t>(1,evolution::damageProduct(hit.damageAmount,active.at(48),3));
				std::vector<std::pair<size_t,int>> load;
				for(size_t kind=0;kind<ailments().size();++kind)
					for(int owner:carried(primary,kind)) load.emplace_back(kind,owner);
				for(auto * other:living())
				{
					if(!source->alive()) break;
					if(other==primary || other->unitSide()!=primary->unitSide()) continue;
					if(std::any_of(load.begin(),load.end(),[&](const auto & mark){return suffers(other,mark.first,mark.second);}))
						injure(other,source,echo);
				}
				break;
			}
		if(primary->alive() && source->alive())
			for(const auto & hit : packet.bsa)
				if(hit.stackAttacked == primary->unitId() && hit.attackerID == source->unitId() && hit.damageAmount > 0)
				{
					packHunt(source,primary);
					break;
				}
		if(!source->alive()) return;
		if(active.count(34))
		{
			// Shadow Army: every second attack leaves a shadow of a quarter of the unit for two rounds.
			if(!counter(source,500034)) setCounter(source,500034,1);
			else
			{
				setCounter(source,500034,0);
				if(auto * shade=spawn(source->unitSide(),source->creatureId(),std::max(1,source->getCount()/4),source->getPosition()))
				{
					descend(source,shade);
					setCounter(shade,500052,battle.battleGetRound()+2);
					observe(shade);
				}
			}
		}
		for(const auto & hit : packet.bsa)
		{
			if(hit.stackAttacked != primary->unitId() || hit.attackerID != source->unitId() || hit.damageAmount <= 0) continue;
			if(active.count(31) && primary->alive() && counter(source,500031))
			{
				// Magic Eater: all stored charges leave with this blow and slow the target.
				const int charges=counter(source,500031);
				setCounter(source,500031,0);
				afflict(primary,5,static_cast<int>(source->unitId()));
				injure(primary,source,evolution::damageProduct(int64_t(std::max(1,source->unitBaseAmount()))*3,int64_t(charges)*active.at(31)));
			}
			if(active.count(11) && primary->alive())
				for(const auto & mark:groundMarks(battle))
					if(mark.kind==11 && mark.owner==static_cast<int>(source->unitId()) && touches(primary,primary->getPosition(),mark.hex,1))
					{
						// Predatory Shadow: the shadow next to the target repeats half of the blow and fades.
						wipe(mark);
						injure(primary,source,std::max<int64_t>(1,evolution::damageProduct(hit.damageAmount,active.at(11),2)));
						break;
					}
			if(active.count(38) && primary->alive() && source->alive())
			{
				// Flesh Corruption: every third blow tears a tenth of the unit away to fight for the bearer.
				const int blows=counter(primary,500038)+1;
				if(blows<3) setCounter(primary,500038,blows);
				else
				{
					setCounter(primary,500038,0);
					if(primary->getCount()>=2)
					{
						const int torn=std::max(1,primary->getCount()/10);
						const auto species=primary->creatureId();
						const auto place=primary->getPosition();
						injure(primary,source,int64_t(torn)*primary->getMaxHealth());
						if(source->alive())
							if(auto * outcast=spawn(source->unitSide(),species,torn,place))
							{
								descend(source,outcast);
								setCounter(outcast,500052,battle.battleGetRound()+2);
								observe(outcast);
							}
					}
				}
			}
			if(active.count(46) && source->alive())
			{
				// Cursed Geometry: one cursed cell per step between the bearer and the target, and two beyond it.
				const auto from=source->getPosition();
				const auto to=primary->getPosition();
				const int length=BattleHex::getDistance(from,to);
				for(int step=1;step<length+3;++step)
				{
					if(step==length) continue;
					for(int index=0;index<fieldCells();++index)
					{
						const auto cell=cellAt(index);
						const bool between=step<length && BattleHex::getDistance(from,cell)==step && BattleHex::getDistance(cell,to)==length-step;
						const bool beyond=step>length && BattleHex::getDistance(from,cell)==step && BattleHex::getDistance(cell,to)==step-length;
						if(!between && !beyond) continue;
						stain(cell,46,source,2);
						break;
					}
				}
			}
			break;
		}
		if(!source->alive()) return;
		if(primary->alive() && source->alive()) swarm(source,primary);
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
		// A shooter that cannot reach its target in melee answers with a shot.
		const bool shot=!CStack::isMeleeAttackPossible(hunter,prey) && battle.battleCanShoot(hunter);
		makeAttack(battle,hunter,prey,0,prey->getPosition(),true,shot,false);
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
