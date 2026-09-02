// runtime/maputil.h
#pragma once

// The associative-container idioms QHash/QMap made implicit (Qt exit, 6d
// string sweep): lookup with a default, lookup as a pointer, take. Works on
// std::map / std::unordered_map and their multi variants. Everything the
// std containers already say well (contains, erase, operator[]) has no
// wrapper here.

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace phi::maps {

/// Hash for std::pair keys (what qHash overloads provided for QPair keys).
struct PairHash {
    template <typename A, typename B>
    [[nodiscard]] std::size_t operator()(const std::pair<A, B> &p) const noexcept
    {
        const std::size_t h1 = std::hash<A>{}(p.first);
        const std::size_t h2 = std::hash<B>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

/// QHash::value(key) / value(key, def): a copy, or the default when absent.
template <typename Map, typename Key>
[[nodiscard]] typename Map::mapped_type valueOr(const Map &map, const Key &key,
                                                typename Map::mapped_type def = {})
{
    const auto it = map.find(key);
    return it == map.end() ? std::move(def) : it->second;
}

/// The lookup QHash::value hid: a pointer into the map, null when absent.
template <typename Map, typename Key>
[[nodiscard]] const typename Map::mapped_type *findPtr(const Map &map, const Key &key)
{
    const auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
}

template <typename Map, typename Key>
[[nodiscard]] typename Map::mapped_type *findPtr(Map &map, const Key &key)
{
    const auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
}

/// QHash::take: remove and return, or the default when absent.
template <typename Map, typename Key>
[[nodiscard]] typename Map::mapped_type take(Map &map, const Key &key,
                                             typename Map::mapped_type def = {})
{
    const auto it = map.find(key);
    if (it == map.end())
        return def;
    typename Map::mapped_type value = std::move(it->second);
    map.erase(it);
    return value;
}

/// QMultiHash::values(key): every value stored under one key.
template <typename MultiMap, typename Key>
[[nodiscard]] std::vector<typename MultiMap::mapped_type> valuesFor(const MultiMap &map,
                                                                    const Key &key)
{
    std::vector<typename MultiMap::mapped_type> out;
    const auto [begin, end] = map.equal_range(key);
    for (auto it = begin; it != end; ++it)
        out.push_back(it->second);
    return out;
}

/// QMultiHash::remove(key, value): drop every (key, value) entry.
template <typename MultiMap, typename Key, typename Value>
void eraseValue(MultiMap &map, const Key &key, const Value &value)
{
    const auto [begin, end] = map.equal_range(key);
    for (auto it = begin; it != end;) {
        if (it->second == value)
            it = map.erase(it);
        else
            ++it;
    }
}

/// QList::contains for sequence containers (maps and sets have their own).
template <typename Seq, typename Value>
[[nodiscard]] bool contains(const Seq &seq, const Value &value)
{
    for (const auto &entry : seq) {
        if (entry == value)
            return true;
    }
    return false;
}

/// QHash::keys - only where a detached list is genuinely needed; iteration
/// sites should use structured bindings instead.
template <typename Map>
[[nodiscard]] std::vector<typename Map::key_type> keys(const Map &map)
{
    std::vector<typename Map::key_type> out;
    out.reserve(map.size());
    for (const auto &[key, value] : map)
        out.push_back(key);
    return out;
}

template <typename Map>
[[nodiscard]] std::vector<typename Map::mapped_type> values(const Map &map)
{
    std::vector<typename Map::mapped_type> out;
    out.reserve(map.size());
    for (const auto &[key, value] : map)
        out.push_back(value);
    return out;
}

} // namespace phi::maps

// Product-side spelling, same shape as str.h's alias.
namespace phicore { namespace maps = phi::maps; }
