#pragma once

// A minimal, dependency-free JSON reader, purpose-built for
// config/option-sets.json's schema: a top-level object whose values are
// themselves objects (each optionally holding a "tokens" array of strings,
// plus other scalar fields this reader doesn't need) or a couple of plain
// scalar fields. This exists only so tests/option_sets_test.cpp (Task
// 1e.1's C++ anti-drift guard) doesn't need to pull a third-party JSON
// library into the C++ build -- see that task's brief. It is NOT a
// general-purpose JSON parser: no fidelity claims beyond what that one
// file needs (in particular, \u escapes are skipped rather than decoded,
// since config/option-sets.json's content is plain ASCII).

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace minijson {

class Value {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Type type = Type::kNull;
  bool bool_value = false;
  double number_value = 0.0;
  std::string string_value;
  std::vector<Value> array_value;
  std::map<std::string, Value> object_value;

  // The object member named `key`, or nullptr if this value isn't an
  // object or has no such key.
  const Value* Get(const std::string& key) const {
    if (type != Type::kObject) return nullptr;
    const auto it = object_value.find(key);
    return it == object_value.end() ? nullptr : &it->second;
  }

  // This array's elements' string_value fields, in order. Empty if this
  // value isn't an array.
  std::vector<std::string> StringArray() const {
    std::vector<std::string> out;
    if (type != Type::kArray) return out;
    out.reserve(array_value.size());
    for (const Value& v : array_value) out.push_back(v.string_value);
    return out;
  }
};

namespace internal {

// A straightforward recursive-descent parser over the full JSON grammar
// (objects, arrays, strings, numbers, booleans, null) -- general enough to
// not choke on config/option-sets.json's shape changing a little, without
// pulling in a dependency.
class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Value ParseDocument() {
    SkipWhitespace();
    Value v = ParseValue();
    SkipWhitespace();
    return v;
  }

 private:
  char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  char Take() { return text_.at(pos_++); }

  void SkipWhitespace() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  void Expect(char c) {
    if (Peek() != c) {
      throw std::runtime_error(std::string("minijson: expected '") + c +
                                "' at offset " + std::to_string(pos_));
    }
    ++pos_;
  }

  Value ParseValue() {
    SkipWhitespace();
    switch (Peek()) {
      case '{':
        return ParseObject();
      case '[':
        return ParseArray();
      case '"':
        return ParseString();
      case 't':
      case 'f':
        return ParseBool();
      case 'n':
        return ParseNull();
      default:
        return ParseNumber();
    }
  }

  Value ParseObject() {
    Value v;
    v.type = Value::Type::kObject;
    Expect('{');
    SkipWhitespace();
    if (Peek() == '}') {
      ++pos_;
      return v;
    }
    while (true) {
      SkipWhitespace();
      const Value key = ParseString();
      SkipWhitespace();
      Expect(':');
      Value val = ParseValue();
      v.object_value[key.string_value] = std::move(val);
      SkipWhitespace();
      if (Peek() == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    SkipWhitespace();
    Expect('}');
    return v;
  }

  Value ParseArray() {
    Value v;
    v.type = Value::Type::kArray;
    Expect('[');
    SkipWhitespace();
    if (Peek() == ']') {
      ++pos_;
      return v;
    }
    while (true) {
      v.array_value.push_back(ParseValue());
      SkipWhitespace();
      if (Peek() == ',') {
        ++pos_;
        SkipWhitespace();
        continue;
      }
      break;
    }
    SkipWhitespace();
    Expect(']');
    return v;
  }

  Value ParseString() {
    Value v;
    v.type = Value::Type::kString;
    Expect('"');
    std::string out;
    while (Peek() != '"') {
      if (pos_ >= text_.size()) {
        throw std::runtime_error("minijson: unterminated string");
      }
      const char c = Take();
      if (c != '\\') {
        out += c;
        continue;
      }
      const char escaped = Take();
      switch (escaped) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u':
          // Not needed for config/option-sets.json's plain-ASCII content;
          // skip the 4 hex digits rather than decoding them.
          pos_ += 4;
          break;
        default:
          out += escaped;
      }
    }
    Expect('"');
    v.string_value = out;
    return v;
  }

  Value ParseBool() {
    Value v;
    v.type = Value::Type::kBool;
    if (text_.compare(pos_, 4, "true") == 0) {
      v.bool_value = true;
      pos_ += 4;
    } else if (text_.compare(pos_, 5, "false") == 0) {
      v.bool_value = false;
      pos_ += 5;
    } else {
      throw std::runtime_error("minijson: invalid literal at offset " +
                                std::to_string(pos_));
    }
    return v;
  }

  Value ParseNull() {
    if (text_.compare(pos_, 4, "null") != 0) {
      throw std::runtime_error("minijson: invalid literal at offset " +
                                std::to_string(pos_));
    }
    pos_ += 4;
    return Value();
  }

  Value ParseNumber() {
    const size_t start = pos_;
    if (Peek() == '-') ++pos_;
    while (pos_ < text_.size() &&
           (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
            text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E' ||
            text_[pos_] == '+' || text_[pos_] == '-')) {
      ++pos_;
    }
    if (pos_ == start) {
      throw std::runtime_error("minijson: invalid number at offset " +
                                std::to_string(start));
    }
    Value v;
    v.type = Value::Type::kNumber;
    v.number_value = std::stod(text_.substr(start, pos_ - start));
    return v;
  }

  const std::string& text_;
  size_t pos_ = 0;
};

}  // namespace internal

// Parses `text` as a single JSON document. Throws std::runtime_error on
// malformed input.
inline Value Parse(const std::string& text) {
  return internal::Parser(text).ParseDocument();
}

}  // namespace minijson
