// Unit tests for the peer protocol v2 building blocks. No Bluetooth needed;
// run in CI after the build. Vectors cross-checked against Android's
// PeerIdentityStore.headsetIdFromAddress and membership digest in Python.

#include "PeerDialPolicy.hpp"
#include "ProximityRules.hpp"
#include "TriggerApps.hpp"
#include "v2/PeerV2Arbitration.hpp"
#include "v2/PeerV2Frame.hpp"
#include "v2/PeerV2Identity.hpp"
#include "v2/PeerV2Json.hpp"
#include "v2/PeerV2Message.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace Propods::v2;

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond) do { if (cond) ++g_passed; else { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool throwsCode(const std::function<void()>& f, const std::string& code) {
    try { f(); } catch (const std::invalid_argument& e) { return code.empty() || code == e.what(); }
    return false;
}

static const char* kA = "0bff4075-2728-4f39-8cfb-2087d1cf6fd9";
static const char* kB = "11111111-2222-4333-8444-555555555555";
static const char* kDigestAB = "d03a3d3b1962670917a4787d53254c5d470769c0d8270b982052a2eba1a5eec3";

static void identityTests() {
    CHECK(headsetIdFromAddress(0x20521D8933C9ull) == "83201ba7-5401-8cd4-86b2-ea1172e1bdd1");
    CHECK(formatAddress(0x98597A1041E9ull) == "98:59:7A:10:41:E9");
    CHECK(membershipDigest({kA, kB}) == kDigestAB);
    const std::string u = randomUuid();
    CHECK(isCanonicalUuid(u) && u[14] == '4');
    CHECK(!isCanonicalUuid("0BFF4075-2728-4F39-8CFB-2087D1CF6FD9"));
}

static void jsonTests() {
    CHECK(throwsCode([] { parseJson(R"({"a":1,"a":2})"); }, "duplicate_json_key"));
    CHECK(throwsCode([] { parseJson(R"({"a":1.5})"); }, "integer_required"));
    CHECK(throwsCode([] { parseJson(R"({"a":1} x)"); }, "trailing_json"));
    CHECK(throwsCode([] { parseJson("{\"a\":\"\x01\"}"); }, "invalid_control_character"));
    CHECK(throwsCode([] { parseJson("{\"a\":\"\xC3\"}"); }, "invalid_utf8"));
    const Json v = parseJson(R"( {"n":-12,"s":"xé\n","b":true,"z":null,"l":[1,"2"],"o":{}} )");
    const JsonObject& o = *v.object();
    CHECK(*field(o, "n")->integer() == -12);
    CHECK(*field(o, "s")->string() == "x\xC3\xA9\n");
    CHECK(writeJson(parseJson(R"({"k":["a",1,false,null]})")) == R"({"k":["a",1,false,null]})");
}

static void frameTests() {
    const auto bytes = encodeFrame(Frame{FrameType::Ping, R"({"nonce":"x"})"});
    CHECK(bytes.size() == 10 + 13);
    CHECK(bytes[0] == 'E' && bytes[3] == '2' && bytes[4] == 2 && bytes[5] == 3 && bytes[9] == 13);
    // Two frames split at awkward points arrive intact.
    std::vector<std::uint8_t> two = bytes;
    two.insert(two.end(), bytes.begin(), bytes.end());
    FrameDecoder d;
    std::size_t got = 0;
    for (std::size_t i = 0; i < two.size(); i += 7) {
        const std::size_t n = std::min<std::size_t>(7, two.size() - i);
        got += d.feed(std::span<const std::uint8_t>(two.data() + i, n)).size();
    }
    CHECK(got == 2 && d.buffered() == 0);
    FrameDecoder bad;
    const std::uint8_t junk[10] = {'X', 'P', 'H', '2', 2, 1, 0, 0, 0, 0};
    CHECK(throwsCode([&] { bad.feed(junk); }, "invalid_magic"));
    const std::uint8_t big[10] = {'E', 'P', 'H', '2', 2, 1, 0, 0, 0x10, 0x01};
    FrameDecoder bad2;
    CHECK(throwsCode([&] { bad2.feed(big); }, "oversized_payload"));
}

static void messageTests() {
    // HELLO exactly as Android encodes it.
    const std::string hello = std::string(R"({"installationId":")") + kA + R"(","sessionId":")" + kB +
        R"(","capabilities":["acquire-v1","snapshot"],"initiatorId":")" + kA + R"(","connectionId":")" + kB + R"("})";
    CHECK(decode(Frame{FrameType::Hello, hello}).fields.size() == 5);
    CHECK(throwsCode([&] { decode(Frame{FrameType::Hello, R"({"installationId":"x"})"}); }, "invalid_fields"));

    const JsonObject snap{{"sessionId", kB}, {"revision", std::int64_t{3}}, {"headsetId", kA},
        {"ownership", "released"}, {"audioUse", "idle"}, {"lastOwnerId", nullptr},
        {"lastOwnerRevision", nullptr}, {"acquisition", nullptr}};
    const Frame sf = encode(FrameType::Snapshot, snap);
    CHECK(decode(sf).fields.size() == 8);
    JsonObject badSnap = snap;
    badSnap[3].second = Json("mine");
    CHECK(throwsCode([&] { encode(FrameType::Snapshot, badSnap); }, "invalid_ownership"));

    const JsonObject request{{"requestId", kB}, {"headsetId", kA}, {"requesterId", kA},
        {"requesterSessionId", kB}, {"membership", JsonArray{Json(kA), Json(kB)}},
        {"membershipDigest", kDigestAB},
        {"snapshotRevisions", JsonObject{{kA, std::int64_t{1}}, {kB, std::int64_t{2}}}}};
    CHECK(decode(encode(FrameType::AcquireRequest, request)).type == FrameType::AcquireRequest);
    JsonObject wrongDigest = request;
    wrongDigest[5].second = Json(std::string(64, 'a'));
    CHECK(throwsCode([&] { encode(FrameType::AcquireRequest, wrongDigest); }, "membership_digest_mismatch"));
    JsonObject unsorted = request;
    unsorted[4].second = Json(JsonArray{Json(kB), Json(kA)});
    CHECK(throwsCode([&] { encode(FrameType::AcquireRequest, unsorted); }, "invalid_membership"));

    // Capabilities: any well-formed name is accepted (unknown ones are ignored).
    const auto helloWith = [&](const std::string& caps) {
        return Frame{FrameType::Hello, std::string(R"({"installationId":")") + kA + R"(","sessionId":")" + kB +
            R"(","capabilities":)" + caps + R"(,"initiatorId":")" + kA + R"(","connectionId":")" + kB + R"("})"};
    };
    CHECK(decode(helloWith(R"(["acquire-v1","acquire-v2","snapshot"])")).fields.size() == 5);
    CHECK(decode(helloWith(R"(["acquire-v1","future-x","snapshot"])")).fields.size() == 5);
    CHECK(decode(helloWith("[]")).fields.size() == 5);
    CHECK(throwsCode([&] { decode(helloWith(R"(["Bad Name"])")); }, "invalid_capabilities"));
    CHECK(throwsCode([&] { decode(helloWith(R"(["-x"])")); }, "invalid_capabilities"));
    CHECK(throwsCode([&] { decode(helloWith(R"(["snapshot","acquire-v1"])")); }, "invalid_capabilities"));
    CHECK(throwsCode([&] { decode(helloWith(R"(["snapshot","snapshot"])")); }, "invalid_capabilities"));
    CHECK(throwsCode([&] { decode(helloWith("[1]")); }, "invalid_capabilities"));

    // ACQUIRE_REQUEST: optional intent, nothing else extra.
    for (const char* intent : {"call", "manual", "media", "passive"}) {
        JsonObject withIntent = request;
        withIntent.push_back({"intent", intent});
        CHECK(decode(encode(FrameType::AcquireRequest, withIntent)).type == FrameType::AcquireRequest);
    }
    JsonObject badIntent = request;
    badIntent.push_back({"intent", "urgent"});
    CHECK(throwsCode([&] { encode(FrameType::AcquireRequest, badIntent); }, "invalid_intent"));
    JsonObject extraField = request;
    extraField.push_back({"bogus", "x"});
    CHECK(throwsCode([&] { encode(FrameType::AcquireRequest, extraField); }, "invalid_fields"));

    // ACQUIRE_DENY reasons, including the forward-compatible ones.
    for (const char* reason : {"HELD", "CALL_ACTIVE", "POLICY"}) {
        const JsonObject deny{{"requestId", kB}, {"headsetId", kA}, {"requesterId", kA},
            {"requesterSessionId", kB}, {"membership", JsonArray{Json(kA), Json(kB)}},
            {"membershipDigest", kDigestAB}, {"grantorId", kB}, {"grantorSessionId", kA}, {"reason", reason}};
        CHECK(decode(encode(FrameType::AcquireDeny, deny)).type == FrameType::AcquireDeny);
    }
    const JsonObject denyBad{{"requestId", kB}, {"headsetId", kA}, {"requesterId", kA},
        {"requesterSessionId", kB}, {"membership", JsonArray{Json(kA), Json(kB)}},
        {"membershipDigest", kDigestAB}, {"grantorId", kB}, {"grantorSessionId", kA}, {"reason", "NOPE"}};
    CHECK(throwsCode([&] { encode(FrameType::AcquireDeny, denyBad); }, "invalid_deny_reason"));

    const JsonObject relay{{"subtype", "battery"}, {"data", "AAE="}};
    CHECK(decode(encode(FrameType::RelayData, relay)).fields.size() == 2);
    JsonObject relayBad = relay;
    relayBad[1].second = Json("AAF=");  // non-canonical padding bits
    CHECK(throwsCode([&] { encode(FrameType::RelayData, relayBad); }, "invalid_relay_data"));
}

static void arbitrationTests() {
    using namespace std::chrono;
    // Smallest (initiatorId, connectionId) wins; both ends compute the same.
    CHECK(preferredConnection({}) == -1);
    CHECK(preferredConnection({{kB, "c1"}, {kA, "c9"}}) == 1);
    CHECK(preferredConnection({{kA, "c2"}, {kA, "c1"}}) == 1);
    CHECK(preferredConnection({{kA, "c1"}}) == 0);
    // First dial: smaller installationId at once, the other after 3 s, unknown peer at once.
    CHECK(firstDialDelay(kA, kB) == milliseconds{0});
    CHECK(firstDialDelay(kB, kA) == milliseconds{3000});
    CHECK(firstDialDelay(kB, "") == milliseconds{0});
    CHECK(firstDialDelay("", kA) == milliseconds{0});
    CHECK(firstDialDelay(kA, kA) == milliseconds{0});
    // Duplicate legacy links.
    CHECK(chooseLegacyLink(kA, kB, true) == LegacyLinkChoice::KeepExisting);   // ours, we are smaller
    CHECK(chooseLegacyLink(kA, kB, false) == LegacyLinkChoice::UseNew);        // theirs was first, but ours is wanted
    CHECK(chooseLegacyLink(kB, kA, true) == LegacyLinkChoice::UseNew);         // peer is smaller: use its link
    CHECK(chooseLegacyLink(kB, kA, false) == LegacyLinkChoice::KeepExisting);
    CHECK(chooseLegacyLink(kA, "", true) == LegacyLinkChoice::KeepExisting);   // unknown id: first up wins
    CHECK(chooseLegacyLink(kA, "", false) == LegacyLinkChoice::KeepExisting);
}

static void dialPolicyTests() {
    using namespace std::chrono;
    using namespace Propods;
    // Android PeerRetryPolicy: 2, 4, 8, then 15 s, repeating; jitter 0.8..1.2.
    CHECK(peerRetryDelay(0, 1.0) == milliseconds{2000});
    CHECK(peerRetryDelay(1, 1.0) == milliseconds{4000});
    CHECK(peerRetryDelay(2, 1.0) == milliseconds{8000});
    CHECK(peerRetryDelay(3, 1.0) == milliseconds{15000});
    CHECK(peerRetryDelay(4, 1.0) == milliseconds{15000});
    CHECK(peerRetryDelay(99, 1.0) == milliseconds{15000});
    CHECK(peerRetryDelay(-1, 1.0) == milliseconds{2000});
    CHECK(peerRetryDelay(0, 0.8) == milliseconds{1600});
    CHECK(peerRetryDelay(0, 1.2) == milliseconds{2400});
    CHECK(peerRetryDelay(3, 0.8) == milliseconds{12000});
    CHECK(peerRetryDelay(3, 1.2) == milliseconds{18000});
    CHECK(peerRetryDelay(0, 5.0) == milliseconds{2400});   // clamped
    CHECK(peerRetryDelay(0, 0.1) == milliseconds{1600});
    CHECK(peerRetryDelay(0, 1.0, true) == milliseconds{15000});  // audio floor
    CHECK(peerRetryDelay(3, 1.0, true) == milliseconds{15000});
    for (int i = 0; i < 1000; ++i) {
        const double j = peerRetryJitter();
        CHECK(j >= 0.8 && j <= 1.2);
    }
    CHECK(kPeerConnectTimeout == milliseconds{8000});
    // No EveryPods service on the peer: 15 s, 30 s, 1 min, 2 min, then 5 min.
    CHECK(peerServiceMissingDelay(1, 1.0) == milliseconds{15000});
    CHECK(peerServiceMissingDelay(2, 1.0) == milliseconds{30000});
    CHECK(peerServiceMissingDelay(3, 1.0) == milliseconds{60000});
    CHECK(peerServiceMissingDelay(4, 1.0) == milliseconds{120000});
    CHECK(peerServiceMissingDelay(5, 1.0) == milliseconds{300000});
    CHECK(peerServiceMissingDelay(600, 1.0) == milliseconds{300000});
    CHECK(peerServiceMissingDelay(0, 1.0) == milliseconds{15000});
    CHECK(peerServiceMissingDelay(5, 0.8) == milliseconds{240000});
    CHECK(peerServiceMissingDelay(5, 9.0) == milliseconds{360000});  // clamped
    CHECK(std::string(PeerServiceMissing("x").what()) == "x");
    CHECK(peerLinkTooShort(milliseconds{500}));     // accepted, then closed at once
    CHECK(peerLinkTooShort(milliseconds{9999}));
    CHECK(!peerLinkTooShort(milliseconds{10000}));
    CHECK(!peerLinkTooShort(milliseconds{600000}));
}

// Beacon bytes as captured on 2026-11-23 (after the company id).
static void proximityTests() {
    using namespace Propods;
    bool inCase = true;
    const std::uint8_t inEars[] = {0x07, 0x19, 0x01, 0x27, 0x20, 0x0b, 0x99, 0x8f, 0x11, 0x00};
    CHECK(parseProximityBeacon(inEars, sizeof inEars, inCase) && !inCase);

    std::uint8_t stowed[] = {0x07, 0x19, 0x01, 0x27, 0x20, 0x51, 0x99, 0x8f, 0x08, 0x00};
    inCase = false;
    CHECK(parseProximityBeacon(stowed, sizeof stowed, inCase) && inCase);
    stowed[8] = 0x00;  // lid open
    CHECK(parseProximityBeacon(stowed, sizeof stowed, inCase) && !inCase);

    // Worn = a pod in an ear (gates the playback hold).
    bool worn = false;
    CHECK(parseProximityBeacon(inEars, sizeof inEars, inCase, worn) && worn);
    CHECK(parseProximityBeacon(stowed, sizeof stowed, inCase, worn) && !worn);

    // Shorter type-0x07 message from a nearby Apple device: byte 8 is 0xff.
    const std::uint8_t shortMsg[] = {0x07, 0x11, 0x06, 0x29, 0x20, 0x0a, 0x51, 0xff, 0xff, 0x5a};
    CHECK(!parseProximityBeacon(shortMsg, sizeof shortMsg, inCase));
    const std::uint8_t other[] = {0x10, 0x05, 0x01};
    CHECK(!parseProximityBeacon(other, sizeof other, inCase));
    CHECK(!parseProximityBeacon(inEars, 8, inCase));  // truncated

    // Lid bit: byte 8 bit 3 set = closed.
    bool worn2 = false, lidOpen = true;
    stowed[8] = 0x08;
    CHECK(parseProximityBeacon(stowed, sizeof stowed, inCase, worn2, lidOpen) && !lidOpen);
    stowed[8] = 0x00;
    CHECK(parseProximityBeacon(stowed, sizeof stowed, inCase, worn2, lidOpen) && lidOpen);

    // A lid opening needs evidence the lid was shut first. Scan started at 1000.
    CHECK(!lidOpeningStarts(1600, 1000, 0, 0));          // open right after the scan starts (lid left open)
    CHECK(lidOpeningStarts(1600, 1000, 0, 1200));        // closed beacon seen first
    CHECK(lidOpeningStarts(12000, 1000, 0, 0));          // 11 s of silence (shut case), then open
    CHECK(!lidOpeningStarts(13000, 1000, 12000, 0));     // same opening, 1 s later
    CHECK(lidOpeningStarts(16000, 1000, 13000, 15000));  // closed in between
    CHECK(!lidOpeningStarts(70000, 1000, 16000, 15000)); // still open, 54 s later
    CHECK(lidOpeningStarts(77000, 1000, 16000, 15000));  // a minute without lid-open beacons

    // Availability: in case never expires; a recent out-of-case beacon wins.
    constexpr std::int64_t t = 1'000'000;
    CHECK(airpodsAvailableFor(t, 0, 0));                                  // nothing seen
    CHECK(!airpodsAvailableFor(t, t - 1000, 0));                          // in case
    CHECK(!airpodsAvailableFor(t + 600'000, t - 1000, 0));                // still in case 10 min later
    CHECK(!airpodsAvailableFor(t, t - 5000, t - 25'000));                 // in case now, last out beacon 25 s ago
    CHECK(airpodsAvailableFor(t, t - 1000, t - 10'000));                  // other AirPods in case, ours out 10 s ago
    CHECK(airpodsAvailableFor(t, t - 30'000, t - 1000));                  // latest says out
    CHECK(!airpodsAvailableFor(t, t - 1000, t - kOutOfCaseWindowMs));     // out beacon just expired
}

static void triggerAppTests() {
    using Propods::TriggerApp;
    using Propods::TriggerAppList;
    using Propods::TriggerEvent;
    using Propods::TriggerRule;
    using Propods::normalizeExeName;
    using Propods::triggerFor;
    using Propods::isCallMic;
    CHECK(normalizeExeName(L"Dots.EXE") == L"dots.exe");
    CHECK(normalizeExeName(L"  dots ") == L"dots.exe");
    CHECK(normalizeExeName(L"C:\\Program Files\\OpenAI\\Dots.exe") == L"dots.exe");
    CHECK(normalizeExeName(L"dots.exe\r") == L"dots.exe");  // CRLF config line
    CHECK(normalizeExeName(L"").empty());
    CHECK(normalizeExeName(L".exe").empty());
    CHECK(normalizeExeName(L"bad|name").empty());

    TriggerAppList list;
    // Built-in catalog, pre-ticked.
    const auto teams = list.resolve(L"MS-Teams.exe");
    CHECK(teams.builtIn && !teams.changed && teams.takeOnMic && !teams.takeOnPlay && teams.keepOnMic);
    CHECK(triggerFor(teams, true) == TriggerEvent::Call);
    CHECK(triggerFor(teams, false) == TriggerEvent::None);  // a chat ping doesn't take them
    CHECK(isCallMic(teams, true) && !isCallMic(teams, false));
    const auto zoom = list.resolve(L"zoom.exe");
    CHECK(!zoom.takeOnMic && !zoom.takeOnPlay && zoom.keepOnMic);
    const auto chrome = list.resolve(L"chrome.exe");
    CHECK(chrome.builtIn && !chrome.takeOnMic && chrome.takeOnPlay && chrome.keepOnMic);
    CHECK(triggerFor(chrome, false) == TriggerEvent::None);  // its plays come from the media controls
    const auto vlc = list.resolve(L"vlc.exe");
    CHECK(triggerFor(vlc, false) == TriggerEvent::Media);    // outside the media controls
    CHECK(list.listed().size() == Propods::builtInTriggerApps().size());

    // Any other app: take on play (rule 4), not saved.
    const auto other = list.resolve(L"Dots.exe");
    CHECK(other.exe == L"dots.exe" && !other.builtIn && !other.saved);
    CHECK(other.takeOnPlay && !other.takeOnMic && !other.keepOnMic);
    CHECK(triggerFor(other, true) == TriggerEvent::None && triggerFor(other, false) == TriggerEvent::None);

    // Settings are saved from what is in force; nothing ticked is kept.
    CHECK(list.setRule(L"dots", TriggerRule::KeepOnMic, true));
    auto dots = list.resolve(L"dots.exe");
    CHECK(dots.saved && dots.takeOnPlay && dots.keepOnMic && !dots.takeOnMic);
    CHECK(!list.setRule(L"dots", TriggerRule::KeepOnMic, true));  // no change
    CHECK(list.setRule(L"dots", TriggerRule::KeepOnMic, false));
    CHECK(list.setRule(L"dots", TriggerRule::TakeOnPlay, false));
    dots = list.resolve(L"dots.exe");
    CHECK(dots.saved && !dots.takeOnMic && !dots.takeOnPlay && !dots.keepOnMic);  // never
    CHECK(list.userApps().size() == 1);

    // Built-ins can be changed, show as changed, and reset.
    CHECK(!list.resetToBuiltIn(L"zoom.exe"));                 // nothing to reset
    CHECK(list.setRule(L"zoom", TriggerRule::TakeOnMic, true));
    const auto zoomChanged = list.resolve(L"zoom.exe");
    CHECK(zoomChanged.builtIn && zoomChanged.changed && zoomChanged.takeOnMic && zoomChanged.keepOnMic);
    CHECK(list.resetToBuiltIn(L"zoom.exe"));
    CHECK(!list.resolve(L"zoom.exe").changed && !list.resolve(L"zoom.exe").takeOnMic);
    CHECK(!list.resetToBuiltIn(L"dots.exe"));                 // not a built-in

    // Rule 3: first seen in a call -> all three, saved; never over a saved or built-in app.
    CHECK(list.adoptCallApp(L"chatgpt.exe"));
    const auto gpt = list.resolve(L"chatgpt.exe");
    CHECK(gpt.saved && gpt.takeOnMic && gpt.takeOnPlay && gpt.keepOnMic);
    CHECK(!list.adoptCallApp(L"chatgpt.exe"));
    CHECK(!list.adoptCallApp(L"dots.exe"));
    CHECK(!list.adoptCallApp(L"zoom.exe"));

    // Media-controls starts: the matched app's take-on-play decides.
    CHECK(list.takesOnMediaStart(L"Chrome"));
    CHECK(!list.takesOnMediaStart(L"MSTeams_8wekyb3d8bbwe!MSTeams"));  // Teams: play unticked
    CHECK(!list.takesOnMediaStart(L"Dots.exe"));                       // saved with nothing ticked
    CHECK(list.takesOnMediaStart(L"SomethingElse.exe"));               // unknown: rule 4
    CHECK(list.takesOnMediaStart(L""));
    CHECK(list.setRule(L"chrome", TriggerRule::TakeOnPlay, false));
    CHECK(!list.takesOnMediaStart(L"Chrome"));
    CHECK(Propods::gsmtcAppMatchesExe(L"308046B0AF4A39CB", L"firefox.exe"));
    CHECK(!Propods::gsmtcAppMatchesExe(L"MSEdge", L"chrome.exe"));

    // Config: lines merged per app, nothing-ticked entries kept, built-ins can be saved.
    TriggerApp mic{L"Dots"};      mic.takeOnMic = true;
    TriggerApp keep{L"dots.exe"}; keep.keepOnMic = true;
    TriggerApp none{L"idle.exe"};
    TriggerApp vlcOff{L"vlc.exe"};
    TriggerApp empty{L""};        empty.keepOnMic = true;
    list.setUserApps({mic, keep, none, vlcOff, empty});
    CHECK(list.userApps().size() == 3);
    CHECK(list.resolve(L"dots.exe").takeOnMic && list.resolve(L"dots.exe").keepOnMic && !list.resolve(L"dots.exe").takeOnPlay);
    CHECK(list.resolve(L"idle.exe").saved && !list.resolve(L"idle.exe").takeOnPlay);
    CHECK(list.resolve(L"vlc.exe").changed && triggerFor(list.resolve(L"vlc.exe"), false) == TriggerEvent::None);
    CHECK(list.listed().size() == Propods::builtInTriggerApps().size() + 2);  // + dots, idle
}

int main() {
    identityTests();
    jsonTests();
    frameTests();
    messageTests();
    arbitrationTests();
    dialPolicyTests();
    proximityTests();
    triggerAppTests();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
