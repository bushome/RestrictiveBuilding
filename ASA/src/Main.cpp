// RestrictiveBuilding - stops players from placing structures on top of the spots that matter: spawn
// points, beaver dams, mission/city/tribute terminals, supply drops, OSD/element drops, explorer notes
// and any custom locations from config.json.
//
// How it works:
//   - Shortly after the server is ready, every actor in the loaded levels is checked against each zone
//     type's ActorClasses. World-owned matches become protected spots. Supply drop points are read from
//     the map's supply crate volumes. The scan repeats every RescanIntervalMinutes and keeps old spots.
//     It looks at a slice of the level actors per second, so no single server tick does the whole map.
//   - APrimalStructure::IsAllowedToBuild is hooked. When the game would allow a placement and the
//     placement point is inside a zone whose Whitelist does not cover the structure, the hook refuses it,
//     and tells the player in chat.
//
// Everything here runs on the game thread (the hook, AsaApi's one-second timer callback and the
// commands), so the globals below need no locking.

#include "API/ARK/Ark.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "ZoneLogic.h"

#ifndef PROJECT_NAME
#define PROJECT_NAME "RestrictiveBuilding"
#endif

namespace
{
	using namespace RestrictiveBuilding;

	const char* const kHookIsAllowedToBuild =
		"APrimalStructure.IsAllowedToBuild(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,FPlacementData&,bool,UE::Math::TRotator<double>,bool,bool,APawn*)";

	const char* const kCmdReload = "RestrictiveBuilding.Reload";
	const char* const kCmdRescan = "RestrictiveBuilding.Rescan";
	const char* const kCmdReport = "RestrictiveBuilding.Report";
	const char* const kCmdNearby = "RestrictiveBuilding.Nearby";
	const char* const kCmdDestroy = "RestrictiveBuilding.DestroyStuff";
	const char* const kTimerId = "RestrictiveBuilding.Timer";

	// Field paths the scan reads. Both exist in the game's own symbol data.
	const char* const kLevelActorsField = "ULevel.Actors2";
	const char* const kSupplySpawnPointsField = "ASupplyCrateSpawningVolume.LinkedSpawnPointEntries";

	// IsAllowedToBuild returns 1 when the game allows a placement (seen in placement traces on ASA and ASE).
	// The plugin refuses by returning 0 instead.
	constexpr int kAllowedResult = 1;
	constexpr int kRefuseResult = 0;

	// The first scan waits this long after the server reports itself ready, so late-loading levels are in.
	constexpr int kFirstScanDelaySeconds = 15;

	// Level actors a scan looks at per second. A busy map (200,000+ actors) takes under a minute, and no
	// single tick pays for the whole map.
	constexpr int kScanActorsPerTick = 5000;

	using Clock = std::chrono::steady_clock;

	// What the plugin knows about one UClass, worked out once per class.
	struct ClassEntry
	{
		ClassInfo info;
		unsigned zone_mask = 0;          // built-in zone types this class creates (ClassifyActorClass)
		bool is_supply_volume = false;   // ASupplyCrateSpawningVolume or a subclass
	};

	// Raw view of a game TArray, used where the element type is not declared in the API headers.
	struct RawArray
	{
		unsigned char* data;
		int num;
		int max;
	};

	struct ScanStats
	{
		int scans = 0;
		size_t actors_seen = 0;
		size_t supply_volumes = 0;
		size_t supply_points_read = 0;
		size_t supply_stride = 0;     // element size found for the supply spawn point list (0: unknown)
		bool supply_unreadable = false;
		double last_scan_ms = 0.0;      // time spent scanning, summed over the ticks
		double last_scan_seconds = 0.0; // wall time from start to finish
		double last_scan_worst_tick_ms = 0.0;
		// Per zone type: class name -> spots it created (all scans since the last full rebuild).
		std::array<std::map<std::string, int>, kZoneTypeCount> classes;
	};

	Settings g_settings;
	bool g_settings_loaded = false;
	ZoneRegistry g_zones;
	ScanStats g_stats;
	std::unordered_map<UClass*, ClassEntry> g_class_cache;
	std::unordered_map<AShooterPlayerController*, Clock::time_point> g_last_message;

	bool g_hook_installed = false;
	std::string g_help_command; // registered chat command, if any

	int g_ready_seconds = 0;     // seconds since the server reported ready
	bool g_first_scan_done = false;
	int g_seconds_since_scan = 0;

	// A scan in progress. Matches keep their location and class name, not the actor, so an actor that
	// goes away mid-scan (a destroyed beaver dam) leaves nothing stale behind. Spots found earlier stay in
	// force until the scan finishes, so placements are never unprotected while it runs.
	// Set of actor pointers seen by a scan, used to check that a supply volume's spawn point entries are
	// real actors. One flat table sized from the previous scan: inserting never allocates per element and
	// never rehashes mid-scan (which made one slice of the scan cost several ms), and releasing it at the
	// end is a single free instead of one per actor.
	class PointerSet
	{
	public:
		void Reserve(size_t expected)
		{
			size_t capacity = 1024;
			while (capacity < expected * 2)
				capacity <<= 1;
			slots_.assign(capacity, 0);
			size_ = 0;
		}

		void Insert(const void* pointer)
		{
			const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
			if (value == 0)
				return;
			if ((size_ + 1) * 2 > slots_.size())
				Grow();
			size_t index = Slot(value);
			while (slots_[index] != 0)
			{
				if (slots_[index] == value)
					return;
				index = (index + 1) & (slots_.size() - 1);
			}
			slots_[index] = value;
			++size_;
		}

		size_t count(const void* pointer) const
		{
			const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
			if (value == 0 || slots_.empty())
				return 0;
			size_t index = Slot(value);
			while (slots_[index] != 0)
			{
				if (slots_[index] == value)
					return 1;
				index = (index + 1) & (slots_.size() - 1);
			}
			return 0;
		}

		void Release()
		{
			std::vector<uintptr_t>().swap(slots_);
			size_ = 0;
		}

	private:
		size_t Slot(uintptr_t value) const
		{
			// Actors are 16-byte aligned; drop the low bits, then spread with a multiplicative hash.
			return static_cast<size_t>(((value >> 4) * 0x9E3779B97F4A7C15ull) >> 20) & (slots_.size() - 1);
		}

		// Only when the map grew past twice the previous scan; a fresh table is built.
		void Grow()
		{
			std::vector<uintptr_t> old;
			old.swap(slots_);
			slots_.assign((std::max)(old.size() * 2, static_cast<size_t>(1024)), 0);
			size_ = 0;
			for (uintptr_t value : old)
			{
				if (value != 0)
					Insert(reinterpret_cast<const void*>(value));
			}
		}

		std::vector<uintptr_t> slots_;
		size_t size_ = 0;
	};

	struct PendingSpot
	{
		Vec3 location;
		std::string source;
		unsigned mask = 0;
	};

	struct ScanJob
	{
		bool running = false;
		bool rebuild = false;
		size_t level = 0;
		int32 actor = 0;
		Clock::time_point started{};
		double busy_ms = 0.0;
		double worst_tick_ms = 0.0;
		size_t seen = 0;
		PointerSet known;
		std::vector<AActor*> supply_volumes;
		std::vector<PendingSpot> matches;
	};
	ScanJob g_scan;

	// ------------------------------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------------------------------

	std::string ToUtf8(const FString& text)
	{
		return AsaApi::Tools::Utf8Encode(std::wstring(*text));
	}

	FString FromUtf8(const std::string& text)
	{
		return FString(AsaApi::Tools::Utf8Decode(text));
	}

	Vec3 ToVec(const FVector& v)
	{
		return Vec3{ v.X, v.Y, v.Z };
	}

	FVector ToFVector(const Vec3& v)
	{
		return FVector(v.x, v.y, v.z);
	}

	UWorld* World()
	{
		return AsaApi::GetApiUtils().GetWorld();
	}

	const ClassEntry& EntryFor(UClass* cls)
	{
		static const ClassEntry empty;
		if (cls == nullptr)
			return empty;

		const auto found = g_class_cache.find(cls);
		if (found != g_class_cache.end())
			return found->second;

		ClassEntry entry;

		// Own class first, then every parent up to UObject, blueprint and native alike.
		int depth = 0;
		for (UStruct* current = cls; current != nullptr && depth < 32; current = current->SuperStructField(), ++depth)
			entry.info.class_chain.push_back(current->NameField().ToString().ToString());

		entry.info.blueprint = AsaApi::IApiUtils::GetClassBlueprint(cls).ToString();
		entry.zone_mask = ClassifyActorClass(g_settings, entry.info);

		bool is_pawn = false;
		for (const std::string& name : entry.info.class_chain)
		{
			entry.is_supply_volume = entry.is_supply_volume || name == "SupplyCrateSpawningVolume";
			is_pawn = is_pawn || name == "Pawn";
		}

		// Creatures and characters never create zones, whatever their name matches (an "Obelisk" squid).
		if (is_pawn)
			entry.zone_mask = 0;

		return g_class_cache.emplace(cls, std::move(entry)).first->second;
	}

	std::string ShortChain(const ClassInfo& info, size_t count = 3)
	{
		std::string text;
		for (size_t i = 0; i < info.class_chain.size() && i < count; ++i)
		{
			if (i > 0)
				text += " <- ";
			text += info.class_chain[i];
		}
		return text.empty() ? std::string("?") : text;
	}

	std::string OwnClassName(const ClassInfo& info)
	{
		return info.class_chain.empty() ? std::string("?") : info.class_chain.front();
	}

	std::string ZoneMaskNames(unsigned mask)
	{
		std::string text;
		for (size_t i = 0; i < kZoneTypeCount; ++i)
		{
			if ((mask & (1u << i)) == 0)
				continue;
			if (!text.empty())
				text += ",";
			text += ZoneTypeKey(ZoneTypeAt(i));
		}
		return text;
	}

	void SendChat(AShooterPlayerController* player, const std::string& text)
	{
		const std::wstring wide_text = AsaApi::Tools::Utf8Decode(text);
		// The finished text is an argument, never the format string, so braces in it are safe.
		AsaApi::GetApiUtils().SendChatMessage(player, FromUtf8(g_settings.chat.sender), L"{}", wide_text.c_str());
	}

	void SendAdmin(AShooterPlayerController* player, const std::string& text, bool ok = true)
	{
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			const std::wstring wide_line = AsaApi::Tools::Utf8Decode(line);
			AsaApi::GetApiUtils().SendServerMessage(player, ok ? FColorList::Green : FColorList::Red, L"{}", wide_line.c_str());
		}
	}

	// ------------------------------------------------------------------------------------------
	// Scanning
	// ------------------------------------------------------------------------------------------

	// Copies a pointer-sized value from game memory, returning false instead of crashing if the
	// address cannot be read. No C++ objects in here: __try needs a function without unwinding.
	bool ReadPointerGuarded(const unsigned char* base, size_t offset, const void** out)
	{
		__try
		{
			*out = *reinterpret_cast<const void* const*>(base + offset);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	template <typename Visitor>
	void ForEachWorldActor(UWorld* world, Visitor&& visit)
	{
		for (const TObjectPtr<ULevel>& level_ptr : world->LevelsField())
		{
			ULevel* level = level_ptr.Get();
			if (level == nullptr)
				continue;

			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			for (AActor* actor : actors)
			{
				if (actor != nullptr)
					visit(actor);
			}
		}
	}

	void AddSpot(ZoneType type, const Vec3& location, const std::string& source)
	{
		if (g_zones.Add(type, location, source))
			++g_stats.classes[static_cast<size_t>(type)][source];
	}

	void ReadSupplyDropPoints(const std::vector<AActor*>& volumes, const PointerSet& known)
	{
		const auto is_known = [&known](const void* pointer) { return known.count(pointer) != 0; };

		// The spawn point list's element type is not in the API headers. Its first member is the spawn
		// point actor; work out the element size once, from the longest list.
		if (g_stats.supply_stride == 0)
		{
			const RawArray* longest = nullptr;
			for (AActor* volume : volumes)
			{
				const RawArray* entries = GetNativePointerField<RawArray*>(volume, kSupplySpawnPointsField);
				if (entries->data != nullptr && entries->num > 0 && (longest == nullptr || entries->num > longest->num))
					longest = entries;
			}

			if (longest != nullptr && longest->num >= 2)
			{
				const unsigned char* data = longest->data;
				g_stats.supply_stride = DetectLeadingPointerStride(longest->num,
					[data](size_t offset, const void*& out) { return ReadPointerGuarded(data, offset, &out); }, is_known);

				if (g_stats.supply_stride != 0)
					Log::GetLog()->info("Supply drop points: spawn point entries are {} bytes", g_stats.supply_stride);
				else
					g_stats.supply_unreadable = true;
			}
		}

		for (AActor* volume : volumes)
		{
			const RawArray* entries = GetNativePointerField<RawArray*>(volume, kSupplySpawnPointsField);
			if (entries->data == nullptr || entries->num <= 0)
				continue;

			// Element 0 sits at offset 0 whatever the element size is.
			const int count = g_stats.supply_stride != 0 ? entries->num : 1;
			for (int i = 0; i < count; ++i)
			{
				const void* pointer = nullptr;
				if (!ReadPointerGuarded(entries->data, static_cast<size_t>(i) * g_stats.supply_stride, &pointer))
					break;
				if (pointer == nullptr || !is_known(pointer))
					continue;

				AActor* point = static_cast<AActor*>(const_cast<void*>(pointer));
				const ClassEntry& point_entry = EntryFor(point->ClassField());
				if (point_entry.is_supply_volume)
					continue; // a volume listed as its own spawn point: its center is not a drop point

				++g_stats.supply_points_read;
				AddSpot(ZoneType::SupplyDrops, ToVec(point->GetLocation()), OwnClassName(point_entry.info));
			}
		}
	}

	/**
	 * \brief Starts a scan of the loaded levels; StepScan does the work a slice per second. rebuild=true
	 * replaces every spot found so far when the scan finishes (used after a config reload, because
	 * ActorClasses or BPIgnore may have changed). A scan already running starts over.
	 */
	void StartScan(bool rebuild)
	{
		const bool was_rebuild = g_scan.running && g_scan.rebuild;
		g_scan = ScanJob();
		g_scan.running = true;
		g_scan.rebuild = rebuild || was_rebuild;
		g_scan.started = Clock::now();
		// Room for a map 25% larger than the last one seen (or 256k actors on the first scan).
		g_scan.known.Reserve(g_stats.actors_seen > 0 ? g_stats.actors_seen + g_stats.actors_seen / 4 : 262144);
	}

	void FinishScan()
	{
		g_scan.running = false;

		if (g_scan.rebuild)
		{
			g_zones.Clear();
			for (auto& classes : g_stats.classes)
				classes.clear();
			g_stats.supply_points_read = 0;
			g_stats.supply_unreadable = false;
		}

		for (const PendingSpot& spot : g_scan.matches)
		{
			for (size_t i = 0; i < kZoneTypeCount; ++i)
			{
				if (spot.mask & (1u << i))
					AddSpot(ZoneTypeAt(i), spot.location, spot.source);
			}
		}

		// Supply crate volumes and their spawn points are part of the map and stay for its lifetime.
		g_stats.supply_volumes = g_scan.supply_volumes.size();
		if (g_settings.read_supply_volumes)
			ReadSupplyDropPoints(g_scan.supply_volumes, g_scan.known);

		++g_stats.scans;
		g_stats.actors_seen = g_scan.seen;
		g_stats.last_scan_ms = g_scan.busy_ms;
		g_stats.last_scan_seconds = std::chrono::duration<double>(Clock::now() - g_scan.started).count();
		g_stats.last_scan_worst_tick_ms = g_scan.worst_tick_ms;
		g_seconds_since_scan = 0;

		std::string counts;
		for (size_t i = 0; i < kZoneTypeCount; ++i)
		{
			counts += (i ? ", " : "");
			counts += std::string(ZoneTypeKey(ZoneTypeAt(i))) + "=" + std::to_string(g_zones.Count(ZoneTypeAt(i)));
		}

		Log::GetLog()->info("Scan {}: {} actors in {:.0f} s ({:.0f} ms of work, worst tick {:.1f} ms). Spots: {}",
			g_stats.scans, g_scan.seen, g_stats.last_scan_seconds, g_stats.last_scan_ms, g_stats.last_scan_worst_tick_ms,
			counts);

		if (g_stats.supply_unreadable)
			Log::GetLog()->warn("Supply drop points could not be read from the {} supply crate volume(s); only SupplyDrops "
				"ActorClasses are used", g_stats.supply_volumes);

		g_scan.known.Release();
		g_scan.supply_volumes.clear();
		g_scan.matches.clear();
	}

	// One slice of the scan: up to kScanActorsPerTick level actors.
	void StepScan()
	{
		UWorld* world = World();
		if (world == nullptr)
			return;

		const auto tick_start = Clock::now();
		int budget = kScanActorsPerTick;
		auto& levels = world->LevelsField();

		while (budget > 0)
		{
			if (g_scan.level >= static_cast<size_t>(levels.Num()))
				break;

			ULevel* level = levels[static_cast<int32>(g_scan.level)].Get();
			if (level == nullptr)
			{
				++g_scan.level;
				g_scan.actor = 0;
				continue;
			}

			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			while (budget > 0 && g_scan.actor < actors.Num())
			{
				AActor* actor = actors[g_scan.actor++];
				--budget;
				if (actor == nullptr)
					continue;

				++g_scan.seen;
				g_scan.known.Insert(actor);

				const ClassEntry& entry = EntryFor(actor->ClassField());
				if (entry.is_supply_volume)
					g_scan.supply_volumes.push_back(actor);

				// Player and tribe owned actors never create a zone (a Tek Transmitter is a tribute terminal).
				if (entry.zone_mask != 0 && actor->TargetingTeamField() < kFirstPlayerTeam)
					g_scan.matches.push_back({ ToVec(actor->GetLocation()), OwnClassName(entry.info), entry.zone_mask });
			}
			if (g_scan.actor >= actors.Num())
			{
				++g_scan.level;
				g_scan.actor = 0;
			}
		}

		const double ms = std::chrono::duration<double, std::milli>(Clock::now() - tick_start).count();
		g_scan.busy_ms += ms;
		g_scan.worst_tick_ms = (std::max)(g_scan.worst_tick_ms, ms);

		if (g_scan.level >= static_cast<size_t>(levels.Num()))
			FinishScan();
	}

	// ------------------------------------------------------------------------------------------
	// Config
	// ------------------------------------------------------------------------------------------

	Settings LoadSettings()
	{
		const std::string path = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME "/config.json";

		std::ifstream file(path);
		if (!file.is_open())
			throw std::runtime_error("cannot open " + path);

		nlohmann::json root;
		try
		{
			// Last argument: allow // and /* */ comments in config.json.
			root = nlohmann::json::parse(file, nullptr, true, true);
		}
		catch (const nlohmann::json::exception& error)
		{
			throw std::runtime_error(std::string("config.json is not valid JSON: ") + error.what());
		}

		return ParseSettings(root);
	}

	void HelpChat(AShooterPlayerController* player, FString* message, int mode, int something);

	void RegisterHelpCommand()
	{
		if (g_help_command == g_settings.chat.help_command)
			return;

		if (!g_help_command.empty())
			AsaApi::GetCommands().RemoveChatCommand(FString(g_help_command));

		g_help_command = g_settings.chat.help_command;
		if (!g_help_command.empty())
			AsaApi::GetCommands().AddChatCommand(FString(g_help_command), &HelpChat);
	}

	bool ReloadSettings(std::string& error)
	{
		try
		{
			Settings loaded = LoadSettings();

			for (const std::string& warning : loaded.warnings)
				Log::GetLog()->warn("Config: {}", warning);

			g_settings = std::move(loaded);
			g_settings_loaded = true;
			g_class_cache.clear(); // zone masks depend on ActorClasses and BPIgnore
			RegisterHelpCommand();

			Log::GetLog()->info("Config loaded: plugin {}, {} custom location(s)",
				g_settings.enabled ? "enabled" : "disabled", g_settings.custom_locations.size());
			return true;
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Placement hook
	// ------------------------------------------------------------------------------------------

	void OnRefused(AShooterPlayerController* player, const ZoneHit& hit, bool admin, const ClassInfo& structure)
	{
		const auto now = Clock::now();
		auto& last = g_last_message[player];
		const bool cooled_down = last == Clock::time_point{}
			|| std::chrono::duration<double>(now - last).count() >= g_settings.chat.cooldown_seconds;

		if (cooled_down)
		{
			last = now;
			SendChat(player, BlockedMessage(g_settings, hit, admin));
		}

		if (g_settings.log_blocked)
		{
			Log::GetLog()->info("Refused {} for {} ({}): {} \"{}\" at {} of {} foundations",
				OwnClassName(structure), ToUtf8(AsaApi::IApiUtils::GetEOSIDFromController(player)),
				ToUtf8(AsaApi::IApiUtils::GetCharacterName(player)),
				hit.custom ? "custom location" : ZoneTypeKey(hit.type), hit.source,
				FormatFoundations(hit.distance), FormatFoundations(hit.radius));
		}
	}

	bool ShouldRefuse(APrimalStructure* structure, APlayerController* controller, const FVector& location)
	{
		if (!g_settings_loaded || !g_settings.enabled || structure == nullptr || controller == nullptr)
			return false;

		if (!controller->IsA(AShooterPlayerController::GetPrivateStaticClass()))
			return false;

		auto* player = static_cast<AShooterPlayerController*>(controller);
		const bool admin = player->bIsAdmin()();
		if (admin && g_settings.allow_admins)
			return false;

		const ClassEntry& structure_entry = EntryFor(structure->ClassField());

		static const ClassInfo no_item;
		UClass* item_class = structure->GetConsumesPrimalItem().uClass;
		const ClassInfo& item = item_class != nullptr ? EntryFor(item_class).info : no_item;

		const std::optional<ZoneHit> hit = g_zones.FindBlockingZone(g_settings, ToVec(location), structure_entry.info, item);
		if (!hit)
			return false;

		OnRefused(player, *hit, admin, structure_entry.info);
		return true;
	}


	DECLARE_HOOK(APrimalStructure_IsAllowedToBuild, int, APrimalStructure*, APlayerController*, UE::Math::TVector<double>,
		UE::Math::TRotator<double>, FPlacementData*, bool, UE::Math::TRotator<double>, bool, bool, APawn*);

	int Hook_APrimalStructure_IsAllowedToBuild(APrimalStructure* _this, APlayerController* PC, UE::Math::TVector<double> AtLocation,
		UE::Math::TRotator<double> AtRotation, FPlacementData* OutPlacementData, bool bDontAdjustForMaxRange,
		UE::Math::TRotator<double> PlayerViewRotation, bool bFinalPlacement, bool bUseOriginalOutPlacementData, APawn* BuildingPawnOverride)
	{
		const int result = APrimalStructure_IsAllowedToBuild_original(_this, PC, AtLocation, AtRotation, OutPlacementData,
			bDontAdjustForMaxRange, PlayerViewRotation, bFinalPlacement, bUseOriginalOutPlacementData, BuildingPawnOverride);

		// Placement trace while testing (LogBlockedPlacements): every call, with the game's own result.
		if (g_settings.log_blocked)
		{
			const auto nearest = g_zones.FindNearestZone(g_settings, ToVec(AtLocation));
			Log::GetLog()->info("Trace IsAllowedToBuild: {} result={} final={} player={} at ({:.0f}, {:.0f}, {:.0f}) nearest zone {} fdn",
				_this != nullptr ? OwnClassName(EntryFor(_this->ClassField()).info) : std::string("?"), result, bFinalPlacement,
				PC != nullptr, AtLocation.X, AtLocation.Y, AtLocation.Z, nearest ? FormatFoundations(nearest->distance) : std::string("-"));
		}

		// Only placements the game itself allows are checked; a refusal from the game stands as it is.
		if (result != kAllowedResult)
			return result;

		try
		{
			if (ShouldRefuse(_this, PC, AtLocation))
				return kRefuseResult;
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("Placement check failed: {}", exception.what());
		}

		return result;
	}

	// ------------------------------------------------------------------------------------------
	// Commands
	// ------------------------------------------------------------------------------------------

	// The words after the command name, e.g. "RestrictiveBuilding.Nearby 20" -> { "20" }.
	std::vector<std::string> CommandArgs(FString* command)
	{
		std::vector<std::string> args;
		if (command == nullptr)
			return args;

		std::istringstream words(ToUtf8(*command));
		std::string word;
		words >> word; // the command itself
		while (words >> word)
			args.push_back(word);
		return args;
	}

	double ArgNumber(const std::vector<std::string>& args, size_t index, double fallback)
	{
		if (index >= args.size())
			return fallback;
		try
		{
			const double value = std::stod(args[index]);
			return value > 0.0 ? value : fallback;
		}
		catch (const std::exception&)
		{
			return fallback;
		}
	}

	std::string ReportText()
	{
		std::ostringstream out;
		out << "RestrictiveBuilding (" << (g_settings.enabled ? "enabled" : "DISABLED") << "): "
			<< g_stats.scans << " scan(s), last " << g_stats.actors_seen << " actors in "
			<< static_cast<int>(g_stats.last_scan_seconds) << " s (" << static_cast<int>(g_stats.last_scan_ms)
			<< " ms of work, worst tick " << static_cast<int>(g_stats.last_scan_worst_tick_ms) << " ms)"
			<< (g_scan.running ? "; a scan is running now" : "") << "\n";

		for (size_t i = 0; i < kZoneTypeCount; ++i)
		{
			const ZoneType type = ZoneTypeAt(i);
			const ZoneTypeSettings& ts = g_settings.types[i];
			out << ZoneTypeKey(type) << ": " << g_zones.Count(type) << " spot(s), "
				<< (ts.enabled ? FormatFoundations(ts.range_foundations * kFoundationUnits) + " foundations" : std::string("off"));

			// The classes that created the spots, most first.
			std::vector<std::pair<std::string, int>> classes(g_stats.classes[i].begin(), g_stats.classes[i].end());
			std::sort(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			for (size_t c = 0; c < classes.size(); ++c)
				out << (c == 0 ? " - " : ", ") << classes[c].first << " x" << classes[c].second;

			if (type == ZoneType::SupplyDrops)
			{
				out << " (" << g_stats.supply_volumes << " volume(s)";
				if (!g_settings.read_supply_volumes)
					out << ", volume reading off";
				else if (g_stats.supply_unreadable)
					out << ", spawn points unreadable";
				out << ")";
			}
			out << "\n";
		}

		size_t custom = 0;
		g_zones.ForEachZone(g_settings, [&custom](const ZoneHit& hit)
			{
				custom += hit.custom ? 1 : 0;
				return true;
			});
		out << "Custom locations enabled: " << custom;
		return out.str();
	}

	std::string ReloadCommandText()
	{
		std::string error;
		if (!ReloadSettings(error))
		{
			Log::GetLog()->error("Config reload failed: {}", error);
			return "RestrictiveBuilding: reload failed - " + error;
		}

		StartScan(true);
		g_first_scan_done = true;
		return "RestrictiveBuilding: config reloaded; rescan started (" + std::to_string(g_zones.Total()) +
			" spots stay in force until it finishes, under a minute)";
	}

	std::string RescanCommandText()
	{
		StartScan(true);
		g_first_scan_done = true;
		return "RestrictiveBuilding: rescan started (" + std::to_string(g_zones.Total()) +
			" spots stay in force until it finishes, under a minute)";
	}

	std::string DestroyStuffText(bool confirm)
	{
		struct Victim
		{
			APrimalStructure* structure;
			std::string zone;
		};

		std::unordered_map<AActor*, Victim> victims;
		UClass* structure_class = APrimalStructure::GetPrivateStaticClass();

		g_zones.ForEachZone(g_settings, [&](const ZoneHit& hit)
			{
				const TArray<AActor*> actors = AsaApi::GetApiUtils().GetAllActorsInRange(ToFVector(hit.center),
					static_cast<float>(hit.radius), EServerOctreeGroup::STRUCTURES);

				for (AActor* actor : actors)
				{
					if (actor == nullptr || victims.count(actor) != 0 || !actor->IsA(structure_class))
						continue;
					if (actor->TargetingTeamField() < kFirstPlayerTeam)
						continue; // never touch the map's own structures

					auto* structure = static_cast<APrimalStructure*>(actor);
					if (structure->SaddleDinoField().Get() != nullptr)
						continue; // built on a platform saddle; it only passes by

					if (Distance(ToVec(actor->GetLocation()), hit.center) > hit.radius)
						continue;

					const ClassInfo& info = EntryFor(actor->ClassField()).info;
					UClass* item_class = structure->GetConsumesPrimalItem().uClass;
					static const ClassInfo no_item;
					const ClassInfo& item = item_class != nullptr ? EntryFor(item_class).info : no_item;
					if (ZoneRegistry::IsWhitelisted(g_settings, hit, info, item))
						continue;

					victims.emplace(actor, Victim{ structure, std::string(hit.custom ? "custom location" : ZoneTypeKey(hit.type)) + " " + hit.source });
				}
				return true;
			});

		if (!confirm)
		{
			std::map<std::string, int> per_zone;
			for (const auto& [actor, victim] : victims)
				++per_zone[victim.zone];

			std::string text = "RestrictiveBuilding: " + std::to_string(victims.size()) + " player structure(s) are inside protected areas.";
			for (const auto& [zone, count] : per_zone)
				text += "\n  " + zone + ": " + std::to_string(count);
			if (!victims.empty())
				text += "\nRun \"" + std::string(kCmdDestroy) + " confirm\" to destroy them.";
			return text;
		}

		for (const auto& [actor, victim] : victims)
		{
			const FVector location = actor->GetLocation();
			Log::GetLog()->info("DestroyStuff: destroying {} of team {} at ({:.0f}, {:.0f}, {:.0f}) in {}",
				OwnClassName(EntryFor(actor->ClassField()).info), actor->TargetingTeamField(), location.X, location.Y, location.Z, victim.zone);
			victim.structure->Destroy(false, true);
		}

		return "RestrictiveBuilding: destroyed " + std::to_string(victims.size()) + " structure(s)";
	}

	std::string NearbyText(AShooterPlayerController* player, double foundations)
	{
		UWorld* world = World();
		if (world == nullptr)
			return "No world";

		const Vec3 origin = ToVec(AsaApi::IApiUtils::GetPosition(player));
		const double radius = foundations * kFoundationUnits;

		struct Found
		{
			double distance;
			AActor* actor;
		};
		std::vector<Found> found;
		ForEachWorldActor(world, [&](AActor* actor)
			{
				const double distance = Distance(ToVec(actor->GetLocation()), origin);
				if (distance > radius || actor->TargetingTeamField() >= kFirstPlayerTeam)
					return; // players' own things are never zones

				// Buffs and controllers ride along with characters, and plain meshes are scenery: never zones.
				const ClassInfo& info = EntryFor(actor->ClassField()).info;
				if (!info.class_chain.empty() && info.class_chain.front() == "StaticMeshActor")
					return;
				for (const std::string& name : info.class_chain)
				{
					if (name == "PrimalBuff" || name == "Controller")
						return;
				}
				found.push_back(Found{ distance, actor });
			});

		std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.distance < b.distance; });

		std::ostringstream out;
		out << found.size() << " actor(s) within " << FormatFoundations(radius) << " foundations (nearest first, full list in ArkApi.log):";
		Log::GetLog()->info("Nearby: {} actor(s) within {} foundations of ({:.0f}, {:.0f}, {:.0f})", found.size(),
			FormatFoundations(radius), origin.x, origin.y, origin.z);

		size_t shown = 0;
		for (const Found& entry : found)
		{
			const ClassEntry& info = EntryFor(entry.actor->ClassField());
			const int team = entry.actor->TargetingTeamField();
			const std::string zones = info.zone_mask != 0 ? ZoneMaskNames(info.zone_mask) : "-";

			Log::GetLog()->info("  {} fdn: {} | team {} | zones {} | {}", FormatFoundations(entry.distance),
				ShortChain(info.info, 8), team, zones, info.info.blueprint);

			if (shown < 15)
			{
				out << "\n" << FormatFoundations(entry.distance) << ": " << ShortChain(info.info) << " [" << zones << "]";
				++shown;
			}
		}
		return out.str();
	}

	AShooterPlayerController* AdminFrom(APlayerController* controller)
	{
		auto* player = static_cast<AShooterPlayerController*>(controller);
		if (player == nullptr || !player->bIsAdmin()())
			return nullptr;
		return player;
	}

	void ConsoleReload(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = ReloadCommandText();
			SendAdmin(admin, text, text.find("reload failed") == std::string::npos);
		}
	}

	void ConsoleRescan(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
			SendAdmin(admin, RescanCommandText());
	}

	void ConsoleReport(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = ReportText();
			Log::GetLog()->info("Report:\n{}", text);
			SendAdmin(admin, text);
		}
	}

	void ConsoleNearby(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
			SendAdmin(admin, NearbyText(admin, ArgNumber(CommandArgs(command), 0, 10.0)));
	}

	void ConsoleDestroy(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::vector<std::string> args = CommandArgs(command);
			SendAdmin(admin, DestroyStuffText(!args.empty() && ToLower(args[0]) == "confirm"));
		}
	}

	void RconReply(RCONClientConnection* connection, RCONPacket* packet, const std::string& text)
	{
		FString reply(text);
		connection->SendMessageW(packet->Id, 0, &reply);
	}

	void RconReload(RCONClientConnection* connection, RCONPacket* packet, UWorld*) { RconReply(connection, packet, ReloadCommandText()); }
	void RconRescan(RCONClientConnection* connection, RCONPacket* packet, UWorld*) { RconReply(connection, packet, RescanCommandText()); }
	void RconReport(RCONClientConnection* connection, RCONPacket* packet, UWorld*) { RconReply(connection, packet, ReportText()); }

	void RconDestroy(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		const std::vector<std::string> args = CommandArgs(&packet->Body);
		RconReply(connection, packet, DestroyStuffText(!args.empty() && ToLower(args[0]) == "confirm"));
	}

	void HelpChat(AShooterPlayerController* player, FString* /*message*/, int /*mode*/, int /*something*/)
	{
		if (player == nullptr || !g_settings_loaded)
			return;

		const Vec3 position = ToVec(AsaApi::IApiUtils::GetPosition(player));
		const std::optional<ZoneHit> nearest = g_zones.FindNearestZone(g_settings, position);
		SendChat(player, HelpMessage(g_settings, nearest));
	}

	// ------------------------------------------------------------------------------------------
	// Timer
	// ------------------------------------------------------------------------------------------

	void OnTimer()
	{
		if (AsaApi::GetApiUtils().GetStatus() != AsaApi::ServerStatus::Ready)
			return;

		++g_ready_seconds;

		if (!g_first_scan_done)
		{
			if (g_ready_seconds >= kFirstScanDelaySeconds)
			{
				g_first_scan_done = true;
				StartScan(true);
			}
		}
		else if (!g_scan.running && g_settings.rescan_minutes > 0 && ++g_seconds_since_scan >= g_settings.rescan_minutes * 60)
		{
			StartScan(false);
		}

		if (g_scan.running)
			StepScan();

		// Forget message cooldowns once a minute.
		if (g_ready_seconds % 60 == 0)
		{
			const auto now = Clock::now();
			for (auto it = g_last_message.begin(); it != g_last_message.end();)
			{
				if (now - it->second > std::chrono::minutes(1))
					it = g_last_message.erase(it);
				else
					++it;
			}
		}
	}

	void SafeTimer()
	{
		try
		{
			OnTimer();
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("Timer failed: {}", exception.what());
		}
	}
} // namespace

// Called by AsaApi when the plugin is loaded.
extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(PROJECT_NAME);

	std::string error;
	if (!ReloadSettings(error))
	{
		Log::GetLog()->error("Config not loaded: {}", error);
		Log::GetLog()->error("Nothing is protected until config.json is fixed and {} is run", kCmdReload);
	}

	g_hook_installed = AsaApi::GetHooks().SetHook(kHookIsAllowedToBuild,
		reinterpret_cast<LPVOID>(&Hook_APrimalStructure_IsAllowedToBuild), &APrimalStructure_IsAllowedToBuild_original);
	if (!g_hook_installed)
		Log::GetLog()->error("Could not hook {}; placements cannot be checked", kHookIsAllowedToBuild);

	AsaApi::GetCommands().AddConsoleCommand(kCmdReload, &ConsoleReload);
	AsaApi::GetCommands().AddConsoleCommand(kCmdRescan, &ConsoleRescan);
	AsaApi::GetCommands().AddConsoleCommand(kCmdReport, &ConsoleReport);
	AsaApi::GetCommands().AddConsoleCommand(kCmdNearby, &ConsoleNearby);
	AsaApi::GetCommands().AddConsoleCommand(kCmdDestroy, &ConsoleDestroy);

	AsaApi::GetCommands().AddRconCommand(kCmdReload, &RconReload);
	AsaApi::GetCommands().AddRconCommand(kCmdRescan, &RconRescan);
	AsaApi::GetCommands().AddRconCommand(kCmdReport, &RconReport);
	AsaApi::GetCommands().AddRconCommand(kCmdDestroy, &RconDestroy);

	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &SafeTimer);
}

// Called by AsaApi when the plugin is unloaded.
extern "C" __declspec(dllexport) void Plugin_Unload()
{
	AsaApi::GetCommands().RemoveOnTimerCallback(kTimerId);

	for (const char* command : { kCmdReload, kCmdRescan, kCmdReport, kCmdNearby, kCmdDestroy })
		AsaApi::GetCommands().RemoveConsoleCommand(command);
	for (const char* command : { kCmdReload, kCmdRescan, kCmdReport, kCmdDestroy })
		AsaApi::GetCommands().RemoveRconCommand(command);

	if (!g_help_command.empty())
	{
		AsaApi::GetCommands().RemoveChatCommand(FString(g_help_command));
		g_help_command.clear();
	}

	if (g_hook_installed)
	{
		AsaApi::GetHooks().DisableHook(kHookIsAllowedToBuild, reinterpret_cast<LPVOID>(&Hook_APrimalStructure_IsAllowedToBuild));
		g_hook_installed = false;
	}
}
