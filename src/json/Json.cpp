#include "Json.hpp"
#include <cctype>
#include <cstdlib>
#include <algorithm>

namespace Omega {

// ============================================================================
// OLD JSON CLASS IMPLEMENTATION
// ============================================================================
void JSON::trim(std::string& s) {
    while(!s.empty() && (s.front()==' '||s.front()=='"')) s.erase(s.begin());
    while(!s.empty() && (s.back()==' '||s.back()=='"')) s.pop_back();
}

bool JSON::parse(const std::string& s, std::unordered_map<std::string, std::string>& kv) {
    kv.clear();
    size_t i = 0;
    while (i < s.size()) {
        // Find key
        size_t ks = s.find('"', i);
        if (ks == std::string::npos) break;
        size_t ke = s.find('"', ks + 1);
        if (ke == std::string::npos) break;
        std::string key = s.substr(ks + 1, ke - ks - 1);

        // Find colon
        size_t col = s.find(':', ke);
        if (col == std::string::npos) break;

        // Find value
        size_t vs = col + 1;
        while (vs < s.size() && s[vs] == ' ') vs++;
        if (vs >= s.size()) break;

        std::string val;
        if (s[vs] == '"') {
            size_t ve = s.find('"', vs + 1);
            if (ve == std::string::npos) break;
            val = s.substr(vs + 1, ve - vs - 1);
            i = ve + 1;
        } else if (s[vs] == '{' || s[vs] == '[') {
            // Skip nested objects/arrays
            int depth = 1;
            size_t ve = vs + 1;
            char open = s[vs];
            char close = (open == '{') ? '}' : ']';
            while (ve < s.size() && depth > 0) {
                if (s[ve] == open) depth++;
                else if (s[ve] == close) depth--;
                ve++;
            }
            val = s.substr(vs, ve - vs);
            i = ve;
        } else {
            size_t ve = s.find_first_of(",}", vs);
            if (ve == std::string::npos) ve = s.size();
            val = s.substr(vs, ve - vs);
            trim(val);
            i = ve;
        }

        kv[key] = val;
    }
    return !kv.empty();
}

bool JSON::parseDepth(const std::string& s,
                      std::vector<std::pair<double,double>>& bids,
                      std::vector<std::pair<double,double>>& asks) {
    bids.clear();
    asks.clear();
    
    auto parseArray = [](const std::string& arr, std::vector<std::pair<double,double>>& out) {
        size_t i = 0;
        while (i < arr.size()) {
            size_t start = arr.find('[', i);
            if (start == std::string::npos) break;
            size_t end = arr.find(']', start);
            if (end == std::string::npos) break;
            
            std::string item = arr.substr(start + 1, end - start - 1);
            size_t comma = item.find(',');
            if (comma != std::string::npos) {
                std::string p = item.substr(0, comma);
                std::string q = item.substr(comma + 1);
                // Remove quotes
                p.erase(std::remove(p.begin(), p.end(), '"'), p.end());
                q.erase(std::remove(q.begin(), q.end(), '"'), q.end());
                try {
                    out.push_back({std::stod(p), std::stod(q)});
                } catch (...) {}
            }
            i = end + 1;
        }
    };
    
    size_t bidsPos = s.find("\"bids\"");
    size_t asksPos = s.find("\"asks\"");
    
    if (bidsPos != std::string::npos) {
        size_t start = s.find('[', bidsPos);
        if (start != std::string::npos) {
            int depth = 1;
            size_t end = start + 1;
            while (end < s.size() && depth > 0) {
                if (s[end] == '[') depth++;
                else if (s[end] == ']') depth--;
                end++;
            }
            parseArray(s.substr(start, end - start), bids);
        }
    }
    
    if (asksPos != std::string::npos) {
        size_t start = s.find('[', asksPos);
        if (start != std::string::npos) {
            int depth = 1;
            size_t end = start + 1;
            while (end < s.size() && depth > 0) {
                if (s[end] == '[') depth++;
                else if (s[end] == ']') depth--;
                end++;
            }
            parseArray(s.substr(start, end - start), asks);
        }
    }
    
    return !bids.empty() || !asks.empty();
}

// ============================================================================
// NEW Json CLASS IMPLEMENTATION  
// ============================================================================
void Json::skipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() && std::isspace(json[pos])) pos++;
}

std::string Json::parseString(const std::string& json, size_t& pos) {
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;  // skip opening quote
    
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    if (pos < json.size()) pos++;  // skip closing quote
    return result;
}

double Json::parseNumber(const std::string& json, size_t& pos) {
    size_t start = pos;
    if (json[pos] == '-') pos++;
    while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+' || json[pos] == '-')) {
        pos++;
    }
    std::string numStr = json.substr(start, pos - start);
    try {
        return std::stod(numStr);
    } catch (...) {
        return 0.0;
    }
}

JsonValue Json::parseArray(const std::string& json, size_t& pos) {
    JsonValue arr = JsonValue::array();
    pos++;  // skip '['
    skipWhitespace(json, pos);
    
    while (pos < json.size() && json[pos] != ']') {
        auto val = std::make_shared<JsonValue>(parseValue(json, pos));
        arr.push(val);
        
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            pos++;
            skipWhitespace(json, pos);
        }
    }
    if (pos < json.size()) pos++;  // skip ']'
    return arr;
}

JsonValue Json::parseObject(const std::string& json, size_t& pos) {
    JsonValue obj = JsonValue::object();
    pos++;  // skip '{'
    skipWhitespace(json, pos);
    
    while (pos < json.size() && json[pos] != '}') {
        skipWhitespace(json, pos);
        std::string key = parseString(json, pos);
        skipWhitespace(json, pos);
        
        if (pos < json.size() && json[pos] == ':') {
            pos++;
            skipWhitespace(json, pos);
            auto val = std::make_shared<JsonValue>(parseValue(json, pos));
            obj.set(key, val);
        }
        
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            pos++;
            skipWhitespace(json, pos);
        }
    }
    if (pos < json.size()) pos++;  // skip '}'
    return obj;
}

JsonValue Json::parseValue(const std::string& json, size_t& pos) {
    skipWhitespace(json, pos);
    
    if (pos >= json.size()) return JsonValue();
    
    char c = json[pos];
    
    if (c == '{') {
        return parseObject(json, pos);
    } else if (c == '[') {
        return parseArray(json, pos);
    } else if (c == '"') {
        return JsonValue(parseString(json, pos));
    } else if (c == 't' && json.substr(pos, 4) == "true") {
        pos += 4;
        return JsonValue(true);
    } else if (c == 'f' && json.substr(pos, 5) == "false") {
        pos += 5;
        return JsonValue(false);
    } else if (c == 'n' && json.substr(pos, 4) == "null") {
        pos += 4;
        return JsonValue();
    } else if (c == '-' || std::isdigit(c)) {
        return JsonValue(parseNumber(json, pos));
    }
    
    return JsonValue();
}

JsonValue Json::parse(const std::string& json) {
    size_t pos = 0;
    return parseValue(json, pos);
}

} // namespace Omega
