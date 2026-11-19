// SPDX-License-Identifier: GPL-2.0-or-later
// Implemented in the ArenaDraft translation unit to share its card and map builders.
namespace
{
	std::unique_ptr<evolution::RoguelikeRun> run;
	bool verifying = false;
	bool eliteFight = false;
	unsigned verificationWaves = 15;
	bool choosingRecipient = false;

	std::filesystem::path checkpointPath()
	{
		return std::filesystem::path(VCMIDirs::get().userDataPath().native()) / "evolution-run.sav";
	}

	void checkpoint()
	{
		if(verifying) return;
		const auto dest = checkpointPath();
		if(run->phase == evolution::Phase::Defeat)
		{
			std::filesystem::remove(dest);
			return;
		}
		std::filesystem::create_directories(dest.parent_path());
		auto temp = dest;
		temp += ".tmp";
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		out.exceptions(std::ios::badbit | std::ios::failbit);
		run->save(out);
		out.close();
		std::filesystem::rename(temp, dest);
	}
	int verificationResult = -1;

	const JsonNode & evolutionCatalog()
	{
		static const JsonNode data(JsonPath::builtin("config/evolution"));
		if(data["mutations"].Struct().size() < 3)
			throw std::runtime_error("Evolution mutation catalog missing or incomplete");
		return data;
	}

	ArenaOffer evolutionCard(std::string title, std::string detail)
	{
		ArenaOffer card;
		card.description = title;
		card.build = [title, detail](Widgets & widgets)
		{
			widgets.push_back(std::make_shared<CLabel>(CARD_CENTER, 62, FONT_BIG,
				ETextAlignment::CENTER, Colors::YELLOW, title, ArenaCard::WIDTH-12));
			widgets.push_back(std::make_shared<CMultiLineLabel>(Rect(12, 108, ArenaCard::WIDTH-24, 192),
				FONT_MEDIUM, ETextAlignment::TOPCENTER, Colors::WHITE, detail));
		};
		return card;
	}

	void appendEffects(JsonNode & target, const JsonNode & definition, unsigned rank)
	{
		for(auto effect : definition["effects"].Vector())
		{
			if(effect["type"].String() == "ON_COMBAT_EVENT")
			{
				// Event payloads grant terminal bonuses. Team echoes use the server queue.
				for(auto & action : effect["addInfo"]["effect"].Vector())
				{
					auto & bonus = action["bonus"];
					auto type = bonus["type"].String();
					if(action["action"].String() != "bonus" || type == "ON_COMBAT_EVENT")
						throw std::runtime_error("Invalid Evolution event payload");
					auto & amount = bonus["val"].Integer();
					amount = evolution::engineInt(evolution::Amount(amount) * rank);
				}
			}
			else
			{
				auto & amount = effect["val"].Integer();
				amount = evolution::engineInt(evolution::Amount(amount) * rank);
			}
			target.Vector().push_back(std::move(effect));
		}
	}
	void appendMutation(JsonNode & effects, const JsonNode & def, unsigned rank)
	{
		appendEffects(effects, def, rank);
		const auto & evolution = def["awaken"];
		if(!evolution.isNull() && rank >= evolution["rank"].Integer())
			appendEffects(effects, evolution, 1);
	}
}

void ArenaDraft::startVerification()
{
	verifying = true;
	if(const char * value = std::getenv("EVOLUTION_TEST_WAVES"))
	{
		const std::string count(value);
		if(count.empty() || count.find_first_not_of("0123456789") != std::string::npos)
			throw std::invalid_argument("Invalid EVOLUTION_TEST_WAVES");
		const auto waves = std::stoull(count);
		if(!waves || waves > UINT32_MAX) throw std::out_of_range("Invalid EVOLUTION_TEST_WAVES");
		verificationWaves = static_cast<unsigned>(waves);
	}
	CMainMenu::openLobby(ESelectionScreen::newGame, true, {}, ELoadMode::NONE, true);
	GAME->server().arenaMode = true;
}

int ArenaDraft::verificationExitCode()
{
	return verifying ? (verificationResult < 0 ? 2 : verificationResult) : 0;
}

void ArenaDraft::verificationFrame()
{
	if(!verifying)
		return;
	if(verificationResult >= 0)
		GAME->onShutdownRequested(false);
	if(auto result = ENGINE->windows().topWindow<BattleResultWindow>())
		result->bExitf();
	else if(auto cards = ENGINE->windows().topWindow<ArenaChoiceWindow>())
	{
		size_t index = 0;
		if(run && run->phase == evolution::Phase::Reward && !choosingRecipient)
		{
			for(const char * preferred : {"fission", "flurry", "leech", "mass", "impact", "vigor"})
			{
				auto it = std::find(run->offers.begin(), run->offers.end(), preferred);
				if(it != run->offers.end())
				{
					index = static_cast<size_t>(it - run->offers.begin());
					break;
				}
			}
		}
		cards->select(static_cast<int>(index));
		cards->choose();
	}
	else if(auto battle = ENGINE->windows().topWindow<BattleWindow>())
	{
		if(battle->owner.tacticsMode)
			battle->bTacticPhaseEnd();
		else if(battle->owner.makingTurn() && !battle->owner.curInt->isAutoFightOn)
			battle->bAutofightf();
	}
}

bool ArenaDraft::evolutionBattle()
{
	return run && run->phase == evolution::Phase::Battle;
}

void ArenaDraft::abandonEvolution()
{
	run.reset();
}

void ArenaDraft::chooseMode()
{
	auto self = shared_from_this();
	std::vector<ArenaOffer> cards{
		evolutionCard("Evolution Arena", "Выбери героя и отряд. После победы получай мутации. Каждая пятая волна — босс."),
		evolutionCard("Одиночная арена", "Подготовь армию для одного боя.")};
	if(!verifying && std::filesystem::exists(checkpointPath()))
		cards.push_back(evolutionCard("Продолжить", "Вернуться к последней волне или к тем же трём картам награды. Новый забег заменит сохранение после выбора стартового отряда."));
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Арена против компьютера", "Выбери режим", cards,
		[self](int pick)
		{
			if(pick == 2)
			{
				std::ifstream in(checkpointPath(), std::ios::binary);
				run = std::make_unique<evolution::RoguelikeRun>(evolution::RoguelikeRun::restore(in));
				if(run->phase == evolution::Phase::Reward) self->chooseEvolutionReward();
				else self->buildEvolutionBattle();
				return;
			}
			if(pick == 1)
			{
				self->stepDifficulty();
				return;
			}
			auto seed = verifying ? 2048u : std::random_device{}();
			if(const char * value = std::getenv("EVOLUTION_SEED"))
			{
				std::string input(value);
				if(input.empty() || input.find_first_not_of("0123456789") != std::string::npos)
					throw std::invalid_argument("EVOLUTION_SEED must be an unsigned 32-bit integer");
				auto number = std::stoull(input);
				if(number > UINT32_MAX)
					throw std::out_of_range("EVOLUTION_SEED exceeds 32 bits");
				seed = static_cast<uint32_t>(number);
			}
			evolutionCatalog();
			run = std::make_unique<evolution::RoguelikeRun>(seed);
			self->rng.seed(run->random());
			self->stepChooseHero();
		}, [self]() { self->cancel(); }, nullptr);
}

void ArenaDraft::chooseEvolutionCreature()
{
	const auto choices = offerCreatures(player(), 1);
	std::vector<ArenaOffer> cards;
	for(const auto & choice : choices)
		cards.push_back(creatureOffer(choice.first, 24));
	auto self = shared_from_this();
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Выбери стартовый отряд", "Зерно " + std::to_string(run->seed),
		cards, [self, choices](int index)
		{
			run->begin(self->player().hero->getHeroTypeID().getNum(), self->player().primary,
				choices.at(index).first.getNum(), 24);
			std::vector<ArenaOffer> starts{
				evolutionCard("Поход", "24 бойца и обычные характеристики героя."),
				evolutionCard("Ополчение", "36 бойцов, но атака и защита героя ниже на 1 (минимум 0)."),
				evolutionCard("Ветераны", "16 бойцов. Герой получает +2 к атаке и +2 к защите для всей армии.")};
			ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Условия старта", "Выбери состав экспедиции", starts,
				[self](int pick){
					if(pick == 1)
					{
						run->army.count = 36;
						for(int i=0;i<2;++i) run->primary[i] = std::max(0,run->primary[i]-1);
					}
					if(pick == 2)
					{
						run->army.count = 16;
						for(int i=0;i<2;++i) run->primary[i] += 2;
					}
					self->buildEvolutionBattle();
				}, [self](){ self->cancel(); }, nullptr);
		}, [self]() { self->cancel(); }, nullptr);
}

void ArenaDraft::settleEvolution(bool won, const std::array<int, 3> & survivors, int64_t casualties)
{
	if(!evolutionBattle())
		return;
	std::vector<std::string> pool;
	std::map<std::string, unsigned> rarities;
	for(const auto & entry : evolutionCatalog()["mutations"].Struct())
	{
		pool.push_back(entry.first);
		rarities.emplace(entry.first, static_cast<unsigned>(entry.second["rarity"].Integer()));
	}
	std::vector<int> reserves(survivors.begin()+1, survivors.begin()+run->rosterSize());
	run->finish(won, survivors[0], casualties, std::move(pool), rarities, eliteFight, reserves);
	try { checkpoint(); } catch(const std::exception & e) { logGlobal->error("Evolution checkpoint: %s", e.what()); }
}

void ArenaDraft::afterEvolutionGame()
{
	if(!run)
		return;
	if(verifying)
	{
		bool won = run->phase == evolution::Phase::Reward;
		logGlobal->info("Evolution playtest: wave %s, victory %d, roster %s, casualties %s",
			run->wave.str(), won, run->army.count.str(), run->kills.str());
		if(!won || run->wave == verificationWaves)
		{
			verificationResult = won ? 0 : 1;
			run.reset();
			return;
		}
	}
	if(run->phase == evolution::Phase::Reward)
	{
		auto continuing = std::move(run);
		CMainMenu::openLobby(ESelectionScreen::newGame, true, {}, ELoadMode::NONE, true);
		GAME->server().arenaMode = true;
		run = std::move(continuing);
	}
	else
	{
		const auto summary = "Забег окончен\nВолна " + run->wave.str()
			+ "   Потери врага: " + run->kills.str() + "\nМутации: "
			+ std::to_string(run->history.size()) + "   Сочетания: " + std::to_string(run->synergies.size())
			+ "\nЗерно: " + std::to_string(run->seed);
		run.reset();
		CInfoWindow::showInfoDialog(summary, {});
	}
}

void ArenaDraft::chooseEvolutionReward()
{
	if(run->rosterSize() == 1) { showEvolutionCards(0); return; }
	std::vector<ArenaOffer> options;
	for(size_t i = 0; i < run->rosterSize(); ++i)
	{
		const auto & line = run->stack(i);
		if(line.count <= INT32_MAX) options.push_back(creatureOffer(CreatureID(line.creature), evolution::engineInt(line.count)));
		else options.push_back(evolutionCard(CreatureID(line.creature).toCreature()->getNamePluralTranslated(), evolution::displayAmount(line.count)));
	}
	choosingRecipient = true;
	auto self = shared_from_this();
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Кого развивать?", "Выбери отряд для мутации", options,
		[self](int i){ self->showEvolutionCards(i); }, [self](){ self->cancel(); }, nullptr);
}

void ArenaDraft::showEvolutionCards(size_t slot)
{
	choosingRecipient = false;
	std::vector<ArenaOffer> cards;
	const auto & catalog = evolutionCatalog()["mutations"];
	for(const auto & id : run->offers)
	{
		const auto & entry = catalog[id];
		static const std::array<std::string, 4> labels{"Обычная", "Редкая", "Эпическая", "Легендарная"};
		std::string hint;
		if(!run->rank(id, slot))
			for(const auto & combo : evolutionCatalog()["synergies"].Vector())
			{
				bool includesCard = false;
				bool otherRanksReady = true;
				for(const auto & part : combo["requires"].Vector())
				{
					if(part.String() == id) includesCard = true;
					else if(!run->rank(part.String(), slot)) otherRanksReady = false;
				}
				if(includesCard && otherRanksReady)
					hint += "\nОткроет: " + combo["name"].String();
			}
		cards.push_back(evolutionCard(entry["name"].String(), labels.at(entry["rarity"].Integer()) + "\n\n" + entry["text"].String()
			+ "\n\nТекущий ранг: " + std::to_string(run->rank(id, slot)) + hint));
	}
	auto self = shared_from_this();
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Волна " + run->wave.str() + " пройдена",
		"Мутация для отряда: " + CreatureID(run->stack(slot).creature).toCreature()->getNamePluralTranslated(), cards,
		[self, slot](int index) { run->pick(index, slot); self->buildEvolutionBattle(); },
		[self]() { self->cancel(); }, nullptr);
}

void ArenaDraft::buildEvolutionBattle()
try
{
	const auto & data = evolutionCatalog();
	checkpoint(); // before consuming the RNG, so resuming recreates the same opponent
	eliteFight = false;
		if(!player().hero)
		player().hero = createHero(HeroTypeID(run->hero));
	if(run->rosterSize() < (run->wave >= 4 ? 3u : 2u))
	{
		const int tier = static_cast<int>(run->rosterSize()) + 1;
		rng.seed(run->random());
		auto choices = offerCreatures(player(), tier);
		std::vector<ArenaOffer> cards;
		for(auto & choice : choices)
		{
			const int budget = tier == 2 ? 650 : 1150;
			choice.second = static_cast<int>(std::max<int64_t>(2, budget / std::max<int64_t>(1, choice.first.toCreature()->getAIValue())));
			cards.push_back(creatureOffer(choice.first, choice.second));
		}
		auto self = shared_from_this();
		ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Подкрепление", "Выбери новый постоянный отряд", cards,
			[self, choices](int i){
				const auto & choice = choices.at(i);
				run->recruit(choice.first.getNum(), choice.second);
				self->buildEvolutionBattle();
			}, [self](){ self->cancel(); }, nullptr);
		return;
	}
	rng.seed(run->random());
	map->evolutionSeed = static_cast<int>(run->random() % INT32_MAX) + 1;
	randomizer->setSeed(map->evolutionSeed);
	const auto plan = evolution::encounter(run->wave, map->evolutionSeed, data["otherBosses"].Vector().size()+1);
	const auto & bossRule = plan.bossIndex ? data["otherBosses"].Vector().at(plan.bossIndex-1) : data["boss"];
	const auto & storms = data["anomalies"].Vector();
	const auto & weather = storms.at((map->evolutionSeed / 7u) % storms.size());
	const auto & schools = data["schools"].Vector();
	const size_t firstSchool = randomInt(0,static_cast<int>(schools.size())-1);
	player().primary = run->primary;
	std::string discoveries;
	std::string roster;
	for(size_t slot = 0; slot < run->rosterSize(); ++slot)
	{
		const auto & line = run->stack(slot);
		const auto creature = CreatureID(line.creature);
		giveCreature(player(), static_cast<int>(slot)+1, creature, evolution::engineInt(line.count));
		auto & effects = player().hero->getStackPtr(SlotID(static_cast<int>(slot)))->evolutionEffects;
		for(const auto & mutation : line.mutations)
			appendMutation(effects, data["mutations"][mutation.first], mutation.second);
		if(plan.anomaly) appendEffects(effects, weather, 1);

		// Resolve run-level effects before crossing VCMI's signed 32-bit bonus boundary.
		const auto bonus = run->lineageDamage(slot);
		if(bonus > 0)
		{
			JsonNode damage;
			damage["type"].String() = "CREATURE_DAMAGE";
			damage["subtype"].String() = "creatureDamageBoth";
			damage["val"].Integer() = evolution::engineInt(bonus);
			effects.Vector().push_back(damage);
		}
		for(const auto & combo : data["synergies"].Vector())
		{
			std::vector<std::string> ingredients;
			for(const auto & id : combo["requires"].Vector())
				ingredients.push_back(id.String());
			if(!run->hasCombination(ingredients, slot))
				continue;
			appendEffects(effects, combo, 1);
			const auto & name = combo["name"].String();
			if(std::find(run->synergies.begin(), run->synergies.end(), name) == run->synergies.end())
			{
				run->synergies.push_back(name);
				discoveries += "\nОткрыто сочетание: " + name;
			}
		}
		roster += "\n" + creature.toCreature()->getNamePluralTranslated() + " × " + evolution::displayAmount(line.count);
	}
	auto heroes = offerHeroes(player().hero->getHeroTypeID());
	enemy().hero = createHero(heroes.at(0));
	const auto & initial = enemy().hero->getHeroClass()->primarySkillInitial;
	std::copy_n(initial.begin(), 4, enemy().primary.begin());
	const int tier = run->wave >= 25 ? 7 : 1 + (evolution::engineInt(run->wave)-1) / 4;
	const int formation = !run->boss() && run->wave >= 3 ? randomInt(0, 2) : 0;
	const int baseSlots = run->wave < 3 ? 3 : (run->wave < 8 ? 4 : 5);
	const int slots = baseSlots + (formation == 1 ? 1 : formation == 2 ? -1 : 0);
	const int frontShare = formation == 1 ? 22 : formation == 2 ? 65 : 40;
	std::vector<std::string> traits(slots);
	for(int slot = 0; slot < slots; ++slot)
	{
		auto pool = ArenaRules::creaturePool(slot == 0 ? std::min(7, tier + (formation == 2 ? 1 : 0)) : std::max(1, tier-1));
		auto id = pool.at(randomInt(0, static_cast<int>(pool.size()) - 1));
		const int share = slot == 0 ? frontShare : (100-frontShare) / (slots-1);
		auto cost = std::max<int64_t>(1, id.toCreature()->getAIValue());
		int amount = evolution::engineInt(2 + run->wave / (slot == 0 ? 1 : 3) + run->threat() * share / (100 * cost));
		giveCreature(enemy(), slot+1, id, amount);
		auto & effects = enemy().hero->getStackPtr(SlotID(slot))->evolutionEffects;
		if(plan.anomaly) appendEffects(effects, weather, 1);
		if(run->boss() && slot == 0)
		{
			appendEffects(effects, bossRule, 1);
			traits[slot] += " · " + bossRule["name"].String();
		}
		if(slot < plan.mutatedStacks)
		{
			const auto & pair = schools.at((firstSchool + slot) % schools.size()).Vector();
			std::vector<std::string> ids;
			for(size_t n=0; n < (plan.pair ? pair.size() : 1u); ++n)
			{
				const auto & id = pair.at(n).String();
				const auto & entry = data["mutations"][id];
				appendMutation(effects, entry, plan.mutationRank);
				traits[slot] += " · " + entry["name"].String() + " " + std::to_string(plan.mutationRank);
				ids.push_back(id);
			}
			if(plan.recipe)
				for(const auto & recipe : data["synergies"].Vector())
				{
					const auto & parts = recipe["requires"].Vector();
					if(std::all_of(parts.begin(),parts.end(),[&](const JsonNode & part){ return std::find(ids.begin(),ids.end(),part.String()) != ids.end(); }))
					{
						appendEffects(effects,recipe,1);
						traits[slot] += " · " + recipe["name"].String();
					}
				}
		}
	}

	std::string detail = "Твоя армия:" + roster;
	if(run->boss())
		detail += "\nБосс: " + bossRule["name"].String() + ". " + bossRule["text"].String();
	const std::array<std::string,3> formations{"Смешанное войско", "Орда", "Ударный отряд"};
	if(!run->boss()) detail += "\nПротивник: " + formations[formation];
	bool eliteAvailable = !run->boss() && run->wave % 3 == 0;
	for(int i=0;i<slots && eliteAvailable;++i)
		eliteAvailable = (evolution::Amount(enemy().hero->getStackPtr(SlotID(i))->getCount()) * 8 + 4) / 5 <= INT32_MAX;
	std::string scouting = "Армия противника";
	if(plan.anomaly)
	{
		detail += "\nАномалия: " + weather["name"].String();
		scouting += "\n" + weather["name"].String() + ". " + weather["text"].String();
	}
	for(int i = 0; i < slots; ++i)
	{
		const auto * enemyStack = enemy().hero->getStackPtr(SlotID(i));
		scouting += "\n" + enemyStack->getName() + " × " + std::to_string(enemyStack->getCount()) + traits[i];
		if(eliteAvailable)
			scouting += "  (элита: " + evolution::Amount((evolution::Amount(enemyStack->getCount()) * 8 + 4) / 5).str() + ")";
	}
	for(size_t slot = 0; slot < run->rosterSize(); ++slot)
	{
		const auto & line = run->stack(slot);
		scouting += "\n\n" + CreatureID(line.creature).toCreature()->getNamePluralTranslated() + " × " + line.count.str();
		for(const auto & recipe : data["synergies"].Vector())
		{
			std::vector<std::string> required;
			for(const auto & id : recipe["requires"].Vector()) required.push_back(id.String());
			if(run->hasCombination(required,slot)) scouting += "\nСочетание: " + recipe["name"].String();
		}
		scouting += "\nРост за забег: " + evolution::displayAmount(line.harvest) + " очк. · Бонус урона к началу волны: +" + evolution::displayAmount(run->lineageDamage(slot));
		for(const auto & mutation : line.mutations)
			scouting += "\n" + data["mutations"][mutation.first]["name"].String() + " × " + std::to_string(mutation.second);
	}
	auto self = shared_from_this();
	std::vector<ArenaOffer> cards{evolutionCard("В бой", detail + discoveries)};
	if(eliteAvailable)
		cards.push_back(evolutionCard("Элитная охота", "Все вражеские отряды на 60% больше (округление вверх). За победу среди трёх наград обязательно появится эпическая или легендарная мутация."));
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Evolution Arena - Волна " + run->wave.str(),
		"Кнопка «Твой герой» — разведка и мутации. В бою A — автобой.", cards, [self, slots](int pick)
		{
			eliteFight = pick == 1;
			if(eliteFight)
				for(int i = 0; i < slots; ++i)
				{
					auto * stack = self->enemy().hero->getStackPtr(SlotID(i));
					const int size = evolution::engineInt((evolution::Amount(stack->getCount()) * 8 + 4) / 5);
					auto inherited = stack->evolutionEffects;
					self->giveCreature(self->enemy(), i+1, stack->getCreatureID(), size);
					self->enemy().hero->getStackPtr(SlotID(i))->evolutionEffects = std::move(inherited);
				}
			self->startBattle();
		},
		[self]() { self->cancel(); }, [scouting]() { CInfoWindow::showInfoDialog(scouting, {}); });
}
catch(const std::overflow_error &)
{
	cancel();
	CInfoWindow::showInfoDialog("Эта волна достигла числового предела VCMI. Сохранение начала волны оставлено в профиле.", {});
}
