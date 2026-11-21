// Unit tests for the peer protocol v2 building blocks. No Bluetooth needed;
// run in CI after the build. Vectors cross-checked against Android's
// PeerIdentityStore.headsetIdFromAddress and membership digest in Python.

#include "PeerDialPolicy.hpp"
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
}

int main() {
    identityTests();
    jsonTests();
    frameTests();
    messageTests();
    arbitrationTests();
    dialPolicyTests();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
