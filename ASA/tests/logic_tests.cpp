// Stand-alone tests for src/ZoneLogic.h. No game, AsaApi DLL or Windows needed.
//
// Build and run from the project folder (any C++20 compiler):
//   cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp && logic_tests.exe configs\config.json
//   clang++ -std=c++20 -I extern/AsaApi/AsaApi/Core/Public -I src tests/logic_tests.cpp -o logic_tests && ./logic_tests configs/config.json
//
// The optional argument is a config.json to validate. Add --shipped after it to also check the file
// against the values of the shipped config:
//   logic_tests.exe configs\config.json --shipped
// configs\config.commented.json holds the same values, so it must pass the same check.

#include <fstream>
#include <iostream>
#include <string>

#include "ZoneLogic.h"

using namespace RestrictiveBuilding;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": " #condition "\n"; \
		} \
	} while (false)

#define CHECK_THROWS(expression) \
	do { \
		++g_checks; \
		bool threw = false; \
		try { (void)(expression); } catch (const std::exception&) { threw = true; } \
		if (!threw) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": expected an error from " #expression "\n"; \
		} \
	} while (false)

static ClassInfo Class(std::vector<std::string> chain, std::string blueprint = "")
{
	ClassInfo info;
	info.class_chain = std::move(chain);
	info.blueprint = std::move(blueprint);
	return info;
}

static Settings Parse(const char* text)
{
	return ParseSettings(nlohmann::json::parse(text, nullptr, true, true));
}

static void TestStrings()
{
	CHECK(WildcardMatch("*", ""));
	CHECK(WildcardMatch("*obelisk*", "tributeterminal_obelisk_red"));
	CHECK(WildcardMatch("beaverdam*", "beaverdam"));
	CHECK(!WildcardMatch("beaverdam*", "beaver_character_bp"));
	CHECK(WildcardMatch("r?x", "rex"));
	CHECK(!WildcardMatch("", "x"));

	CHECK(NormalizeClassName("PrimalItemStructure_SleepingBag_Hide_C") == "primalitemstructure_sleepingbag_hide");
	CHECK(NormalizeBlueprintPath("Blueprint'/Game/Mods/X/Bag.Bag'") == "/game/mods/x/bag");
	CHECK(NormalizeBlueprintPath("/Game/Mods/X/Bag.Bag_C") == "/game/mods/x/bag");


	CHECK(FormatTemplate("too close to a {note}.", { { "note", "rune" } }) == "too close to a rune.");
	CHECK(FormatTemplate("old style {}.", { { "", "glitch" } }) == "old style glitch.");
	CHECK(FormatTemplate("{unknown} {a}", { { "a", "1" } }) == "{unknown} 1");
	CHECK(FormatTemplate("{a}", { { "a", "{a}!" } }) == "{a}!");
	CHECK(FormatTemplate("broken {a", { { "a", "1" } }) == "broken {a");

	CHECK(FormatFoundations(1500.0) == "5");
	CHECK(FormatFoundations(450.0) == "1.5");
}

static void TestPatterns()
{
	const ClassInfo bag = Class({ "PrimalItemStructure_SleepingBag_Hide_C", "PrimalItemStructureGeneric_C", "PrimalItem" },
		"Blueprint'/Game/PrimalEarth/CoreBlueprints/Items/Structures/Misc/PrimalItemStructure_SleepingBag_Hide.PrimalItemStructure_SleepingBag_Hide'");

	CHECK(PatternMatches(MakePattern("PrimalItemStructure_SleepingBag_Hide"), bag));
	CHECK(PatternMatches(MakePattern("primalitemstructure_sleepingbag_hide_c"), bag));
	CHECK(PatternMatches(MakePattern("PrimalItemStructureGeneric"), bag)); // parent class
	CHECK(PatternMatches(MakePattern("*SleepingBag*"), bag));
	CHECK(!PatternMatches(MakePattern("PrimalItemStructure_SleepingBag"), bag));
	CHECK(PatternMatches(MakePattern("/Game/PrimalEarth/CoreBlueprints/Items/Structures/Misc/PrimalItemStructure_SleepingBag_Hide"), bag));
	CHECK(PatternMatches(MakePattern("/Game/PrimalEarth/*"), bag));
	CHECK(PatternMatches(MakePattern("/Structures/Misc/"), bag)); // folder: contains
	CHECK(PatternMatches(MakePattern("Blueprint'/Game/PrimalEarth/CoreBlueprints/Items/'"), bag)); // folder in a wrapper
	CHECK(!PatternMatches(MakePattern("/modname1/"), bag));
	CHECK(!PatternMatches(MakePattern("/Game/Mods/*"), bag));

	// A path entry never matches on class name alone, and a class entry never matches a path.
	const ClassInfo no_path = Class({ "PrimalItemStructure_SleepingBag_Hide_C" });
	CHECK(!PatternMatches(MakePattern("/Game/PrimalEarth/*"), no_path));
	CHECK(!PatternMatches(MakePattern(""), bag));
}

static const char* kBaseConfig = R"({
	// comments are allowed
	"General": { "AdminBuildOverride": true, "RescanIntervalMinutes": 5 },
	"Zones": {
		"SpawnPoints": { "RangeInFoundations": 5, "Whitelist": [ "PrimalItemStructure_SleepingBag_Hide" ] },
		"TributeTerminals": { "RangeInFoundations": 30, "BPIgnore": [ "/Game/Mods/Transmitters/" ] },
		"BeaverSpawns": { "Enabled": false, "RangeInFoundations": 75 },
		"ExplorerNotes": { "RangeInFoundations": 5, "NoteName": "rune" }
	},
	"CustomLocations": [
		{ "Name": "Arena", "Location": [ 1000, 30000, 0 ], "RangeInFoundations": 10, "Whitelist": [ "*Sign*" ] },
		{ "Name": "Off", "Enabled": false, "Location": [ 0, 0, 0 ], "RangeInFoundations": 100 },
		{ "Location": [ 90000, 0, 0 ], "RangeInFoundations": 1, "Message": "Nope {range}" }
	]
})";

static void TestParsing()
{
	const Settings s = Parse(kBaseConfig);
	CHECK(s.enabled);
	CHECK(s.allow_admins);
	CHECK(s.rescan_minutes == 5);
	CHECK(s.Type(ZoneType::SpawnPoints).range_foundations == 5.0);
	CHECK(s.Type(ZoneType::SpawnPoints).whitelist.size() == 1);
	CHECK(!s.Type(ZoneType::BeaverSpawns).enabled);
	// Types left out keep their defaults: enabled, 10 foundations, default classes and message.
	CHECK(s.Type(ZoneType::MissionTerminals).enabled);
	CHECK(s.Type(ZoneType::MissionTerminals).range_foundations == 10.0);
	CHECK(s.Type(ZoneType::MissionTerminals).actor_classes.size() == DefaultActorClasses(ZoneType::MissionTerminals).size());
	CHECK(s.Type(ZoneType::MissionTerminals).message == DefaultMessage(ZoneType::MissionTerminals));
	CHECK(s.custom_locations.size() == 3);
	CHECK(!s.custom_locations[1].enabled);
	CHECK(s.warnings.empty());
	CHECK(s.explorer_note_name == "rune");
	CHECK(Parse("{}").explorer_note_name == "explorer note");

	// Bypass-list keys (TribeIDs, Permissions) are accepted but reported.
	const Settings bypass = Parse(R"({ "Zones": { "SpawnPoints": { "TribeIDs": [ 1 ], "Permissions": [ "VIP1" ] } },
		"CustomLocations": [ { "Location": [0,0,0], "RangeInFoundations": 1, "TribeIDs": [ 2 ] } ] })");
	CHECK(bypass.warnings.size() == 3);
	for (const auto& warning : bypass.warnings)
		CHECK(warning.find("no bypass lists") != std::string::npos);

	// Typos are reported, not silently ignored.
	CHECK(Parse(R"({ "General": { "Enabeld": true } })").warnings.size() == 1);
	CHECK(Parse(R"({ "Zones": { "Obelisks": {} } })").warnings.size() == 1);

	// Everything off: warned.
	const Settings off = Parse(R"({ "Zones": { "SpawnPoints": {"Enabled": false}, "BeaverSpawns": {"Enabled": false},
		"MissionTerminals": {"Enabled": false}, "CityTerminals": {"Enabled": false}, "TributeTerminals": {"Enabled": false},
		"SupplyDrops": {"Enabled": false}, "HordeDrops": {"Enabled": false}, "ExplorerNotes": {"Enabled": false} } })");
	CHECK(off.warnings.size() == 1);

	CHECK_THROWS(Parse("[]"));
	CHECK_THROWS(Parse(R"({ "General": { "Enabled": "yes" } })"));
	CHECK_THROWS(Parse(R"({ "General": { "RescanIntervalMinutes": -1 } })"));
	CHECK_THROWS(Parse(R"({ "General": { "RescanIntervalMinutes": 1.5 } })"));
	CHECK_THROWS(Parse(R"({ "Chat": { "HelpCommand": "/no build" } })"));
	CHECK_THROWS(Parse(R"({ "Chat": { "MessageCooldownSeconds": -2 } })"));
	CHECK_THROWS(Parse(R"({ "Zones": [] })"));
	CHECK_THROWS(Parse(R"({ "Zones": { "SpawnPoints": { "RangeInFoundations": -5 } } })"));
	CHECK_THROWS(Parse(R"({ "Zones": { "SpawnPoints": { "RangeInFoundations": "5" } } })"));
	CHECK_THROWS(Parse(R"({ "Zones": { "SpawnPoints": { "Whitelist": "bag" } } })"));
	CHECK_THROWS(Parse(R"({ "Zones": { "SpawnPoints": { "Whitelist": [ 5 ] } } })"));
	CHECK_THROWS(Parse(R"({ "Zones": { "ExplorerNotes": { "NoteName": 1 } } })"));
	CHECK(Parse(R"({ "Zones": { "SpawnPoints": { "NoteName": "x" } } })").warnings.size() == 1); // only ExplorerNotes has one
	CHECK_THROWS(Parse(R"({ "CustomLocations": {} })"));
	// The old per-map grouping has no Location, so it is refused.
	CHECK_THROWS(Parse(R"({ "CustomLocations": [ { "MapName": "X", "Locations": [] } ] })"));
	CHECK_THROWS(Parse(R"({ "CustomLocations": [ { "RangeInFoundations": 1 } ] })"));              // no Location
	CHECK_THROWS(Parse(R"({ "CustomLocations": [ { "Location": [1,2], "RangeInFoundations": 1 } ] })"));
	CHECK_THROWS(Parse(R"({ "CustomLocations": [ { "Location": [1,2,"3"], "RangeInFoundations": 1 } ] })"));
	CHECK_THROWS(Parse(R"({ "CustomLocations": [ { "Location": [1,2,3] } ] })"));                 // no range
	CHECK_THROWS(Parse(R"({ "CustomLocations": [ 5 ] })"));

	// The error names the place that is wrong.
	try
	{
		Parse(R"({ "CustomLocations": [ { "Location": [1,2,3], "RangeInFoundations": 1 }, { "Location": [1,2,3] } ] })");
	}
	catch (const std::exception& error)
	{
		const std::string message = error.what();
		CHECK(message.find("CustomLocations[1]") != std::string::npos);
		CHECK(message.find("RangeInFoundations") != std::string::npos);
	}
}

static void TestClassification()
{
	const Settings s = Parse(kBaseConfig);
	const auto bit = [](ZoneType type) { return 1u << static_cast<unsigned>(type); };

	CHECK(ClassifyActorClass(s, Class({ "PlayerStart", "NavigationObjectBase", "Actor", "Object" })) == bit(ZoneType::SpawnPoints));
	CHECK(ClassifyActorClass(s, Class({ "PrimalPlayerStart_C", "PlayerStart", "Actor" })) == bit(ZoneType::SpawnPoints));
	CHECK(ClassifyActorClass(s, Class({ "TributeTerminal_C", "PrimalStructureItemContainer", "Actor" },
		"Blueprint'/Game/PrimalEarth/Structures/TributeTerminal.TributeTerminal'")) == bit(ZoneType::TributeTerminals));
	CHECK(ClassifyActorClass(s, Class({ "Obelisk_Red_C", "Actor" })) == bit(ZoneType::TributeTerminals));
	// BPIgnore wins over ActorClasses.
	CHECK(ClassifyActorClass(s, Class({ "ModTransmitter_C", "TributeTerminal_C", "Actor" },
		"Blueprint'/Game/Mods/Transmitters/ModTransmitter.ModTransmitter'")) == 0);
	// Disabled types still classify, so turning one on with a reload works without a new scan.
	CHECK(ClassifyActorClass(s, Class({ "BeaverDam_C", "PrimalStructureItemContainer" })) == bit(ZoneType::BeaverSpawns));
	// A beaver is not a beaver dam.
	CHECK(ClassifyActorClass(s, Class({ "Beaver_Character_BP_C", "Dino_Character_BP_C" })) == 0);
	CHECK(ClassifyActorClass(s, Class({ "MissionDispatcher_MultiUsePylon_C", "MissionDispatcher", "Actor" })) == bit(ZoneType::MissionTerminals));
	// Class chains as seen on TheIsland_WP.
	CHECK(ClassifyActorClass(s, Class({ "ExplorerChest_Rockwell_C", "ExplorerChest_Base_C", "Actor", "PrimalActor", "Object" }))
		== bit(ZoneType::ExplorerNotes));
	CHECK(ClassifyActorClass(s, Class({ "ExplorerChest_DearJane_C", "NamedExplorerChest_UnlockPersistentItem_Base_C", "Actor" }))
		== bit(ZoneType::ExplorerNotes));
	CHECK(ClassifyActorClass(s, Class({ "ExplorerChest_Glitch_C", "Actor" })) == bit(ZoneType::ExplorerNotes));
	CHECK(ClassifyActorClass(s, Class({ "PointOfInterestBP_MissionStart_Debugging_C", "PointOfInterestBP_MissionStart_C",
		"PointOfInterest_Base_BP_C", "PointOfInterestActor", "Actor" })) == bit(ZoneType::ExplorerNotes));
	// Other Genesis mission start points are not glitches.
	CHECK(ClassifyActorClass(s, Class({ "PointOfInterestBP_MissionStart_C", "PointOfInterest_Base_BP_C", "Actor" })) == 0);
	CHECK(ClassifyActorClass(s, Class({ "BeaverDam_C", "StructureItemContainerBaseBP_C", "PrimalStructureItemContainer",
		"PrimalStructure" })) == bit(ZoneType::BeaverSpawns));
	CHECK(ClassifyActorClass(s, Class({ "StaticMeshActor", "Actor", "Object" })) == 0);
}

static void TestZones()
{
	const Settings s = Parse(kBaseConfig);
	ZoneRegistry zones;

	CHECK(zones.Add(ZoneType::SpawnPoints, { 0, 0, 0 }, "PlayerStart"));
	CHECK(!zones.Add(ZoneType::SpawnPoints, { 2, -3, 1 }, "PlayerStart"));     // same spot again
	CHECK(zones.Add(ZoneType::TributeTerminals, { 0, 0, 0 }, "TributeTerminal_C")); // same spot, other type
	CHECK(zones.Add(ZoneType::TributeTerminals, { 50000, 0, 0 }, "TributeTerminal_C"));
	CHECK(zones.Add(ZoneType::BeaverSpawns, { -20000, 0, 0 }, "BeaverDam_C"));      // type disabled
	CHECK(!zones.Add(ZoneType::SpawnPoints, { std::nan(""), 0, 0 }, "PlayerStart"));
	CHECK(zones.Total() == 4);
	CHECK(zones.Count(ZoneType::TributeTerminals) == 2);

	const ClassInfo foundation = Class({ "StoneFloor_C", "PrimalStructure" });
	const ClassInfo foundation_item = Class({ "PrimalItemStructure_StoneFloor_C", "PrimalItemStructureGeneric_C" });
	const ClassInfo bag = Class({ "SleepingBagHide_C", "PrimalStructureBed" });
	const ClassInfo bag_item = Class({ "PrimalItemStructure_SleepingBag_Hide_C" });
	const ClassInfo sign = Class({ "StructureSign_Wood_C", "PrimalStructureSign" });
	const ClassInfo none;

	// Inside the spawn point (5 foundations = 1500) and the tribute terminal (30 = 9000): the closer
	// center wins; both are at distance 1000, the first found is kept.
	auto hit = zones.FindBlockingZone(s, { 1000, 0, 0 }, foundation, foundation_item);
	CHECK(hit.has_value());
	CHECK(!hit->custom);

	// The bag is whitelisted at spawn points but not at tribute terminals, so the terminal refuses it.
	hit = zones.FindBlockingZone(s, { 1000, 0, 0 }, bag, bag_item);
	CHECK(hit.has_value());
	CHECK(hit->type == ZoneType::TributeTerminals);
	CHECK(hit->radius == 9000.0);

	// Outside the spawn point radius, inside the terminal radius.
	hit = zones.FindBlockingZone(s, { 0, 3000, 0 }, foundation, foundation_item);
	CHECK(hit.has_value() && hit->type == ZoneType::TributeTerminals);
	CHECK(hit->distance == 3000.0);

	// Exactly on the edge counts as inside; just past it does not.
	CHECK(zones.FindBlockingZone(s, { 50000, 9000, 0 }, foundation, foundation_item).has_value());
	CHECK(!zones.FindBlockingZone(s, { 50000, 9001, 0 }, foundation, foundation_item).has_value());

	// Height counts: the zone is a sphere.
	CHECK(!zones.FindBlockingZone(s, { 50000, 0, 9500 }, foundation, foundation_item).has_value());

	// The disabled beaver type refuses nothing.
	CHECK(!zones.FindBlockingZone(s, { -20000, 0, 0 }, foundation, foundation_item).has_value());

	// Custom locations; disabled ones never refuse.
	hit = zones.FindBlockingZone(s, { 1000, 32000, 0 }, foundation, none);
	CHECK(hit.has_value() && hit->custom && hit->source == "Arena");
	CHECK(!zones.FindBlockingZone(s, { 1000, 32000, 0 }, sign, none).has_value()); // whitelisted there
	CHECK(!zones.FindBlockingZone(s, { 0, 0, 30000 }, foundation, none).has_value());   // "Off" is disabled

	hit = zones.FindBlockingZone(s, { 90000, 100, 0 }, foundation, none);
	CHECK(hit.has_value() && hit->custom);
	CHECK(BlockedMessage(s, *hit, false) == "Nope 1");
	CHECK(hit->source == "custom location");

	// Messages.
	hit = zones.FindBlockingZone(s, { 0, 3000, 0 }, foundation, foundation_item);
	CHECK(BlockedMessage(s, *hit, false) == "You can not build here you are too close to a tribute terminal.");
	CHECK(BlockedMessage(s, *hit, true)
		== "You can not build here you are too close to a tribute terminal. [TributeTerminal_C, 10/30 foundations]");

	ZoneRegistry notes;
	notes.Add(ZoneType::ExplorerNotes, { 0, 0, 0 }, "RuneStone_C");
	hit = notes.FindBlockingZone(s, { 0, 0, 100 }, foundation, foundation_item);
	CHECK(hit.has_value());
	CHECK(BlockedMessage(s, *hit, false) == "You can not build here you are too close to a rune.");

	// Help: inside, nearest (to the edge), none.
	hit = zones.FindNearestZone(s, { 50000, 600, 0 });
	CHECK(hit.has_value());
	CHECK(HelpMessage(s, hit).find("inside a protected area around a tribute terminal: 2 of 30") != std::string::npos);
	hit = zones.FindNearestZone(s, { 50000 + 9000 + 3000, 0, 0 });
	CHECK(HelpMessage(s, hit).find("10 foundations away") != std::string::npos);
	ZoneRegistry empty;
	Settings no_custom = s;
	no_custom.custom_locations.clear();
	CHECK(HelpMessage(no_custom, empty.FindNearestZone(no_custom, { 0, 0, 0 })) == s.chat.help_none);
	// Custom locations exist even with nothing scanned.
	CHECK(empty.FindNearestZone(s, { 0, 0, 0 }).has_value());

	// A reload that changes a range takes effect without a new scan.
	Settings wider = s;
	wider.Type(ZoneType::SpawnPoints).range_foundations = 100;
	wider.Type(ZoneType::TributeTerminals).enabled = false;
	hit = zones.FindBlockingZone(wider, { 0, 20000, 0 }, foundation, foundation_item);
	CHECK(hit.has_value() && hit->type == ZoneType::SpawnPoints);

	zones.Clear();
	CHECK(zones.Total() == 0);
	CHECK(zones.Add(ZoneType::SpawnPoints, { 0, 0, 0 }, "PlayerStart"));
}

static void TestStride()
{
	// Fake actors and a fake array of { actor*, float weight, padding } = 16-byte elements.
	int actors[4] = {};
	const auto known = [&](const void* pointer)
	{
		for (const int& actor : actors)
			if (pointer == &actor)
				return true;
		return false;
	};

	struct Entry16 { void* actor; float weight; int pad; };
	Entry16 entries16[4] = { { &actors[0], 1.0f, 0 }, { &actors[1], 0.5f, 0 }, { &actors[2], 2.0f, 0 }, { &actors[3], 1.0f, 0 } };

	auto reader = [](const void* base, size_t size)
	{
		return [base, size](size_t offset, const void*& out)
		{
			if (offset + sizeof(void*) > size)
				return false;
			std::memcpy(&out, static_cast<const unsigned char*>(base) + offset, sizeof(out));
			return true;
		};
	};

	CHECK(DetectLeadingPointerStride(4, reader(entries16, sizeof(entries16)), known) == 16);
	CHECK(DetectLeadingPointerStride(1, reader(entries16, sizeof(entries16)), known) == 0); // cannot tell
	CHECK(DetectLeadingPointerStride(0, reader(entries16, sizeof(entries16)), known) == 0);

	// Zero weights make a too-small size look valid; the size that finds more actors still wins.
	Entry16 zero_weights[4] = { { &actors[0], 0.0f, 0 }, { &actors[1], 0.0f, 0 }, { &actors[2], 0.0f, 0 }, { &actors[3], 0.0f, 0 } };
	CHECK(DetectLeadingPointerStride(4, reader(zero_weights, sizeof(zero_weights)), known) == 16);

	// An empty entry is tolerated.
	Entry16 with_null[3] = { { &actors[0], 1.0f, 0 }, { nullptr, 1.0f, 0 }, { &actors[2], 1.0f, 0 } };
	CHECK(DetectLeadingPointerStride(3, reader(with_null, sizeof(with_null)), known) == 16);

	// 40-byte elements.
	struct Entry40 { void* actor; double a, b, c, d; };
	Entry40 entries40[3] = { { &actors[0], 1, 2, 3, 4 }, { &actors[1], 5, 6, 7, 8 }, { &actors[2], 9, 1, 2, 3 } };
	CHECK(DetectLeadingPointerStride(3, reader(entries40, sizeof(entries40)), known) == 40);

	// Elements that do not start with an actor pointer: no size fits, and reading never goes past
	// what the reader allows.
	struct NotPointer { int index; int serial; double x; };
	NotPointer soft[3] = { { 1, 2, 0 }, { 3, 4, 0 }, { 5, 6, 0 } };
	CHECK(DetectLeadingPointerStride(3, reader(soft, sizeof(soft)), known) == 0);
}

// Checks the config.json that ships with the plugin (or whichever file is passed in).
static void TestConfigFile(const std::string& path, bool shipped_expectations)
{
	std::ifstream file(path);
	if (!file.is_open())
	{
		++g_failures;
		std::cerr << "FAILED: cannot open " << path << "\n";
		return;
	}

	Settings settings;
	try
	{
		settings = ParseSettings(nlohmann::json::parse(file, nullptr, true, true));
	}
	catch (const std::exception& error)
	{
		++g_failures;
		std::cerr << "FAILED: " << path << ": " << error.what() << "\n";
		return;
	}

	std::cout << path << ": " << settings.custom_locations.size() << " custom location(s)\n";
	for (size_t i = 0; i < kZoneTypeCount; ++i)
	{
		const auto& ts = settings.types[i];
		std::cout << "  " << ZoneTypeKey(ZoneTypeAt(i)) << ": " << (ts.enabled ? "on" : "off") << ", "
			<< ts.range_foundations << " foundations, " << ts.actor_classes.size() << " actor class(es), "
			<< ts.whitelist.size() << " whitelisted\n";
	}
	for (const auto& warning : settings.warnings)
		std::cout << "  warning: " << warning << "\n";

	if (!shipped_expectations)
		return;

	CHECK(settings.warnings.empty());
	CHECK(settings.enabled);
	CHECK(!settings.allow_admins);
	CHECK(settings.chat.sender == "SERVER");

	// The ranges of the config this plugin replaced.
	const double ranges[kZoneTypeCount] = { 5, 75, 20, 20, 30, 5, 20, 5 };
	for (size_t i = 0; i < kZoneTypeCount; ++i)
	{
		CHECK(settings.types[i].enabled);
		CHECK(settings.types[i].range_foundations == ranges[i]);
		CHECK(settings.types[i].whitelist.size() == 2);
		CHECK(settings.types[i].message == DefaultMessage(ZoneTypeAt(i)));
		CHECK(settings.types[i].actor_classes.size() == DefaultActorClasses(ZoneTypeAt(i)).size());
	}

	CHECK(settings.explorer_note_name == "explorer note");
	CHECK(settings.custom_locations.size() == 1);
	CHECK(!settings.custom_locations[0].enabled); // shipped example is off

	ZoneRegistry zones;
	const ClassInfo foundation = Class({ "StoneFloor_C", "PrimalStructure" });
	CHECK(!zones.FindBlockingZone(settings, { 12222, 122222, 122222 }, foundation, ClassInfo()).has_value());
}

int main(int argc, char** argv)
{
	TestStrings();
	TestPatterns();
	TestParsing();
	TestClassification();
	TestZones();
	TestStride();

	if (argc > 1)
		TestConfigFile(argv[1], argc > 2 && std::string(argv[2]) == "--shipped");

	std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
