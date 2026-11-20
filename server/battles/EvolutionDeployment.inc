// SPDX-License-Identifier: GPL-2.0-or-later
// The first seven companies belong to the adventure hero. All further companies
// are ordinary battle participants, and are never written back into hero slots.
namespace evolution_server
{
bool deployCompany(CGameHandler & server,const CBattleInfoCallback & battle,BattleSide side,
	CreatureID species,int count,BattleHex preferred,std::vector<Bonus> effects)
{
	const auto position=battle.getAvailableHex(species,side,preferred.toInt());
	if(!position.isValid()) return false;
	battle::UnitInfo unit;
	unit.id=battle.battleNextUnitId();
	unit.type=species;
	unit.count=count;
	unit.position=position;
	unit.side=side;
	unit.summoned=false;
	BattleUnitsChanged add;
	add.battleID=battle.getBattle()->getBattleID();
	add.changedStacks.emplace_back(unit.id,UnitChanges::EOperation::ADD);
	unit.save(add.changedStacks.back().data);
	server.sendAndApply(add);

	Bonus company;
	company.type=BonusType::NONE;
	company.subtype=BonusCustomSubtype(500701);
	company.val=1;
	company.duration=BonusDuration::ONE_BATTLE;
	effects.push_back(company);
	SetStackEffect bonuses;
	bonuses.battleID=add.battleID;
	bonuses.toAdd.emplace_back(unit.id,std::move(effects));
	server.sendAndApply(bonuses);
	return true;
}
}
