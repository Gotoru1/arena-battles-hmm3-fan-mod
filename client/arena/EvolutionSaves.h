// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "RoguelikeRun.h"
#include <filesystem>
#include <fstream>

namespace evolution
{
class SaveSlots
{
public:
	explicit SaveSlots(std::filesystem::path directory) : directory(std::move(directory)) {}

	std::filesystem::path path(int slot) const
	{
		if(slot < 0 || slot >= 3) throw std::out_of_range("Invalid Evolution save slot");
		if(slot == 0) return directory / "evolution2-run.sav";
		return directory / ("evolution2-run-" + std::to_string(slot+1) + ".sav");
	}

	bool legacyExists(int slot) const
	{
		const auto name = slot ? "evolution-run-" + std::to_string(slot+1) + ".sav" : "evolution-run.sav";
		return std::filesystem::exists(directory / name);
	}

	bool exists(int slot) const { return std::filesystem::exists(path(slot)); }

	RoguelikeRun read(int slot) const
	{
		std::ifstream input(path(slot),std::ios::binary);
		std::string signature;
		if(!(input >> signature) || (signature != "Evolution2/1" && signature != "Evolution2/2")) throw std::runtime_error("Несовместимая версия сохранения");
		Amount last = 0;
		if(signature == "Evolution2/2" && (!(input >> last) || last < 0)) throw std::runtime_error("Повреждено сохранение");
		auto run = RoguelikeRun::restore(input);
		if(last > run.wave) throw std::runtime_error("Неверная волна подкрепления");
		run.lastRecruitmentWave = last;
		return run;
	}

	void write(int slot, const RoguelikeRun & state) const
	{
		const auto dest = path(slot);
		if(state.phase == Phase::Defeat)
		{
			std::filesystem::remove(dest);
			return;
		}
		std::filesystem::create_directories(directory);
		// A unique staging directory keeps independent clients from sharing a partial file.
		auto stage = dest;
		do
		{
			stage = dest;
			stage += ".writing-" + std::to_string(std::random_device{}());
		} while(!std::filesystem::create_directory(stage));
		const auto temp = stage / "state.tmp";
		try
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			out.exceptions(std::ios::badbit | std::ios::failbit);
			out << "Evolution2/2 " << state.lastRecruitmentWave << '\n';
			state.save(out);
			out.close();
			std::filesystem::rename(temp, dest);
		}
		catch(...)
		{
			std::error_code ignored;
			std::filesystem::remove(temp,ignored);
			std::filesystem::remove(stage,ignored);
			throw;
		}
		std::error_code ignored;
		std::filesystem::remove(stage,ignored);
	}

private:
	std::filesystem::path directory;
};
}
