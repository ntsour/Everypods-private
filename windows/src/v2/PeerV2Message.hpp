#pragma once

// Strict per-type validation of peer protocol v2 payloads, matching Android's
// PeerV2Message.validate exactly: exact field sets, canonical UUIDs,
// non-negative integers, sorted unique lists and a matching membership
// digest. decode() and encode() throw std::invalid_argument on violation.

#include "PeerV2Frame.hpp"
#include "PeerV2Json.hpp"

namespace Propods::v2 {

struct Message {
    FrameType type;
    JsonObject fields;
};

Message decode(const Frame& frame);
Frame encode(FrameType type, const JsonObject& fields);

// Convenience accessors for validated messages (throw if absent/wrong type).
const std::string& str(const JsonObject& o, std::string_view key);
std::int64_t integer(const JsonObject& o, std::string_view key);
const JsonObject& obj(const JsonObject& o, std::string_view key);
std::vector<std::string> strings(const JsonObject& o, std::string_view key);

}  // namespace Propods::v2
