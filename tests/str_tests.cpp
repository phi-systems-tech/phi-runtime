// The string vocabulary (Qt exit, 6d string sweep): pins the Qt-shaped
// semantics phi-core's sweep translated onto - lenient parsing, positional
// arg, lenient hex/base64 decode - so a helper edit cannot silently change
// what a swept call site means.

#include "phi/runtime/maputil.h"
#include "phi/runtime/str.h"

#include "test_support.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace str = phi::str;
namespace maps = phi::maps;

namespace {

void testTrimAndCase()
{
    CHECK(str::trimmed("  a b\t\n") == "a b");
    CHECK(str::trimmed("") == "");
    CHECK(str::trimmed("\r\n") == "");
    CHECK(str::simplified("  a\t\tb \n c ") == "a b c");
    CHECK(str::toLower("MiXeD-09") == "mixed-09");
    CHECK(str::toUpper("MiXeD-09") == "MIXED-09");
    CHECK(str::equalsIgnoreCase("Content-Length", "content-length"));
    CHECK(!str::equalsIgnoreCase("a", "ab"));
    CHECK(str::startsWithIgnoreCase("HTTP/1.1", "http/"));
    CHECK(str::endsWithIgnoreCase("file.JSON", ".json"));
    CHECK(str::containsIgnoreCase("Transfer-Encoding: Chunked", "chunked"));
}

void testSplitJoin()
{
    const std::vector<std::string> parts = str::split("a,b,,c", ',');
    CHECK(parts.size() == 4 && parts[2].empty());
    const std::vector<std::string> skipped = str::split("a,b,,c", ',', false);
    CHECK(skipped.size() == 3 && skipped[2] == "c");
    CHECK(str::split("", ',').size() == 1);      // Qt: one empty part
    CHECK(str::split("", ',', false).empty());   // Qt::SkipEmptyParts
    const std::vector<std::string> wide = str::split("a::b::c", std::string_view("::"));
    CHECK(wide.size() == 3 && wide[1] == "b");
    CHECK(str::join({"a", "b", "c"}, ", ") == "a, b, c");
    CHECK(str::join({}, ",") == "");
}

void testSliceAndReplace()
{
    CHECK(str::mid("abcdef", 2, 3) == "cde");
    CHECK(str::mid("abc", 5) == "");             // clamps, no throw
    CHECK(str::left("abc", 10) == "abc");
    CHECK(str::right("abcdef", 2) == "ef");
    CHECK(str::replaceAll("a.b.c", ".", "->") == "a->b->c");
    CHECK(str::replaceAll("aaa", "aa", "b") == "ba");
}

void testNumbersAndParsing()
{
    CHECK(str::number(42) == "42");
    CHECK(str::number(-7LL) == "-7");
    CHECK(str::number(2.5) == "2.5");
    CHECK(str::number(0.1) == "0.1");            // shortest round-trip

    bool ok = false;
    CHECK(str::toInt(" 42 ", &ok) == 42 && ok);
    CHECK(str::toInt("+7", &ok) == 7 && ok);
    CHECK(str::toInt("-13", &ok) == -13 && ok);
    CHECK(str::toInt("42x", &ok) == 0 && !ok);   // full match required
    CHECK(str::toInt("", &ok) == 0 && !ok);
    CHECK(str::toInt("0x1f", &ok, 16) == 31 && ok);
    CHECK(str::toInt("1f", &ok, 16) == 31 && ok);
    CHECK(str::toUInt("-1", &ok) == 0 && !ok);
    CHECK(str::toLongLong("9007199254740993", &ok) == 9007199254740993LL && ok);
    CHECK(str::toDouble("2.5e1", &ok) == 25.0 && ok);
    CHECK(str::toDouble(" +0.5 ", &ok) == 0.5 && ok);
    CHECK(str::toDouble("nope", &ok) == 0.0 && !ok);
}

void testArg()
{
    CHECK(str::arg("%1 = %2", "x", 5) == "x = 5");
    CHECK(str::arg("%2%1", "b", "a") == "ab");
    CHECK(str::arg("no placeholders") == "no placeholders");
    CHECK(str::arg("%1 and %3", "a", "b") == "a and %3"); // unmatched stays
    CHECK(str::arg("100%", 1) == "100%");                 // bare percent
    CHECK(str::arg("%11", "a") == "a1");                  // %1 then literal 1
    const std::string twelve =
        str::arg("%12", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l");
    CHECK(twelve == "l");                                 // two digits when they fit
}

void testHexBase64()
{
    CHECK(str::toHex(std::string("\x00\xffH", 3)) == "00ff48");
    CHECK(str::fromHex("00ff48") == std::string("\x00\xffH", 3));
    CHECK(str::fromHex("f-f") == std::string(1, '\xff')); // non-hex skipped
    CHECK(str::fromHex("abc") == std::string("\x0a\xbc", 2)); // odd: leading zero
    CHECK(str::toBase64("any carnal pleasure.") == "YW55IGNhcm5hbCBwbGVhc3VyZS4=");
    CHECK(str::toBase64("ab") == "YWI=");
    CHECK(str::toBase64("a") == "YQ==");
    CHECK(str::fromBase64("YW55IGNhcm5hbCBwbGVhc3VyZS4=") == "any carnal pleasure.");
    CHECK(str::fromBase64("YQ==") == "a");
    CHECK(str::fromBase64(" YW\n55 ") == "any");          // lenient
    for (int i = 0; i < 256; ++i) {
        const std::string one(1, static_cast<char>(i));
        CHECK(str::fromHex(str::toHex(one)) == one);
        CHECK(str::fromBase64(str::toBase64(one)) == one);
    }
}

void testMaps()
{
    std::unordered_map<std::string, int> m{{"a", 1}, {"b", 2}};
    CHECK(maps::valueOr(m, "a") == 1);
    CHECK(maps::valueOr(m, "z") == 0);
    CHECK(maps::valueOr(m, "z", -1) == -1);
    CHECK(maps::findPtr(m, "b") != nullptr && *maps::findPtr(m, "b") == 2);
    CHECK(maps::findPtr(m, "z") == nullptr);
    CHECK(maps::take(m, "a") == 1 && !m.contains("a"));
    CHECK(maps::take(m, "a", 9) == 9);
    CHECK(maps::keys(m).size() == 1 && maps::values(m).size() == 1);
}

} // namespace

int main()
{
    testTrimAndCase();
    testSplitJoin();
    testSliceAndReplace();
    testNumbersAndParsing();
    testArg();
    testHexBase64();
    testMaps();
    return phitest::report("str_tests");
}
