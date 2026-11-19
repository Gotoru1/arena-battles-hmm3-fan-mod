// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production reaction dispatcher with a small packet-applying battlefield.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
enum class BonusType { EVOLUTION_TRAIT, EVOLUTION_WOUND, EVOLUTION_PARASITE, FLYING, BLOCKS_RETALIATION, TWO_HEX_ATTACK_BREATH, ATTACKS_ALL_ADJACENT, NO_MELEE_PENALTY, NO_DISTANCE_PENALTY };
enum class BonusDuration { PERMANENT, ONE_BATTLE };
struct Bonus { BonusType type{}; int val{}; BonusDuration duration{}; };
using CreatureID = int;
enum class BattleSide { LEFT, RIGHT };
struct BattleHex {
	int pos=-1;
	bool isValid() const {return pos>=0 && pos<21;}
	int toInt() const {return pos;}
	bool operator==(BattleHex other) const {return pos==other.pos;}
	std::vector<BattleHex> getNeighbouringTiles() const {return {{pos-1},{pos+1}};}
	static int getDistance(BattleHex a,BattleHex b){return std::abs(a.pos-b.pos);}
};
enum class EHealLevel { RESURRECT };
enum class EHealPower { ONE_BATTLE };
struct Data {int species{}, count{}, health{}; BattleHex pos; BattleSide side{};};
namespace battle {
struct UnitInfo {int id{}, type{}, count{}; BattleHex position; BattleSide side{};bool summoned=false;
	void save(Data & out){out={type,count,count*10,position,side};}
};}
struct UnitChanges {enum class EOperation {ADD, RESET_STATE};int id; EOperation op; Data data;
	UnitChanges(int id,EOperation op):id(id),op(op){}
};
struct BattleStackAttacked {
	int stackAttacked{},attackerID{}; int64_t damageAmount{};bool dead=false;bool killed() const{return dead;}
};
struct BattleAttack {std::vector<BattleStackAttacked> bsa;};
struct CStack {
	int id;Data data;std::vector<Bonus> bonuses;
	int unitId() const{return id;} bool alive() const{return data.health>0;}
	int getCount() const{return (data.health+9)/10;}
	auto unitSide() const{return data.side;} auto creatureId() const{return data.species;}
	auto getPosition() const{return data.pos;} int getAvailableHealth() const{return data.health;}
	auto getBonusesOfType(BonusType t) const{
		auto result=std::make_shared<std::vector<std::shared_ptr<Bonus>>>();
		for(auto b:bonuses) if(b.type==t)result->push_back(std::make_shared<Bonus>(b));
		return result;
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
struct StacksInjured {int battleID{};std::vector<BattleStackAttacked> stacks;};
struct SetStackEffect {int battleID{};std::vector<std::pair<int,std::vector<Bonus>>> toAdd;};
struct CBattleInfoCallback {
	std::map<int,CStack> units;int round=1;
	int getBattleID() const{return 1;}auto getBattle() const{return this;}
	int battleNextUnitId() const{return units.empty()?1:units.rbegin()->first+1;}
	auto battleGetStackByID(int id,bool alive=true) const{
		auto p=units.find(id);return p==units.end()||(alive&&!p->second.alive())?nullptr:&p->second;
	}
	auto battleGetAllStacks() const{std::vector<const CStack*> v;for(auto & e:units)v.push_back(&e.second);return v;}
	bool accessible(BattleHex h,const CStack*) const{
		if(!h.isValid())return false;
		for(auto & e:units)if(e.second.alive()&&e.second.data.pos==h)return false;
		return true;
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
	void sendAndApply(const SetStackEffect & p){for(auto & v:p.toAdd)for(auto b:v.second)field.units.at(v.first).bonuses.push_back(b);}
	void sendAndApply(const StacksInjured & p){for(auto & h:p.stacks)field.units.at(h.stackAttacked).data.health-=int(h.damageAmount);}
	void sendAndApply(const BattleUnitsChanged & p){for(auto & c:p.changedStacks){
		if(c.op==UnitChanges::EOperation::ADD) field.units.emplace(c.id,CStack{c.id,c.data,{}});
		else field.units.at(c.id).data=c.data;
	}}
	void sendAndApply(const BattleStackMoved & p){field.units.at(p.stack).data.pos=p.tilesToMove.back();}
};
namespace evolution {int64_t damageProduct(int64_t a,int b,int c){return a*b/c;}}
struct BattleActionProcessor {CGameHandler * gameHandler;
	void evolutionRound(const CBattleInfoCallback &,bool);
	void evolutionAttack(const CBattleInfoCallback &,const CStack *,const CStack *,const BattleAttack &);
};
#include "../../server/battles/EvolutionReactions.inc"

int main(){
	CBattleInfoCallback field;
	auto add=[&](int pos,BattleSide side,std::vector<int> mutations)->CStack &{
		int id=field.battleNextUnitId();
		auto & u=field.units.emplace(id,CStack{id,{id,10,100,{pos},side},{}}).first->second;
		for(int m:mutations)u.bonuses.push_back({BonusType::EVOLUTION_TRAIT,m});
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
	assert(field.units.at(5).bonuses.size()==5);
	BattleAttack hit{{{enemy.id,carrier.id,30,false}}};
	actions.evolutionAttack(field,&carrier,&enemy,hit);
	assert(secondary.data.health==90 && enemy.data.pos.pos!=5);
	assert(enemy.hasBonusOfType(BonusType::EVOLUTION_PARASITE) && enemy.hasBonusOfType(BonusType::EVOLUTION_WOUND));
	carrier.data.health=72;field.round=3;
	actions.evolutionRound(field,false);
	assert(carrier.data.health==74 && enemy.data.health==98 && ally.bonuses.size()==5);
	actions.evolutionAttack(field,&ally,&enemy,{{{enemy.id,ally.id,8,false}}});
	assert(enemy.data.health==96);
	enemy.data.pos={5};enemy.data.health=0;enemy.bonuses.push_back({BonusType::FLYING,1});
	actions.evolutionAttack(field,&carrier,&enemy,{{{enemy.id,carrier.id,30,true}}});
	assert(carrier.hasBonusOfType(BonusType::FLYING));
	assert(field.units.size()==6 && field.units.at(6).data.side==BattleSide::LEFT);
	assert(field.units.at(6).bonuses.size()>=7);
	ally.data.health=0;actions.evolutionAttack(field,&secondary,&ally,{{{ally.id,secondary.id,30,true}}});
	assert(carrier.bonuses.size()>8 || field.units.at(5).bonuses.size()>5);
}
