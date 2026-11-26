#include "core/resume.hpp"

#include <gtest/gtest.h>

#include <string>

using dm::core::if_range_validator;

// Behaviour checked by these tests (step 4, If-Range, RFC 9110 section 13.1.5):
//   * a strong ETag ("...") is the validator to send in If-Range, with or without Last-Modified;
//   * a weak ETag (W/"...") must not be sent in If-Range: Last-Modified is used instead;
//   * without a usable ETag, Last-Modified is used; without either, nothing is sent (empty);
//   * the value is returned exactly as the server sent it.
namespace {
    constexpr const char* strong = "\"v1-abc\"";
    constexpr const char* weak = "W/\"v1-abc\"";
    constexpr const char* date = "Wed, 21 Oct 2025 07:28:00 GMT";

    TEST(IfRangeValidatorTest, StrongEtagIsPreferred) {
        EXPECT_EQ(if_range_validator(strong, date), strong) << "an ETag identifies the exact version";
    }

    TEST(IfRangeValidatorTest, StrongEtagIsEnoughOnItsOwn) {
        EXPECT_EQ(if_range_validator(strong, ""), strong) << "many servers send an ETag only";
    }

    TEST(IfRangeValidatorTest, WeakEtagFallsBackToLastModified) {
        EXPECT_EQ(if_range_validator(weak, date), date)
                << "a weak ETag may match a different version: joining two versions would corrupt the file";
    }

    TEST(IfRangeValidatorTest, WeakEtagAloneGivesNothing) {
        EXPECT_EQ(if_range_validator(weak, ""), "");
    }

    TEST(IfRangeValidatorTest, LastModifiedIsUsedWithoutEtag) {
        EXPECT_EQ(if_range_validator("", date), date);
    }

    TEST(IfRangeValidatorTest, NothingToSendWithoutValidators) {
        EXPECT_EQ(if_range_validator("", ""), "") << "empty: no If-Range header at all";
    }

    TEST(IfRangeValidatorTest, EmptyStrongEtagIsStillAnEtag) {
        EXPECT_EQ(if_range_validator("\"\"", date), "\"\"") << "\"\" is a valid (if odd) strong ETag";
    }

    TEST(IfRangeValidatorTest, WeakMarkerAloneIsNotStrong) {
        EXPECT_EQ(if_range_validator("W/", date), date);
    }

    TEST(IfRangeValidatorTest, ValuesAreReturnedUnchanged) {
        const std::string etag = "\"a b\\\"c\"";   // spaces and an escaped quote inside
        EXPECT_EQ(if_range_validator(etag, date), etag) << "If-Range must send back exactly what came";
        EXPECT_EQ(if_range_validator("", "  odd date  "), "  odd date  ");
    }
}
