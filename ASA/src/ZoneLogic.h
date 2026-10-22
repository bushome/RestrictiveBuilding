#pragma once

// Pure logic for RestrictiveBuilding: config parsing, class matching, the list of protected zones and
// the "is this placement allowed" check.
//
// This header deliberately has no dependency on the game or on server API game types (only on the
// nlohmann json.hpp that ships with AsaApi and AseApi), so it can be compiled and unit-tested on any
// machine. It is identical in the ASA and ASE folders; keep the two copies in sync. See tests/logic_tests.cpp.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"

namespace RestrictiveBuilding
{
	// One foundation is 300 Unreal units in both ASE and ASA.
	constexpr double kFoundationUnits = 300.0;

	// Player and tribe teams start at 50000; anything below belongs to the world.
	constexpr int kFirstPlayerTeam = 50000;

	// ------------------------------------------------------------------------------------------
	// String helpers
	// ------------------------------------------------------------------------------------------

	inline std::string ToLower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		return out;
	}

	inline std::string Trim(std::string_view text)
	{
		const char* whitespace = " \t\r\n";
		const size_t first = text.find_first_not_of(whitespace);
		if (first == std::string_view::npos)
			return {};
		const size_t last = text.find_last_not_of(whitespace);
		return std::string(text.substr(first, last - first + 1));
	}

	inline bool EndsWith(std::string_view text, std::string_view suffix)
	{
		return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
	}

	inline bool StartsWith(std::string_view text, std::string_view prefix)
	{
		return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
	}

	/**
	 * \brief Wildcard match. '*' matches any run of characters (including none), '?' matches exactly one.
	 * Both arguments are expected to be lower-cased already.
	 */
	inline bool WildcardMatch(std::string_view pattern, std::string_view text)
	{
		size_t p = 0, t = 0;
		size_t star = std::string_view::npos, star_text = 0;

		while (t < text.size())
		{
			if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
			{
				++p;
				++t;
			}
			else if (p < pattern.size() && pattern[p] == '*')
			{
				star = p++;
				star_text = t;
			}
			else if (star != std::string_view::npos)
			{
				p = star + 1;
				t = ++star_text;
			}
			else
			{
				return false;
			}
		}

		while (p < pattern.size() && pattern[p] == '*')
			++p;

		return p == pattern.size();
	}

	/**
	 * \brief Replaces {key} placeholders. Unknown placeholders are left untouched, and a value that
	 * itself looks like a placeholder is never expanded again.
	 */
	inline std::string FormatTemplate(const std::string& text,
		const std::vector<std::pair<std::string, std::string>>& values)
	{
		std::string out;
		out.reserve(text.size());

		size_t pos = 0;
		while (pos < text.size())
		{
			const size_t open = text.find('{', pos);
			if (open == std::string::npos)
			{
				out.append(text, pos, std::string::npos);
				break;
			}

			out.append(text, pos, open - pos);
			const size_t close = text.find('}', open + 1);
			if (close == std::string::npos)
			{
				out.append(text, open, std::string::npos);
				break;
			}

			const std::string key = text.substr(open + 1, close - open - 1);
			bool replaced = false;
			for (const auto& [name, value] : values)
			{
				if (name == key)
				{
					out += value;
					replaced = true;
					break;
				}
			}
			if (!replaced)
				out.append(text, open, close - open + 1);

			pos = close + 1;
		}

		return out;
	}

	/**
	 * \brief Lower-cases a class name and drops the blueprint "_C" suffix:
	 * "PrimalItemStructure_SleepingBag_Hide_C" -> "primalitemstructure_sleepingbag_hide".
	 */
	inline std::string NormalizeClassName(std::string_view name)
	{
		std::string out = ToLower(Trim(name));
		if (EndsWith(out, "_c"))
			out.resize(out.size() - 2);
		return out;
	}

	// Lower-cases a blueprint path and removes a Blueprint'...' wrapper, keeping everything else.
	inline std::string UnwrapBlueprintPath(std::string_view path)
	{
		std::string out = ToLower(Trim(path));

		const size_t open_quote = out.find('\'');
		if (open_quote != std::string::npos)
		{
			out.erase(0, open_quote + 1);
			const size_t close_quote = out.find('\'');
			if (close_quote != std::string::npos)
				out.erase(close_quote);
		}
		return out;
	}

	/**
	 * \brief Reduces every common way of writing a blueprint path to the same string:
	 * "Blueprint'/Game/Mods/X/Bag.Bag'", "/Game/Mods/X/Bag.Bag_C" and "/Game/Mods/X/Bag"
	 * all become "/game/mods/x/bag".
	 */
	inline std::string NormalizeBlueprintPath(std::string_view path)
	{
		std::string out = UnwrapBlueprintPath(path);

		const size_t last_slash = out.find_last_of('/');
		const size_t dot = out.find('.', last_slash == std::string::npos ? 0 : last_slash);
		if (dot != std::string::npos)
			out.erase(dot);

		if (EndsWith(out, "_c"))
			out.resize(out.size() - 2);

		return out;
	}

	// ------------------------------------------------------------------------------------------
	// Class patterns
	// ------------------------------------------------------------------------------------------

	/**
	 * \brief One entry from a Whitelist, ActorClasses or BPIgnore list.
	 *
	 *   "PrimalItemStructure_SleepingBag_Hide"  class name (the _C suffix is optional), also matching
	 *                                          every class that inherits from it
	 *   "*Obelisk*"                            wildcard on any class name in the inheritance chain
	 *   "/Game/Mods/SomeMod/Bag.Bag"           exact blueprint path (any common spelling)
	 *   "/Game/Mods/SomeMod/<wildcard>"        a * or ? wildcard on the blueprint path
	 *   "/SomeMod/"                            a path ending in '/' matches any blueprint path containing it
	 */
	struct Pattern
	{
		std::string raw;       // exactly as written in config.json (used for logging)
		std::string lowered;   // normalised form used for matching
		bool is_path = false;  // contains '/', so it is matched against the blueprint path
		bool has_wildcard = false;
		bool is_folder = false; // path ending in '/': "contains" match
	};

	inline Pattern MakePattern(const std::string& entry)
	{
		Pattern pattern;
		pattern.raw = Trim(entry);
		pattern.is_path = pattern.raw.find('/') != std::string::npos;
		pattern.has_wildcard = pattern.raw.find_first_of("*?") != std::string::npos;

		if (pattern.is_path)
		{
			const std::string unwrapped = UnwrapBlueprintPath(pattern.raw);
			if (pattern.has_wildcard)
			{
				pattern.lowered = unwrapped;
			}
			else if (EndsWith(unwrapped, "/"))
			{
				pattern.is_folder = true;
				pattern.lowered = unwrapped;
			}
			else
			{
				pattern.lowered = NormalizeBlueprintPath(pattern.raw);
			}
		}
		else
		{
			pattern.lowered = pattern.has_wildcard ? ToLower(pattern.raw) : NormalizeClassName(pattern.raw);
		}

		return pattern;
	}

	/**
	 * \brief What the plugin knows about one class: its own name first, then each parent class up the
	 * inheritance chain (blueprint and native), plus the blueprint path of the class itself.
	 */
	struct ClassInfo
	{
		std::vector<std::string> class_chain;
		std::string blueprint;
	};

	inline bool PatternMatches(const Pattern& pattern, const ClassInfo& info)
	{
		if (pattern.lowered.empty())
			return false;

		if (pattern.is_path)
		{
			if (info.blueprint.empty())
				return false;

			if (pattern.is_folder)
				return UnwrapBlueprintPath(info.blueprint).find(pattern.lowered) != std::string::npos;

			if (pattern.has_wildcard)
			{
				const std::string unwrapped = UnwrapBlueprintPath(info.blueprint);
				return WildcardMatch(pattern.lowered, unwrapped) || WildcardMatch(pattern.lowered, NormalizeBlueprintPath(unwrapped));
			}

			return NormalizeBlueprintPath(info.blueprint) == pattern.lowered;
		}

		for (const std::string& name : info.class_chain)
		{
			const std::string normalized = NormalizeClassName(name);
			if (pattern.has_wildcard ? WildcardMatch(pattern.lowered, normalized) : normalized == pattern.lowered)
				return true;
		}
		return false;
	}

	inline const Pattern* FindMatch(const std::vector<Pattern>& patterns, const ClassInfo& info)
	{
		for (const Pattern& pattern : patterns)
		{
			if (PatternMatches(pattern, info))
				return &pattern;
		}
		return nullptr;
	}

	inline bool AnyMatch(const std::vector<Pattern>& patterns, const ClassInfo& info)
	{
		return FindMatch(patterns, info) != nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Zone types
	// ------------------------------------------------------------------------------------------

	enum class ZoneType : int
	{
		SpawnPoints = 0,
		BeaverSpawns,
		MissionTerminals,
		CityTerminals,
		TributeTerminals,
		SupplyDrops,
		HordeDrops,
		ExplorerNotes,
	};

	constexpr size_t kZoneTypeCount = 8;

	// The key of each type in config.json "Zones", in ZoneType order.
	inline const char* ZoneTypeKey(ZoneType type)
	{
		static const char* const keys[kZoneTypeCount] = {
			"SpawnPoints", "BeaverSpawns", "MissionTerminals", "CityTerminals",
			"TributeTerminals", "SupplyDrops", "HordeDrops", "ExplorerNotes" };
		return keys[static_cast<size_t>(type)];
	}

	inline ZoneType ZoneTypeAt(size_t index)
	{
		return static_cast<ZoneType>(index);
	}

	/**
	 * \brief Built-in starting points for "ActorClasses", used when the config leaves the key out.
	 * Supply drops are found through the map's supply crate volumes as well (see the plugin), so
	 * their list starts empty.
	 */
	inline std::vector<std::string> DefaultActorClasses(ZoneType type)
	{
		switch (type)
		{
		case ZoneType::SpawnPoints:      return { "PlayerStart" };
		case ZoneType::BeaverSpawns:     return { "BeaverDam*" };
		case ZoneType::MissionTerminals: return { "MissionDispatcher*" };
		case ZoneType::CityTerminals:    return { "*CityTerminal*" };
		case ZoneType::TributeTerminals: return { "TributeTerminal*", "*Obelisk*" };
		case ZoneType::SupplyDrops:      return {};
		case ZoneType::HordeDrops:       return { "*HordeCrate*SpawnPoint*", "*ElementNode*", "*ElementVein*" };
		case ZoneType::ExplorerNotes:    return { "ExplorerChest*", "NamedExplorerChest*", "PointOfInterestBP_MissionStart_Debugging" };
		}
		return {};
	}

	struct ZoneTypeSettings
	{
		bool enabled = true;
		double range_foundations = 10.0;
		std::vector<Pattern> whitelist;     // structures allowed inside the zone anyway
		std::vector<Pattern> actor_classes; // world actors that create a zone of this type
		std::vector<Pattern> ignore;        // world actors that never create a zone (BPIgnore)
		std::string message;                // chat text when a placement is refused
	};

	struct Vec3
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	inline double Distance(const Vec3& a, const Vec3& b)
	{
		const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	struct CustomLocation
	{
		std::string name;
		bool enabled = true;
		Vec3 location;
		double range_foundations = 0.0;
		std::vector<Pattern> whitelist;
		std::string message; // empty: Chat.MessageCustom
	};

	struct ChatSettings
	{
		std::string sender = "SERVER";
		double cooldown_seconds = 2.0;
		std::string custom_message = "You can not build here.";
		// Appended for admins so they can tell which zone refused the placement.
		std::string admin_suffix = " [{source}, {distance}/{range} foundations]";
		std::string help_command = "/nobuild";
		std::string help_inside = "You are inside a protected area around a {zone}: {distance} of {range} foundations from its center.";
		std::string help_nearest = "The nearest protected area is around a {zone}, {distance} foundations away (it reaches {range} foundations).";
		std::string help_none = "There is no protected area on this map.";
	};

	struct Settings
	{
		bool enabled = true;
		bool allow_admins = false;
		int rescan_minutes = 10;
		bool log_blocked = false;
		bool read_supply_volumes = true;

		ChatSettings chat;

		std::array<ZoneTypeSettings, kZoneTypeCount> types;

		// What {note} says. One config per server, so set it per map: "glitch" on Genesis, "rune" on Fjordur.
		std::string explorer_note_name = "explorer note";

		std::vector<CustomLocation> custom_locations;

		// Non-fatal problems found while reading the config. The plugin logs these.
		std::vector<std::string> warnings;

		const ZoneTypeSettings& Type(ZoneType type) const { return types[static_cast<size_t>(type)]; }
		ZoneTypeSettings& Type(ZoneType type) { return types[static_cast<size_t>(type)]; }
	};

	// Default chat text per type. These match the messages of the plugin this one replaces.
	inline std::string DefaultMessage(ZoneType type)
	{
		switch (type)
		{
		case ZoneType::SpawnPoints:      return "You can not build here you are too close to a spawn point.";
		case ZoneType::BeaverSpawns:     return "You can not build here you are too close to a beaver spawn point.";
		case ZoneType::MissionTerminals: return "You can not build here you are too close to a mission terminal.";
		case ZoneType::CityTerminals:    return "You can not build here you are too close to a city terminal.";
		case ZoneType::TributeTerminals: return "You can not build here you are too close to a tribute terminal.";
		case ZoneType::SupplyDrops:      return "You can not build here you are too close to a supply drop.";
		case ZoneType::HordeDrops:       return "You can not build here you are too close to a orbital supply drop or element node.";
		case ZoneType::ExplorerNotes:    return "You can not build here you are too close to a {note}.";
		}
		return "You can not build here.";
	}

	// Short name of a type for {zone} placeholders and logs.
	inline std::string DefaultLabel(ZoneType type)
	{
		switch (type)
		{
		case ZoneType::SpawnPoints:      return "spawn point";
		case ZoneType::BeaverSpawns:     return "beaver spawn point";
		case ZoneType::MissionTerminals: return "mission terminal";
		case ZoneType::CityTerminals:    return "city terminal";
		case ZoneType::TributeTerminals: return "tribute terminal";
		case ZoneType::SupplyDrops:      return "supply drop";
		case ZoneType::HordeDrops:       return "orbital supply drop or element node";
		case ZoneType::ExplorerNotes:    return "explorer note";
		}
		return "protected location";
	}

	// ------------------------------------------------------------------------------------------
	// Config parsing
	// ------------------------------------------------------------------------------------------

	namespace detail
	{
		template <typename T>
		T ReadValue(const nlohmann::json& object, const char* key, T fallback, const std::string& where)
		{
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
				return fallback;

			try
			{
				return it->get<T>();
			}
			catch (const nlohmann::json::exception&)
			{
				throw std::runtime_error(where + "\"" + key + "\" has the wrong type (found " + it->type_name() + ")");
			}
		}

		inline bool ReadBool(const nlohmann::json& object, const char* key, bool fallback, const std::string& where)
		{
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
				return fallback;
			if (!it->is_boolean())
				throw std::runtime_error(where + "\"" + key + "\" must be true or false (found " + it->type_name() + ")");
			return it->get<bool>();
		}

		inline double ReadNumber(const nlohmann::json& object, const char* key, double fallback, const std::string& where)
		{
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
				return fallback;
			if (!it->is_number())
				throw std::runtime_error(where + "\"" + key + "\" must be a number (found " + it->type_name() + ")");
			return it->get<double>();
		}

		inline std::string ReadString(const nlohmann::json& object, const char* key, const std::string& fallback, const std::string& where)
		{
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
				return fallback;
			if (!it->is_string())
				throw std::runtime_error(where + "\"" + key + "\" must be text in quotes (found " + it->type_name() + ")");
			return it->get<std::string>();
		}

		inline const nlohmann::json& ReadSection(const nlohmann::json& object, const char* key, bool array,
			const nlohmann::json& empty, const std::string& where)
		{
			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
				return empty;
			if (array ? !it->is_array() : !it->is_object())
				throw std::runtime_error(where + "\"" + key + "\" must be " + (array ? "a list [...]" : "an object {...}"));
			return *it;
		}

		inline std::vector<Pattern> ReadPatterns(const nlohmann::json& object, const char* key,
			const std::vector<std::string>& fallback, const std::string& where)
		{
			std::vector<Pattern> patterns;

			const auto it = object.find(key);
			if (it == object.end() || it->is_null())
			{
				for (const std::string& entry : fallback)
					patterns.push_back(MakePattern(entry));
				return patterns;
			}

			if (!it->is_array())
				throw std::runtime_error(where + "\"" + key + "\" must be a list [...]");

			for (const auto& entry : *it)
			{
				if (!entry.is_string())
					throw std::runtime_error(where + "\"" + key + "\": every entry must be text in quotes (found " + entry.type_name() + ")");

				Pattern pattern = MakePattern(entry.get<std::string>());
				if (!pattern.lowered.empty())
					patterns.push_back(std::move(pattern));
			}
			return patterns;
		}

		inline double ReadRange(const nlohmann::json& object, double fallback, const std::string& where)
		{
			const double range = ReadNumber(object, "RangeInFoundations", fallback, where);
			if (range < 0.0)
				throw std::runtime_error(where + "\"RangeInFoundations\" cannot be negative");
			return range;
		}

		// Warns about keys the plugin does not read, so a typo does not silently do nothing.
		inline void CheckKeys(const nlohmann::json& object, std::initializer_list<const char*> known,
			const std::string& where, std::vector<std::string>& warnings)
		{
			for (auto it = object.begin(); it != object.end(); ++it)
			{
				bool found = false;
				for (const char* key : known)
				{
					if (it.key() == key)
					{
						found = true;
						break;
					}
				}
				if (found)
					continue;

				if (it.key() == "TribeIDs" || it.key() == "Permissions")
					warnings.push_back(where + "\"" + it.key() + "\" is ignored: this plugin has no bypass lists");
				else
					warnings.push_back(where + "unknown setting \"" + it.key() + "\" is ignored");
			}
		}

		inline Vec3 ReadLocation(const nlohmann::json& object, const std::string& where)
		{
			const auto it = object.find("Location");
			if (it == object.end() || !it->is_array() || it->size() != 3)
				throw std::runtime_error(where + "\"Location\" must be a list of three numbers [x, y, z]");

			for (const auto& value : *it)
			{
				if (!value.is_number())
					throw std::runtime_error(where + "\"Location\" must be a list of three numbers [x, y, z]");
			}

			return Vec3{ (*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>() };
		}
	} // namespace detail

	/**
	 * \brief Builds Settings from parsed config.json. Throws std::runtime_error with a readable
	 * message if the config cannot be used.
	 */
	inline Settings ParseSettings(const nlohmann::json& root)
	{
		using namespace detail;

		if (!root.is_object())
			throw std::runtime_error("config.json must contain a JSON object at the top level");

		const nlohmann::json empty_object = nlohmann::json::object();
		const nlohmann::json empty_array = nlohmann::json::array();

		Settings settings;
		CheckKeys(root, { "General", "Chat", "Zones", "CustomLocations" }, "", settings.warnings);

		// --- General ---
		const auto& general = ReadSection(root, "General", false, empty_object, "");
		const std::string g = "General: ";
		CheckKeys(general, { "Enabled", "AdminBuildOverride",
			"RescanIntervalMinutes", "LogBlockedPlacements", "ReadSupplyCrateVolumes" }, g, settings.warnings);

		settings.enabled = ReadBool(general, "Enabled", settings.enabled, g);
		settings.allow_admins = ReadBool(general, "AdminBuildOverride", settings.allow_admins, g);
		settings.log_blocked = ReadBool(general, "LogBlockedPlacements", settings.log_blocked, g);
		settings.read_supply_volumes = ReadBool(general, "ReadSupplyCrateVolumes", settings.read_supply_volumes, g);

		const double rescan = ReadNumber(general, "RescanIntervalMinutes", settings.rescan_minutes, g);
		if (rescan < 0.0 || rescan != std::floor(rescan))
			throw std::runtime_error(g + "\"RescanIntervalMinutes\" must be a whole number, 0 or more");
		settings.rescan_minutes = static_cast<int>(rescan);


		// --- Chat ---
		const auto& chat = ReadSection(root, "Chat", false, empty_object, "");
		const std::string c = "Chat: ";
		CheckKeys(chat, { "SenderName", "MessageCooldownSeconds", "MessageCannotBuildCustom", "AdminSuffix",
			"HelpCommand", "HelpInside", "HelpNearest", "HelpNone" }, c, settings.warnings);

		auto& cs = settings.chat;
		cs.sender = ReadString(chat, "SenderName", cs.sender, c);
		cs.cooldown_seconds = ReadNumber(chat, "MessageCooldownSeconds", cs.cooldown_seconds, c);
		cs.custom_message = ReadString(chat, "MessageCannotBuildCustom", cs.custom_message, c);
		cs.admin_suffix = ReadString(chat, "AdminSuffix", cs.admin_suffix, c);
		cs.help_command = Trim(ReadString(chat, "HelpCommand", cs.help_command, c));
		cs.help_inside = ReadString(chat, "HelpInside", cs.help_inside, c);
		cs.help_nearest = ReadString(chat, "HelpNearest", cs.help_nearest, c);
		cs.help_none = ReadString(chat, "HelpNone", cs.help_none, c);

		if (cs.cooldown_seconds < 0.0)
			throw std::runtime_error(c + "\"MessageCooldownSeconds\" cannot be negative");
		if (!cs.help_command.empty() && cs.help_command.find(' ') != std::string::npos)
			throw std::runtime_error(c + "\"HelpCommand\" cannot contain spaces");

		// --- Zones ---
		const auto& zones = ReadSection(root, "Zones", false, empty_object, "");
		{
			std::vector<const char*> keys;
			for (size_t i = 0; i < kZoneTypeCount; ++i)
				keys.push_back(ZoneTypeKey(ZoneTypeAt(i)));

			for (auto it = zones.begin(); it != zones.end(); ++it)
			{
				bool found = false;
				for (const char* key : keys)
					found = found || it.key() == key;
				if (!found)
					settings.warnings.push_back("Zones: unknown zone type \"" + it.key() + "\" is ignored");
			}
		}

		for (size_t i = 0; i < kZoneTypeCount; ++i)
		{
			const ZoneType type = ZoneTypeAt(i);
			const std::string where = std::string("Zones \"") + ZoneTypeKey(type) + "\": ";
			const auto& section = ReadSection(zones, ZoneTypeKey(type), false, empty_object, "Zones: ");
			if (type == ZoneType::ExplorerNotes)
				CheckKeys(section, { "Enabled", "RangeInFoundations", "Whitelist", "ActorClasses", "BPIgnore", "Message", "NoteName" },
					where, settings.warnings);
			else
				CheckKeys(section, { "Enabled", "RangeInFoundations", "Whitelist", "ActorClasses", "BPIgnore", "Message" },
					where, settings.warnings);

			ZoneTypeSettings& ts = settings.types[i];
			ts.enabled = ReadBool(section, "Enabled", ts.enabled, where);
			ts.range_foundations = ReadRange(section, ts.range_foundations, where);
			ts.whitelist = ReadPatterns(section, "Whitelist", {}, where);
			ts.actor_classes = ReadPatterns(section, "ActorClasses", DefaultActorClasses(type), where);
			ts.ignore = ReadPatterns(section, "BPIgnore", {}, where);
			ts.message = ReadString(section, "Message", DefaultMessage(type), where);

			if (ts.enabled && ts.range_foundations <= 0.0)
				settings.warnings.push_back(where + "enabled with a range of 0, so it protects nothing");

			if (type == ZoneType::ExplorerNotes)
				settings.explorer_note_name = ReadString(section, "NoteName", settings.explorer_note_name, where);
		}

		// --- CustomLocations ---
		const auto& custom = ReadSection(root, "CustomLocations", true, empty_array, "");
		for (size_t l = 0; l < custom.size(); ++l)
		{
			const std::string where = "CustomLocations[" + std::to_string(l) + "]: ";
			const auto& entry = custom[l];
			if (!entry.is_object())
				throw std::runtime_error(where + "must be an object {...}");

			CheckKeys(entry, { "Name", "Enabled", "Location", "RangeInFoundations", "Whitelist", "Message" },
				where, settings.warnings);

			CustomLocation location;
			location.name = ReadString(entry, "Name", "", where);
			location.enabled = ReadBool(entry, "Enabled", true, where);
			location.location = ReadLocation(entry, where);
			if (entry.find("RangeInFoundations") == entry.end())
				throw std::runtime_error(where + "\"RangeInFoundations\" is required");
			location.range_foundations = ReadRange(entry, 0.0, where);
			location.whitelist = ReadPatterns(entry, "Whitelist", {}, where);
			location.message = ReadString(entry, "Message", "", where);

			settings.custom_locations.push_back(std::move(location));
		}

		bool any_enabled = false;
		for (const auto& ts : settings.types)
			any_enabled = any_enabled || ts.enabled;
		for (const auto& location : settings.custom_locations)
			any_enabled = any_enabled || location.enabled;
		if (!any_enabled)
			settings.warnings.push_back("no zone type and no custom location is enabled, so nothing is protected");

		return settings;
	}

	// ------------------------------------------------------------------------------------------
	// Zones
	// ------------------------------------------------------------------------------------------

	// A protected spot found in the world by the scan.
	struct FoundPoint
	{
		Vec3 location;
		std::string source; // class name of the actor that created it
	};

	// Result of a placement check or a zone lookup.
	struct ZoneHit
	{
		bool custom = false;
		ZoneType type = ZoneType::SpawnPoints;   // when !custom
		const CustomLocation* location = nullptr; // when custom
		Vec3 center;
		double radius = 0.0;   // Unreal units
		double distance = 0.0; // Unreal units, from the tested point to the center
		std::string source;    // actor class, or the custom location's name
	};

	/**
	 * \brief Every protected spot the scan found on the current map, by zone type. Radii are not
	 * stored: they come from the settings at check time, so a config reload changes them at once.
	 */
	class ZoneRegistry
	{
	public:
		void Clear()
		{
			for (auto& points : points_)
				points.clear();
			keys_.clear();
		}

		/**
		 * \brief Adds a spot unless the same type already has one at (practically) the same place.
		 * Returns true if it was new.
		 */
		bool Add(ZoneType type, const Vec3& location, std::string source)
		{
			if (!std::isfinite(location.x) || !std::isfinite(location.y) || !std::isfinite(location.z))
				return false;

			// Positions are rounded to 10 units for the duplicate check, which is far below any useful range.
			const auto round = [](double value) { return static_cast<long long>(std::llround(value / 10.0)); };
			std::string key = std::to_string(static_cast<int>(type)) + ":" + std::to_string(round(location.x)) + ":"
				+ std::to_string(round(location.y)) + ":" + std::to_string(round(location.z));

			if (!keys_.insert(std::move(key)).second)
				return false;

			points_[static_cast<size_t>(type)].push_back(FoundPoint{ location, std::move(source) });
			return true;
		}

		const std::vector<FoundPoint>& Points(ZoneType type) const { return points_[static_cast<size_t>(type)]; }

		size_t Count(ZoneType type) const { return points_[static_cast<size_t>(type)].size(); }

		size_t Total() const
		{
			size_t total = 0;
			for (const auto& points : points_)
				total += points.size();
			return total;
		}

		/**
		 * \brief Calls visit(hit) for every enabled zone on this map (built-in and custom). Stops early if
		 * visit returns false.
		 */
		template <typename Visitor>
		void ForEachZone(const Settings& settings, Visitor&& visit) const
		{
			for (size_t i = 0; i < kZoneTypeCount; ++i)
			{
				const ZoneTypeSettings& ts = settings.types[i];
				if (!ts.enabled || ts.range_foundations <= 0.0)
					continue;

				const double radius = ts.range_foundations * kFoundationUnits;
				for (const FoundPoint& point : points_[i])
				{
					ZoneHit hit;
					hit.type = ZoneTypeAt(i);
					hit.center = point.location;
					hit.radius = radius;
					hit.source = point.source;
					if (!visit(hit))
						return;
				}
			}

			for (const CustomLocation& location : settings.custom_locations)
			{
				if (!location.enabled || location.range_foundations <= 0.0)
					continue;

				ZoneHit hit;
				hit.custom = true;
				hit.location = &location;
				hit.center = location.location;
				hit.radius = location.range_foundations * kFoundationUnits;
				hit.source = location.name.empty() ? std::string("custom location") : location.name;
				if (!visit(hit))
					return;
			}
		}

		/**
		 * \brief True if the zone's Whitelist covers the structure or the item it is built from.
		 */
		static bool IsWhitelisted(const Settings& settings, const ZoneHit& hit, const ClassInfo& structure, const ClassInfo& item)
		{
			const std::vector<Pattern>& whitelist = hit.custom ? hit.location->whitelist : settings.Type(hit.type).whitelist;
			return AnyMatch(whitelist, structure) || AnyMatch(whitelist, item);
		}

		/**
		 * \brief The zone that refuses a structure at this point, or nothing if the placement is fine.
		 * A zone refuses when the point is inside its radius and neither the structure nor the item it
		 * is built from is on that zone's Whitelist. If several zones refuse, the closest one is returned.
		 */
		std::optional<ZoneHit> FindBlockingZone(const Settings& settings, const Vec3& point,
			const ClassInfo& structure, const ClassInfo& item) const
		{
			std::optional<ZoneHit> best;

			ForEachZone(settings, [&](ZoneHit& hit)
				{
					const double distance = Distance(point, hit.center);
					if (distance > hit.radius)
						return true;
					if (best && distance >= best->distance)
						return true;

					if (IsWhitelisted(settings, hit, structure, item))
						return true;

					hit.distance = distance;
					best = hit;
					return true;
				});

			return best;
		}

		/**
		 * \brief The closest enabled zone to a point (inside it or not), for the help command.
		 */
		std::optional<ZoneHit> FindNearestZone(const Settings& settings, const Vec3& point) const
		{
			std::optional<ZoneHit> best;
			ForEachZone(settings, [&](ZoneHit& hit)
				{
					// Measured to the edge, so a big zone whose edge is close counts as close.
					const double distance = Distance(point, hit.center);
					const double to_edge = distance - hit.radius;
					if (!best || to_edge < best->distance - best->radius)
					{
						hit.distance = distance;
						best = hit;
					}
					return true;
				});
			return best;
		}

	private:
		std::array<std::vector<FoundPoint>, kZoneTypeCount> points_;
		std::unordered_set<std::string> keys_;
	};

	/**
	 * \brief Which built-in zone types an actor class creates, as a bit mask (bit = ZoneType index).
	 * The caller skips player-owned actors and anything that is not a world actor.
	 */
	inline unsigned ClassifyActorClass(const Settings& settings, const ClassInfo& info)
	{
		unsigned mask = 0;
		for (size_t i = 0; i < kZoneTypeCount; ++i)
		{
			const ZoneTypeSettings& ts = settings.types[i];
			if (AnyMatch(ts.actor_classes, info) && !AnyMatch(ts.ignore, info))
				mask |= 1u << i;
		}
		return mask;
	}

	// ------------------------------------------------------------------------------------------
	// Messages
	// ------------------------------------------------------------------------------------------

	inline std::string FormatFoundations(double units)
	{
		const double foundations = units / kFoundationUnits;
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%.1f", foundations);
		std::string text = buffer;
		if (EndsWith(text, ".0"))
			text.resize(text.size() - 2);
		return text;
	}

	inline std::string ZoneLabel(const Settings& settings, const ZoneHit& hit)
	{
		if (hit.custom)
			return hit.source;
		if (hit.type == ZoneType::ExplorerNotes)
			return settings.explorer_note_name;
		return DefaultLabel(hit.type);
	}

	inline std::vector<std::pair<std::string, std::string>> HitPlaceholders(const Settings& settings, const ZoneHit& hit)
	{
		const std::string& note = settings.explorer_note_name;
		return {
			{ "zone", ZoneLabel(settings, hit) },
			{ "note", note },
			{ "source", hit.source },
			{ "name", hit.custom ? hit.source : ZoneLabel(settings, hit) },
			{ "distance", FormatFoundations(hit.distance) },
			{ "range", FormatFoundations(hit.radius) },
		};
	}

	/**
	 * \brief The chat text for a refused placement. Admins get AdminSuffix appended.
	 */
	inline std::string BlockedMessage(const Settings& settings, const ZoneHit& hit, bool admin)
	{
		std::string text;
		if (hit.custom)
			text = hit.location->message.empty() ? settings.chat.custom_message : hit.location->message;
		else
			text = settings.Type(hit.type).message;

		if (admin)
			text += settings.chat.admin_suffix;

		return FormatTemplate(text, HitPlaceholders(settings, hit));
	}

	inline std::string HelpMessage(const Settings& settings, const std::optional<ZoneHit>& nearest)
	{
		if (!nearest)
			return settings.chat.help_none;

		ZoneHit hit = *nearest;
		const bool inside = hit.distance <= hit.radius;
		if (!inside)
			hit.distance -= hit.radius; // "{distance} foundations away" means away from the edge

		return FormatTemplate(inside ? settings.chat.help_inside : settings.chat.help_nearest,
			HitPlaceholders(settings, hit));
	}

	// ------------------------------------------------------------------------------------------
	// Supply crate spawn points
	// ------------------------------------------------------------------------------------------

	/**
	 * \brief Works out the element size of a game array whose elements are structs starting with an
	 * actor pointer (the supply crate volume's spawn point list), without knowing the struct layout.
	 *
	 * Each candidate size is tried, smallest first. A size fits when every element's leading pointer is
	 * either empty or a known world actor, and at least two of them are known actors. Smaller wrong sizes
	 * fail at the second element, because they land on the struct's other fields.
	 *
	 * read(offset, out) must copy the pointer-sized value at byte offset `offset` into `out` and return
	 * false if that memory cannot be read (the plugin guards it, because a wrong size can run past the
	 * array). Returns 0 if no candidate fits or the array has fewer than two elements; element 0 is
	 * always at offset 0, so a one-element array needs no size.
	 */
	template <typename ReadPointer, typename IsKnownActor>
	size_t DetectLeadingPointerStride(int count, ReadPointer&& read, IsKnownActor&& is_known_actor)
	{
		if (count < 2)
			return 0;

		// The size that finds the most actors wins; on a tie the smaller one. (A too-small size can still
		// fit when the struct's other fields are zero, but it then skips elements and finds fewer actors.)
		static constexpr size_t kCandidates[] = { 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128 };
		size_t best_stride = 0;
		int best_known = 1;
		for (const size_t stride : kCandidates)
		{
			bool fits = true;
			int known = 0;
			for (int i = 0; i < count && fits; ++i)
			{
				const void* pointer = nullptr;
				if (!read(static_cast<size_t>(i) * stride, pointer))
				{
					fits = false;
					break;
				}

				if (pointer == nullptr)
					continue;

				if (is_known_actor(pointer))
					++known;
				else
					fits = false;
			}

			if (fits && known > best_known)
			{
				best_stride = stride;
				best_known = known;
			}
		}
		return best_stride;
	}
} // namespace RestrictiveBuilding
