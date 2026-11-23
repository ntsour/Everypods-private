#include "PeerV2Message.hpp"

#include "PeerV2Identity.hpp"

#include <algorithm>
#include <set>

namespace Propods::v2 {

namespace {

[[noreturn]] void invalid(const std::string& code) { throw std::invalid_argument(code); }

using Keys = std::set<std::string>;

const Keys kSnapshotFields{"sessionId", "revision", "headsetId", "ownership", "audioUse",
                           "lastOwnerId", "lastOwnerRevision", "acquisition"};
const Keys kRequestFields{"requestId", "headsetId", "requesterId", "requesterSessionId",
                          "membership", "membershipDigest", "snapshotRevisions"};
const Keys kIntents{"call", "manual", "media", "passive"};
// Capability names are open-ended: unknown but well-formed names are ignored.
bool validCapabilityName(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (c == '-' && i > 0))) return false;
    }
    return true;
}
const Keys kOwnership{"unknown", "held", "released"};
const Keys kAudioUse{"unknown", "idle", "media", "call"};
const Keys kDenials{"HELD", "UNKNOWN", "STALE_SNAPSHOT", "MEMBERSHIP_MISMATCH", "BUSY", "UNSUPPORTED", "SESSION_CHANGED", "CALL_ACTIVE", "POLICY"};
const Keys kRelayTypes{"battery", "anc", "ear_state"};

Keys with(Keys base, std::initializer_list<const char*> add, std::initializer_list<const char*> remove = {}) {
    for (const char* r : remove) base.erase(r);
    for (const char* a : add) base.insert(a);
    return base;
}

void exact(const JsonObject& f, const Keys& keys) {
    Keys present;
    for (const auto& [k, v] : f) present.insert(k);
    if (present != keys) invalid("invalid_fields");
}

void uuid(const JsonObject& f, const char* key) {
    const Json* v = field(f, key);
    if (!v || !v->string() || !isCanonicalUuid(*v->string())) invalid(std::string("invalid_") + key);
}

void nullableUuid(const JsonObject& f, const char* key) {
    const Json* v = field(f, key);
    if (v && !v->isNull()) uuid(f, key);
}

void nonnegative(const JsonObject& f, const char* key) {
    const Json* v = field(f, key);
    if (!v || !v->integer() || *v->integer() < 0) invalid(std::string("invalid_") + key);
}

void inSet(const JsonObject& f, const char* key, const Keys& allowed, const char* code) {
    const Json* v = field(f, key);
    if (!v || !v->string() || !allowed.count(*v->string())) invalid(code);
}

void digest(const JsonObject& f) {
    const Json* v = field(f, "membershipDigest");
    if (!v || !v->string() || v->string()->size() != 64) invalid("invalid_membership_digest");
    for (const char c : *v->string())
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) invalid("invalid_membership_digest");
}

void acquireIdentity(const JsonObject& f, bool grantor) {
    for (const char* k : {"requestId", "headsetId", "requesterId", "requesterSessionId"}) uuid(f, k);
    digest(f);
    if (grantor) { uuid(f, "grantorId"); uuid(f, "grantorSessionId"); }
}

std::vector<std::string> membership(const JsonObject& f) {
    const Json* v = field(f, "membership");
    if (!v || !v->array()) invalid("invalid_membership");
    std::vector<std::string> ids;
    for (const auto& e : *v->array()) {
        if (!e.string() || !isCanonicalUuid(*e.string())) invalid("invalid_membership");
        ids.push_back(*e.string());
    }
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (ids.empty() || ids != sorted) invalid("invalid_membership");
    const auto has = [&](const char* key) {
        const Json* x = field(f, key);
        return x && x->string() && std::find(ids.begin(), ids.end(), *x->string()) != ids.end();
    };
    if (!has("requesterId")) invalid("invalid_membership");
    if (const Json* g = field(f, "grantorId"); g && !g->isNull() && !has("grantorId")) invalid("invalid_membership");
    if (membershipDigest(ids) != *field(f, "membershipDigest")->string()) invalid("membership_digest_mismatch");
    return ids;
}

void snapshot(const JsonObject& f) {
    exact(f, kSnapshotFields);
    uuid(f, "sessionId");
    nonnegative(f, "revision");
    nullableUuid(f, "headsetId");
    inSet(f, "ownership", kOwnership, "invalid_ownership");
    inSet(f, "audioUse", kAudioUse, "invalid_audio_use");
    nullableUuid(f, "lastOwnerId");
    const Json* owner = field(f, "lastOwnerId");
    const Json* ownerRev = field(f, "lastOwnerRevision");
    const bool bothNull = owner->isNull() && ownerRev->isNull();
    const bool bothSet = owner->string() && ownerRev->integer() && *ownerRev->integer() >= 0;
    if (!bothNull && !bothSet) invalid("invalid_last_owner");
    const Json* a = field(f, "acquisition");
    if (!a->isNull()) {
        if (!a->object()) invalid("invalid_acquisition");
        const JsonObject& acq = *a->object();
        exact(acq, {"requestId", "requesterId", "phase"});
        uuid(acq, "requestId");
        uuid(acq, "requesterId");
        inSet(acq, "phase", {"preparing", "in_progress"}, "invalid_acquisition_phase");
    }
}

bool canonicalBase64(const std::string& s, std::size_t maxBytes) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (s.size() % 4 != 0) return false;
    std::size_t pad = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '=') { if (i < s.size() - 2) return false; ++pad; continue; }
        if (pad || kAlphabet.find(s[i]) == std::string::npos) return false;
    }
    const std::size_t bytes = s.size() / 4 * 3 - pad;
    if (bytes > maxBytes) return false;
    // Canonical: unused low bits of the last data character must be zero.
    if (pad == 1 && (kAlphabet.find(s[s.size() - 2]) & 0x03) != 0) return false;
    if (pad == 2 && (kAlphabet.find(s[s.size() - 3]) & 0x0F) != 0) return false;
    return true;
}

void validate(FrameType type, const JsonObject& f) {
    switch (type) {
        case FrameType::Hello: {
            exact(f, {"installationId", "sessionId", "capabilities", "initiatorId", "connectionId"});
            for (const char* k : {"installationId", "sessionId", "initiatorId", "connectionId"}) uuid(f, k);
            const Json* caps = field(f, "capabilities");
            if (!caps->array()) invalid("invalid_capabilities");
            std::vector<std::string> list;
            for (const auto& c : *caps->array()) {
                if (!c.string() || !validCapabilityName(*c.string())) invalid("invalid_capabilities");
                list.push_back(*c.string());
            }
            auto sorted = list;
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            if (list != sorted) invalid("invalid_capabilities");
            break;
        }
        case FrameType::Snapshot: snapshot(f); break;
        case FrameType::Ping: exact(f, {"nonce"}); uuid(f, "nonce"); break;
        case FrameType::Pong: {
            exact(f, {"nonce", "snapshot"});
            uuid(f, "nonce");
            const Json* s = field(f, "snapshot");
            if (!s->object()) invalid("invalid_snapshot");
            snapshot(*s->object());
            break;
        }
        case FrameType::AcquireRequest: {
            // "intent" is optional (absent = legacy request); any other extra field is rejected.
            if (field(f, "intent")) {
                inSet(f, "intent", kIntents, "invalid_intent");
                exact(f, with(kRequestFields, {"intent"}));
            } else {
                exact(f, kRequestFields);
            }
            acquireIdentity(f, false);
            const auto members = membership(f);
            const Json* revs = field(f, "snapshotRevisions");
            if (!revs->object()) invalid("invalid_snapshot_revisions");
            Keys revKeys;
            for (const auto& [k, v] : *revs->object()) {
                if (!isCanonicalUuid(k) || !v.integer() || *v.integer() < 0) invalid("invalid_snapshot_revisions");
                revKeys.insert(k);
            }
            if (revKeys != Keys(members.begin(), members.end())) invalid("invalid_snapshot_revisions");
            break;
        }
        case FrameType::AcquireGrant:
            exact(f, with(kRequestFields, {"grantorId", "grantorSessionId", "grantId"}, {"snapshotRevisions"}));
            acquireIdentity(f, true); uuid(f, "grantId"); membership(f);
            break;
        case FrameType::AcquireDeny:
            exact(f, with(kRequestFields, {"grantorId", "grantorSessionId", "reason"}, {"snapshotRevisions"}));
            acquireIdentity(f, true); membership(f);
            inSet(f, "reason", kDenials, "invalid_deny_reason");
            break;
        case FrameType::AcquireCancel:
            exact(f, {"requestId", "requesterId", "requesterSessionId", "membershipDigest"});
            for (const char* k : {"requestId", "requesterId", "requesterSessionId"}) uuid(f, k);
            digest(f);
            break;
        case FrameType::RelayData: {
            exact(f, {"subtype", "data"});
            inSet(f, "subtype", kRelayTypes, "invalid_relay_subtype");
            const Json* d = field(f, "data");
            if (!d->string() || !canonicalBase64(*d->string(), 2048)) invalid("invalid_relay_data");
            break;
        }
        case FrameType::AcquireStart:
        case FrameType::AcquireStartAck:
            exact(f, with(kRequestFields, {"grantorId", "grantorSessionId", "grantId"}, {"snapshotRevisions"}));
            acquireIdentity(f, true); uuid(f, "grantId");
            break;
    }
}

}  // namespace

Message decode(const Frame& frame) {
    Json value = parseJson(frame.payload);
    const JsonObject* o = value.object();
    if (!o) invalid("payload_not_object");
    validate(frame.type, *o);
    return Message{frame.type, *o};
}

Frame encode(FrameType type, const JsonObject& fields) {
    validate(type, fields);
    std::string payload = writeJson(Json(fields));
    if (payload.size() > kMaxPayload) invalid("oversized_payload");
    return Frame{type, std::move(payload)};
}

const std::string& str(const JsonObject& o, std::string_view key) {
    const Json* v = field(o, key);
    if (!v || !v->string()) invalid("missing_" + std::string(key));
    return *v->string();
}

std::int64_t integer(const JsonObject& o, std::string_view key) {
    const Json* v = field(o, key);
    if (!v || !v->integer()) invalid("missing_" + std::string(key));
    return *v->integer();
}

const JsonObject& obj(const JsonObject& o, std::string_view key) {
    const Json* v = field(o, key);
    if (!v || !v->object()) invalid("missing_" + std::string(key));
    return *v->object();
}

std::vector<std::string> strings(const JsonObject& o, std::string_view key) {
    const Json* v = field(o, key);
    if (!v || !v->array()) invalid("missing_" + std::string(key));
    std::vector<std::string> out;
    for (const auto& e : *v->array()) {
        if (!e.string()) invalid("invalid_" + std::string(key));
        out.push_back(*e.string());
    }
    return out;
}

}  // namespace Propods::v2
