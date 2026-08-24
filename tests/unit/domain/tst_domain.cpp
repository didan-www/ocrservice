#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Identifiers.h"
#include "Ports.h"
#include "Recognition.h"

namespace {

using namespace ocrservice::domain;

RecognitionId recognitionId() {
    return RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910");
}

DeviceId deviceId() {
    return DeviceId::parse("device-001");
}

CaptureId captureId() {
    return CaptureId::parse("b15c7268-b211-4a4c-a765-49b922af1910");
}

UtcTimePoint capturedAt() {
    return UtcTimePoint(1000);
}

UtcTimePoint startedAt() {
    return UtcTimePoint(2000);
}

TEST(UuidTest, AcceptsCanonicalUppercaseAndNormalizesToLowercase) {
    const auto value = Uuid::parse("A15C7268-B211-4A4C-A765-49B922AF1910");
    EXPECT_EQ(value.version(), 4U);
    EXPECT_EQ(value.toString(), "a15c7268-b211-4a4c-a765-49b922af1910");
}

TEST(UuidTest, AcceptsRfcVersionsOneThroughEight) {
    for (char version = '1'; version <= '8'; ++version) {
        std::string value = "a15c7268-b211-4a4c-a765-49b922af1910";
        value[14] = version;
        EXPECT_EQ(Uuid::parse(value).version(), static_cast<std::uint8_t>(version - '0'));
    }
}

TEST(UuidTest, RejectsNilVersionZeroNonRfcVariantAndNonCanonicalForms) {
    for (const std::string value : {
             "00000000-0000-0000-0000-000000000000",
             "a15c7268-b211-0a4c-a765-49b922af1910",
             "a15c7268-b211-9a4c-a765-49b922af1910",
             "a15c7268-b211-4a4c-0765-49b922af1910",
             "a15c7268-b211-4a4c-c765-49b922af1910",
             "a15c7268-b211-4a4c-a765-49b922af191-",
             "{a15c7268-b211-4a4c-a765-49b922af1910}",
             "a15c7268b2114a4ca76549b922af1910",
             " a15c7268-b211-4a4c-a765-49b922af1910",
             "g15c7268-b211-4a4c-a765-49b922af1910",
         }) {
        EXPECT_THROW(Uuid::parse(value), DomainError) << value;
    }
}

TEST(UuidTest, CreatesVersionFourWithRfcVariantFromProvidedRandomBytes) {
    std::array<std::uint8_t, 16> bytes{};
    bytes.fill(0xFFU);
    const auto value = Uuid::v4(bytes);
    EXPECT_EQ(value.version(), 4U);
    EXPECT_EQ(value.bytes()[8] & 0xC0U, 0x80U);
    EXPECT_EQ(Uuid::parse(value.toString()), value);
}

TEST(DeviceIdTest, EnforcesConfirmedAsciiSyntaxAndCaseSensitivity) {
    EXPECT_EQ(DeviceId::parse("A.b_C-9").value(), "A.b_C-9");
    EXPECT_FALSE(DeviceId::parse("Device-1") == DeviceId::parse("device-1"));
    EXPECT_EQ(DeviceId::parse(std::string(64U, 'a')).value().size(), 64U);

    const std::vector<std::string> invalid = {
             "",
             ".device",
             "-device",
             "_device",
             "device/1",
             "device\\1",
             "device+",
             "device#",
             "device 1",
             u8"设备一",
             std::string(65U, 'a'),
    };
    for (const auto& value : invalid) {
        EXPECT_THROW(DeviceId::parse(value), DomainError) << value;
    }
}

TEST(PlateValueTest, NormalizesChinesePlateAndAllowsEmptyKeywordOnly) {
    EXPECT_EQ(PlateNumber::parse(u8"\u3000京a12345\u0085").value(), u8"京A12345");
    EXPECT_EQ(PlateKeyword::parse("  ab  ").value(), "AB");
    EXPECT_TRUE(PlateKeyword::parse("   ").empty());
    EXPECT_THROW(PlateNumber::parse("   "), DomainError);
    EXPECT_THROW(PlateKeyword::parse("A B"), DomainError);
}

TEST(SafeIntegerTest, EnforcesJsonInteroperabilityBounds) {
    EXPECT_TRUE(isJsonSafeInteger(-static_cast<std::int64_t>(kJsonSafeIntegerMaximum)));
    EXPECT_TRUE(isJsonSafeUnsigned(kJsonSafeIntegerMaximum));
    EXPECT_FALSE(isJsonSafeUnsigned(kJsonSafeIntegerMaximum + 1U));
    EXPECT_NO_THROW(requirePositiveJsonSafe(1U, "id"));
    EXPECT_THROW(requirePositiveJsonSafe(0U, "id"), DomainError);
    EXPECT_THROW(requireNonNegativeJsonSafe(kJsonSafeIntegerMaximum + 1U, "total"), DomainError);
}

TEST(RecognitionOutcomeTest, KeepsSuccessAndModelFailuresDisjoint) {
    const auto success = RecognitionOutcome::succeeded(PlateNumber::parse(u8"京A12345"));
    ASSERT_TRUE(success.isSuccess());
    EXPECT_EQ(success.plateNumber().value(), u8"京A12345");
    EXPECT_THROW(success.failureCode(), DomainError);

    const auto failure = RecognitionOutcome::failed(ModelFailureCode::plateNotFound);
    EXPECT_FALSE(failure.isSuccess());
    EXPECT_EQ(failure.failureCode(), ModelFailureCode::plateNotFound);
    EXPECT_THROW(failure.plateNumber(), DomainError);
    EXPECT_EQ(toString(failure.failureCode()), "PLATE_NOT_FOUND");
}

TEST(RecognitionSnapshotTest, AcceptsAllThreeExactStateCombinations) {
    const RecognitionSnapshot processing(
        recognitionId(), 1U, deviceId(), RecognitionStatus::processing, std::nullopt,
        std::nullopt, std::nullopt, capturedAt(), startedAt(), std::nullopt, std::nullopt);
    EXPECT_EQ(processing.revision(), 1U);
    EXPECT_FALSE(processing.completedAt());

    const RecognitionSnapshot succeeded(
        recognitionId(), 2U, deviceId(), RecognitionStatus::succeeded,
        PlateNumber::parse(u8"京A12345"), std::nullopt, std::nullopt, capturedAt(),
        startedAt(), UtcTimePoint(2120), 120U);
    EXPECT_TRUE(succeeded.plateNumber());
    EXPECT_EQ(gateActionFor(succeeded.status()), GateAction::open);

    const RecognitionSnapshot failed(
        recognitionId(), 2U, deviceId(), RecognitionStatus::failed, std::nullopt,
        RecognitionFailureCode::plateNotFound, std::string(u8"未检测到车牌"), capturedAt(),
        startedAt(), UtcTimePoint(2120), 120U);
    EXPECT_TRUE(failed.errorCode());
    EXPECT_EQ(gateActionFor(failed.status()), GateAction::keepClosed);
    EXPECT_FALSE(gateActionFor(processing.status()));
}

TEST(RecognitionSnapshotTest, RejectsInvalidNullRevisionAndTimeCombinations) {
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 2U, deviceId(), RecognitionStatus::processing, std::nullopt,
            std::nullopt, std::nullopt, capturedAt(), startedAt(), std::nullopt, std::nullopt),
        DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 1U, deviceId(), RecognitionStatus::succeeded,
            PlateNumber::parse("ABC"), std::nullopt, std::nullopt, capturedAt(), startedAt(),
            UtcTimePoint(2100), 100U),
        DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 2U, deviceId(), RecognitionStatus::succeeded, std::nullopt,
            std::nullopt, std::nullopt, capturedAt(), startedAt(), UtcTimePoint(2100), 100U),
        DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 2U, deviceId(), RecognitionStatus::failed, std::nullopt,
            RecognitionFailureCode::modelInferenceError, std::string(), capturedAt(), startedAt(),
            UtcTimePoint(2100), 100U),
        DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 2U, deviceId(), RecognitionStatus::failed, std::nullopt,
            RecognitionFailureCode::modelInferenceError, std::string("failure"), capturedAt(),
            startedAt(), UtcTimePoint(1999), 100U),
        DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), kJsonSafeIntegerMaximum + 1U, deviceId(),
            RecognitionStatus::succeeded, PlateNumber::parse("ABC"), std::nullopt, std::nullopt,
            capturedAt(), startedAt(), UtcTimePoint(2100), 100U),
        DomainError);
}

TEST(EnumTest, RejectsEveryInvalidUnderlyingEnumValue) {
    const auto invalidStatus = static_cast<RecognitionStatus>(255);
    const auto invalidGateAction = static_cast<GateAction>(255);
    const auto invalidListType = static_cast<AccessListType>(255);
    const auto invalidModelFailure = static_cast<ModelFailureCode>(255);
    const auto invalidRecognitionFailure = static_cast<RecognitionFailureCode>(255);
    const auto invalidImageFormat = static_cast<ImageFormat>(255);
    const auto invalidImageMime = static_cast<ImageMime>(255);
    const auto invalidPublishFailure = static_cast<PublishFailure>(255);

    EXPECT_THROW(toString(invalidStatus), DomainError);
    EXPECT_THROW(toString(invalidGateAction), DomainError);
    EXPECT_THROW(toString(invalidListType), DomainError);
    EXPECT_THROW(toString(invalidModelFailure), DomainError);
    EXPECT_THROW(toString(invalidRecognitionFailure), DomainError);
    EXPECT_THROW(toRecognitionFailureCode(invalidModelFailure), DomainError);
    EXPECT_THROW(gateActionFor(invalidStatus), DomainError);
    EXPECT_THROW(RecognitionOutcome::failed(invalidModelFailure), DomainError);
    EXPECT_THROW(AccessListFilter(invalidListType, PlateKeyword::parse("")), DomainError);
    EXPECT_THROW(mimeFor(invalidImageFormat), DomainError);
    EXPECT_THROW(toString(invalidImageMime), DomainError);
    EXPECT_THROW(PublishAttempt::rejected(invalidPublishFailure), DomainError);
    std::array<std::uint8_t, 32> digestBytes{};
    const Sha256Digest digest(digestBytes);
    const auto path = RelativeImagePath::parseGenerated(
        "2026/08/15/a15c7268-b211-4a4c-a765-49b922af1910.jpg");
    std::array<std::uint8_t, 1> compressed{0x01U};
    EXPECT_THROW(
        SaveImageCommand(
            recognitionId(), capturedAt(), invalidImageFormat,
            ByteView(compressed.data(), compressed.size()), digest),
        DomainError);
    EXPECT_THROW(StoredImage(path, invalidImageMime, 1U, digest), DomainError);
    EXPECT_THROW(
        NewRecognition(
            recognitionId(), deviceId(), captureId(), digest, path, invalidImageMime, 1U,
            capturedAt(), startedAt()),
        DomainError);
    EXPECT_THROW(AccessListConflict{invalidListType}, DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 1U, deviceId(), invalidStatus, std::nullopt, std::nullopt,
            std::nullopt, capturedAt(), startedAt(), std::nullopt, std::nullopt),
        DomainError);
    EXPECT_THROW(
        RecognitionSnapshot(
            recognitionId(), 2U, deviceId(), RecognitionStatus::failed, std::nullopt,
            invalidRecognitionFailure, std::string("failure"), capturedAt(), startedAt(),
            UtcTimePoint(2100), 100U),
        DomainError);
}

TEST(FilterTest, KeepsPagingSeparateAndValidatesHalfOpenRange) {
    const HistoryFilter filter(UtcTimePoint(100), UtcTimePoint(200), deviceId());
    EXPECT_EQ(filter.startInclusiveUtc().unixMilliseconds(), 100);
    ASSERT_TRUE(filter.deviceId());
    EXPECT_EQ(filter.deviceId()->value(), "device-001");
    EXPECT_THROW(HistoryFilter(UtcTimePoint(100), UtcTimePoint(100)), DomainError);
    EXPECT_THROW(HistoryFilter(UtcTimePoint(200), UtcTimePoint(100)), DomainError);

    const PageRequest page(PageRequest::maximumPage());
    EXPECT_EQ(page.page(), 2147483647U);
    EXPECT_EQ(PageRequest::pageSize(), 100U);
    EXPECT_EQ(PageRequest::maximumPage(), 2147483647U);
    EXPECT_THROW(PageRequest(0U), DomainError);
    EXPECT_THROW(PageRequest(PageRequest::maximumPage() + 1U), DomainError);
    EXPECT_THROW(PageRequest(kJsonSafeIntegerMaximum + 1U), DomainError);

    const AccessListFilter access(AccessListType::white, PlateKeyword::parse(" ab "));
    EXPECT_EQ(access.listType(), AccessListType::white);
    EXPECT_EQ(access.keyword().value(), "AB");
    EXPECT_EQ(RecognitionTask(recognitionId()).recognitionId(), recognitionId());
}

TEST(RelativeImagePathTest, RejectsAbsoluteTraversalAndNonGeneratedCharacters) {
    EXPECT_EQ(
        RelativeImagePath::parseGenerated(
            "2026/08/15/a15c7268-b211-4a4c-a765-49b922af1910.jpg")
            .value(),
        "2026/08/15/a15c7268-b211-4a4c-a765-49b922af1910.jpg");
    for (const std::string value : {
             "",
             "/absolute.jpg",
             "2026//image.jpg",
             "2026/../image.jpg",
             "2026/./image.jpg",
             "2026\\image.jpg",
             "2026/image name.jpg",
         }) {
        EXPECT_THROW(RelativeImagePath::parseGenerated(value), DomainError) << value;
    }
}

TEST(BgrImageViewTest, ValidatesNonOwningBgr8LayoutWithoutOpenCvTypes) {
    std::array<std::uint8_t, 16> pixels{};
    const BgrImageView packed(pixels.data(), 12U, 2U, 2U, 6U);
    EXPECT_EQ(packed.width(), 2U);
    EXPECT_EQ(packed.height(), 2U);
    EXPECT_EQ(packed.rowStride(), 6U);

    const BgrImageView padded(pixels.data(), 16U, 2U, 2U, 10U);
    EXPECT_EQ(padded.byteSize(), 16U);
    EXPECT_THROW(BgrImageView(nullptr, 12U, 2U, 2U, 6U), DomainError);
    EXPECT_THROW(BgrImageView(pixels.data(), 11U, 2U, 2U, 6U), DomainError);
    EXPECT_THROW(BgrImageView(pixels.data(), 12U, 2U, 2U, 5U), DomainError);
    EXPECT_THROW(
        BgrImageView(
            pixels.data(),
            pixels.size(),
            std::numeric_limits<std::size_t>::max(),
            1U,
            pixels.size()),
        DomainError);
}

class VectorReader final : public ImageReader {
public:
    explicit VectorReader(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    std::size_t read(std::uint8_t* destination, const std::size_t capacity) override {
        const auto count = std::min(capacity, bytes_.size() - offset_);
        for (std::size_t index = 0U; index < count; ++index) {
            destination[index] = bytes_[offset_ + index];
        }
        offset_ += count;
        return count;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

TEST(PortTypesTest, ImageFileIsMoveOnlyAndPublishAttemptIsExplicit) {
    static_assert(!std::is_copy_constructible_v<ImageFile>);
    static_assert(std::is_move_constructible_v<ImageFile>);

    ImageFile file(
        std::make_unique<VectorReader>(std::vector<std::uint8_t>{1U, 2U, 3U}),
        ImageMime::jpeg,
        3U);
    std::array<std::uint8_t, 3> output{};
    EXPECT_EQ(file.read(output.data(), output.size()), 3U);
    EXPECT_EQ(output, (std::array<std::uint8_t, 3>{1U, 2U, 3U}));
    EXPECT_EQ(file.mime(), ImageMime::jpeg);

    EXPECT_TRUE(PublishAttempt::accepted().wasAccepted());
    const auto rejected = PublishAttempt::rejected(PublishFailure::notConnected);
    EXPECT_FALSE(rejected.wasAccepted());
    ASSERT_TRUE(rejected.failure());
    EXPECT_EQ(*rejected.failure(), PublishFailure::notConnected);
}

TEST(RepositoryPortTypesTest, ValidateRecordsPagingAndCursorResults) {
    std::array<std::uint8_t, 32> digestBytes{};
    const Sha256Digest digest(digestBytes);
    const AdminUserRecord admin(
        1U, "admin", u8"演示管理员", "$2b$12$abcdefghijklmnopqrstuv", true);
    EXPECT_EQ(admin.id(), 1U);
    EXPECT_TRUE(admin.enabled());
    EXPECT_THROW(
        AdminUserRecord(0U, "admin", "display", "hash", true), DomainError);

    const DeviceRecord device(deviceId(), u8"入口设备", digest, "device-001", true);
    EXPECT_EQ(device.deviceId(), deviceId());
    EXPECT_EQ(device.httpTokenHash(), digest);
    EXPECT_THROW(
        DeviceRecord(deviceId(), "", digest, "device-001", true), DomainError);

    const AccessListRecord record(
        1U, AccessListType::white, PlateNumber::parse(u8"京A12345"), "", u8"演示管理员",
        UtcTimePoint(3000));
    EXPECT_EQ(record.listType(), AccessListType::white);
    const PageResult<AccessListRecord> page(
        std::vector<AccessListRecord>{record}, PageRequest(1U), 1U);
    EXPECT_EQ(page.items().size(), 1U);
    EXPECT_EQ(page.request().page(), 1U);
    EXPECT_EQ(page.total(), 1U);
    EXPECT_THROW(
        PageResult<AccessListRecord>(
            std::vector<AccessListRecord>{record}, PageRequest(1U), 0U),
        DomainError);

    const NewAccessListRecord command(
        AccessListType::black, PlateNumber::parse("ABC123"), "remark", 1U, "display",
        UtcTimePoint(3000));
    EXPECT_EQ(command.createdByUserId(), 1U);
    EXPECT_THROW(
        NewAccessListRecord(
            static_cast<AccessListType>(255), PlateNumber::parse("ABC123"), "", 1U,
            "display", UtcTimePoint(3000)),
        DomainError);

    const HistoryCursorResult cursor(10U, true);
    EXPECT_EQ(cursor.recordsVisited(), 10U);
    EXPECT_TRUE(cursor.fullyConsumed());
    EXPECT_THROW(
        HistoryCursorResult(kJsonSafeIntegerMaximum + 1U, false), DomainError);
}

}  // namespace
