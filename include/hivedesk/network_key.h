#pragma once
#include <algorithm>
#include <string_view>

namespace hive {
template<class Character>
inline bool valid_network_key_text(std::basic_string_view<Character> key) {
    return key.size() >= 4 && key.size() <= 64 &&
        std::all_of(key.begin(), key.end(), [](Character c) { return c >= 33 && c <= 126; });
}
inline bool valid_network_key(std::string_view key) { return valid_network_key_text(key); }
inline bool valid_network_key(std::wstring_view key) { return valid_network_key_text(key); }
}
