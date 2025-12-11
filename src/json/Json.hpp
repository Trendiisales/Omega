#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace Omega {

// ============================================================================
// OLD INTERFACE (JSON class) - for backward compatibility
// ============================================================================
class JSON {
public:
    static bool parse(const std::string& s, std::unordered_map<std::string, std::string>& kv);
    static bool parseDepth(const std::string& s,
                           std::vector<std::pair<double,double>>& bids,
                           std::vector<std::pair<double,double>>& asks);
private:
    static void trim(std::string& s);
};

// ============================================================================
// NEW INTERFACE (Json/JsonValue classes) - for advanced parsing
// ============================================================================
class JsonValue {
public:
    using Object = std::unordered_map<std::string, std::shared_ptr<JsonValue>>;
    using Array = std::vector<std::shared_ptr<JsonValue>>;
    
    JsonValue() : type_(Type::Null) {}
    JsonValue(double n) : type_(Type::Number), number_(n) {}
    JsonValue(const std::string& s) : type_(Type::String), string_(s) {}
    JsonValue(bool b) : type_(Type::Bool), bool_(b) {}
    
    static JsonValue object() { JsonValue v; v.type_ = Type::Object; return v; }
    static JsonValue array() { JsonValue v; v.type_ = Type::Array; return v; }
    
    bool is_null() const { return type_ == Type::Null; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    
    double get_number() const { return number_; }
    const std::string& get_string() const { return string_; }
    bool get_bool() const { return bool_; }
    
    size_t size() const { return array_.size(); }
    
    JsonValue& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            static JsonValue null_val;
            return null_val;
        }
        auto it = object_.find(key);
        if (it == object_.end()) {
            object_[key] = std::make_shared<JsonValue>();
            return *object_[key];
        }
        return *it->second;
    }
    
    const JsonValue& operator[](const std::string& key) const {
        if (type_ != Type::Object) {
            static JsonValue null_val;
            return null_val;
        }
        auto it = object_.find(key);
        if (it == object_.end()) {
            static JsonValue null_val;
            return null_val;
        }
        return *it->second;
    }
    
    JsonValue& operator[](size_t idx) {
        if (type_ != Type::Array || idx >= array_.size()) {
            static JsonValue null_val;
            return null_val;
        }
        return *array_[idx];
    }
    
    const JsonValue& operator[](size_t idx) const {
        if (type_ != Type::Array || idx >= array_.size()) {
            static JsonValue null_val;
            return null_val;
        }
        return *array_[idx];
    }
    
    void set(const std::string& key, std::shared_ptr<JsonValue> val) {
        type_ = Type::Object;
        object_[key] = val;
    }
    
    void push(std::shared_ptr<JsonValue> val) {
        type_ = Type::Array;
        array_.push_back(val);
    }

private:
    enum class Type { Null, Number, String, Bool, Object, Array };
    Type type_;
    double number_ = 0;
    std::string string_;
    bool bool_ = false;
    Object object_;
    Array array_;
};

class Json {
public:
    static JsonValue parse(const std::string& json);
    
private:
    static JsonValue parseValue(const std::string& json, size_t& pos);
    static JsonValue parseObject(const std::string& json, size_t& pos);
    static JsonValue parseArray(const std::string& json, size_t& pos);
    static std::string parseString(const std::string& json, size_t& pos);
    static double parseNumber(const std::string& json, size_t& pos);
    static void skipWhitespace(const std::string& json, size_t& pos);
};

} // namespace Omega
