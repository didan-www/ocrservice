#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Utf8.h"

namespace {

using ocrservice::text::Utf8Error;
using ocrservice::text::countCodePoints;
using ocrservice::text::decodeUtf8;
using ocrservice::text::isUnicodeWhitespace;
using ocrservice::text::normalizePlateLike;

TEST(Utf8Test, DecodesAndCountsUnicodeCodePointsInsteadOfBytes) {
    const std::string plate = u8"京A12345";
    const auto decoded = decodeUtf8(plate);
    EXPECT_EQ(decoded.size(), 7U);
    EXPECT_EQ(countCodePoints(plate), 7U);
    EXPECT_EQ(decoded.front(), 0x4EAC);

    const std::string supplementary = u8"A😀B";
    EXPECT_EQ(countCodePoints(supplementary), 3U);
}

TEST(Utf8Test, RejectsMalformedTruncatedOverlongAndSurrogateSequences) {
    const std::vector<std::string> invalid = {
        std::string("\xFF", 1),
        std::string("\xE4\xBA", 2),
        std::string("\xC0\xAF", 2),
        std::string("\xED\xA0\x80", 3),
    };
    for (const auto& value : invalid) {
        EXPECT_THROW(decodeUtf8(value), Utf8Error);
    }
}

TEST(Utf8Test, UsesTheConfirmedUnicodeWhitespaceSet) {
    for (const std::int32_t codePoint : {
             0x0009,
             0x000A,
             0x000B,
             0x000C,
             0x000D,
             0x0085,
             0x0020,
             0x00A0,
             0x2028,
             0x2029,
             0x3000,
         }) {
        EXPECT_TRUE(isUnicodeWhitespace(codePoint)) << codePoint;
    }
    EXPECT_FALSE(isUnicodeWhitespace('A'));
    EXPECT_FALSE(isUnicodeWhitespace(0x200B));
}

TEST(Utf8Test, NormalizesPlateByCodePointAndAsciiCase) {
    EXPECT_EQ(normalizePlateLike(u8"\u3000京a12345\u0085", false), u8"京A12345");
    EXPECT_EQ(normalizePlateLike(" abc-123 ", false), "ABC-123");

    std::string sixteen;
    for (int index = 0; index < 16; ++index) {
        sixteen += u8"京";
    }
    EXPECT_EQ(countCodePoints(normalizePlateLike(sixteen, false)), 16U);
    EXPECT_THROW(normalizePlateLike(sixteen + u8"京", false), Utf8Error);
}

TEST(Utf8Test, RejectsInternalWhitespaceAndHandlesEmptyKeyword) {
    EXPECT_THROW(normalizePlateLike(u8"京 A12345", false), Utf8Error);
    EXPECT_THROW(normalizePlateLike(u8"京\u0085A12345", false), Utf8Error);
    EXPECT_THROW(normalizePlateLike("   ", false), Utf8Error);
    EXPECT_EQ(normalizePlateLike(u8"\u3000\u0085", true), "");
}

}  // namespace
