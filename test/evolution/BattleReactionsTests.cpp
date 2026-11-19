// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production reaction dispatcher with a small packet-applying battlefield.
#include <algorithm>
#include <cassert>
#include <functional>
#include <random>
#include <string>
#include "../../lib/battle/EvolutionChain.h"
#include <string>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
using BonusCustomSubtype=int;
enum class BonusType { NONE, EVOLUTION_BROOD, EVOLUTION_FAMILY, EVOLUTION_SEEN, EVOLUTION_TRAIT, EVOLUTION_WOUND, EVOLUTION_PARASITE, FLYING, BLOCKS_RETALIATION, TWO_HEX_ATTACK_BREATH, ATTACKS_ALL_ADJACENT, NO_MELEE_PENALTY, NO_DISTANCE_PENALTY };
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
	int pos=-1;
	bool isValid() const {return pos>=0 && pos<21;}
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
struct BattleSpellCast { BattleSide side;std::set<unsigned> affectedCres,resistedCres; };
struct StacksInjured {int battleID{};std::vector<BattleStackAttacked> stacks;};
struct SetStackEffect {int battleID{};std::vector<std::pair<int,std::vector<Bonus>>> toAdd,toRemove;};
struct CBattleInfoCallback {
	std::map<int,CStack> units;int round=1;
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
		for(int d=0;d<21;++d)for(int p:{near+d,near-d})if(accessible({p},nullptr))return {p};
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
};
namespace evolution {int64_t damageProduct(int64_t a,int64_t b,int c=1){return a*b/c;}}
namespace evolution_server {class EvolutionReactions;}
struct BattleActionProcessor {CGameHandler * gameHandler;
	uint64_t reactionSerial=0;
	evolution_server::EvolutionReactions * activeReaction=nullptr;
	bool evolutionThreatened(const CBattleInfoCallback &,const CStack *,BattleHex);
	void evolutionMoved(const CBattleInfoCallback &,const CStack *);
	void evolutionDamage(const CBattleInfoCallback &,const StacksInjured &);
	void evolutionSpell(const CBattleInfoCallback &,const BattleSpellCast &);
	void withEvolution(const CBattleInfoCallback &,const std::function<void(evolution_server::EvolutionReactions &)> &);
	void makeAttack(const CBattleInfoCallback & battle,const CStack * from,const CStack * to,int,BattleHex,bool,bool,bool){
		auto & target=gameHandler->field.units.at(to->unitId());
		target.data.health=0;
		evolutionAttack(battle,from,to,{{{to->unitId(),from->unitId(),10,true,1}}});
	}
	void moveStack(const CBattleInfoCallback &,int id,BattleHex h){gameHandler->field.units.at(id).data.pos=h;}
	void evolutionRound(const CBattleInfoCallback &,bool);
	void evolutionAttack(const CBattleInfoCallback &,const CStack *,const CStack *,const BattleAttack &);
};
#include "../../server/battles/EvolutionReactions.inc"

static std::vector<int> randomRun(unsigned seed)
{
	std::mt19937 random(seed);
	const std::vector<int> traits{1,3,8,9,10,15,18,19,21,23,25,27,32,39,50,51,56,57,59,60,61,67,68,78,87};
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

int main(){
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
	assert(carrier.data.health==74 && enemy.data.health==98 && ally.bonuses.size()==6);
	actions.evolutionAttack(field,&ally,&enemy,{{{enemy.id,ally.id,8,false}}});
	assert(enemy.data.health==96);
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
	nest.units.at(2).bonuses.push_back({BonusType::EVOLUTION_PARASITE,1});
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
	magic.units.at(2).data.health=0;
	magicActions.evolutionDamage(magic,{1,{{2,-1,38,true,4}}});
	assert(magic.units.size()==3);
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
