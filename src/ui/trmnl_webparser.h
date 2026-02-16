// Lightweight streaming HTML parser for terminal UI
// Provides simple selector-based extraction (tag, attr, attr_value substring, index)
#pragma once

#include <string>
#include <vector>
#include <functional>

namespace ui {

enum class MatchMode {
    TEXT, // extract visible text inside element
    HTML  // extract inner HTML of element
};

struct Selector {
    std::string tag;        // tag name to match (lowercase recommended)
    std::string attr;       // attribute name to match (optional)
    std::string attr_value; // optional substring to match inside attribute value
    MatchMode mode = MatchMode::TEXT;
    int index = -1; // 1-based occurrence index to return, -1 = all
};

class TrmnlWebParser {
public:
    TrmnlWebParser();
    ~TrmnlWebParser();

    // streaming API: feed chunks of HTML data
    void reset();
    void feed(const char* data, size_t len);
    void finish();

    // query matches collected so far (does not clear internal storage)
    std::vector<std::string> getMatches(const Selector& sel) const;

    // convenience: parse a whole buffer and return matches for selector
    static std::vector<std::string> parseAll(const std::string& html, const Selector& sel);

    // optional: register a callback invoked each time a match is completed
    void setMatchCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    Impl* pimpl;
};

} // namespace ui
