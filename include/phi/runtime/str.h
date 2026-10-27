// runtime/str.h
#pragma once

// The string vocabulary of the Qt exit (6d string sweep). Text is std::string
// holding UTF-8; raw bytes are std::string too - the QString/QByteArray split
// dissolves, and with it every fromUtf8/toUtf8 crossing. These free functions
// cover the QString idioms the daemons and adapters actually use; everything
// C++ already has (starts_with, ends_with, contains, substr) is used directly
// and has no wrapper here.
//
// Semantics follow Qt where callers relied on them:
//  - the parsers accept surrounding ASCII whitespace and a leading '+',
//    require a full match, and return 0 (with *ok=false) on anything else;
//    base 16 accepts an optional 0x prefix
//  - number(double) renders the shortest round-tripping form, like
//    QString::number(d, 'g') rendered doubles into messages
//  - case operations are ASCII-only, which is all the protocol and config
//    strings here ever needed (no locale, no Unicode case folding)
//
// Header-only on purpose: no ABI, no SONAME coupling - the installed
// libphi-runtime.so does not need to match while this header grows.

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace phi::str {

/// qPrintable: the C string for a printf-style call site. Valid for the full
/// expression the call appears in, exactly the lifetime qPrintable gave.
[[nodiscard]] inline const char *cstr(const std::string &s) noexcept
{
    return s.c_str();
}

// ---------------------------------------------------------------- whitespace

[[nodiscard]] constexpr bool isSpace(char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

[[nodiscard]] constexpr std::string_view trimmedView(std::string_view s) noexcept
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline std::string trimmed(std::string_view s)
{
    return std::string(trimmedView(s));
}

/// Whitespace runs become one space, ends are trimmed (QString::simplified).
[[nodiscard]] inline std::string simplified(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const char c : s) {
        if (isSpace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------- ASCII case

[[nodiscard]] constexpr char lowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr char upperAscii(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] inline std::string toLower(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = lowerAscii(c);
    return out;
}

[[nodiscard]] inline std::string toUpper(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = upperAscii(c);
    return out;
}

[[nodiscard]] constexpr bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lowerAscii(lhs[i]) != lowerAscii(rhs[i]))
            return false;
    }
    return true;
}

[[nodiscard]] constexpr bool startsWithIgnoreCase(std::string_view s, std::string_view prefix) noexcept
{
    return s.size() >= prefix.size() && equalsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

[[nodiscard]] constexpr bool endsWithIgnoreCase(std::string_view s, std::string_view suffix) noexcept
{
    return s.size() >= suffix.size() && equalsIgnoreCase(s.substr(s.size() - suffix.size()), suffix);
}

[[nodiscard]] inline bool containsIgnoreCase(std::string_view s, std::string_view needle) noexcept
{
    if (needle.empty())
        return true;
    if (s.size() < needle.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= s.size(); ++i) {
        if (equalsIgnoreCase(s.substr(i, needle.size()), needle))
            return true;
    }
    return false;
}

// -------------------------------------------------------------- split / join

/// QString::split. keepEmpty=false is Qt::SkipEmptyParts.
[[nodiscard]] inline std::vector<std::string> split(std::string_view s, char sep,
                                                    bool keepEmpty = true)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            if (i > start || keepEmpty)
                out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (!keepEmpty && s.empty())
        out.clear();
    return out;
}

[[nodiscard]] inline std::vector<std::string> split(std::string_view s, std::string_view sep,
                                                    bool keepEmpty = true)
{
    std::vector<std::string> out;
    if (sep.empty()) {
        if (!s.empty() || keepEmpty)
            out.emplace_back(s);
        return out;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(sep, start);
        const std::size_t end = (pos == std::string_view::npos) ? s.size() : pos;
        if (end > start || keepEmpty)
            out.emplace_back(s.substr(start, end - start));
        if (pos == std::string_view::npos)
            break;
        start = pos + sep.size();
    }
    if (!keepEmpty && s.empty())
        out.clear();
    return out;
}

[[nodiscard]] inline std::string join(const std::vector<std::string> &parts, std::string_view sep)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += sep;
        out += parts[i];
    }
    return out;
}

// ------------------------------------------------------------------ slicing

/// QString::mid - clamps instead of throwing like substr does.
[[nodiscard]] inline std::string mid(std::string_view s, std::size_t pos,
                                     std::size_t n = std::string_view::npos)
{
    if (pos >= s.size())
        return {};
    return std::string(s.substr(pos, n));
}

[[nodiscard]] inline std::string left(std::string_view s, std::size_t n)
{
    return std::string(s.substr(0, n));
}

[[nodiscard]] inline std::string right(std::string_view s, std::size_t n)
{
    if (n >= s.size())
        return std::string(s);
    return std::string(s.substr(s.size() - n));
}

[[nodiscard]] inline std::string replaceAll(std::string_view s, std::string_view from,
                                            std::string_view to)
{
    if (from.empty())
        return std::string(s);
    std::string out;
    out.reserve(s.size());
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(from, start);
        if (pos == std::string_view::npos)
            break;
        out += s.substr(start, pos - start);
        out += to;
        start = pos + from.size();
    }
    out += s.substr(start);
    return out;
}

// ---------------------------------------------------------------- formatting

template <typename Int>
    requires std::is_integral_v<Int>
[[nodiscard]] inline std::string number(Int value)
{
    std::array<char, 24> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), end);
}

[[nodiscard]] inline std::string number(double value)
{
    std::array<char, 32> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), end);
}

/// QString::arg's fieldWidth-with-fill (right-justified into `width`).
[[nodiscard]] inline std::string padLeft(std::string s, std::size_t width, char fill = ' ')
{
    if (s.size() < width)
        s.insert(0, width - s.size(), fill);
    return s;
}

namespace detail {

[[nodiscard]] inline std::string renderArg(std::string_view v) { return std::string(v); }
[[nodiscard]] inline std::string renderArg(const std::string &v) { return v; }
[[nodiscard]] inline std::string renderArg(const char *v) { return v ? std::string(v) : std::string(); }
[[nodiscard]] inline std::string renderArg(char v) { return std::string(1, v); }
template <typename Num>
    requires(std::is_arithmetic_v<Num> && !std::is_same_v<Num, char> && !std::is_same_v<Num, bool>)
[[nodiscard]] inline std::string renderArg(Num v)
{
    return number(v);
}

} // namespace detail

/// QString::arg, all placeholders at once: %1..%99 are replaced positionally;
/// a placeholder without a matching argument stays literal.
template <typename... Args>
[[nodiscard]] inline std::string arg(std::string_view format, const Args &...args)
{
    const std::array<std::string, sizeof...(Args)> rendered{detail::renderArg(args)...};
    std::string out;
    out.reserve(format.size() + 16 * sizeof...(Args));
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size() || format[i + 1] < '0'
            || format[i + 1] > '9') {
            out.push_back(format[i]);
            continue;
        }
        std::size_t index = static_cast<std::size_t>(format[i + 1] - '0');
        std::size_t used = 1;
        if (i + 2 < format.size() && format[i + 2] >= '0' && format[i + 2] <= '9') {
            const std::size_t twoDigit = index * 10 + static_cast<std::size_t>(format[i + 2] - '0');
            if (twoDigit >= 1 && twoDigit <= rendered.size()) {
                index = twoDigit;
                used = 2;
            }
        }
        if (index >= 1 && index <= rendered.size()) {
            out += rendered[index - 1];
            i += used;
        } else {
            out.push_back('%');
        }
    }
    return out;
}

// ------------------------------------------------------------------- parsing

namespace detail {

template <typename Int>
[[nodiscard]] inline Int parseIntegral(std::string_view s, bool *ok, int base)
{
    const auto fail = [ok]() -> Int {
        if (ok)
            *ok = false;
        return 0;
    };
    s = trimmedView(s);
    bool negative = false;
    if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    if (negative && !std::is_signed_v<Int>)
        return fail();
    if (base == 16 && s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s.remove_prefix(2);
    if (s.empty())
        return fail();
    // from_chars parses the sign itself only for signed types; feed it the
    // digits and apply the sign here so unsigned "+5" works like Qt's.
    using Wide = std::conditional_t<std::is_signed_v<Int>, long long, unsigned long long>;
    Wide value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc() || end != s.data() + s.size())
        return fail();
    if (negative)
        value = -value;
    if (value < static_cast<Wide>(std::numeric_limits<Int>::min())
        || value > static_cast<Wide>(std::numeric_limits<Int>::max()))
        return fail();
    if (ok)
        *ok = true;
    return static_cast<Int>(value);
}

} // namespace detail

[[nodiscard]] inline long long toLongLong(std::string_view s, bool *ok = nullptr, int base = 10)
{
    return detail::parseIntegral<long long>(s, ok, base);
}

[[nodiscard]] inline unsigned long long toULongLong(std::string_view s, bool *ok = nullptr,
                                                    int base = 10)
{
    return detail::parseIntegral<unsigned long long>(s, ok, base);
}

[[nodiscard]] inline int toInt(std::string_view s, bool *ok = nullptr, int base = 10)
{
    return detail::parseIntegral<int>(s, ok, base);
}

[[nodiscard]] inline unsigned int toUInt(std::string_view s, bool *ok = nullptr, int base = 10)
{
    return detail::parseIntegral<unsigned int>(s, ok, base);
}

[[nodiscard]] inline double toDouble(std::string_view s, bool *ok = nullptr)
{
    const auto fail = [ok]() -> double {
        if (ok)
            *ok = false;
        return 0.0;
    };
    s = trimmedView(s);
    bool negative = false;
    if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    if (s.empty())
        return fail();
    double value = 0.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || end != s.data() + s.size())
        return fail();
    if (ok)
        *ok = true;
    return negative ? -value : value;
}

// ------------------------------------------------------------- hex / base64

/// Lowercase hex; `sep` (when non-zero) goes between bytes, as
/// QByteArray::toHex(sep) put it.
[[nodiscard]] inline std::string toHex(std::string_view bytes, char sep = '\0')
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * (sep ? 3 : 2));
    for (const char c : bytes) {
        if (sep && !out.empty())
            out.push_back(sep);
        const auto b = static_cast<unsigned char>(c);
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

/// QByteArray::fromHex: non-hex characters are skipped; an odd digit count
/// reads as if a leading zero were present.
[[nodiscard]] inline std::string fromHex(std::string_view text)
{
    std::string digits;
    digits.reserve(text.size());
    for (const char c : text) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            digits.push_back(c);
    }
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned>(c - '0');
        return static_cast<unsigned>(lowerAscii(c) - 'a' + 10);
    };
    std::string out;
    out.reserve((digits.size() + 1) / 2);
    std::size_t i = 0;
    if (digits.size() % 2 == 1) {
        out.push_back(static_cast<char>(nibble(digits[0])));
        i = 1;
    }
    for (; i + 1 < digits.size(); i += 2)
        out.push_back(static_cast<char>((nibble(digits[i]) << 4) | nibble(digits[i + 1])));
    return out;
}

[[nodiscard]] inline std::string toBase64(std::string_view bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16)
            | (static_cast<unsigned char>(bytes[i + 1]) << 8)
            | static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16)
            | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

/// QByteArray::fromBase64 in its default lenient mode: characters outside the
/// alphabet (including '=') are skipped, a trailing partial group is dropped.
[[nodiscard]] inline std::string fromBase64(std::string_view text)
{
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::string out;
    out.reserve((text.size() / 4) * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

// ------------------------------------------------------------ MAC addresses

/// The twelve hex digits of a MAC, lowercase and without separators
/// ("d885acbbd498"); empty when `mac` is not one. Colons, dashes and dots
/// (Cisco style) are all separators. The form an id is made from.
[[nodiscard]] inline std::string macKey(std::string_view mac)
{
    std::string hex;
    for (const char c : trimmedView(mac)) {
        if (c == ':' || c == '-' || c == '.')
            continue;
        const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit || hex.size() == 12)
            return {};
        hex.push_back(lowerAscii(c));
    }
    return hex.size() == 12 ? hex : std::string();
}

/// A MAC in its canonical text form, "aa:bb:cc:dd:ee:ff"; anything that is
/// not a MAC comes back trimmed and otherwise unchanged, so a caller can
/// still show what it was given.
[[nodiscard]] inline std::string normalizedMac(std::string_view mac)
{
    const std::string key = macKey(mac);
    if (key.empty())
        return trimmed(mac);
    std::string out;
    out.reserve(17);
    for (std::size_t i = 0; i < key.size(); i += 2) {
        if (!out.empty())
            out.push_back(':');
        out.append(key, i, 2);
    }
    return out;
}

} // namespace phi::str

// phi-core code lives in namespace phicore and says str::...; this alias is
// the product-side spelling (it goes when core moves to phi:: wholesale).
namespace phicore { namespace str = phi::str; }
