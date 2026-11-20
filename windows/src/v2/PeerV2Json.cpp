#include "PeerV2Json.hpp"

#include <charconv>
#include <cstdio>

namespace Propods::v2 {

namespace {

[[noreturn]] void invalid(const char* code) { throw std::invalid_argument(code); }

bool validUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= s.size()) return false;  // continuation bytes i+1..i+n must exist
        for (size_t k = 1; k <= n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : m_s(s) {}

    Json parse() {
        Json v = value(0);
        ws();
        if (m_i != m_s.size()) invalid("trailing_json");
        return v;
    }

private:
    static constexpr int kMaxDepth = 32;

    Json value(int depth) {
        if (depth > kMaxDepth) invalid("json_too_deep");
        ws();
        if (m_i >= m_s.size()) invalid("incomplete_json");
        switch (m_s[m_i]) {
            case '{': return object(depth + 1);
            case '[': return array(depth + 1);
            case '"': return Json(string());
            case 't': literal("true"); return Json(true);
            case 'f': literal("false"); return Json(false);
            case 'n': literal("null"); return Json(nullptr);
            default:
                if (m_s[m_i] == '-' || (m_s[m_i] >= '0' && m_s[m_i] <= '9')) return Json(number());
                invalid("invalid_json");
        }
    }

    Json object(int depth) {
        ++m_i;
        ws();
        JsonObject out;
        if (take('}')) return Json(std::move(out));
        while (true) {
            ws();
            if (m_i >= m_s.size() || m_s[m_i] != '"') invalid("invalid_json_key");
            std::string key = string();
            for (const auto& [k, v] : out)
                if (k == key) invalid("duplicate_json_key");
            ws();
            if (!take(':')) invalid("invalid_json");
            Json v = value(depth);
            out.emplace_back(std::move(key), std::move(v));
            ws();
            if (take('}')) return Json(std::move(out));
            if (!take(',')) invalid("invalid_json");
        }
    }

    Json array(int depth) {
        ++m_i;
        ws();
        JsonArray out;
        if (take(']')) return Json(std::move(out));
        while (true) {
            out.push_back(value(depth));
            ws();
            if (take(']')) return Json(std::move(out));
            if (!take(',')) invalid("invalid_json");
        }
    }

    std::uint32_t hex4() {
        if (m_i + 4 > m_s.size()) invalid("invalid_escape");
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_s[m_i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else invalid("invalid_escape");
        }
        return v;
    }

    std::string string() {
        if (!take('"')) invalid("invalid_json_string");
        std::string out;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i++];
            if (c == '"') return out;
            if (c == '\\') {
                if (m_i >= m_s.size()) invalid("invalid_escape");
                const char e = m_s[m_i++];
                switch (e) {
                    case '"': case '\\': case '/': out += e; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        std::uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && m_i + 6 <= m_s.size() &&
                            m_s[m_i] == '\\' && m_s[m_i + 1] == 'u') {
                            m_i += 2;
                            const std::uint32_t low = hex4();
                            if (low < 0xDC00 || low > 0xDFFF) invalid("invalid_escape");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                            invalid("invalid_escape");
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: invalid("invalid_escape");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                invalid("invalid_control_character");
            } else {
                out += c;
            }
        }
        invalid("unterminated_string");
    }

    std::int64_t number() {
        const size_t start = m_i;
        if (m_s[m_i] == '-') ++m_i;
        if (m_i >= m_s.size()) invalid("invalid_number");
        if (m_s[m_i] == '0') {
            ++m_i;
        } else {
            if (m_s[m_i] < '1' || m_s[m_i] > '9') invalid("invalid_number");
            while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9') ++m_i;
        }
        if (m_i < m_s.size() && (m_s[m_i] == '.' || m_s[m_i] == 'e' || m_s[m_i] == 'E' || m_s[m_i] == '+'))
            invalid("integer_required");
        std::int64_t v = 0;
        const auto r = std::from_chars(m_s.data() + start, m_s.data() + m_i, v);
        if (r.ec != std::errc{} || r.ptr != m_s.data() + m_i) invalid("invalid_number");
        return v;
    }

    void literal(std::string_view token) {
        if (m_s.substr(m_i, token.size()) != token) invalid("invalid_json");
        m_i += token.size();
    }

    bool take(char c) {
        if (m_i < m_s.size() && m_s[m_i] == c) { ++m_i; return true; }
        return false;
    }

    void ws() {
        while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\r' || m_s[m_i] == '\n')) ++m_i;
    }

    std::string_view m_s;
    size_t m_i = 0;
};

void quote(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void write(std::string& out, const Json& v) {
    if (v.isNull()) { out += "null"; return; }
    if (const auto* b = v.boolean()) { out += *b ? "true" : "false"; return; }
    if (const auto* n = v.integer()) { out += std::to_string(*n); return; }
    if (const auto* s = v.string()) { quote(out, *s); return; }
    if (const auto* a = v.array()) {
        out += '[';
        for (size_t i = 0; i < a->size(); ++i) { if (i) out += ','; write(out, (*a)[i]); }
        out += ']';
        return;
    }
    const auto* o = v.object();
    out += '{';
    for (size_t i = 0; i < o->size(); ++i) {
        if (i) out += ',';
        quote(out, (*o)[i].first);
        out += ':';
        write(out, (*o)[i].second);
    }
    out += '}';
}

}  // namespace

const Json* field(const JsonObject& object, std::string_view key) {
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

Json parseJson(std::string_view utf8) {
    if (!validUtf8(utf8)) invalid("invalid_utf8");
    return Parser(utf8).parse();
}

std::string writeJson(const Json& value) {
    std::string out;
    write(out, value);
    return out;
}

}  // namespace Propods::v2
