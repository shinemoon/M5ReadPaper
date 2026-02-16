#include "trmnl_webparser.h"

#include <cctype>
#include <algorithm>
#include <sstream>
#include <map>

namespace ui {

namespace {
static inline std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c){ return std::tolower(c); });
    return r;
}

struct FoundElement {
    std::string tag;
    std::map<std::string,std::string> attrs;
    std::string inner_html;
    std::string inner_text;
};

}

struct TrmnlWebParser::Impl {
    Impl() {
        reset();
    }
    void reset() {
        state = State::Data;
        current_tag.clear();
        current_attr_name.clear();
        current_attr_value.clear();
        current_raw_tag.clear();
        inside_quote = false;
        is_end_tag = false;
        self_closing = false;
        stack.clear();
        founds.clear();
    }

    enum class State { Data, TagOpen, TagName, AttrName, AttrEq, AttrValue, InComment, EndTag } state;
    std::string current_tag;
    std::string current_attr_name;
    std::string current_attr_value;
    std::string current_raw_tag;
    bool inside_quote = false;
    bool is_end_tag = false;
    bool self_closing = false;

    std::vector<FoundElement> stack; // nested elements being captured
    std::vector<FoundElement> founds; // completed elements
    std::function<void(const std::string&)> callback;
};

TrmnlWebParser::TrmnlWebParser() : pimpl(new Impl()) {}
TrmnlWebParser::~TrmnlWebParser(){ delete pimpl; }

void TrmnlWebParser::reset() { pimpl->reset(); }

void TrmnlWebParser::setMatchCallback(std::function<void(const std::string&)> cb) {
    pimpl->callback = cb;
}

void TrmnlWebParser::feed(const char* data, size_t len) {
    auto& I = *pimpl;
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        switch (I.state) {
        case Impl::State::Data:
            if (c == '<') {
                I.state = Impl::State::TagOpen;
                I.current_raw_tag.clear();
                I.current_raw_tag.push_back(c);
            } else {
                if (!I.stack.empty()) {
                    I.stack.back().inner_html.push_back(c);
                    I.stack.back().inner_text.push_back(c);
                }
            }
            break;

        case Impl::State::TagOpen:
            I.current_raw_tag.push_back(c);
            if (c == '!') {
                // comment or doctype
                I.state = Impl::State::InComment;
            } else if (c == '/') {
                I.is_end_tag = true;
                I.current_tag.clear();
                I.state = Impl::State::EndTag;
            } else if (std::isalpha((unsigned char)c)) {
                I.is_end_tag = false;
                I.current_tag.clear();
                I.current_tag.push_back(std::tolower((unsigned char)c));
                I.state = Impl::State::TagName;
            } else {
                // unexpected, go back to data
                I.state = Impl::State::Data;
            }
            break;

        case Impl::State::TagName:
            I.current_raw_tag.push_back(c);
            if (std::isspace((unsigned char)c)) {
                I.state = Impl::State::AttrName;
                I.current_attr_name.clear();
                I.current_attr_value.clear();
            } else if (c == '>') {
                // complete start tag
                FoundElement fe;
                fe.tag = toLower(I.current_tag);
                if (!I.is_end_tag) {
                    // push to stack
                    I.stack.push_back(fe);
                }
                I.state = Impl::State::Data;
                I.current_raw_tag.clear();
            } else if (c == '/') {
                // possibly self-closing; keep reading
                I.self_closing = true;
            } else {
                I.current_tag.push_back(std::tolower((unsigned char)c));
            }
            break;

        case Impl::State::AttrName:
            I.current_raw_tag.push_back(c);
            if (c == '>') {
                // finish tag
                FoundElement fe;
                fe.tag = toLower(I.current_tag);
                if (!I.is_end_tag) {
                    I.stack.push_back(fe);
                } else {
                    // end tag immediately closes top; handle below
                    if (!I.stack.empty() && I.stack.back().tag == toLower(I.current_tag)) {
                        FoundElement closed = I.stack.back();
                        I.stack.pop_back();
                        I.founds.push_back(closed);
                        if (I.callback) I.callback(closed.inner_html);
                    }
                }
                I.state = Impl::State::Data;
                I.current_raw_tag.clear();
                I.current_attr_name.clear();
                I.current_attr_value.clear();
                I.current_tag.clear();
            } else if (c == '/') {
                I.self_closing = true;
            } else if (std::isspace((unsigned char)c)) {
                // skip
            } else if (c == '=') {
                I.state = Impl::State::AttrEq;
            } else if (c == '"' || c == '\'') {
                // unlikely here
            } else {
                // start attribute name
                I.current_attr_name.clear();
                I.current_attr_name.push_back(std::tolower((unsigned char)c));
                I.state = Impl::State::AttrName;
                // continue accumulating name via same state
            }
            break;

        case Impl::State::AttrEq:
            I.current_raw_tag.push_back(c);
            if (c == '"' || c == '\'') {
                I.inside_quote = true;
                I.current_attr_value.clear();
                I.state = Impl::State::AttrValue;
            } else if (std::isspace((unsigned char)c)) {
                // skip
            } else {
                I.current_attr_value.clear();
                I.current_attr_value.push_back(c);
                I.state = Impl::State::AttrValue;
            }
            break;

        case Impl::State::AttrValue:
            I.current_raw_tag.push_back(c);
            if (I.inside_quote) {
                if (c == '"' || c == '\'') {
                    // finish attr value
                    if (!I.stack.empty()) {
                        I.stack.back().attrs[I.current_attr_name] = I.current_attr_value;
                    }
                    I.current_attr_name.clear();
                    I.current_attr_value.clear();
                    I.inside_quote = false;
                    I.state = Impl::State::AttrName;
                } else {
                    I.current_attr_value.push_back(c);
                }
            } else {
                if (std::isspace((unsigned char)c)) {
                    if (!I.stack.empty()) {
                        I.stack.back().attrs[I.current_attr_name] = I.current_attr_value;
                    }
                    I.current_attr_name.clear();
                    I.current_attr_value.clear();
                    I.state = Impl::State::AttrName;
                } else if (c == '>') {
                    if (!I.stack.empty()) {
                        I.stack.back().attrs[I.current_attr_name] = I.current_attr_value;
                    }
                    I.current_attr_name.clear();
                    I.current_attr_value.clear();
                    I.state = Impl::State::Data;
                    I.current_raw_tag.clear();
                } else {
                    I.current_attr_value.push_back(c);
                }
            }
            break;

        case Impl::State::InComment:
            // very simple comment skip until -->
            I.current_raw_tag.push_back(c);
            if (I.current_raw_tag.size() >= 3) {
                size_t s = I.current_raw_tag.size();
                if (I.current_raw_tag[s-3] == '-' && I.current_raw_tag[s-2] == '-' && I.current_raw_tag[s-1] == '>') {
                    I.state = Impl::State::Data;
                    I.current_raw_tag.clear();
                }
            }
            break;

        case Impl::State::EndTag:
            I.current_raw_tag.push_back(c);
            if (std::isalpha((unsigned char)c)) {
                I.current_tag.push_back(std::tolower((unsigned char)c));
            } else if (std::isspace((unsigned char)c)) {
                // ignore spacing in end tag
            } else if (c == '>') {
                // close top if matches
                if (!I.stack.empty() && I.stack.back().tag == toLower(I.current_tag)) {
                    FoundElement closed = I.stack.back();
                    I.stack.pop_back();
                    I.founds.push_back(closed);
                    if (I.callback) I.callback(closed.inner_html);
                    // if still inside a parent, append child's html/text to parent
                    if (!I.stack.empty()) {
                        I.stack.back().inner_html += closed.inner_html;
                        I.stack.back().inner_text += closed.inner_text;
                    }
                }
                I.state = Impl::State::Data;
                I.current_tag.clear();
                I.current_raw_tag.clear();
            }
            break;

        }
    }
}

void TrmnlWebParser::finish() {
    // close remaining open elements
    auto& I = *pimpl;
    while (!I.stack.empty()) {
        FoundElement f = I.stack.back();
        I.stack.pop_back();
        I.founds.push_back(f);
        if (I.callback) I.callback(f.inner_html);
    }
}

std::vector<std::string> TrmnlWebParser::getMatches(const Selector& sel) const {
    std::vector<std::string> out;
    auto& I = *pimpl;
    int count = 0;
    std::string tagLower = toLower(sel.tag);
    for (const auto& f : I.founds) {
        if (!sel.tag.empty() && toLower(f.tag) != tagLower) continue;
        if (!sel.attr.empty()) {
            auto it = f.attrs.find(sel.attr);
            if (it == f.attrs.end()) continue;
            if (!sel.attr_value.empty()) {
                if (toLower(it->second).find(toLower(sel.attr_value)) == std::string::npos) continue;
            }
        }
        ++count;
        if (sel.index > 0) {
            if (count == sel.index) {
                out.push_back(sel.mode == MatchMode::HTML ? f.inner_html : f.inner_text);
                break;
            }
        } else {
            out.push_back(sel.mode == MatchMode::HTML ? f.inner_html : f.inner_text);
        }
    }
    return out;
}

std::vector<std::string> TrmnlWebParser::parseAll(const std::string& html, const Selector& sel) {
    TrmnlWebParser p;
    p.feed(html.c_str(), html.size());
    p.finish();
    return p.getMatches(sel);
}

} // namespace ui
