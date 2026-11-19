// SPDX-License-Identifier: GPL-2.0-or-later
// Implemented in the ArenaDraft translation unit to share its card and map builders.
namespace
{
	std::unique_ptr<evolution::RoguelikeRun> run;
	bool verifying = false;
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
				// Only terminal stat grants can be produced by an event. No nested event dispatch.
				for(auto & action : effect["addInfo"]["effect"].Vector())
				{
					auto & bonus = action["bonus"];
					auto type = bonus["type"].String();
					if(action["action"].String() != "bonus" || (type != "PRIMARY_SKILL" && type != "CREATURE_DAMAGE"))
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
}

void ArenaDraft::startVerification()
{
	verifying = true;
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
		if(run && run->phase == evolution::Phase::Reward)
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
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Арена против компьютера", "Выбери режим", cards,
		[self](int pick)
		{
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
			self->buildEvolutionBattle();
		}, [self]() { self->cancel(); }, nullptr);
}

void ArenaDraft::settleEvolution(bool won, int survivors, int64_t casualties)
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
	run->finish(won, survivors, casualties, std::move(pool), rarities);
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
		if(!won || run->wave == 15)
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
	std::vector<ArenaOffer> cards;
	const auto & catalog = evolutionCatalog()["mutations"];
	for(const auto & id : run->offers)
	{
		const auto & entry = catalog[id];
		static const std::array<std::string, 4> labels{"Обычная", "Редкая", "Эпическая", "Легендарная"};
		cards.push_back(evolutionCard(entry["name"].String(), labels.at(entry["rarity"].Integer()) + "\n\n" + entry["text"].String()
			+ "\n\nТекущий ранг: " + std::to_string(run->rank(id))));
	}
	auto self = shared_from_this();
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Волна " + run->wave.str() + " пройдена",
		"Выбери одну мутацию для отряда", cards,
		[self](int index) { run->pick(index); self->buildEvolutionBattle(); },
		[self]() { self->cancel(); }, nullptr);
}

void ArenaDraft::buildEvolutionBattle()
{
	const auto & data = evolutionCatalog();
	rng.seed(run->random());
	map->evolutionSeed = static_cast<int>(run->random() % INT32_MAX) + 1;
	randomizer->setSeed(map->evolutionSeed);
	if(!player().hero)
		player().hero = createHero(HeroTypeID(run->hero));
	player().primary = run->primary;
	const auto creature = CreatureID(run->army.creature);
	giveCreature(player(), 1, creature, evolution::engineInt(run->army.count));
	auto & effects = player().hero->getStackPtr(SlotID(0))->evolutionEffects;
	for(const auto & mutation : run->army.mutations)
		appendEffects(effects, data["mutations"][mutation.first], mutation.second);

	// Resolve run-level effects before crossing VCMI's signed 32-bit bonus boundary.
	const auto bonus = run->lineageDamage();
	if(bonus > 0)
	{
		JsonNode damage;
		damage["type"].String() = "CREATURE_DAMAGE";
		damage["subtype"].String() = "creatureDamageBoth";
		damage["val"].Integer() = evolution::engineInt(bonus);
		effects.Vector().push_back(damage);
	}
	std::string discoveries;
	for(const auto & combo : data["synergies"].Vector())
	{
		std::vector<std::string> ingredients;
		for(const auto & id : combo["requires"].Vector())
			ingredients.push_back(id.String());
		if(!run->hasCombination(ingredients))
			continue;
		appendEffects(effects, combo, 1);
		const auto & name = combo["name"].String();
		if(std::find(run->synergies.begin(), run->synergies.end(), name) == run->synergies.end())
		{
			run->synergies.push_back(name);
			discoveries += "\nОткрыто сочетание: " + name;
		}
	}
	auto heroes = offerHeroes(player().hero->getHeroTypeID());
	enemy().hero = createHero(heroes.at(0));
	const auto & initial = enemy().hero->getHeroClass()->primarySkillInitial;
	std::copy_n(initial.begin(), 4, enemy().primary.begin());
	const int tier = run->wave >= 25 ? 7 : 1 + (evolution::engineInt(run->wave)-1) / 4;
	const int slots = run->wave < 3 ? 3 : (run->wave < 8 ? 4 : 5);
	for(int slot = 0; slot < slots; ++slot)
	{
		auto pool = ArenaRules::creaturePool(slot == 0 ? tier : std::max(1, tier-1));
		auto id = pool.at(randomInt(0, static_cast<int>(pool.size()) - 1));
		const int share = slot == 0 ? 40 : 60 / (slots-1);
		auto cost = std::max<int64_t>(1, id.toCreature()->getAIValue());
		int amount = evolution::engineInt(2 + run->wave / (slot == 0 ? 1 : 3) + run->threat() * share / (100 * cost));
		giveCreature(enemy(), slot+1, id, amount);
		if(run->boss() && slot == 0)
			appendEffects(enemy().hero->getStackPtr(SlotID(slot))->evolutionEffects, data["boss"], 1);
	}

	std::string detail = "Твой отряд: " + run->army.count.str() + " " + creature.toCreature()->getNamePluralTranslated();
	if(run->boss())
		detail += "\nБосс: " + enemy().hero->getStackPtr(SlotID(0))->getName() + ". " + data["boss"]["text"].String();
	auto self = shared_from_this();
	std::vector<ArenaOffer> cards{evolutionCard("В бой", detail + discoveries)};
	ENGINE->windows().createAndPushWindow<ArenaChoiceWindow>("Evolution Arena - Волна " + run->wave.str(),
		"В свой ход нажми A для автобоя с анимацией", cards, [self](int) { self->startBattle(); },
		[self]() { self->cancel(); }, nullptr);
}
