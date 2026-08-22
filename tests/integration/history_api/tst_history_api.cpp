#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "AuthService.h"
#include "CsvSerialization.h"
#include "CsvService.h"
#include "HistoryService.h"
#include "HttpServer.h"
#include "ProtocolTime.h"
#include "RecognitionController.h"
#include "RequestId.h"

namespace ocrservice {
namespace {

using namespace std::chrono_literals;
using services::auth::Authenticated;
using services::auth::AuthenticationResult;
using services::auth::AuthFailure;
using services::auth::AuthSession;
using services::auth::IAuthService;
using services::auth::LoginResult;
using services::auth::OperationResult;

constexpr std::string_view kValidToken = "valid";

std::string uuidText(const std::uint64_t value) {
    char output[37]{};
    const int written = std::snprintf(
        output,
        sizeof(output),
        "00000000-0000-4000-8000-%012llx",
        static_cast<unsigned long long>(value));
    if (written != 36) {
        throw std::runtime_error("failed to format UUID");
    }
    return std::string(output);
}

domain::RecognitionId recognitionId(const std::uint64_t value) {
    return domain::RecognitionId::parse(uuidText(value));
}

domain::CaptureId captureId(const std::uint64_t value) {
    return domain::CaptureId::parse(uuidText(value + 1000U));
}

domain::UtcTimePoint protocolTime(const std::string_view value) {
    return serialization::time::parseProtocolTime(value);
}

domain::RecognitionRecord processingRecord(
    const std::uint64_t id,
    const std::string_view device = "device-001",
    const domain::ImageMime mime = domain::ImageMime::jpeg,
    const std::uint64_t size = 4U) {
    const auto captured = protocolTime("2026-08-15T12:30:44.000+08:00");
    const auto started = protocolTime("2026-08-15T12:30:45.000+08:00");
    return domain::RecognitionRecord(
        domain::RecognitionSnapshot(
            recognitionId(id),
            1U,
            domain::DeviceId::parse(device),
            domain::RecognitionStatus::processing,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            captured,
            started,
            std::nullopt,
            std::nullopt),
        captureId(id),
        domain::Sha256Digest::parseHex(std::string(64U, 'a')),
        domain::RelativeImagePath::parseGenerated(
            "2026/08/15/" + uuidText(id) +
            (mime == domain::ImageMime::jpeg ? ".jpg" : ".png")),
        mime,
        size);
}

domain::RecognitionRecord succeededRecord(const std::uint64_t id) {
    const auto captured = protocolTime("2026-08-15T12:31:44.000+08:00");
    const auto started = protocolTime("2026-08-15T12:31:45.000+08:00");
    const auto completed = protocolTime("2026-08-15T12:31:45.120+08:00");
    return domain::RecognitionRecord(
        domain::RecognitionSnapshot(
            recognitionId(id),
            2U,
            domain::DeviceId::parse("device-001"),
            domain::RecognitionStatus::succeeded,
            domain::PlateNumber::parse("京A12345"),
            std::nullopt,
            std::nullopt,
            captured,
            started,
            completed,
            120U),
        captureId(id),
        domain::Sha256Digest::parseHex(std::string(64U, 'b')),
        domain::RelativeImagePath::parseGenerated("2026/08/15/" + uuidText(id) + ".png"),
        domain::ImageMime::png,
        5U);
}

domain::RecognitionRecord failedRecord(const std::uint64_t id) {
    const auto captured = protocolTime("2026-08-15T12:32:44.000+08:00");
    const auto started = protocolTime("2026-08-15T12:32:45.000+08:00");
    const auto completed = protocolTime("2026-08-15T12:32:45.321+08:00");
    return domain::RecognitionRecord(
        domain::RecognitionSnapshot(
            recognitionId(id),
            2U,
            domain::DeviceId::parse("device-002"),
            domain::RecognitionStatus::failed,
            std::nullopt,
            domain::RecognitionFailureCode::modelInferenceError,
            std::string("失败,含\"引号\"\r\n换行"),
            captured,
            started,
            completed,
            321U),
        captureId(id),
        domain::Sha256Digest::parseHex(std::string(64U, 'c')),
        domain::RelativeImagePath::parseGenerated("2026/08/15/" + uuidText(id) + ".jpg"),
        domain::ImageMime::jpeg,
        6U);
}

class MemoryReader final : public domain::ImageReader {
public:
    MemoryReader(std::vector<std::uint8_t> bytes, std::optional<std::size_t> failAt)
        : bytes_(std::move(bytes)), failAt_(failAt) {}

    std::size_t read(std::uint8_t* destination, const std::size_t capacity) override {
        if (failAt_ && offset_ >= *failAt_) {
            throw std::runtime_error("injected image read failure");
        }
        auto remaining = bytes_.size() - offset_;
        if (failAt_) {
            remaining = std::min(remaining, *failAt_ - offset_);
        }
        const auto count = std::min(capacity, remaining);
        if (count != 0U) {
            std::memcpy(destination, bytes_.data() + offset_, count);
            offset_ += count;
        }
        return count;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::optional<std::size_t> failAt_;
    std::size_t offset_ = 0U;
};

class VectorCursor final : public domain::IHistoryCursor {
public:
    VectorCursor(
        std::vector<domain::RecognitionRecord> records,
        std::optional<std::size_t> failAt = std::nullopt)
        : records_(std::move(records)), failAt_(failAt) {}

    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> next() override {
        if (failAt_ && index_ == *failAt_) {
            ++index_;
            return domain::RepositoryFailure::internal;
        }
        if (index_ == records_.size()) {
            return std::optional<domain::RecognitionRecord>{};
        }
        return std::optional<domain::RecognitionRecord>(records_.at(index_++));
    }

private:
    std::vector<domain::RecognitionRecord> records_;
    std::optional<std::size_t> failAt_;
    std::size_t index_ = 0U;
};

class FakeRecognitionRepository final : public domain::IRecognitionRepository {
public:
    FakeRecognitionRepository()
        : history_{failedRecord(3U), succeededRecord(2U), processingRecord(1U)},
          records_{history_[0], history_[1], history_[2],
                   processingRecord(4U), processingRecord(5U), processingRecord(6U),
                   processingRecord(7U), processingRecord(8U)} {}

    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> findByCapture(
        const domain::DeviceId&,
        const domain::CaptureId&) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> findById(
        const domain::RecognitionId& id) override {
        if (id == recognitionId(90U)) {
            return domain::RepositoryFailure::unavailable;
        }
        if (id == recognitionId(91U)) {
            return domain::RepositoryFailure::internal;
        }
        const auto found = std::find_if(
            records_.begin(), records_.end(), [&id](const auto& record) {
                return record.snapshot().recognitionId() == id;
            });
        return found == records_.end() ? std::optional<domain::RecognitionRecord>{} :
                                         std::optional<domain::RecognitionRecord>(*found);
    }

    domain::RepositoryResult<domain::RecognitionRecord> insertProcessing(
        const domain::NewRecognition&) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<domain::RecognitionRecord> finalize(
        const domain::FinalizeRecognition&) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<std::vector<domain::RecognitionRecord>> failInterruptedOnStartup(
        domain::UtcTimePoint) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<domain::PageResult<domain::RecognitionRecord>> queryHistory(
        const domain::HistoryFilter& filter,
        const domain::PageRequest& page) override {
        if (filter.deviceId() && filter.deviceId()->value() == "db-unavailable") {
            return domain::RepositoryFailure::unavailable;
        }
        if (filter.deviceId() && filter.deviceId()->value() == "internal") {
            return domain::RepositoryFailure::internal;
        }
        std::vector<domain::RecognitionRecord> filtered;
        for (const auto& record : history_) {
            const auto captured = record.snapshot().capturedAt();
            if (captured >= filter.startInclusiveUtc() &&
                captured < filter.endExclusiveUtc() &&
                (!filter.deviceId() || record.snapshot().deviceId() == *filter.deviceId())) {
                filtered.push_back(record);
            }
        }
        const auto total = static_cast<std::uint64_t>(filtered.size());
        if (page.page() != 1U) {
            filtered.clear();
        }
        return domain::PageResult<domain::RecognitionRecord>(
            std::move(filtered), page, total);
    }

    domain::RepositoryResult<std::unique_ptr<domain::IHistoryCursor>> openHistoryCursor(
        const domain::HistoryFilter& filter) override {
        const auto device = filter.deviceId() ? filter.deviceId()->value() : std::string{};
        if (device == "db-unavailable") {
            return domain::RepositoryFailure::unavailable;
        }
        if (device == "internal") {
            return domain::RepositoryFailure::internal;
        }
        std::vector<domain::RecognitionRecord> records;
        if (device == "mid-fail") {
            records = history_;
        } else if (device != "empty") {
            for (const auto& record : history_) {
                const auto captured = record.snapshot().capturedAt();
                if (captured >= filter.startInclusiveUtc() &&
                    captured < filter.endExclusiveUtc() &&
                    (!filter.deviceId() || record.snapshot().deviceId() == *filter.deviceId())) {
                    records.push_back(record);
                }
            }
        }
        std::unique_ptr<domain::IHistoryCursor> cursor = std::make_unique<VectorCursor>(
            std::move(records), device == "mid-fail" ? std::optional<std::size_t>(1U) :
                                                        std::nullopt);
        return cursor;
    }

    domain::RepositoryResult<domain::HistoryCursorResult> visitHistory(
        const domain::HistoryFilter&,
        const domain::HistoryVisitor&) override {
        return domain::RepositoryFailure::internal;
    }

private:
    std::vector<domain::RecognitionRecord> history_;
    std::vector<domain::RecognitionRecord> records_;
};

class FakeImageStorage final : public domain::IImageStorage {
public:
    domain::StorageResult<domain::StoredImage> saveAtomically(
        const domain::SaveImageCommand&) override {
        return domain::StorageFailure::internal;
    }

    domain::StorageResult<domain::ImageFile> openForRead(
        const domain::RelativeImagePath& path) override {
        const auto id = path.value().substr(path.value().find_last_of('/') + 1U, 36U);
        if (id == uuidText(6U)) {
            return domain::StorageFailure::notFound;
        }
        if (id == uuidText(7U)) {
            return domain::StorageFailure::readFailed;
        }
        const auto numeric = id == uuidText(2U) ? 2U :
                             id == uuidText(4U) ? 4U :
                             id == uuidText(5U) ? 5U :
                             id == uuidText(8U) ? 8U : 1U;
        const domain::ImageMime mime = numeric == 2U || numeric == 4U ?
                                           domain::ImageMime::png : domain::ImageMime::jpeg;
        std::vector<std::uint8_t> bytes = numeric == 2U ?
                                              std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U} :
                                              std::vector<std::uint8_t>{9U, 8U, 7U, 6U};
        if (numeric == 5U) {
            bytes.pop_back();
        }
        const auto failAt = numeric == 8U ? std::optional<std::size_t>(2U) : std::nullopt;
        return domain::ImageFile(
            std::make_unique<MemoryReader>(bytes, failAt),
            mime,
            static_cast<std::uint64_t>(bytes.size()));
    }

    void removeBestEffort(const domain::RelativeImagePath&) noexcept override {}
};

class FakeAuthService final : public IAuthService {
public:
    FakeAuthService()
        : clientId_(domain::Uuid::parse("11111111-2222-4333-8444-555555555555")),
          expiresAt_(protocolTime("2099-08-23T20:30:00.000+08:00")) {}

    LoginResult login(std::string_view, std::string_view, const domain::Uuid&) override {
        return AuthFailure::internal;
    }

    AuthenticationResult authenticate(const std::string_view token) override {
        if (token == "expired") {
            return AuthFailure::tokenExpired;
        }
        if (token != kValidToken) {
            return AuthFailure::tokenInvalid;
        }
        return Authenticated{AuthSession{1U, "演示管理员", clientId_, expiresAt_, expiresAt_}};
    }

    OperationResult logout(std::string_view) override { return AuthFailure::internal; }
    OperationResult heartbeat(std::string_view, const domain::Uuid&) override {
        return AuthFailure::internal;
    }

private:
    domain::Uuid clientId_;
    domain::UtcTimePoint expiresAt_;
};

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

struct ParsedResponse final {
    int status = 0;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
    bool complete = false;
};

ParsedResponse parseResponse(const std::string& raw) {
    ParsedResponse response;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return response;
    }
    std::istringstream headers(raw.substr(0U, headerEnd));
    std::string statusLine;
    std::getline(headers, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    status >> version >> response.status;
    std::string line;
    while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers[lowercase(line.substr(0U, colon))] =
                trim(line.substr(colon + 1U));
        }
    }
    const std::string wireBody = raw.substr(headerEnd + 4U);
    const auto length = response.headers.find("content-length");
    if (length != response.headers.end()) {
        const auto expected = static_cast<std::size_t>(std::stoull(length->second));
        response.complete = wireBody.size() >= expected;
        response.body = wireBody.substr(0U, std::min(wireBody.size(), expected));
        return response;
    }
    const auto transfer = response.headers.find("transfer-encoding");
    if (transfer == response.headers.end() || lowercase(transfer->second) != "chunked") {
        response.body = wireBody;
        response.complete = true;
        return response;
    }
    std::size_t position = 0U;
    while (position < wireBody.size()) {
        const auto lineEnd = wireBody.find("\r\n", position);
        if (lineEnd == std::string::npos) {
            return response;
        }
        std::size_t chunkSize = 0U;
        std::istringstream sizeInput(wireBody.substr(position, lineEnd - position));
        sizeInput >> std::hex >> chunkSize;
        if (!sizeInput) {
            return response;
        }
        position = lineEnd + 2U;
        if (chunkSize == 0U) {
            response.complete = wireBody.size() >= position + 2U &&
                                wireBody.compare(position, 2U, "\r\n") == 0;
            return response;
        }
        if (wireBody.size() < position + chunkSize + 2U) {
            return response;
        }
        response.body.append(wireBody, position, chunkSize);
        position += chunkSize;
        if (wireBody.compare(position, 2U, "\r\n") != 0) {
            return response;
        }
        position += 2U;
    }
    return response;
}

std::string socketExchange(const std::uint16_t port, const std::string& request) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(socket, asio::buffer(request));
    socket.non_blocking(true);
    std::string raw;
    std::array<char, 16384> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("socket response timed out");
            }
            std::this_thread::sleep_for(2ms);
            continue;
        }
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            break;
        }
        if (error) {
            throw std::runtime_error("socket response failed");
        }
    }
    return raw;
}

std::string getRequest(
    const std::string& target,
    const std::string_view token = kValidToken) {
    return "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " +
           std::string(token) + "\r\nConnection: close\r\n\r\n";
}

nlohmann::json jsonBody(const ParsedResponse& response) {
    return nlohmann::json::parse(response.body);
}

void expectFailure(
    const ParsedResponse& response,
    const int status,
    const std::string_view code) {
    ASSERT_EQ(response.status, status);
    ASSERT_TRUE(response.complete);
    ASSERT_EQ(response.headers.at("content-type"), "application/json");
    const auto body = jsonBody(response);
    EXPECT_EQ(body.size(), 5U);
    EXPECT_FALSE(body.at("success").get<bool>());
    EXPECT_EQ(body.at("code"), code);
    EXPECT_TRUE(body.at("data").is_null());
}

class HistoryApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<http::server::HttpServer>(
            0U, http::middleware::makeRandomRequestIdGenerator());
        controller_ = std::make_unique<http::controllers::recognition::RecognitionController>(
            auth_, history_, csv_);
        controller_->registerRoutes(*server_);
        thread_ = std::thread([this] { server_->run(); });
        server_->waitUntilStarted();
        port_ = server_->port();
        ASSERT_NE(port_, 0U);
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ParsedResponse call(
        const std::string& target,
        const std::string_view token = kValidToken) const {
        return parseResponse(socketExchange(port_, getRequest(target, token)));
    }

    FakeRecognitionRepository repository_;
    FakeImageStorage storage_;
    FakeAuthService auth_;
    services::history::HistoryService history_{repository_, storage_};
    services::csv::CsvService csv_{repository_};
    std::unique_ptr<http::server::HttpServer> server_;
    std::unique_ptr<http::controllers::recognition::RecognitionController> controller_;
    std::thread thread_;
    std::uint16_t port_ = 0U;
};

TEST_F(HistoryApiTest, AuthenticationPrecedesPathAndQueryDecoding) {
    expectFailure(
        call("/api/v1/recognitions/not-a-uuid?unknown=1", "invalid"),
        401,
        "AUTH_TOKEN_INVALID");
    expectFailure(
        call("/api/v1/recognitions?bad", "expired"),
        401,
        "AUTH_TOKEN_EXPIRED");
    expectFailure(
        call("/api/v1/recognitions/not-a-uuid?unknown=1"),
        400,
        "INVALID_REQUEST");
    expectFailure(
        call("/api/v1/recognitions/not-a-uuid"),
        400,
        "INVALID_REQUEST");
    expectFailure(
        call("/api/v1/recognitions/" + uuidText(1U) + "/image?download=1"),
        400,
        "INVALID_REQUEST");
}

TEST_F(HistoryApiTest, DetailAndHistoryReturnExactQtJsonWithinBudget) {
    auto started = std::chrono::steady_clock::now();
    const auto detail = call("/api/v1/recognitions/" + uuidText(2U));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_EQ(detail.status, 200);
    const auto detailBody = jsonBody(detail);
    ASSERT_EQ(detailBody.size(), 5U);
    const auto& data = detailBody.at("data");
    EXPECT_EQ(data.size(), 12U);
    EXPECT_EQ(data.at("recognitionId"), uuidText(2U));
    EXPECT_EQ(data.at("status"), "SUCCEEDED");
    EXPECT_EQ(data.at("plateNumber"), "京A12345");
    EXPECT_TRUE(data.at("errorCode").is_null());

    const std::string query =
        "/api/v1/recognitions?startTime=2026-08-15T12:30:00.000+08:00&"
        "endTime=2026-08-15T12:33:00.000%2B08%3A00&page=1&pageSize=100";
    started = std::chrono::steady_clock::now();
    const auto history = call(query);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_EQ(history.status, 200);
    const auto page = jsonBody(history).at("data");
    EXPECT_EQ(page.size(), 4U);
    EXPECT_EQ(page.at("page"), 1U);
    EXPECT_EQ(page.at("pageSize"), 100U);
    EXPECT_EQ(page.at("total"), 3U);
    ASSERT_EQ(page.at("items").size(), 3U);
    EXPECT_EQ(page.at("items").at(0).at("recognitionId"), uuidText(3U));

    const auto beyond = call(
        "/api/v1/recognitions?startTime=2026-08-15T12:30:00.000+08:00&"
        "endTime=2026-08-15T12:33:00.000+08:00&page=2&pageSize=100");
    ASSERT_EQ(beyond.status, 200);
    EXPECT_TRUE(jsonBody(beyond).at("data").at("items").empty());
    EXPECT_EQ(jsonBody(beyond).at("data").at("page"), 2U);

    expectFailure(call("/api/v1/recognitions/" + uuidText(89U)), 404,
                  "RECOGNITION_NOT_FOUND");
    expectFailure(call("/api/v1/recognitions/" + uuidText(90U)), 503,
                  "DATABASE_UNAVAILABLE");

    expectFailure(
        call(
            "/api/v1/recognitions?startTime=2026-08-15T12:30:00.000+08:00&"
            "endTime=2026-08-15T12:33:00.000+08:00&page=1&pageSize=50"),
        400,
        "INVALID_REQUEST");
    expectFailure(
        call(
            "/api/v1/recognitions?startTime=2026-08-15T12:30:00.000+08:00&"
            "startTime=2026-08-15T12:30:00.000+08:00&"
            "endTime=2026-08-15T12:33:00.000+08:00&page=1&pageSize=100"),
        400,
        "INVALID_REQUEST");
    expectFailure(
        call(
            "/api/v1/recognitions?startTime=2026-08-15T12:30:00.000+08:00&"
            "endTime=2026-08-15T12:33:00.000+08:00&page=1&pageSize=100&unknown=1"),
        400,
        "INVALID_REQUEST");
}

TEST_F(HistoryApiTest, ImageStreamsExactMimeAndMapsPreHeaderFailures) {
    auto started = std::chrono::steady_clock::now();
    const auto jpeg = call("/api/v1/recognitions/" + uuidText(1U) + "/image");
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
    ASSERT_EQ(jpeg.status, 200);
    EXPECT_TRUE(jpeg.complete);
    EXPECT_EQ(jpeg.headers.at("content-type"), "image/jpeg");
    EXPECT_EQ(jpeg.body, std::string("\x09\x08\x07\x06", 4U));

    const auto png = call("/api/v1/recognitions/" + uuidText(2U) + "/image");
    ASSERT_EQ(png.status, 200);
    EXPECT_EQ(png.headers.at("content-type"), "image/png");
    EXPECT_EQ(png.body, std::string("\x01\x02\x03\x04\x05", 5U));

    expectFailure(call("/api/v1/recognitions/" + uuidText(6U) + "/image"), 404,
                  "IMAGE_NOT_FOUND");
    expectFailure(call("/api/v1/recognitions/" + uuidText(89U) + "/image"), 404,
                  "IMAGE_NOT_FOUND");
    expectFailure(call("/api/v1/recognitions/" + uuidText(4U) + "/image"), 500,
                  "INTERNAL_ERROR");
    expectFailure(call("/api/v1/recognitions/" + uuidText(5U) + "/image"), 500,
                  "INTERNAL_ERROR");
    expectFailure(call("/api/v1/recognitions/" + uuidText(7U) + "/image"), 500,
                  "INTERNAL_ERROR");

    const auto interrupted = call("/api/v1/recognitions/" + uuidText(8U) + "/image");
    EXPECT_EQ(interrupted.status, 200);
    EXPECT_FALSE(interrupted.complete);
    EXPECT_EQ(interrupted.body, std::string("\x09\x08", 2U));
}

TEST_F(HistoryApiTest, CsvUsesExactBytesAndSeparatesPreHeaderFromMidStreamFailures) {
    const std::string base =
        "/api/v1/recognitions/export?startTime=2026-08-15T12:30:00.000+08:00&"
        "endTime=2026-08-15T12:33:00.000+08:00";
    auto started = std::chrono::steady_clock::now();
    const auto csv = call(base);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 120s);
    ASSERT_EQ(csv.status, 200);
    ASSERT_TRUE(csv.complete);
    EXPECT_EQ(csv.headers.at("content-type"), "text/csv; charset=utf-8");
    const std::string expected =
        "\xEF\xBB\xBF记录ID,设备,车牌号,状态,错误码,错误信息,拍摄时间,开始时间,完成时间,耗时(ms)\r\n" +
        uuidText(3U) +
        ",device-002,,FAILED,MODEL_INFERENCE_ERROR,\"失败,含\"\"引号\"\"\r\n换行\","
        "2026-08-15T12:32:44.000+08:00,2026-08-15T12:32:45.000+08:00,"
        "2026-08-15T12:32:45.321+08:00,321\r\n" +
        uuidText(2U) +
        ",device-001,京A12345,SUCCEEDED,,,2026-08-15T12:31:44.000+08:00,"
        "2026-08-15T12:31:45.000+08:00,2026-08-15T12:31:45.120+08:00,120\r\n" +
        uuidText(1U) +
        ",device-001,,PROCESSING,,,2026-08-15T12:30:44.000+08:00,"
        "2026-08-15T12:30:45.000+08:00,,\r\n";
    EXPECT_EQ(csv.body, expected);

    const auto deviceCsv = call(base + "&deviceId=device-001");
    ASSERT_EQ(deviceCsv.status, 200);
    ASSERT_TRUE(deviceCsv.complete);
    EXPECT_EQ(deviceCsv.body.find(uuidText(3U)), std::string::npos);
    const auto succeededPosition = deviceCsv.body.find(uuidText(2U));
    const auto processingPosition = deviceCsv.body.find(uuidText(1U));
    ASSERT_NE(succeededPosition, std::string::npos);
    ASSERT_NE(processingPosition, std::string::npos);
    EXPECT_LT(succeededPosition, processingPosition);

    const auto empty = call(base + "&deviceId=empty");
    ASSERT_EQ(empty.status, 200);
    EXPECT_TRUE(empty.complete);
    EXPECT_EQ(
        empty.body,
        "\xEF\xBB\xBF记录ID,设备,车牌号,状态,错误码,错误信息,拍摄时间,开始时间,完成时间,耗时(ms)\r\n");

    expectFailure(call(base + "&deviceId=db-unavailable"), 503, "DATABASE_UNAVAILABLE");
    expectFailure(call(base + "&deviceId=internal"), 500, "INTERNAL_ERROR");

    const auto interrupted = call(base + "&deviceId=mid-fail");
    EXPECT_EQ(interrupted.status, 200);
    EXPECT_FALSE(interrupted.complete);
    EXPECT_TRUE(interrupted.body.rfind("\xEF\xBB\xBF", 0U) == 0U);
    EXPECT_NE(interrupted.body.find(uuidText(3U)), std::string::npos);
    EXPECT_EQ(interrupted.body.find(uuidText(2U)), std::string::npos);
}

TEST(CsvSerializationTest, AppliesOnlyNecessaryRfc4180Quoting) {
    EXPECT_EQ(serialization::csv::escapeField("plain"), "plain");
    EXPECT_EQ(serialization::csv::escapeField("a,b"), "\"a,b\"");
    EXPECT_EQ(serialization::csv::escapeField("a\"b"), "\"a\"\"b\"");
    EXPECT_EQ(serialization::csv::escapeField("a\r\nb"), "\"a\r\nb\"");
}

}  // namespace
}  // namespace ocrservice
