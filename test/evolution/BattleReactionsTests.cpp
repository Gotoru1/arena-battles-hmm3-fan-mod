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
enum class BonusType { NONE, EVOLUTION_BROOD, EVOLUTION_FAMILY, EVOLUTION_SEEN, EVOLUTION_TRAIT, EVOLUTION_WOUND, EVOLUTION_PARASITE, FLYING, BLOCKS_RETALIATION, TWO_HEX_ATTACK_BREATH, ATTACKS_ALL_ADJACENT, NO_MELEE_PENALTY, NO_DISTANCE_PENALTY, STACKS_SPEED };
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
enum class EHealPower { ONE_BATTLE };
struct Data {int species{}, count{}, health{}; BattleHex pos; BattleSide side{};};
namespace battle {
struct UnitInfo {int id{}, type{}, count{}; BattleHex position; BattleSide side{};bool summoned=false;
	void save(Data & out){out={type,count,count*10,position,side};}
};}
struct UnitChanges {enum class EOperation {ADD, RESET_STATE, REMOVE};int id; EOperation op; Data data;int64_t healthDelta=0;
	UnitChanges(int id,EOperation op):id(id),op(op){}
};
struct BattleStackAttacked {
	int stackAttacked{},attackerID{}; int64_t damageAmount{};bool dead=false;int killedAmount=0;bool killed() const{return dead;}
};
struct BattleAttack {std::vector<BattleStackAttacked> bsa;};
struct CStack {
	int id;Data data;std::vector<Bonus> bonuses;
	struct TemporaryHealth{int getResurrected() const{return 0;}} health;
	bool base=true;
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
		if(c.op==UnitChanges::EOperation::ADD) field.units.emplace(c.id,CStack{c.id,c.data,{}, {}, false});
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
		assert(!mark.animation.name.empty() && !mark.appearAnimation.name.empty() && mark.turnsRemaining!=0);
		for(const auto & other:field.marks) assert(other->uniqueID!=mark.uniqueID);
		field.marks.push_back(change.data);
	}}
};
namespace evolution {int64_t damageProduct(int64_t a,int64_t b,int c=1){return a*b/c;}}
namespace evolution_server {class EvolutionReactions;}
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
	const std::vector<int> traits{1,2,3,8,9,10,13,15,18,19,21,23,24,25,27,32,35,39,48,50,51,56,57,59,60,61,67,68,71,77,78,82,87};
	CBattleInfoCallback field;
	CGameHandler server{field,{}};
	BattleActionProcessor actions{&server};
	for(int id=1;id<=4;++id)
	{
		auto & unit=field.units.emplace(id,CStack{id,{id,6,60,{id*4},id<3?BattleSide::LEFT:BattleSide::RIGHT},{}}).first->second;
		for(int pick=0;pick<5;++pick)unit.bonuses.push_back(trait(traits[random()%traits.size()]));
	}
	actions.evolutionRound(field,true);
	for(int step=0;step<45;++step)
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
		auto & from=field.units.at(left[random()%left.size()]);
		auto & to=field.units.at(right[random()%right.size()]);
		const auto before=to.getCount();
		const auto damage=std::min(to.data.health,static_cast<int>(1+random()%36));
		to.data.health-=damage;
		actions.evolutionAttack(field,&from,&to,{{{to.id,from.id,damage,!to.alive(),before-to.getCount()}}});
		if(step%3==2){++field.round;actions.evolutionRound(field,false);}
	}
	std::vector<int> fingerprint;
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

int main(){
	playableAlone();
	groundMarks();
	deathStrike();
	packHunt();
	sporesReactorAndDomino();
	adaptiveDamageAndDebt();
	growingEnemyArmies();
	parasiteBurstAndGenerations();
	defenseAndConversion();
	for(unsigned seed=1;seed<=120;++seed) assert(randomRun(seed)==randomRun(seed));
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
	assert(field.units.at(5).bonuses.size()==6);
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
