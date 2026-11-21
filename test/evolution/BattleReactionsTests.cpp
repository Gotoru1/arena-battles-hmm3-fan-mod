// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production reaction dispatcher with a small packet-applying battlefield.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <functional>
#include <random>
#include <string>
#include "../../lib/battle/EvolutionChain.h"
#include "../../client/arena/EvolutionEnemyArmy.h"
#include <string>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
using BonusCustomSubtype=int;
enum class BonusType { NONE, EVOLUTION_BROOD, EVOLUTION_FAMILY, EVOLUTION_SEEN, EVOLUTION_TRAIT, EVOLUTION_WOUND, EVOLUTION_PARASITE, FLYING, BLOCKS_RETALIATION, TWO_HEX_ATTACK_BREATH, ATTACKS_ALL_ADJACENT, NO_MELEE_PENALTY, NO_DISTANCE_PENALTY, STACKS_SPEED, ADDITIONAL_ATTACK };
enum class BonusDuration { PERMANENT, ONE_BATTLE };
struct NumericSubtype {
	int value;
	NumericSubtype(int n=0):value(n){}
	int getNum() const{return value;}
	operator int() const{return value;}
};
struct Bonus { BonusType type{}; int val{}; BonusDuration duration{}; NumericSubtype subtype{}; };
struct CreatureID { int value; CreatureID(int v=0):value(v){} static int decode(std::string s){return s=="arenabattles:evolutionEgg"?500:501;} int getNum() const{return value;} operator int() const{return value;} };
enum class BattleSide { LEFT, RIGHT };
struct BattleHex {
	static inline int fieldSize=21;
	int pos=-1;
	bool isValid() const {return pos>=0 && pos<fieldSize;}
	int toInt() const {return pos;}
	bool operator!=(BattleHex other) const {return pos!=other.pos;}
	bool operator==(BattleHex other) const {return pos==other.pos;}
	std::vector<BattleHex> getNeighbouringTiles() const {return {{pos-1},{pos+1}};}
	static int getDistance(BattleHex a,BattleHex b){return std::abs(a.pos-b.pos);}
};
static Bonus trait(int id){return {BonusType::EVOLUTION_TRAIT,1,BonusDuration::PERMANENT,id};}
enum class EHealLevel { RESURRECT };
enum class EHealPower { ONE_BATTLE, PERMANENT };
struct Data {int species{}, count{}, health{}; BattleHex pos; BattleSide side{}; bool summoned=false;};
namespace battle {
struct UnitInfo {int id{}, type{}, count{}; BattleHex position; BattleSide side{};bool summoned=false;
	void save(Data & out){out={type,count,count*10,position,side,summoned};}
};}
struct UnitChanges {enum class EOperation {ADD, RESET_STATE, REMOVE};int id; EOperation op; Data data;int64_t healthDelta=0;
	UnitChanges(int id,EOperation op):id(id),op(op){}
};
struct BattleStackAttacked {
	int stackAttacked{},attackerID{}; int64_t damageAmount{};bool dead=false;int killedAmount=0;bool killed() const{return dead;}
};
struct BattleAttack {std::vector<BattleStackAttacked> bsa;bool ranged=false;bool shot() const{return ranged;}};
struct CStack {
	int id;Data data;std::vector<Bonus> bonuses;
	struct TemporaryHealth{int getResurrected() const{return 0;}} health;
	bool base=true;
	// Engine flags for bodies that vanish on death. The model has no summoned units.
	bool ghostPending=false, summoned=false;
	bool isGhost() const{return false;} bool isClone() const{return false;}
	static bool isMeleeAttackPossible(const CStack *,const CStack *){return true;}
	int unitId() const{return id;} bool alive() const{return data.health>0;}
	int getCount() const{return (data.health+9)/10;}
	int getMaxHealth() const{return 10;}
	int unitBaseAmount() const{return data.count;}
	void damage(int64_t & n){data.health-=int(n);}
	auto unitSide() const{return data.side;} auto creatureId() const{return CreatureID(data.species);}
	auto getPosition() const{return data.pos;} std::vector<BattleHex> getHexes() const{return {data.pos};}
	std::vector<BattleHex> getHexes(BattleHex p) const{return {p};}
	int getAvailableHealth() const{return data.health;}
	auto getBonusesOfType(BonusType t) const{
		auto result=std::make_shared<std::vector<std::shared_ptr<Bonus>>>();
		for(auto b:bonuses) if(b.type==t)result->push_back(std::make_shared<Bonus>(b));
		return result;
	}
	auto getBonusesOfType(BonusType t,int subtype) const {
		auto all=getBonusesOfType(t);
		all->erase(std::remove_if(all->begin(),all->end(),[&](auto b){return b->subtype!=subtype;}),all->end());
		return all;
	}
	bool hasBonusOfType(BonusType t) const{return !getBonusesOfType(t)->empty();}
	void prepareAttacked(BattleStackAttacked & hit,int &) const{hit.dead=hit.damageAmount>=data.health;}
	auto acquireState() const{return std::make_shared<CStack>(*this);}
	void save(Data & to){to=data;}
	void heal(int64_t n,EHealLevel,EHealPower){data.health=std::min<int>(data.count*10,data.health+int(n));}
};
struct Hexes : std::vector<BattleHex>{void insert(BattleHex h){push_back(h);}};
struct BattleStackMoved {int battleID{},stack{};bool teleporting=false;Hexes tilesToMove;};
struct BattleUnitsChanged {int battleID{}; std::vector<UnitChanges> changedStacks;};
struct SpellID { enum Type {NONE=-1, QUICKSAND=10, LAND_MINE=11}; int value; SpellID(int v=0):value(v){} int getNum() const{return value;} bool operator==(SpellID other) const{return value==other.value;} };
struct AnimationPath { std::string name; static AnimationPath builtin(const std::string & n){return {n};} };
struct AudioPath { std::string name; static AudioPath builtin(const std::string & n){return {n};} };
struct ObstacleChanges;
struct CObstacleInstance { BattleHex pos; int uniqueID=-1; int ID=-1; virtual ~CObstacleInstance()=default; };
// Defaults mirror the engine constructor, including the unset trigger that callers must clear.
struct SpellCreatedObstacle : CObstacleInstance {
	int turnsRemaining=-1, casterSpellPower=0, spellLevel=0, minimalDamage=0;
	BattleSide casterSide{};
	SpellID trigger{0};
	bool hidden=false, passable=false, trap=false, removeOnTrigger=false, revealed=false;
	AudioPath appearSound; AnimationPath appearAnimation, animation;
	int animationYOffset=0;
	Hexes customSize;
	void toInfo(ObstacleChanges & info);
};
struct ObstacleChanges {
	enum class EOperation {ADD, REMOVE};
	int id=-1; EOperation operation=EOperation::ADD; std::shared_ptr<SpellCreatedObstacle> data;
	ObstacleChanges()=default;
	ObstacleChanges(int id,EOperation operation):id(id),operation(operation){}
};
inline void SpellCreatedObstacle::toInfo(ObstacleChanges & info){info.id=uniqueID;info.operation=ObstacleChanges::EOperation::ADD;info.data=std::make_shared<SpellCreatedObstacle>(*this);}
struct BattleObstaclesChanged {int battleID{}; std::vector<ObstacleChanges> changes;};
struct BattleSpellCast { BattleSide side;std::set<unsigned> affectedCres,resistedCres; };
struct StacksInjured {int battleID{};std::vector<BattleStackAttacked> stacks;};
struct SetStackEffect {int battleID{};std::vector<std::pair<int,std::vector<Bonus>>> toAdd,toRemove;};
struct CBattleInfoCallback {
	std::map<int,CStack> units;int round=1;
	std::vector<std::shared_ptr<SpellCreatedObstacle>> marks;
	auto battleGetAllObstacles() const{return std::vector<std::shared_ptr<const CObstacleInstance>>(marks.begin(),marks.end());}
	int nextObstacleId() const{int next=0;for(const auto & mark:marks)next=std::max(next,mark->uniqueID+1);return next;}
	mutable int lastId=0;
	int getBattleID() const{return 1;}auto getBattle() const{return this;}
	int battleNextUnitId() const{return lastId=std::max(lastId,units.empty()?0:units.rbegin()->first)+1;}
	auto battleGetStackByID(int id,bool alive=true) const{
		auto p=units.find(id);return p==units.end()||(alive&&!p->second.alive())?nullptr:&p->second;
	}
	auto battleGetAllStacks() const{std::vector<const CStack*> v;for(auto & e:units)v.push_back(&e.second);return v;}
	bool accessible(BattleHex h,const CStack*) const{
		if(!h.isValid())return false;
		for(auto & e:units)if(e.second.alive()&&e.second.data.pos==h)return false;
		return true;
	}
	auto battleGetAvailableHexes(const CStack * unit,bool) const{
		std::vector<BattleHex> result;
		for(auto h:unit->getPosition().getNeighbouringTiles()) if(accessible(h,unit)) result.push_back(h);
		return result;
	}
	const auto & getAccessibility(const CStack*) const{return *this;}
	BattleHex getAvailableHex(int,BattleSide,int near) const{
		for(int d=0;d<BattleHex::fieldSize;++d)for(int p:{near+d,near-d})if(accessible({p},nullptr))return {p};
		return {};
	}
	int battleGetRound() const{return round;}
	bool battleCanShoot(const CStack *) const{return false;}
};
struct Rng {int value=0;int nextInt(int lo,int hi){return lo+(value++%(hi-lo+1));}operator int&(){return value;}};
struct CGameHandler {
	CBattleInfoCallback & field;Rng rng;
	Rng & getRandomGenerator(){return rng;}
	void sendAndApply(const SetStackEffect & p){for(auto & v:p.toRemove) for(auto b:v.second) {
		auto & dest=field.units.at(v.first).bonuses;
		dest.erase(std::remove_if(dest.begin(),dest.end(),[&](auto orig){return orig.type==b.type && orig.subtype==b.subtype && orig.val==b.val;}),dest.end());
	} for(auto & v:p.toAdd)for(auto b:v.second)field.units.at(v.first).bonuses.push_back(b);}
	void sendAndApply(const StacksInjured & p){for(auto & h:p.stacks)field.units.at(h.stackAttacked).data.health-=int(h.damageAmount);}
	void sendAndApply(const BattleUnitsChanged & p){for(auto & c:p.changedStacks){
		if(c.op==UnitChanges::EOperation::ADD) field.units.emplace(c.id,CStack{c.id,c.data,{}, {}, false}).first->second.summoned=c.data.summoned;
		else if(c.op==UnitChanges::EOperation::REMOVE) field.units.erase(c.id);
		else field.units.at(c.id).data=c.data;
	}}
	void sendAndApply(const BattleStackMoved & p){field.units.at(p.stack).data.pos=p.tilesToMove.back();}
	void sendAndApply(const BattleObstaclesChanged & p){for(const auto & change:p.changes){
		if(change.operation==ObstacleChanges::EOperation::REMOVE)
		{
			auto & all=field.marks;
			const auto before=all.size();
			all.erase(std::remove_if(all.begin(),all.end(),[&](const auto & mark){return mark->uniqueID==change.id;}),all.end());
			assert(all.size()+1==before);
			continue;
		}
		// What the engine needs to draw the mark, let units walk over it and never cast a trigger spell.
		const auto & mark=*change.data;
		assert(mark.pos.isValid() && mark.passable && !mark.trap && !mark.hidden && mark.trigger==SpellID(SpellID::NONE));
		assert(mark.customSize.size()==1 && mark.customSize.back()==mark.pos);
		assert(mark.animation.name.rfind("evoMark_",0)==0 && mark.appearAnimation.name.rfind("evoMarkIn_",0)==0 && mark.turnsRemaining!=0);
		assert(mark.animationYOffset==0);
		for(const auto & other:field.marks) assert(other->uniqueID!=mark.uniqueID);
		field.marks.push_back(change.data);
	}}
};
namespace evolution {int64_t damageProduct(int64_t a,int64_t b,int c=1){return a*b/c;}}
namespace evolution_server {class EvolutionReactions;
static std::vector<int> labelledTraits;
static std::vector<std::string> labelledStatuses;
static void labelTrait(Bonus &,int trait){labelledTraits.push_back(trait);}
static void labelStatus(Bonus &,const std::string & name){labelledStatuses.push_back(name);}
// The model has one faction: species below 100 have a lesser form ten numbers lower.
static CreatureID lesserForm(CreatureID id){return id.getNum()>=20 && id.getNum()<100 ? CreatureID(id.getNum()-10) : id;}
}
struct BattleActionProcessor {CGameHandler * gameHandler;
	uint64_t reactionSerial=0;
	evolution_server::EvolutionReactions * activeReaction=nullptr;
	int64_t evolutionIncoming(const CBattleInfoCallback & battle, uint32_t source, const CStack * target, int64_t damage, bool ranged, bool secondary);
	bool evolutionEnabled() const{return true;}
	bool evolutionThreatened(const CBattleInfoCallback &,const CStack *,BattleHex);
	bool evolutionMoved(const CBattleInfoCallback &,const CStack *,BattleHex origin={});
	void evolutionDefended(const CBattleInfoCallback &,const CStack *);
	void evolutionDamage(const CBattleInfoCallback &,const StacksInjured &);
	void evolutionSpell(const CBattleInfoCallback &,const BattleSpellCast &);
	void withEvolution(const CBattleInfoCallback &,const std::function<void(evolution_server::EvolutionReactions &)> &);
	void makeAttack(const CBattleInfoCallback & battle,const CStack * from,const CStack * to,int,BattleHex,bool,bool,bool){
		auto & target=gameHandler->field.units.at(to->unitId());
		if(!strikeDamage)
		{
			target.data.health=0;
			evolutionAttack(battle,from,to,{{{to->unitId(),from->unitId(),10,true,1}}});
			return;
		}
		const int dealt=std::min(strikeDamage,target.data.health);
		const int before=target.getCount();
		target.data.health-=dealt;
		evolutionAttack(battle,from,to,{{{to->unitId(),from->unitId(),dealt,!target.alive(),before-target.getCount()}}});
	}
	// Zero keeps the historical model in which every reactive strike is lethal.
	int strikeDamage=0;
	void moveStack(const CBattleInfoCallback &,int id,BattleHex h){gameHandler->field.units.at(id).data.pos=h;}
	void evolutionRound(const CBattleInfoCallback &,bool);
	void evolutionAttack(const CBattleInfoCallback &,const CStack *,const CStack *,const BattleAttack &);
};
#include "../../server/battles/EvolutionReactions.inc"

static std::vector<int> randomRun(unsigned seed)
{
	std::mt19937 random(seed);
	// Every battle mutation of the active catalog.
	const std::vector<int> traits{1,2,3,4,5,6,7,8,9,10,12,13,14,15,17,18,19,20,21,22,23,24,25,26,27,28,29,30,32,33,34,35,39,40,42,43,45,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,75,77,78,80,81,82,83,85,87,89,90,91,92,94,95,97,99,100};
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	for(int id=1;id<=4;++id)
	{
		auto & unit=field.units.emplace(id,CStack{id,{id,6,60,{id*4},id<3?BattleSide::LEFT:BattleSide::RIGHT},{}}).first->second;
		for(int pick=0;pick<5;++pick)unit.bonuses.push_back(trait(traits[random()%traits.size()]));
	}
	actions.evolutionRound(field,true);
	for(int step=0;step<90;++step)
	{
		std::vector<int> left,right;
		std::set<int> occupied;
		for(const auto & entry:field.units)
		{
			const auto & unit=entry.second;
			assert(unit.data.health>=0 && unit.id==entry.first);
			if(!unit.alive()) continue;
			assert(unit.getPosition().isValid() && occupied.insert(unit.getPosition().pos).second);
			(unit.unitSide()==BattleSide::LEFT?left:right).push_back(unit.id);
		}
		if(left.empty()||right.empty()) break;
		if(step%2) left.swap(right);
		if(step%4==1)
		{
			// Walk to a free cell: trails, traps, spores, territory and guards all react to this.
			auto & walker=field.units.at(left[random()%left.size()]);
			std::vector<int> open;
			for(int cell=0;cell<BattleHex::fieldSize;++cell) if(!occupied.count(cell)) open.push_back(cell);
			if(!open.empty())
			{
				const BattleHex origin=walker.getPosition();
				walker.data.pos={open[random()%open.size()]};
				actions.evolutionMoved(field,&walker,origin);
			}
			continue;
		}
		if(step%7==3)
		{
			actions.evolutionDefended(field,&field.units.at(left[random()%left.size()]));
			continue;
		}
		auto & from=field.units.at(left[random()%left.size()]);
		auto & to=field.units.at(right[random()%right.size()]);
		const auto before=to.getCount();
		const auto damage=std::min(to.data.health,static_cast<int>(1+random()%36));
		to.data.health-=damage;
		actions.evolutionAttack(field,&from,&to,{{{to.id,from.id,damage,!to.alive(),before-to.getCount()}}});
		if(step%3==2)
		{
			++field.round;
			for(auto & mark:field.marks) if(mark->turnsRemaining>0) --mark->turnsRemaining;
			actions.evolutionRound(field,false);
			field.marks.erase(std::remove_if(field.marks.begin(),field.marks.end(),[](const auto & mark){return mark->turnsRemaining==0;}),field.marks.end());
		}
		assert(field.units.size()<400 && field.marks.size()<=static_cast<size_t>(BattleHex::fieldSize)*10);
	}
	std::vector<int> fingerprint;
	for(const auto & mark:field.marks) fingerprint.insert(fingerprint.end(),{mark->uniqueID,mark->pos.pos,mark->spellLevel,mark->casterSpellPower,mark->turnsRemaining});
	for(const auto & entry:field.units)
	{
		const auto & u=entry.second;
		fingerprint.insert(fingerprint.end(),{u.id,u.data.species,u.data.health,u.data.pos.pos,static_cast<int>(u.data.side)});
		for(const auto & b:u.bonuses) fingerprint.insert(fingerprint.end(),{static_cast<int>(b.type),b.subtype.getNum(),b.val});
	}
	return fingerprint;
}

static void defenseAndConversion()
{
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	auto & shooter=field.units.emplace(1,CStack{1,{11,10,100,{1},BattleSide::LEFT},{trait(89)}}).first->second;
	auto & bearer=field.units.emplace(2,CStack{2,{22,10,100,{2},BattleSide::RIGHT},{trait(47),trait(14)}}).first->second;
	auto hit=[&](const CStack & a,const CStack & b,int64_t n,bool ranged,bool secondary=false){
		return actions.evolutionIncoming(field,a.id,&b,n,ranged,secondary);
	};
	assert(hit(shooter,bearer,7000000000LL,true)==0);
	assert(hit(bearer,shooter,10,false,true)==10);
	assert(hit(bearer,shooter,10,false)==7000000010LL);
	assert(hit(bearer,shooter,10,false)==10);
	for(int i=0;i<3;++i)
	{
		assert(hit(shooter,bearer,20,true)==8);
		actions.evolutionAttack(field,&shooter,&bearer,{{{2,1,8,false,0}}});
	}
	assert(field.units.size()==3 && field.units.at(3).unitSide()==bearer.unitSide());
	assert(hit(shooter,bearer,20,false)==20);
	++field.round;
	assert(hit(shooter,bearer,INT64_MAX,true)==0);
	assert(hit(bearer,shooter,2,false)==INT64_MAX);
	assert(hit(shooter,bearer,20,false)==8);
	bearer.data.health=0;
	actions.evolutionAttack(field,&shooter,&bearer,{{{2,1,100,true,10}}});
	const auto & converted=field.units.at(4);
	assert(converted.unitSide()==shooter.unitSide() && converted.creatureId()==bearer.creatureId());
	assert(converted.getCount()==5 && converted.hasBonusOfType(BonusType::EVOLUTION_TRAIT));
}
static void parasiteBurstAndGenerations()
{
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	auto & parent=field.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{trait(9),trait(54),trait(97)}}).first->second;
	auto & prey=field.units.emplace(2,CStack{2,{22,5,50,{5},BattleSide::RIGHT},{}}).first->second;
	auto & guard=field.units.emplace(3,CStack{3,{33,4,40,{6},BattleSide::RIGHT},{}}).first->second;
	actions.evolutionAttack(field,&parent,&prey,{{{2,1,2,false,0}}});
	prey.data.health=0;
	actions.evolutionAttack(field,&parent,&prey,{{{2,1,50,true,5}}});
	// War of Generations broods on its own: the kill leaves an egg, the burst adds a larva beside it.
	assert(field.units.size()==5);
	auto & egg=field.units.at(4);
	assert(egg.data.species==500 && egg.data.pos.pos==5);
	auto & larva=field.units.at(5);
	assert(larva.data.pos.pos==4 && larva.unitSide()==parent.unitSide());
	assert(larva.bonuses.size()>=3 && !larva.getBonusesOfType(BonusType::EVOLUTION_TRAIT,97)->empty());
	assert(larva.getBonusesOfType(BonusType::EVOLUTION_TRAIT,54)->empty());
	// Fill both adjacent cells and kill another infected stack in the same location.
	egg.data.health=0;
	auto & second=field.units.emplace(6,CStack{6,{44,5,50,{5},BattleSide::RIGHT},{}}).first->second;
	actions.evolutionAttack(field,&parent,&second,{{{6,1,2,false,0}}});
	second.data.health=0;
	actions.evolutionAttack(field,&parent,&second,{{{6,1,50,true,5}}});
	assert(guard.data.health==30 && larva.data.health==10 && field.units.size()==7 && field.units.at(7).data.species==500);
	parent.data.health=0;
	actions.evolutionAttack(field,&guard,&parent,{{{1,3,100,true,10}}});
	assert(!larva.getBonusesOfType(BonusType::EVOLUTION_TRAIT,54)->empty());
}
#include "../../server/battles/EvolutionDeployment.inc"

static void growingEnemyArmies()
{
	std::vector<evolution::EnemyTrait> pool{{1,0},{14,0},{47,1},{54,1},{75,2},{89,3}};
	std::set<int> carriers;
	for(unsigned seed=0;seed<120;++seed)
	{
		size_t previous=0;
		for(int wave:{1,2,5,10,15,30,54,55,60,100,1000})
		{
			auto deal=evolution::enemyMutations(wave,seed,pool);
			assert(deal==evolution::enemyMutations(wave,seed,pool));
			assert(deal.size()==static_cast<size_t>(std::min(30,3+(wave-1)/2)));
			size_t total=0;
			for(size_t i=0;i<deal.size();++i)
			{
				assert(std::set<int>(deal[i].begin(),deal[i].end()).size()==deal[i].size());
				if(wave==2 && !deal[i].empty()) carriers.insert(static_cast<int>(i));
				total+=deal[i].size();
			}
			assert(total>=previous && total==std::min(deal.size()*pool.size(),wave==1 ? size_t(0) : size_t(wave/2)));
			previous=total;
		}
	}
	assert(carriers.size()==3);
	// The extra companies surround the centre: left and right edges alternate and no preferred cell repeats.
	std::set<std::pair<int,int>> cells;
	for(int index=0;index<23;++index)
	{
		const auto cell=evolution_server::companyCell(index);
		assert(cells.insert(cell).second && cell.second>=0 && cell.second<=10);
		assert(index%2 ? (cell.first==14 || cell.first==12) : (cell.first==2 || cell.first==4));
	}
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleHex::fieldSize=30;
	for(int i=0;i<7;++i)
		field.units.emplace(i+1,CStack{i+1,{7,3,30,{i},BattleSide::RIGHT},{}});
	for(int i=7;i<30;++i)
		assert(evolution_server::deployCompany(server,field,BattleSide::RIGHT,CreatureID(7),3,{12},{trait(47)}));
	assert(!evolution_server::deployCompany(server,field,BattleSide::RIGHT,CreatureID(7),3,{12},{}));
	assert(field.units.size()==30);
	BattleHex::fieldSize=21;
	std::set<int> occupied;
	for(const auto & entry:field.units)
	{
		const auto & stack=entry.second;
		assert(occupied.insert(stack.getPosition().pos).second);
		assert(stack.getCount()==3 && stack.unitSide()==BattleSide::RIGHT );
		if(stack.base) continue;
		assert(!stack.getBonusesOfType(BonusType::EVOLUTION_TRAIT,47)->empty());
		assert(!stack.getBonusesOfType(BonusType::NONE,500701)->empty());
	}
}

static void adaptiveDamageAndDebt()
{
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	auto & a=field.units.emplace(1,CStack{1,{11,10,100,{1},BattleSide::LEFT},{trait(66)}}).first->second;
	auto & b=field.units.emplace(2,CStack{2,{22,10,100,{2},BattleSide::RIGHT},{trait(53),trait(73)}}).first->second;
	auto hit=[&](int64_t n,bool shot=false){return actions.evolutionIncoming(field,a.id,&b,n,shot,false);};
	assert(hit(40)==40 && hit(40)==30 && hit(40,true)==50);
	actions.evolutionAttack(field,&a,&b,{{{2,1,40,false,0}}});
	assert(hit(30)==50); // 30 + 10 stored, then 25% for switching channel
	assert(hit(200)==0 && hit(200)==150);
	a.data.health=0;
	++field.round;
	actions.evolutionRound(field,false);
	assert(b.data.health==50);
	actions.evolutionRound(field,false);
	assert(b.data.health==50);
	a.data.health=100;
	actions.evolutionAttack(field,&b,&a,{{{1,2,32,false,0}}});
	assert(hit(32)==30); // defensive round: 32 + 8 stored, then resistance
	assert(hit(100)==0);
	++field.round;
	actions.evolutionRound(field,false);
	assert(!b.alive());
}

static bool carries(const CStack & unit,BonusType type,int subtype,int value)
{
	const auto marks=subtype ? unit.getBonusesOfType(type,subtype) : unit.getBonusesOfType(type);
	return std::any_of(marks->begin(),marks->end(),[&](const auto & mark){return mark->val==value;});
}

static void deathStrike()
{
	// A lethal creature hit is held back, the bearer answers at full strength, then dies.
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	actions.strikeDamage=30;
	auto & bearer=field.units.emplace(1,CStack{1,{11,10,100,{4},BattleSide::LEFT},{trait(2)}}).first->second;
	auto & killer=field.units.emplace(2,CStack{2,{22,10,100,{5},BattleSide::RIGHT},{trait(68)}}).first->second;
	assert(actions.evolutionIncoming(field,2,&bearer,40,false,false)==40);
	assert(actions.evolutionIncoming(field,2,&bearer,500,false,false)==0);
	assert(bearer.alive() && carries(bearer,BonusType::NONE,500002,1));
	actions.evolutionAttack(field,&killer,&bearer,{{{1,2,0,false,0}}});
	assert(!bearer.alive() && killer.data.health==70);
	// The original killer keeps the kill: its Recursive Hunter raises a ghost.
	assert(field.units.size()==3 && field.units.at(3).unitSide()==BattleSide::RIGHT);
	assert(bearer.getBonusesOfType(BonusType::NONE,500402)->empty() && bearer.getBonusesOfType(BonusType::NONE,500102)->empty());
	// Once per battle: a revived bearer takes the next lethal hit normally.
	bearer.data.health=20;
	assert(actions.evolutionIncoming(field,2,&bearer,500,false,false)==500);

	// Two bearers cannot bounce the strike forever.
	CBattleInfoCallback duel;
	CGameHandler duelServer{duel,{}};
	BattleActionProcessor duelActions{&duelServer};
	auto & first=duel.units.emplace(1,CStack{1,{11,10,100,{4},BattleSide::LEFT},{trait(2)}}).first->second;
	auto & second=duel.units.emplace(2,CStack{2,{22,10,100,{5},BattleSide::RIGHT},{trait(2)}}).first->second;
	assert(duelActions.evolutionIncoming(duel,1,&second,500,false,false)==0);
	duelActions.evolutionAttack(duel,&first,&second,{{{2,1,0,false,0}}});
	assert(!first.alive() && !second.alive());

	// Reaction damage: a lethal spark lets the bearer hit the spark's owner first.
	CBattleInfoCallback sparks;
	CGameHandler sparkServer{sparks,{}};
	BattleActionProcessor sparkActions{&sparkServer};
	auto & caster=sparks.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{trait(23)}}).first->second;
	auto & front=sparks.units.emplace(2,CStack{2,{22,10,100,{3},BattleSide::RIGHT},{}}).first->second;
	auto & frail=sparks.units.emplace(3,CStack{3,{33,1,10,{5},BattleSide::RIGHT},{trait(2)}}).first->second;
	front.data.health=70;
	sparkActions.evolutionAttack(sparks,&caster,&front,{{{2,1,30,false,3}}});
	assert(!caster.alive() && !frail.alive() && front.alive());
	// A spell has no creature to answer, so the bearer simply dies.
	CBattleInfoCallback spell;
	CGameHandler spellServer{spell,{}};
	BattleActionProcessor spellActions{&spellServer};
	auto & doomed=spell.units.emplace(1,CStack{1,{11,10,0,{4},BattleSide::LEFT},{trait(2)}}).first->second;
	auto & witness=spell.units.emplace(2,CStack{2,{22,10,100,{5},BattleSide::RIGHT},{}}).first->second;
	spellActions.evolutionDamage(spell,{1,{{1,-1,100,true,10}}});
	assert(!doomed.alive() && witness.data.health==100);
}

static void packHunt()
{
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	actions.strikeDamage=7;
	auto & leader=field.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{trait(35)}}).first->second;
	auto & second=field.units.emplace(2,CStack{2,{12,10,100,{3},BattleSide::LEFT},{}}).first->second;
	field.units.emplace(3,CStack{3,{13,10,100,{6},BattleSide::LEFT},{}});
	auto & prey=field.units.emplace(4,CStack{4,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
	auto & other=field.units.emplace(5,CStack{5,{23,100,1000,{9},BattleSide::RIGHT},{}}).first->second;
	auto hit=[&](CStack & from,CStack & to){actions.evolutionAttack(field,&from,&to,{{{to.id,from.id,10,false,0}}});};
	hit(leader,prey);
	hit(second,other);
	hit(second,prey);
	assert(prey.data.health==1000 && other.data.health==1000);
	hit(leader,prey);
	assert(prey.data.health==993);
	// One free attack per target per round, however many hits follow.
	for(int i=0;i<4;++i) hit(second,prey);
	assert(prey.data.health==993);
	// Hits keep adding up between rounds. A zero-damage hit and the enemy's own hits do not count.
	++field.round;
	actions.evolutionAttack(field,&leader,&prey,{{{4,1,0,false,0}}});
	hit(prey,leader);
	assert(prey.data.health==993);
	hit(leader,prey);
	assert(prey.data.health==986);
	hit(second,prey);
	hit(second,prey);
	assert(prey.data.health==986);
	// Without a living bearer the pack does not coordinate.
	leader.data.health=0;
	++field.round;
	for(int i=0;i<4;++i) hit(second,prey);
	assert(prey.data.health==986);

	// Alone, the bearer takes the free attack itself on every third hit.
	CBattleInfoCallback solo;
	CGameHandler soloServer{solo,{}};
	BattleActionProcessor soloActions{&soloServer};
	soloActions.strikeDamage=7;
	auto & lone=solo.units.emplace(1,CStack{1,{11,10,100,{4},BattleSide::LEFT},{trait(35)}}).first->second;
	auto & target=solo.units.emplace(2,CStack{2,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
	for(int round=0;round<2;++round)
	{
		soloActions.evolutionAttack(solo,&lone,&target,{{{2,1,10,false,0}}});
		++solo.round;
	}
	assert(target.data.health==1000);
	soloActions.evolutionAttack(solo,&lone,&target,{{{2,1,10,false,0}}});
	assert(target.data.health==993);
}

static void sporesReactorAndDomino()
{
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	auto & grower=field.units.emplace(1,CStack{1,{11,40,400,{2},BattleSide::LEFT},{trait(77),trait(71)}}).first->second;
	auto & host=field.units.emplace(2,CStack{2,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
	auto & far=field.units.emplace(3,CStack{3,{23,100,1000,{9},BattleSide::RIGHT},{}}).first->second;
	auto hit=[&](CStack & from,CStack & to){actions.evolutionAttack(field,&from,&to,{{{to.id,from.id,10,false,0}}});};
	hit(grower,host);
	assert(carries(host,BonusType::NONE,500077,1));
	hit(grower,host);
	assert(carries(host,BonusType::NONE,500177,1) && host.getBonusesOfType(BonusType::STACKS_SPEED)->front()->val==-2);
	hit(grower,host);
	assert(carries(host,BonusType::EVOLUTION_WOUND,0,1));
	hit(grower,host);
	assert(carries(host,BonusType::NONE,500062,1));
	for(int i=0;i<8;++i) hit(grower,host);
	assert(host.getBonusesOfType(BonusType::STACKS_SPEED)->size()==1 && host.bonuses.size()==5);
	// The wound and the poison each tick for 10 per round. Every third tick releases 2 x 30 at all enemies.
	++field.round;
	actions.evolutionRound(field,false);
	assert(host.data.health==980 && far.data.health==1000);
	++field.round;
	actions.evolutionRound(field,false);
	assert(host.data.health==900 && far.data.health==940);
	++field.round;
	actions.evolutionRound(field,false);
	assert(host.data.health==820 && far.data.health==880 && grower.data.health==400);
	assert(grower.getBonusesOfType(BonusType::NONE,500071)->empty() && grower.getBonusesOfType(BonusType::NONE,500471)->empty());

	// The reactor works alone: its bearer's hits poison, and three ticks of that poison fire it.
	{
		CBattleInfoCallback lone;
		CGameHandler loneServer{lone,{}};
		BattleActionProcessor loneActions{&loneServer};
		auto & core=lone.units.emplace(1,CStack{1,{11,40,400,{2},BattleSide::LEFT},{trait(71)}}).first->second;
		auto & victim=lone.units.emplace(2,CStack{2,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
		loneActions.evolutionAttack(lone,&core,&victim,{{{2,1,10,false,0}}});
		assert(carries(victim,BonusType::NONE,500077,1));
		for(int i=0;i<3;++i){++lone.round;loneActions.evolutionRound(lone,false);}
		assert(victim.data.health==1000-30-60);
	}
	// Domino Law copies one mark to the nearest enemy once a body carries two kinds.
	CBattleInfoCallback chain;
	CGameHandler chainServer{chain,{}};
	BattleActionProcessor chainActions{&chainServer};
	auto & plague=chain.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{trait(9),trait(19),trait(82)}}).first->second;
	auto & a=chain.units.emplace(2,CStack{2,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
	auto & b=chain.units.emplace(3,CStack{3,{23,100,1000,{6},BattleSide::RIGHT},{}}).first->second;
	auto & c=chain.units.emplace(4,CStack{4,{24,100,1000,{9},BattleSide::RIGHT},{}}).first->second;
	auto & friendly=chain.units.emplace(5,CStack{5,{12,10,100,{1},BattleSide::LEFT},{}}).first->second;
	chainActions.evolutionAttack(chain,&plague,&a,{{{2,1,10,false,0}}});
	assert(carries(a,BonusType::EVOLUTION_PARASITE,0,1) && carries(a,BonusType::EVOLUTION_WOUND,0,1) && carries(a,BonusType::NONE,500177,1));
	// The parasite fell first, then the bearer's own slowing mark pushed the wound on, and b passed the parasite to c.
	assert(carries(b,BonusType::EVOLUTION_PARASITE,0,1) && carries(b,BonusType::EVOLUTION_WOUND,0,1) && !carries(b,BonusType::NONE,500177,1));
	assert(carries(c,BonusType::EVOLUTION_PARASITE,0,1) && !c.hasBonusOfType(BonusType::EVOLUTION_WOUND) && friendly.bonuses.empty());
	assert(plague.getBonusesOfType(BonusType::NONE,500501)->empty());
	// Memory Infection links bodies that share a mark. A mark that arrived by domino reveals the combination.
	plague.bonuses.push_back(trait(48));
	plague.bonuses.push_back(trait(62));
	chainActions.evolutionAttack(chain,&plague,&a,{{{2,1,10,false,0}}});
	assert(carries(b,BonusType::NONE,500062,1) && carries(b,BonusType::EVOLUTION_WOUND,0,1));
	assert(carries(c,BonusType::EVOLUTION_PARASITE,0,1));
	assert(carries(plague,BonusType::NONE,500501,4));
	assert(friendly.bonuses.empty());
	// Memory Infection repeats a third of the bearer's damage on every body that shares a mark with the target.
	assert(a.data.health==1000 && b.data.health==997 && c.data.health==997);
	// Saturated bodies stop the cascade: repeating the attack changes nothing.
	std::vector<size_t> before;
	for(const auto & entry:chain.units) before.push_back(entry.second.bonuses.size());
	chainActions.evolutionAttack(chain,&plague,&a,{{{2,1,10,false,0}}});
	std::vector<size_t> after;
	for(const auto & entry:chain.units) after.push_back(entry.second.bonuses.size());
	assert(before==after && b.data.health==994 && c.data.health==994);

	// Alone, Domino Law slows on the first hit and poisons on the second, which starts the chain.
	CBattleInfoCallback bare;
	CGameHandler bareServer{bare,{}};
	BattleActionProcessor bareActions{&bareServer};
	auto & pusher=bare.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{trait(82)}}).first->second;
	auto & near=bare.units.emplace(2,CStack{2,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
	auto & next=bare.units.emplace(3,CStack{3,{23,100,1000,{6},BattleSide::RIGHT},{}}).first->second;
	bareActions.evolutionAttack(bare,&pusher,&near,{{{2,1,10,false,0}}});
	assert(carries(near,BonusType::NONE,500177,1) && next.bonuses.empty());
	bareActions.evolutionAttack(bare,&pusher,&near,{{{2,1,10,false,0}}});
	assert(carries(near,BonusType::NONE,500077,1) && carries(next,BonusType::NONE,500077,1));

	// Alone, Memory Infection marks what its bearer hits and echoes damage between marked bodies.
	CBattleInfoCallback mind;
	CGameHandler mindServer{mind,{}};
	BattleActionProcessor mindActions{&mindServer};
	auto & thinker=mind.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{trait(48)}}).first->second;
	auto & one=mind.units.emplace(2,CStack{2,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second;
	auto & two=mind.units.emplace(3,CStack{3,{23,100,1000,{6},BattleSide::RIGHT},{}}).first->second;
	auto & three=mind.units.emplace(4,CStack{4,{24,100,1000,{9},BattleSide::RIGHT},{}}).first->second;
	mindActions.evolutionAttack(mind,&thinker,&one,{{{2,1,30,false,0}}});
	assert(carries(one,BonusType::NONE,500148,1) && two.data.health==1000);
	mindActions.evolutionAttack(mind,&thinker,&two,{{{3,1,30,false,0}}});
	assert(one.data.health==990 && three.data.health==1000);
	mindActions.evolutionAttack(mind,&thinker,&one,{{{2,1,30,false,0}}});
	assert(two.data.health==990 && three.data.health==1000);
}

// The engine ages obstacles when the round packet is applied, runs the reactions, then removes expired ones.
static void nextRound(CBattleInfoCallback & field,BattleActionProcessor & actions)
{
	++field.round;
	for(auto & mark:field.marks) if(mark->turnsRemaining>0) --mark->turnsRemaining;
	actions.evolutionRound(field,false);
	field.marks.erase(std::remove_if(field.marks.begin(),field.marks.end(),[](const auto & mark){return mark->turnsRemaining==0;}),field.marks.end());
}

static int marksOf(const CBattleInfoCallback & field,int kind,int pos=-1)
{
	int found=0;
	for(const auto & mark:field.marks) if(mark->spellLevel==kind && (pos<0 || mark->pos.pos==pos)) ++found;
	return found;
}

static void groundMarks()
{
	// Blood Trail.
	{
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		auto & bearer=field.units.emplace(1,CStack{1,{11,10,100,{5},BattleSide::LEFT},{trait(4)}}).first->second;
		auto & ally=field.units.emplace(2,CStack{2,{12,10,100,{2},BattleSide::LEFT},{}}).first->second;
		auto & enemy=field.units.emplace(3,CStack{3,{22,10,100,{12},BattleSide::RIGHT},{}}).first->second;
		assert(!actions.evolutionMoved(field,&bearer,{5}) && field.marks.empty());
		bearer.data.pos={7};
		assert(!actions.evolutionMoved(field,&bearer,{5}) && marksOf(field,4,5)==1);
		assert(!actions.evolutionMoved(field,&bearer,{5}) && field.marks.size()==1);
		ally.data.pos={5};
		assert(!actions.evolutionMoved(field,&ally,{2}) && ally.data.health==100 && field.marks.size()==1);
		ally.data.pos={2};
		enemy.data.pos={5};
		assert(!actions.evolutionMoved(field,&enemy,{12}));
		assert(enemy.data.health==95 && carries(enemy,BonusType::EVOLUTION_WOUND,0,1) && field.marks.empty());
		// An untouched trail dries out after five rounds.
		bearer.data.pos={8};
		actions.evolutionMoved(field,&bearer,{7});
		for(int i=0;i<4;++i) nextRound(field,actions);
		assert(marksOf(field,4,7)==1);
		nextRound(field,actions);
		// The wound from the first trail bled twice per round in the meantime.
		assert(field.marks.empty() && enemy.data.health==85);
		// The trail outlives its owner but can no longer hurt anyone.
		bearer.data.pos={9};
		actions.evolutionMoved(field,&bearer,{8});
		field.units.erase(1);
		enemy.data.pos={8};
		actions.evolutionMoved(field,&enemy,{5});
		assert(enemy.data.health==85 && field.marks.empty());
	}
	// Living Trap.
	{
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		auto & trapper=field.units.emplace(1,CStack{1,{11,10,100,{7},BattleSide::LEFT},{trait(6)}}).first->second;
		auto & plain=field.units.emplace(2,CStack{2,{12,10,100,{2},BattleSide::LEFT},{}}).first->second;
		auto & enemy=field.units.emplace(3,CStack{3,{22,10,100,{12},BattleSide::RIGHT},{}}).first->second;
		actions.evolutionDefended(field,&plain);
		assert(field.marks.empty());
		actions.evolutionDefended(field,&trapper);
		assert(marksOf(field,6,8)==1 && field.marks.size()==1 && field.marks.front()->turnsRemaining==-1);
		actions.evolutionDefended(field,&trapper);
		assert(marksOf(field,6,6)==1 && field.marks.size()==2);
		actions.evolutionDefended(field,&trapper);
		assert(field.marks.size()==2);
		for(int i=0;i<8;++i) nextRound(field,actions);
		assert(field.marks.size()==2);
		enemy.data.pos={8};
		assert(actions.evolutionMoved(field,&enemy,{12}));
		assert(enemy.data.health==90 && marksOf(field,6,8)==0 && marksOf(field,6,6)==1);
		assert(!actions.evolutionMoved(field,&enemy,{12}));
		(void)trapper;
	}
	// Spore Cloud.
	{
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		auto & bearer=field.units.emplace(1,CStack{1,{11,30,300,{10},BattleSide::LEFT},{trait(13)}}).first->second;
		auto & first=field.units.emplace(2,CStack{2,{22,10,100,{9},BattleSide::RIGHT},{}}).first->second;
		auto & second=field.units.emplace(3,CStack{3,{23,10,100,{14},BattleSide::RIGHT},{}}).first->second;
		actions.evolutionAttack(field,&first,&bearer,{{{1,2,0,false,0}}});
		assert(field.marks.empty());
		actions.evolutionAttack(field,&first,&bearer,{{{1,2,10,false,1}}});
		assert(marksOf(field,13,11)==1 && field.marks.size()==1);
		assert(!first.hasBonusOfType(BonusType::EVOLUTION_PARASITE));
		// Attacking from inside the cloud infects the attacker with the bearer's parasite.
		second.data.pos={11};
		actions.evolutionMoved(field,&second,{14});
		assert(!second.hasBonusOfType(BonusType::EVOLUTION_PARASITE));
		actions.evolutionAttack(field,&second,&bearer,{{{1,3,10,false,1}}});
		assert(carries(second,BonusType::EVOLUTION_PARASITE,0,1) && field.marks.size()==1);
		// The parasite then feeds the bearer every round.
		bearer.data.health=210;
		nextRound(field,actions);
		assert(second.data.health==93 && bearer.data.health==217);
		nextRound(field,actions);
		assert(field.marks.empty());
	}
	// Mycelium.
	{
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		auto & grower=field.units.emplace(1,CStack{1,{11,10,100,{5},BattleSide::LEFT},{trait(17)}}).first->second;
		auto & ally=field.units.emplace(2,CStack{2,{12,10,50,{2},BattleSide::LEFT},{}}).first->second;
		auto & enemy=field.units.emplace(3,CStack{3,{22,10,100,{12},BattleSide::RIGHT},{}}).first->second;
		grower.data.pos={7};
		actions.evolutionMoved(field,&grower,{5});
		assert(marksOf(field,17,5)==1);
		ally.data.pos={5};
		actions.evolutionMoved(field,&ally,{2});
		assert(ally.data.health==50);
		nextRound(field,actions);
		assert(ally.data.health==60 && ally.bonuses.empty());
		ally.data.pos={2};
		enemy.data.pos={5};
		actions.evolutionMoved(field,&enemy,{12});
		assert(enemy.bonuses.empty());
		nextRound(field,actions);
		assert(carries(enemy,BonusType::NONE,500077,1) && enemy.data.health==98);
		nextRound(field,actions);
		assert(field.marks.empty() && enemy.data.health==96);
	}
	// Territorial Predator.
	{
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		actions.strikeDamage=7;
		auto & predator=field.units.emplace(1,CStack{1,{11,10,100,{5},BattleSide::LEFT},{trait(24)}}).first->second;
		auto & prey=field.units.emplace(2,CStack{2,{22,1,0,{6},BattleSide::RIGHT},{}}).first->second;
		auto & second=field.units.emplace(3,CStack{3,{23,10,100,{12},BattleSide::RIGHT},{}}).first->second;
		auto & third=field.units.emplace(4,CStack{4,{24,10,100,{15},BattleSide::RIGHT},{}}).first->second;
		assert(!actions.evolutionThreatened(field,&second,{7}));
		actions.evolutionAttack(field,&predator,&prey,{{{2,1,10,true,1}}});
		assert(marksOf(field,24,6)==1 && field.marks.front()->turnsRemaining==-1);
		assert(actions.evolutionThreatened(field,&second,{7}) && !actions.evolutionThreatened(field,&second,{8}));
		assert(!actions.evolutionThreatened(field,&predator,{7}));
		second.data.pos={7};
		actions.evolutionMoved(field,&second,{12});
		assert(second.data.health==93);
		// One free strike per round.
		second.data.pos={12};
		third.data.pos={7};
		actions.evolutionMoved(field,&third,{15});
		assert(third.data.health==100);
		nextRound(field,actions);
		actions.evolutionMoved(field,&third,{15});
		assert(third.data.health==93);
		// The predator must stand on its ground.
		nextRound(field,actions);
		predator.data.pos={1};
		actions.evolutionMoved(field,&third,{15});
		assert(third.data.health==93);
	}
}

static int traitCount(const CStack & unit){return static_cast<int>(unit.getBonusesOfType(BonusType::EVOLUTION_TRAIT)->size());}

// Every mutation here is the only one its bearer owns and still has to change the battle.
static void playableAlone()
{
	struct Arena {
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		CStack & bearer;
		CStack & ally;
		CStack & enemy;
		CStack & neighbour;
		explicit Arena(int mutation)
			:bearer(field.units.emplace(1,CStack{1,{11,10,100,{4},BattleSide::LEFT},{trait(mutation)}}).first->second)
			,ally(field.units.emplace(2,CStack{2,{12,10,100,{3},BattleSide::LEFT},{}}).first->second)
			,enemy(field.units.emplace(3,CStack{3,{22,100,1000,{5},BattleSide::RIGHT},{}}).first->second)
			,neighbour(field.units.emplace(4,CStack{4,{23,100,1000,{6},BattleSide::RIGHT},{}}).first->second){}
		void hit(CStack & from,CStack & to,int killed=0){actions.evolutionAttack(field,&from,&to,{{{to.id,from.id,10,false,killed}}});}
		void round(bool first=false){if(!first)++field.round;actions.evolutionRound(field,first);}
	};
	{
		Arena a(19); // Pulsing Wound bleeds without a second attacker.
		a.hit(a.bearer,a.enemy);
		a.round();
		assert(a.enemy.data.health==998);
	}
	{
		Arena a(8); // Blood Debt answers for the bearer's own dead.
		a.hit(a.enemy,a.bearer,1);
		assert(carries(a.bearer,BonusType::NONE,500008,3));
		a.hit(a.bearer,a.enemy);
		assert(a.enemy.data.health==1000);
		a.hit(a.bearer,a.enemy);
		assert(a.enemy.data.health==995);
	}
	{
		Arena a(26); // Viral Chain brings its own slowing mark to spread.
		a.hit(a.bearer,a.enemy);
		assert(carries(a.enemy,BonusType::NONE,500177,1) && a.neighbour.bonuses.empty());
		a.actions.evolutionMoved(a.field,&a.enemy);
		assert(carries(a.neighbour,BonusType::NONE,500177,1) && a.neighbour.hasBonusOfType(BonusType::STACKS_SPEED));
	}
	{
		Arena a(39); // Infected Remains infects by itself, does not drain, and leaves a cocoon.
		a.hit(a.bearer,a.enemy);
		assert(carries(a.enemy,BonusType::EVOLUTION_PARASITE,0,1));
		a.round();
		assert(a.enemy.data.health==1000 && a.bearer.data.health==100);
		a.enemy.data.health=0;
		a.actions.evolutionAttack(a.field,&a.bearer,&a.enemy,{{{3,1,10,true,0}}});
		assert(a.field.units.size()==5 && a.field.units.at(5).data.species==500);
	}
	for(int colony:{60,75,87,95,97})
	{
		Arena a(colony); // Each colony mutation starts its own brood.
		a.hit(a.bearer,a.enemy,1);
		assert(a.field.units.size()==5 && a.field.units.at(5).data.species==500);
		a.round();
		assert(std::any_of(a.field.units.begin(),a.field.units.end(),[](const auto & u){return u.second.data.species==501;}));
	}
	{
		Arena a(10); // Death Legacy with nothing to leave still mutates the heir.
		a.bearer.data.health=0;
		a.actions.evolutionAttack(a.field,&a.enemy,&a.bearer,{{{1,3,100,true,10}}});
		assert(traitCount(a.ally)==1);
	}
	{
		Arena a(50); // Mutation Exchange without a partner mutates its bearer.
		a.round(true);
		assert(traitCount(a.bearer)==2 && traitCount(a.ally)==0);
	}
	{
		Arena a(61); // The Catalyst provides the acquisition it reacts to.
		a.round(true);
		assert(traitCount(a.bearer)==2);
	}
	{
		Arena a(56); // Hive Mind grows an offshoot of a tenth of the unit.
		a.round(true);
		assert(a.field.units.size()==5 && a.field.units.at(5).getCount()==1);
		assert(a.bearer.hasBonusOfType(BonusType::EVOLUTION_FAMILY) && a.field.units.at(5).hasBonusOfType(BonusType::EVOLUTION_FAMILY));
		a.round();
		assert(a.field.units.size()==5);
	}
}

static void remainsGroup()
{
	struct Field {
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		CStack & add(int id,int species,int count,int health,int pos,BattleSide side,std::vector<int> mutations={})
		{
			auto & unit=field.units.emplace(id,CStack{id,{species,count,health,{pos},side},{}}).first->second;
			for(int m:mutations) unit.bonuses.push_back(trait(m));
			return unit;
		}
		void kill(CStack & killer,CStack & victim)
		{
			const int count=victim.getCount();
			victim.data.health=0;
			actions.evolutionAttack(field,&killer,&victim,{{{victim.id,killer.id,10,true,count}}});
		}
	};
	const auto used=[](const CStack & corpse){return carries(corpse,BonusType::NONE,500905,1);};
	{
		Field f; // Corpse Eater: defending next to a body restores a third of its health pool, once per body.
		auto & eater=f.add(1,11,10,40,4,BattleSide::LEFT,{5});
		auto & near=f.add(2,22,6,0,5,BattleSide::RIGHT);
		auto & far=f.add(3,23,6,0,9,BattleSide::RIGHT);
		f.add(4,24,6,60,12,BattleSide::RIGHT);
		f.actions.evolutionDefended(f.field,&eater);
		assert(eater.data.health==60 && used(near) && !used(far));
		f.actions.evolutionDefended(f.field,&eater);
		assert(eater.data.health==60 && !used(far));
	}
	{
		Field f; // Ash Remains: an ally carries the ash into its next attack, an enemy scatters it.
		auto & hunter=f.add(1,11,10,100,4,BattleSide::LEFT,{12});
		auto & prey=f.add(2,22,6,60,5,BattleSide::RIGHT);
		auto & ally=f.add(3,12,10,100,2,BattleSide::LEFT);
		auto & enemy=f.add(4,23,50,500,9,BattleSide::RIGHT);
		auto & second=f.add(5,24,6,60,7,BattleSide::RIGHT);
		f.kill(hunter,prey);
		assert(marksOf(f.field,12,5)==1);
		ally.data.pos={5};
		assert(!f.actions.evolutionMoved(f.field,&ally,{2}) && f.field.marks.empty());
		assert(f.actions.evolutionIncoming(f.field,3,&enemy,10,false,true)==10);
		assert(f.actions.evolutionIncoming(f.field,3,&enemy,10,false,false)==30);
		assert(f.actions.evolutionIncoming(f.field,3,&enemy,10,false,false)==10);
		ally.data.pos={2};
		f.kill(hunter,second);
		assert(marksOf(f.field,12,7)==1);
		enemy.data.pos={7};
		f.actions.evolutionMoved(f.field,&enemy,{9});
		assert(f.field.marks.empty() && f.actions.evolutionIncoming(f.field,4,&hunter,10,false,false)==10);
	}
	{
		Field f; // Predatory Rebirth: once, at the next round, by using up the body of its own kill.
		auto & bearer=f.add(1,11,9,90,4,BattleSide::LEFT,{49});
		auto & prey=f.add(2,22,6,60,5,BattleSide::RIGHT);
		auto & killer=f.add(3,23,50,500,6,BattleSide::RIGHT);
		f.kill(bearer,prey);
		assert(carries(prey,BonusType::NONE,500049,1) && !used(prey));
		f.kill(killer,bearer);
		assert(!bearer.alive());
		nextRound(f.field,f.actions);
		assert(bearer.data.health==30 && used(prey));
		// A second kill leaves a fresh body, but the rebirth is spent.
		auto & another=f.add(4,24,6,60,3,BattleSide::RIGHT);
		f.kill(bearer,another);
		f.kill(killer,bearer);
		nextRound(f.field,f.actions);
		assert(!bearer.alive() && !used(another));
	}
	{
		Field f; // Without a body of its own kill the bearer stays dead.
		auto & bearer=f.add(1,11,9,90,4,BattleSide::LEFT,{49});
		auto & stranger=f.add(2,22,6,0,5,BattleSide::RIGHT);
		auto & killer=f.add(3,23,50,500,6,BattleSide::RIGHT);
		f.kill(killer,bearer);
		nextRound(f.field,f.actions);
		assert(!bearer.alive() && !used(stranger));
	}
	{
		Field f; // Bone Puppet: a quarter of the victim serves for three rounds.
		auto & master=f.add(1,11,10,100,4,BattleSide::LEFT,{52,65});
		auto & prey=f.add(2,22,8,80,5,BattleSide::RIGHT);
		f.add(3,23,50,500,9,BattleSide::RIGHT);
		f.kill(master,prey);
		// The body is used once: the puppet took it, so nothing is left to assimilate.
		assert(f.field.units.size()==4 && used(prey));
		const auto & puppet=f.field.units.at(4);
		assert(puppet.data.species==22 && puppet.getCount()==2 && puppet.unitSide()==BattleSide::LEFT);
		nextRound(f.field,f.actions);
		nextRound(f.field,f.actions);
		assert(f.field.units.count(4));
		nextRound(f.field,f.actions);
		assert(!f.field.units.count(4));
	}
	{
		Field f; // Predatory Assimilation: the remains become fighters of the killer's own kind.
		auto & hunter=f.add(1,11,10,100,4,BattleSide::LEFT,{65});
		auto & prey=f.add(2,22,8,80,5,BattleSide::RIGHT);
		prey.bonuses.push_back({BonusType::FLYING,1});
		f.add(3,23,50,500,9,BattleSide::RIGHT);
		f.kill(hunter,prey);
		assert(f.field.units.size()==4 && used(prey));
		const auto & kin=f.field.units.at(4);
		assert(kin.data.species==11 && kin.getCount()==2 && kin.unitSide()==BattleSide::LEFT && kin.hasBonusOfType(BonusType::FLYING));
	}
	{
		Field f; // Lord of Remains: one body per round, nearest first, half its size in skeletons.
		auto & lord=f.add(1,11,10,100,2,BattleSide::LEFT,{72});
		auto & far=f.add(2,22,8,0,9,BattleSide::RIGHT);
		auto & near=f.add(3,23,4,0,5,BattleSide::LEFT);
		f.add(4,24,50,500,12,BattleSide::RIGHT);
		nextRound(f.field,f.actions);
		assert(used(near) && !used(far) && f.field.units.at(5).getCount()==2 && f.field.units.at(5).unitSide()==BattleSide::LEFT);
		nextRound(f.field,f.actions);
		assert(used(far) && f.field.units.at(6).getCount()==4);
		nextRound(f.field,f.actions);
		assert(f.field.units.size()==6);
		(void)lord;
	}
}

static void bloodAndLinks()
{
	struct Field {
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		CStack & add(int id,int count,int health,int pos,BattleSide side,std::vector<int> mutations={})
		{
			auto & unit=field.units.emplace(id,CStack{id,{10+id,count,health,{pos},side},{}}).first->second;
			for(int m:mutations) unit.bonuses.push_back(trait(m));
			return unit;
		}
		void hit(CStack & from,CStack & to,int damage,int killed=0,bool ranged=false)
		{
			BattleAttack packet{{{to.id,from.id,damage,!to.alive(),killed}}};
			packet.ranged=ranged;
			actions.evolutionAttack(field,&from,&to,packet);
		}
		int64_t incoming(CStack & from,CStack & to,int64_t damage){return actions.evolutionIncoming(field,from.id,&to,damage,false,false);}
	};
	{
		Field f; // Blood Cloning: a quarter of dealt damage heals; what does not fit becomes a copy.
		auto & bearer=f.add(1,10,60,4,BattleSide::LEFT,{30});
		auto & enemy=f.add(2,100,1000,5,BattleSide::RIGHT);
		f.hit(bearer,enemy,40);
		assert(bearer.data.health==70 && f.field.units.size()==2);
		bearer.data.health=100;
		f.hit(bearer,enemy,40);
		assert(f.field.units.size()==3 && f.field.units.at(3).getCount()==1 && f.field.units.at(3).unitSide()==BattleSide::LEFT);
		assert(f.field.units.at(3).hasBonusOfType(BonusType::EVOLUTION_FAMILY) && bearer.getBonusesOfType(BonusType::NONE,500330)->empty());
		// A hit that feeds nothing back makes nothing.
		f.actions.evolutionAttack(f.field,&bearer,&enemy,{{{2,1,0,false,0}}});
		assert(f.field.units.size()==3);
	}
	{
		Field f; // Blood Alchemy: spare healing arms a healthy bearer and shields a wounded one.
		auto & bearer=f.add(1,10,100,4,BattleSide::LEFT,{43});
		auto & enemy=f.add(2,100,1000,5,BattleSide::RIGHT);
		f.hit(bearer,enemy,40);
		assert(f.incoming(bearer,enemy,20)==30 && f.incoming(bearer,enemy,20)==20);
		f.hit(bearer,enemy,40);
		bearer.data.health=40;
		assert(f.incoming(bearer,enemy,20)==20);
		assert(f.incoming(enemy,bearer,12)==6);
		assert(f.incoming(enemy,bearer,12)==8 && f.incoming(enemy,bearer,12)==12);
	}
	{
		Field f; // Living Chain: half of a hit on either bound unit lands on the other after the attack.
		auto & bearer=f.add(1,10,100,4,BattleSide::LEFT,{33});
		auto & ally=f.add(2,10,100,3,BattleSide::LEFT);
		auto & third=f.add(3,10,100,1,BattleSide::LEFT);
		auto & enemy=f.add(4,100,1000,6,BattleSide::RIGHT);
		f.actions.evolutionRound(f.field,true);
		assert(f.incoming(enemy,bearer,40)==20 && ally.data.health==100);
		f.hit(enemy,bearer,20);
		assert(ally.data.health==80 && third.data.health==100);
		assert(f.incoming(enemy,ally,40)==20);
		f.hit(enemy,ally,20);
		assert(bearer.data.health==80);
		assert(f.incoming(enemy,third,40)==40);
		// With the partner gone the full hit arrives.
		ally.data.health=0;
		assert(f.incoming(enemy,bearer,40)==40);

		Field solo; // Alone the chain is bound to its bearer and takes a fifth off every hit.
		auto & lone=solo.add(1,10,100,4,BattleSide::LEFT,{33});
		auto & foe=solo.add(2,100,1000,6,BattleSide::RIGHT);
		solo.actions.evolutionRound(solo.field,true);
		assert(solo.incoming(foe,lone,40)==32);
	}
	{
		Field f; // Entwined Fates: the bearer's dead arm the twin; the twin's kills bring a third of them back.
		auto & bearer=f.add(1,10,100,4,BattleSide::LEFT,{69});
		auto & twin=f.add(2,10,100,3,BattleSide::LEFT);
		auto & enemy=f.add(3,100,1000,6,BattleSide::RIGHT);
		auto & weak=f.add(4,1,10,8,BattleSide::RIGHT);
		auto & other=f.add(5,1,10,9,BattleSide::RIGHT);
		f.actions.evolutionRound(f.field,true);
		bearer.data.health=60;
		f.hit(enemy,bearer,40,4);
		assert(f.incoming(twin,enemy,10)==50 && f.incoming(twin,enemy,10)==10);
		// The bearer's own kill does not heal it while it has a twin.
		other.data.health=0;
		f.hit(bearer,other,10,1);
		assert(bearer.data.health==60);
		weak.data.health=0;
		f.hit(twin,weak,10,1);
		assert(bearer.data.health==73);

		Field solo; // Alone the bearer is its own twin.
		auto & lone=solo.add(1,10,100,4,BattleSide::LEFT,{69});
		auto & foe=solo.add(2,100,1000,6,BattleSide::RIGHT);
		auto & prey=solo.add(3,1,10,8,BattleSide::RIGHT);
		solo.actions.evolutionRound(solo.field,true);
		lone.data.health=70;
		solo.hit(foe,lone,30,3);
		assert(solo.incoming(lone,foe,10)==40);
		prey.data.health=0;
		solo.hit(lone,prey,10,1);
		assert(lone.data.health==80);
	}
	{
		Field f; // Shard Breeding: a quarter of a melee hit goes back and the shards build a copy.
		auto & bearer=f.add(1,10,100,4,BattleSide::LEFT,{40});
		auto & enemy=f.add(2,100,1000,5,BattleSide::RIGHT);
		f.hit(enemy,bearer,40,0,true);
		assert(enemy.data.health==1000 && f.field.units.size()==2);
		f.hit(enemy,bearer,40);
		assert(enemy.data.health==990 && f.field.units.size()==3 && f.field.units.at(3).getCount()==1);
		assert(f.field.units.at(3).unitSide()==BattleSide::LEFT);
	}
}

static bool hasTrait(const CStack & unit,int id){return !unit.getBonusesOfType(BonusType::EVOLUTION_TRAIT,id)->empty();}

static void deathAndTransformation()
{
	struct Field {
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		CStack & add(int id,int species,int count,int pos,BattleSide side,std::vector<int> mutations={})
		{
			auto & unit=field.units.emplace(id,CStack{id,{species,count,count*10,{pos},side},{}}).first->second;
			for(int m:mutations) unit.bonuses.push_back(trait(m));
			return unit;
		}
		void kill(CStack & killer,CStack & victim)
		{
			const int count=victim.getCount();
			victim.data.health=0;
			actions.evolutionAttack(field,&killer,&victim,{{{victim.id,killer.id,10,true,count}}});
		}
	};
	{
		Field f; // Fractal Army: two copies of a sixth each keep the mutations; the copies do not fracture again.
		auto & big=f.add(1,30,30,4,BattleSide::LEFT,{64,23});
		auto & enemy=f.add(2,40,100,8,BattleSide::RIGHT);
		f.kill(enemy,big);
		assert(f.field.units.size()==4);
		for(int id:{3,4})
		{
			auto & copy=f.field.units.at(id);
			assert(copy.getCount()==5 && copy.data.species==30 && copy.unitSide()==BattleSide::LEFT && hasTrait(copy,23) && copy.summoned);
		}
		f.field.units.at(3).data.count=12;
		f.kill(enemy,f.field.units.at(3));
		assert(f.field.units.size()==4);
		auto & small=f.add(9,31,5,1,BattleSide::LEFT,{64});
		f.kill(enemy,small);
		assert(f.field.units.size()==5);
	}
	{
		Field f; // Reverse Evolution: one level lower, the same head count, a second strike, once.
		auto & high=f.add(1,30,8,4,BattleSide::LEFT,{28,23});
		auto & low=f.add(2,15,8,1,BattleSide::LEFT,{28});
		auto & enemy=f.add(3,40,100,8,BattleSide::RIGHT);
		f.kill(enemy,high);
		auto & reborn=f.field.units.at(4);
		assert(reborn.data.species==20 && reborn.getCount()==8 && reborn.unitSide()==BattleSide::LEFT);
		assert(hasTrait(reborn,23) && !hasTrait(reborn,28) && reborn.getBonusesOfType(BonusType::ADDITIONAL_ATTACK)->front()->val==1);
		f.kill(enemy,reborn);
		assert(f.field.units.size()==4);
		// A first-level creature has nothing below it: half of the unit returns as it was.
		f.kill(enemy,low);
		assert(f.field.units.at(5).data.species==15 && f.field.units.at(5).getCount()==4);
	}
	{
		Field f; // Soul Split: a third stays as flying ghosts with the other mutations.
		auto & body=f.add(1,30,9,4,BattleSide::LEFT,{42,23});
		auto & enemy=f.add(2,40,100,8,BattleSide::RIGHT);
		f.kill(enemy,body);
		auto & ghost=f.field.units.at(3);
		assert(ghost.getCount()==3 && ghost.data.species==30 && ghost.hasBonusOfType(BonusType::FLYING) && hasTrait(ghost,23) && !hasTrait(ghost,42));
		f.kill(enemy,ghost);
		assert(f.field.units.size()==3);
	}
	{
		Field f; // Mind Transfer: all mutations and half of the health pool as one attack charge.
		auto & mind=f.add(1,30,10,4,BattleSide::LEFT,{55,23,9});
		auto & heir=f.add(2,31,10,3,BattleSide::LEFT);
		auto & far=f.add(3,32,10,0,BattleSide::LEFT);
		auto & enemy=f.add(4,40,100,8,BattleSide::RIGHT);
		f.kill(enemy,mind);
		assert(hasTrait(heir,23) && hasTrait(heir,9) && !hasTrait(heir,55) && far.bonuses.empty());
		assert(f.actions.evolutionIncoming(f.field,2,&enemy,10,false,false)==60);
		assert(f.actions.evolutionIncoming(f.field,2,&enemy,10,false,false)==10);
	}
	{
		Field f; // Rewritten Death: a hostile echo of half the size, won back with a new mutation, once per battle.
		auto & writer=f.add(1,30,10,2,BattleSide::LEFT,{58});
		auto & first=f.add(2,31,8,4,BattleSide::LEFT);
		auto & second=f.add(3,32,8,0,BattleSide::LEFT);
		auto & enemy=f.add(4,40,100,8,BattleSide::RIGHT);
		f.kill(enemy,first);
		auto & echo=f.field.units.at(5);
		assert(echo.unitSide()==BattleSide::RIGHT && echo.data.species==31 && echo.getCount()==4);
		f.kill(enemy,second);
		assert(f.field.units.size()==5);
		f.kill(writer,echo);
		auto & back=f.field.units.at(6);
		assert(back.unitSide()==BattleSide::LEFT && back.data.species==31 && back.getCount()==4);
		assert(back.getBonusesOfType(BonusType::EVOLUTION_TRAIT)->size()==1);
		// The returned unit is an ordinary summoned body: killing it again makes no further echo.
		f.kill(enemy,back);
		assert(f.field.units.size()==6);
	}
	{
		Field f; // Echo of Infinity: each destroyed enemy unit repeats the attack on the nearest enemy.
		f.actions.strikeDamage=7;
		auto & hunter=f.add(1,30,10,2,BattleSide::LEFT,{94});
		auto & weak=f.add(2,40,1,3,BattleSide::RIGHT);
		auto & tough=f.add(3,41,100,5,BattleSide::RIGHT);
		f.kill(hunter,weak);
		assert(tough.data.health==993);
		// A hit that destroys nobody has no echo.
		f.actions.evolutionAttack(f.field,&hunter,&tough,{{{3,1,10,false,1}}});
		assert(tough.data.health==993);

		Field chain; // In the lethal strike model the echo runs through the whole enemy line and then stops.
		auto & reaper=chain.add(1,30,10,1,BattleSide::LEFT,{94});
		for(int i=2;i<=9;++i) chain.add(i,40,1,i+1,BattleSide::RIGHT);
		chain.kill(reaper,chain.field.units.at(2));
		for(int i=2;i<=9;++i) assert(!chain.field.units.at(i).alive());
		assert(reaper.alive());

		Field loop; // A Scavenger raising a body from every death next to it must not feed the echo forever.
		auto & echoer=loop.add(1,30,10,1,BattleSide::LEFT,{94});
		auto & bait=loop.add(2,40,1,3,BattleSide::RIGHT);
		auto & scavenger=loop.add(3,41,10,4,BattleSide::RIGHT,{18});
		scavenger.data.pos={4};
		loop.kill(echoer,bait);
		// The raised body took the echo. Its own death raised one more, which ended the chain.
		assert(loop.field.units.size()==5 && !loop.field.units.at(4).alive() && loop.field.units.at(5).alive() && scavenger.alive());
	}
	{
		Field f; // Shadow Army: every second attack leaves a quarter of the unit for two rounds.
		auto & caster=f.add(1,30,8,4,BattleSide::LEFT,{34,23});
		auto & enemy=f.add(2,40,100,8,BattleSide::RIGHT);
		auto hit=[&]{f.actions.evolutionAttack(f.field,&caster,&enemy,{{{2,1,10,false,0}}});};
		hit();
		assert(f.field.units.size()==2);
		hit();
		auto & shade=f.field.units.at(3);
		assert(shade.getCount()==2 && shade.data.species==30 && shade.unitSide()==BattleSide::LEFT && !hasTrait(shade,34));
		hit();
		assert(f.field.units.size()==3);
		nextRound(f.field,f.actions);
		assert(f.field.units.count(3));
		nextRound(f.field,f.actions);
		assert(!f.field.units.count(3));
	}
}

static void swarmGroup()
{
	struct Field {
		CBattleInfoCallback field;
		CGameHandler server{field,{}};
		BattleActionProcessor actions{&server};
		Field(){actions.strikeDamage=7;}
		CStack & add(int id,int count,int pos,BattleSide side,std::vector<int> mutations={})
		{
			auto & unit=field.units.emplace(id,CStack{id,{10+id,count,count*10,{pos},side},{}}).first->second;
			for(int m:mutations) unit.bonuses.push_back(trait(m));
			return unit;
		}
		void hit(CStack & from,CStack & to,int killed=0){actions.evolutionAttack(field,&from,&to,{{{to.id,from.id,10,false,killed}}});}
		std::vector<const CStack *> larvae() const
		{
			std::vector<const CStack *> found;
			for(const auto & entry:field.units) if(entry.second.alive() && entry.second.data.species==501) found.push_back(&entry.second);
			return found;
		}
	};
	{
		Field f; // Parasitic Mind: infects, broods, and its larvae hunt the infected on their own.
		auto & mother=f.add(1,10,2,BattleSide::LEFT,{29});
		auto & enemy=f.add(2,100,6,BattleSide::RIGHT);
		auto & clean=f.add(3,100,9,BattleSide::RIGHT);
		f.hit(mother,enemy,1);
		assert(carries(enemy,BonusType::EVOLUTION_PARASITE,0,1));
		nextRound(f.field,f.actions);
		assert(f.larvae().size()==1 && carries(*f.larvae().front(),BonusType::NONE,500190,2));
		assert(enemy.data.health==1000);
		nextRound(f.field,f.actions);
		assert(enemy.data.health==993 && clean.data.health==1000 && mother.data.health==100);
		// Without the mother the larva goes back to waiting for orders.
		mother.data.health=0;
		nextRound(f.field,f.actions);
		assert(enemy.data.health==993);
	}
	{
		Field f; // Hungry Colony: separate larva stacks of one brood merge at the start of a round.
		auto & mother=f.add(1,10,2,BattleSide::LEFT,{45,23});
		auto & enemy=f.add(2,100,8,BattleSide::RIGHT);
		f.hit(mother,enemy,1);
		f.hit(mother,enemy,1);
		nextRound(f.field,f.actions);
		const auto colony=f.larvae();
		assert(colony.size()==1 && colony.front()->getCount()==2 && carries(*colony.front(),BonusType::NONE,500190,2));
		// A third larva joins the colony next round.
		f.hit(mother,enemy,1);
		nextRound(f.field,f.actions);
		assert(f.larvae().size()==1 && f.larvae().front()->getCount()==3);
	}
	{
		Field f; // Sentient Plague: a leech that picks the healthiest clean enemy near an infected one.
		auto & bearer=f.add(1,30,2,BattleSide::LEFT,{81});
		auto & first=f.add(2,50,5,BattleSide::RIGHT);
		auto & small=f.add(3,30,6,BattleSide::RIGHT);
		auto & large=f.add(4,90,7,BattleSide::RIGHT);
		auto & far=f.add(5,200,12,BattleSide::RIGHT);
		bearer.data.health=150;
		f.hit(bearer,first);
		assert(carries(first,BonusType::EVOLUTION_PARASITE,0,1) && !large.hasBonusOfType(BonusType::EVOLUTION_PARASITE));
		nextRound(f.field,f.actions);
		assert(carries(large,BonusType::EVOLUTION_PARASITE,0,1) && !small.hasBonusOfType(BonusType::EVOLUTION_PARASITE));
		assert(first.data.health==495 && large.data.health==895 && bearer.data.health==160);
		nextRound(f.field,f.actions);
		assert(carries(small,BonusType::EVOLUTION_PARASITE,0,1) && !far.hasBonusOfType(BonusType::EVOLUTION_PARASITE));
	}
	{
		Field f; // Swarm Mind: the rest of the brood next to the target joins every strike, once per round each.
		auto & queen=f.add(1,10,2,BattleSide::LEFT,{90});
		auto & enemy=f.add(2,100,6,BattleSide::RIGHT);
		auto & left=f.add(3,5,5,BattleSide::LEFT);
		auto & right=f.add(4,5,7,BattleSide::LEFT);
		auto & stranger=f.add(5,5,9,BattleSide::LEFT);
		for(auto * child:{&left,&right}) child->bonuses.push_back({BonusType::NONE,2,BonusDuration::PERMANENT,500190});
		stranger.data.pos={7};
		right.data.pos={7};
		stranger.data.pos={9};
		f.hit(queen,enemy);
		assert(enemy.data.health==986);
		f.hit(queen,enemy);
		assert(enemy.data.health==986);
		// A child's own strike calls the queen's other children too.
		++f.field.round;
		f.hit(left,enemy);
		assert(enemy.data.health==979);
		// The network needs its queen.
		++f.field.round;
		queen.data.health=0;
		f.hit(left,enemy);
		assert(enemy.data.health==979);
	}
	{
		Field f; // Blood Economy: defending trades a tenth of the unit for an immediate attack.
		auto & bearer=f.add(1,20,2,BattleSide::LEFT,{63});
		auto & enemy=f.add(2,100,6,BattleSide::RIGHT);
		auto & single=f.add(3,1,1,BattleSide::LEFT,{63});
		f.actions.evolutionDefended(f.field,&bearer);
		assert(bearer.data.health==180 && enemy.data.health==993);
		f.actions.evolutionDefended(f.field,&single);
		assert(single.data.health==10 && enemy.data.health==993);
	}
}

static void runGrowthMarkers()
{
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	auto add=[&](int id,int count,int pos,BattleSide side,std::vector<int> mutations)->CStack &{
		auto & unit=field.units.emplace(id,CStack{id,{10+id,count,count*10,{pos},side},{}}).first->second;
		for(int m:mutations) unit.bonuses.push_back(trait(m));
		return unit;
	};
	auto kill=[&](CStack & killer,CStack & victim){
		const int count=victim.getCount();
		victim.data.health=0;
		actions.evolutionAttack(field,&killer,&victim,{{{victim.id,killer.id,10,true,count}}});
	};
	// Vengeance Machine counts allied bodies destroyed while it lives, and never its own death.
	auto & avenger=add(1,10,2,BattleSide::LEFT,{80});
	auto & first=add(2,5,3,BattleSide::LEFT,{});
	auto & second=add(3,5,4,BattleSide::LEFT,{});
	auto & enemy=add(4,100,8,BattleSide::RIGHT,{});
	auto & prey=add(5,1,9,BattleSide::RIGHT,{});
	// Evolutionary Leap needs all three deeds in one battle.
	auto & leaper=add(6,10,1,BattleSide::LEFT,{70});
	kill(enemy,first);
	assert(carries(avenger,BonusType::NONE,500080,1));
	kill(enemy,second);
	assert(carries(avenger,BonusType::NONE,500080,2) && avenger.getBonusesOfType(BonusType::NONE,500080)->size()==1);
	kill(leaper,prey);
	assert(carries(leaper,BonusType::NONE,500170,1) && leaper.getBonusesOfType(BonusType::NONE,500270)->empty());
	leaper.data.health=80;
	actions.evolutionAttack(field,&enemy,&leaper,{{{6,4,20,false,2}}});
	assert(carries(leaper,BonusType::NONE,500170,3) && leaper.getBonusesOfType(BonusType::NONE,500270)->empty());
	nextRound(field,actions);
	assert(leaper.getBonusesOfType(BonusType::NONE,500270)->empty());
	nextRound(field,actions);
	assert(carries(leaper,BonusType::NONE,500170,7) && carries(leaper,BonusType::NONE,500270,1));
	kill(enemy,avenger);
	assert(carries(avenger,BonusType::NONE,500080,2));

	// Final Evolution: eight different mutations work one rank higher for the battle; seven do not.
	CBattleInfoCallback apex;
	CGameHandler apexServer{apex,{}};
	BattleActionProcessor apexActions{&apexServer};
	auto & ready=apex.units.emplace(1,CStack{1,{11,10,100,{2},BattleSide::LEFT},{}}).first->second;
	auto & early=apex.units.emplace(2,CStack{2,{12,10,100,{4},BattleSide::LEFT},{}}).first->second;
	apex.units.emplace(3,CStack{3,{13,10,100,{9},BattleSide::RIGHT},{}});
	for(int m:{100,2,9,14,19,23,47,53}) ready.bonuses.push_back(trait(m));
	for(int m:{100,2,9,14,19,23,47}) early.bonuses.push_back(trait(m));
	apexActions.evolutionRound(apex,true);
	for(int m:{2,9,14,19,23,47,53})
	{
		const auto ranks=ready.getBonusesOfType(BonusType::EVOLUTION_TRAIT,m);
		assert(std::any_of(ranks->begin(),ranks->end(),[](const auto & b){return b->val==2;}));
	}
	assert(ready.getBonusesOfType(BonusType::EVOLUTION_TRAIT,100)->size()==1);
	assert(early.getBonusesOfType(BonusType::EVOLUTION_TRAIT)->size()==7);
}

int main(){
	runGrowthMarkers();
	swarmGroup();
	deathAndTransformation();
	bloodAndLinks();
	remainsGroup();
	playableAlone();
	groundMarks();
	deathStrike();
	packHunt();
	sporesReactorAndDomino();
	adaptiveDamageAndDebt();
	growingEnemyArmies();
	parasiteBurstAndGenerations();
	defenseAndConversion();
	for(unsigned seed=1;seed<=400;++seed) assert(randomRun(seed)==randomRun(seed));
	CBattleInfoCallback field;
	auto add=[&](int pos,BattleSide side,std::vector<int> mutations)->CStack &{
		int id=field.battleNextUnitId();
		auto & u=field.units.emplace(id,CStack{id,{id,10,100,{pos},side},{}}).first->second;
		for(int m:mutations)u.bonuses.push_back(trait(m));
		return u;
	};
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	auto & carrier=add(4,BattleSide::LEFT,{9,18,19,23,27,51,87});
	auto & enemy=add(5,BattleSide::RIGHT,{});
	auto & ally=add(3,BattleSide::LEFT,{10,25,57,78});
	auto & secondary=add(7,BattleSide::RIGHT,{});
	actions.evolutionRound(field,true);
	assert(field.units.size()==5);
	// Four inherited mutations, the family mark, one adaptation and the brood mark.
	assert(field.units.at(5).bonuses.size()==7);
	BattleAttack hit{{{enemy.id,carrier.id,30,false}}};
	actions.evolutionAttack(field,&carrier,&enemy,hit);
	assert(secondary.data.health==90 && enemy.data.pos.pos!=5);
	assert(enemy.hasBonusOfType(BonusType::EVOLUTION_PARASITE) && enemy.hasBonusOfType(BonusType::EVOLUTION_WOUND));
	carrier.data.health=72;field.round=3;
	actions.evolutionRound(field,false);
	// The leech takes 2 for the carrier, then the open wound bleeds 2 more.
	assert(carrier.data.health==74 && enemy.data.health==96 && ally.bonuses.size()==6);
	actions.evolutionAttack(field,&ally,&enemy,{{{enemy.id,ally.id,8,false}}});
	assert(enemy.data.health==94);
	enemy.data.pos={5};enemy.data.health=0;enemy.bonuses.push_back({BonusType::FLYING,1});
	actions.evolutionAttack(field,&carrier,&enemy,{{{enemy.id,carrier.id,30,true}}});
	assert(carrier.hasBonusOfType(BonusType::FLYING));
	assert(field.units.size()==6 && field.units.at(6).data.side==BattleSide::LEFT);
	assert(field.units.at(6).bonuses.size()>=7);
	ally.data.health=0;actions.evolutionAttack(field,&secondary,&ally,{{{ally.id,secondary.id,30,true}}});
	assert(std::any_of(carrier.bonuses.begin(),carrier.bonuses.end(),[](Bonus b){return b.type==BonusType::EVOLUTION_TRAIT && b.subtype==25 && b.val==1;}));
	CBattleInfoCallback family;
	CGameHandler familyServer{family,{}};
	BattleActionProcessor familyActions{&familyServer};
	family.units.emplace(1,CStack{1,{1,4,40,{5},BattleSide::LEFT},{}});
	family.units.emplace(2,CStack{2,{2,4,40,{4},BattleSide::LEFT},{}});
	family.units.emplace(3,CStack{3,{3,4,40,{9},BattleSide::RIGHT},{}});
	auto & root=family.units.at(1);
	for(int m:{23,56,59,61,67,78}) root.bonuses.push_back(trait(m));
	family.units.at(2).bonuses.push_back({BonusType::NO_MELEE_PENALTY,1});
	family.units.at(3).bonuses.push_back({BonusType::FLYING,1});
	familyActions.evolutionRound(family,true);
	assert(root.hasBonusOfType(BonusType::NO_MELEE_PENALTY) && root.hasBonusOfType(BonusType::FLYING));
	assert(family.units.at(4).hasBonusOfType(BonusType::EVOLUTION_FAMILY));
	assert(root.getBonusesOfType(BonusType::NONE,500501)->front()->val==1);
	auto rootRules=root.getBonusesOfType(BonusType::EVOLUTION_TRAIT);
	assert(std::any_of(rootRules->begin(),rootRules->end(),[](auto b){return b->subtype==9 && b->val==1;}));
	assert(std::any_of(rootRules->begin(),rootRules->end(),[](auto b){return b->subtype==23 && b->val==2;}));
	CBattleInfoCallback nest;
	CGameHandler nestServer{nest,{}};
	BattleActionProcessor nestActions{&nestServer};
	nest.units.emplace(1,CStack{1,{1,5,50,{5},BattleSide::LEFT},{}});
	nest.units.emplace(2,CStack{2,{2,2,20,{8},BattleSide::RIGHT},{}});
	for(int m:{1,39,60,87}) nest.units.at(1).bonuses.push_back(trait(m));
	nestActions.evolutionAttack(nest,&nest.units.at(1),&nest.units.at(2),{{{2,1,10,false,1}}});
	assert(nest.units.at(3).data.species==500);
	nestActions.evolutionRound(nest,false);
	assert(nest.units.size()==4);
	assert(nest.units.at(1).getBonusesOfType(BonusType::NONE,500501)->front()->val==2);
	assert(std::any_of(nest.units.begin(),nest.units.end(),[](const auto & u){return u.second.data.species==501 && u.second.bonuses.size()>=5;}));
	// A killed egg never hatches. A living carrier can leave a cocoon on an infected corpse.
	for(auto & pair : nest.units) if(pair.second.data.species==500) pair.second.data.health=0;
	auto n=nest.units.size();
	nestActions.evolutionRound(nest,false);
	assert(nest.units.size()==n);
	// Infected Remains infects with its own hits, so the first attack already left the mark.
	assert(nest.units.at(2).getBonusesOfType(BonusType::EVOLUTION_PARASITE)->size()==1);
	nest.units.at(2).data.health=0;
	nestActions.evolutionAttack(nest,&nest.units.at(1),&nest.units.at(2),{{{2,1,10,true,0}}});
	assert(nest.units.size()==n+1);
	for(int i=0;i<250;++i)
	{
		nest.round++;
		nestActions.evolutionRound(nest,false);
		std::vector<int> occupied;
		for(const auto & pair:nest.units)
		{
			if(!pair.second.alive())continue;
			assert(pair.second.unitSide()==BattleSide::LEFT);
			occupied.push_back(pair.second.data.pos.pos);
		}
		std::sort(occupied.begin(),occupied.end());
		assert(std::adjacent_find(occupied.begin(),occupied.end())==occupied.end());
		assert(occupied.size()<=21);
	}
	CBattleInfoCallback hunt;
	CGameHandler huntServer{hunt,{}};
	BattleActionProcessor huntActions{&huntServer};
	hunt.units.emplace(1,CStack{1,{1,5,50,{3},BattleSide::LEFT},{}});
	hunt.units.emplace(2,CStack{2,{2,2,20,{4},BattleSide::RIGHT},{}});
	hunt.units.emplace(3,CStack{3,{3,2,20,{7},BattleSide::RIGHT},{}});
	for(int m:{15,32}) hunt.units.at(1).bonuses.push_back(trait(m));
	huntActions.evolutionAttack(hunt,&hunt.units.at(1),&hunt.units.at(2),{{{2,1,20,false,7}}});
	assert(hunt.units.size()==3);
	hunt.units.at(2).data.health=0;
	huntActions.evolutionAttack(hunt,&hunt.units.at(1),&hunt.units.at(2),{{{2,1,10,true,2}}});
	assert(hunt.units.size()==4 && hunt.units.at(4).getCount()==1);
	assert(hunt.units.at(1).getBonusesOfType(BonusType::NONE,500032)->front()->val==1);
	assert(hunt.units.at(1).data.pos.pos>=3);
	CBattleInfoCallback partition;
	CGameHandler splitServer{partition,{}};
	BattleActionProcessor splitActions{&splitServer};
	partition.units.emplace(1,CStack{1,{1,10,49,{5},BattleSide::LEFT},{}});
	partition.units.emplace(2,CStack{2,{2,4,40,{8},BattleSide::RIGHT},{}});
	for(int m:{3,23}) partition.units.at(1).bonuses.push_back(trait(m));
	splitActions.evolutionAttack(partition,&partition.units.at(2),&partition.units.at(1),{{{1,2,11,false,1}}});
	assert(partition.units.size()==3 && partition.units.at(1).data.health+partition.units.at(3).data.health==49);
	assert(partition.units.at(1).getCount()+partition.units.at(3).getCount()==5);
	assert(partition.units.at(3).getBonusesOfType(BonusType::NONE,500003)->front()->val==1);
	splitActions.evolutionAttack(partition,&partition.units.at(2),&partition.units.at(1),{{{1,2,1,false,1}}});
	assert(partition.units.size()==3);
	evolution::BattleChain longChain(42);
	longChain.post(evolution::ReactionKind::Hunt,1,2);
	uint64_t processed=0;
	longChain.drain([&](const evolution::Reaction & event){
		assert(event.cause==processed && event.serial==processed+1);
		if(++processed<100000) longChain.post(event.kind,event.source,event.target);
	});
	assert(processed==100000 && longChain.allowHunt(1,2,true) && !longChain.allowHunt(1,2,true));
	CBattleInfoCallback ghosts;
	CGameHandler ghostServer{ghosts,{}};
	BattleActionProcessor ghostActions{&ghostServer};
	ghosts.units.emplace(1,CStack{1,{1,1,10,{1},BattleSide::LEFT},{}});
	ghosts.units.at(1).bonuses.push_back(trait(68));
	for(int i=2;i<=9;++i) ghosts.units.emplace(i,CStack{i,{2,1,i==2?0:10,{i+1},BattleSide::RIGHT},{}});
	ghostActions.evolutionAttack(ghosts,&ghosts.units.at(1),&ghosts.units.at(2),{{{2,1,10,true,1}}});
	for(int i=2;i<=9;++i) assert(!ghosts.units.at(i).alive());
	assert(ghostActions.reactionSerial==1);
	CBattleInfoCallback magic;
	CGameHandler magicServer{magic,{}};
	BattleActionProcessor magicActions{&magicServer};
	magic.units.emplace(1,CStack{1,{1,12,120,{4},BattleSide::LEFT},{}});
	magic.units.emplace(2,CStack{2,{2,5,50,{5},BattleSide::RIGHT},{}});
	magic.units.at(1).bonuses.push_back(trait(21));
	magicActions.evolutionSpell(magic,{BattleSide::LEFT,{1},{}});
	magicActions.evolutionSpell(magic,{BattleSide::RIGHT,{1},{1}});
	assert(magic.units.at(1).bonuses.size()==1);
	magicActions.evolutionSpell(magic,{BattleSide::RIGHT,{1},{}});
	magicActions.evolutionAttack(magic,&magic.units.at(1),&magic.units.at(2),{{{2,1,10,false,0}}});
	assert(magic.units.at(2).data.health==38 && magic.units.at(1).bonuses.size()==1);
	magic.units.at(1).bonuses.push_back(trait(18));
	magic.units.at(1).bonuses.push_back(trait(41));
	magic.units.at(2).bonuses.push_back(trait(19));
	magic.units.at(2).data.health=0;
	magicActions.evolutionDamage(magic,{1,{{2,-1,38,true,4}}});
	assert(magic.units.size()==3);
	// A hero spell has no creature killer; it cannot award a personal trophy.
	assert(magic.units.at(1).getBonusesOfType(BonusType::NONE,500041)->empty());
	magic.units.at(2).data.health=10;
	magic.units.at(2).data.pos={8};
	magic.units.at(2).data.health=0;
	magicActions.evolutionAttack(magic,&magic.units.at(1),&magic.units.at(2),{{{2,1,10,true,1}}});
	assert(magic.units.at(1).getBonusesOfType(BonusType::NONE,500041)->front()->val==19);
	CBattleInfoCallback oath;
	CGameHandler oathServer{oath,{}};
	BattleActionProcessor oathActions{&oathServer};
	for(int i=1;i<=4;++i) oath.units.emplace(i,CStack{i,{i,10,100,{i*3},i<4?BattleSide::LEFT:BattleSide::RIGHT},{}});
	oath.units.at(1).bonuses.push_back(trait(8));
	oath.units.at(2).bonuses.push_back(trait(50));
	oath.units.at(2).bonuses.push_back(trait(23));
	oathActions.evolutionRound(oath,true);
	// Unit two has traded its spark for Blood Debt and is the only witness that marks the killer.
	oath.units.at(3).data.health=0;
	oathActions.evolutionAttack(oath,&oath.units.at(4),&oath.units.at(3),{{{3,4,100,true,10}}});
	assert(oath.units.at(1).getBonusesOfType(BonusType::NONE,500008)->empty());
	assert(oath.units.at(2).getBonusesOfType(BonusType::NONE,500008)->size()==1);
	oathActions.evolutionAttack(oath,&oath.units.at(2),&oath.units.at(4),{{{4,2,10,false,0}}});
	assert(oath.units.at(4).data.health==100);
	oathActions.evolutionAttack(oath,&oath.units.at(1),&oath.units.at(4),{{{4,1,10,false,0}}});
	assert(oath.units.at(4).data.health==95);
	CBattleInfoCallback border;
	CGameHandler borderServer{border,{}};
	BattleActionProcessor borderActions{&borderServer};
	for(int i=1;i<=4;++i)border.units.emplace(i,CStack{i,{i,4,40,{i+3},i==1?BattleSide::LEFT:BattleSide::RIGHT},{}});
	border.units.at(1).bonuses.push_back(trait(20));
	assert(borderActions.evolutionThreatened(border,&border.units.at(2),{5}));
	borderActions.evolutionMoved(border,&border.units.at(2));
	assert(!border.units.at(2).alive() && !borderActions.evolutionThreatened(border,&border.units.at(3),{5}));
	border.round++;
	assert(borderActions.evolutionThreatened(border,&border.units.at(3),{5}));
	for(int id:{9,26}) border.units.at(1).bonuses.push_back(trait(id));
	borderActions.evolutionAttack(border,&border.units.at(1),&border.units.at(3),{{{3,1,1,false,0}}});
	borderActions.evolutionMoved(border,&border.units.at(3));
	assert(border.units.at(4).getBonusesOfType(BonusType::EVOLUTION_PARASITE)->front()->val==1);
	CBattleInfoCallback colony;
	CGameHandler colonyServer{colony,{}};
	BattleActionProcessor colonyActions{&colonyServer};
	colony.units.emplace(1,CStack{1,{1,8,80,{6},BattleSide::LEFT},{}});
	colony.units.emplace(2,CStack{2,{2,6,60,{9},BattleSide::RIGHT},{}});
	for(int id:{7,22,78,95})colony.units.at(1).bonuses.push_back(trait(id));
	colonyActions.evolutionRound(colony,true);
	colony.units.at(1).bonuses.push_back(trait(23));
	colony.units.at(1).data.health=50;
	colonyActions.evolutionAttack(colony,&colony.units.at(2),&colony.units.at(1),{{{1,2,30,false,3}}});
	assert(colony.units.at(1).data.pos.pos!=6 && colony.units.size()==4);
	colony.units.at(1).data.health=0;
	colonyActions.evolutionAttack(colony,&colony.units.at(2),&colony.units.at(1),{{{1,2,50,true,5}}});
	auto heirTraits=colony.units.at(3).getBonusesOfType(BonusType::EVOLUTION_TRAIT);
	assert(std::any_of(heirTraits->begin(),heirTraits->end(),[](auto b){return b->subtype==23;}));
	assert(!colony.units.at(1).alive() && colony.units.size()==5);
	colonyActions.evolutionRound(colony,false);
	assert(colony.units.at(1).getCount()==2 && colony.units.size()==4);
	CBattleInfoCallback plague;
	CGameHandler plagueServer{plague,{}};
	BattleActionProcessor plagueActions{&plagueServer};
	for(int i=1;i<=3;++i)plague.units.emplace(i,CStack{i,{i,10,100,{3+i},i==1?BattleSide::LEFT:BattleSide::RIGHT},{}});
	plague.units.at(1).bonuses.push_back(trait(62));
	plagueActions.evolutionAttack(plague,&plague.units.at(1),&plague.units.at(2),{{{2,1,10,false,0}}});
	plague.units.at(2).data.health=0;
	plagueActions.evolutionAttack(plague,&plague.units.at(1),&plague.units.at(2),{{{2,1,100,true,10}}});
	plagueActions.evolutionRound(plague,false);
	assert(plague.units.at(3).getBonusesOfType(BonusType::NONE,500062)->front()->val==1);
	assert(plague.units.at(3).hasBonusOfType(BonusType::EVOLUTION_PARASITE));
	CBattleInfoCallback brood;
	CGameHandler broodServer{brood,{}};
	BattleActionProcessor broodActions{&broodServer};
	brood.units.emplace(1,CStack{1,{1,10,100,{4},BattleSide::LEFT},{}});
	brood.units.emplace(2,CStack{2,{2,10,100,{8},BattleSide::RIGHT},{}});
	for(int id:{1,75})brood.units.at(1).bonuses.push_back(trait(id));
	broodActions.evolutionAttack(brood,&brood.units.at(1),&brood.units.at(2),{{{2,1,10,false,1}}});
	broodActions.evolutionRound(brood,false);
	auto & larva=brood.units.at(4);
	assert(larva.getBonusesOfType(BonusType::NONE,500075)->front()->val==1);
	larva.data.health=0;
	broodActions.evolutionAttack(brood,&brood.units.at(2),&larva,{{{4,2,10,true,1}}});
	broodActions.evolutionAttack(brood,&brood.units.at(1),&brood.units.at(2),{{{2,1,10,false,1}}});
	broodActions.evolutionRound(brood,false);
	assert(brood.units.at(6).getCount()==2);
}
