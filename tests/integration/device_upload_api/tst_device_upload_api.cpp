#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "DeviceRecognitionController.h"
#include "HttpServer.h"
#include "PosixImageStorage.h"
#include "ProtocolTime.h"
#include "RecognitionAcceptanceService.h"
#include "RequestId.h"

namespace ocrservice {
namespace {

using namespace std::chrono_literals;
using services::recognition::acceptance::AcceptanceFailure;
using services::recognition::acceptance::AcceptanceSucceeded;
using services::recognition::acceptance::AuthorizedDevice;
using services::recognition::acceptance::OpenCvUploadImageValidator;
using services::recognition::acceptance::RecognitionAcceptanceService;

constexpr std::string_view kToken = "device-http-001";
constexpr std::string_view kDisabledToken = "device-http-disabled";
constexpr std::string_view kDeviceId = "device-001";
constexpr std::string_view kCaptureId = "11111111-2222-4333-8444-555555555555";
constexpr std::string_view kCapturedAt = "2026-08-23T10:20:30.123+08:00";
constexpr std::string_view kBoundary = "task017-boundary";

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto pattern =
            (std::filesystem::temp_directory_path() / "ocrservice-upload-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* const created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("temporary directory creation failed");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

domain::Sha256Digest sha256(const std::string_view value) {
    std::array<std::uint8_t, 32> digest{};
    unsigned int length = 0U;
    if (EVP_Digest(
            value.data(),
            value.size(),
            digest.data(),
            &length,
            EVP_sha256(),
            nullptr) != 1 ||
        length != static_cast<unsigned int>(digest.size())) {
        throw std::runtime_error("test digest failed");
    }
    return domain::Sha256Digest(digest);
}

domain::RecognitionId recognitionId(const std::uint64_t sequence) {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0U; index < sizeof(sequence); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes[15U - index] = static_cast<std::uint8_t>((sequence >> shift) & 0xFFU);
    }
    return domain::RecognitionId(domain::Uuid::v4(bytes));
}

domain::RecognitionRecord processingRecord(const domain::NewRecognition& value) {
    return domain::RecognitionRecord(
        domain::RecognitionSnapshot(
            value.recognitionId(),
            1U,
            value.deviceId(),
            domain::RecognitionStatus::processing,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            value.capturedAtUtc(),
            value.startedAtUtc(),
            std::nullopt,
            std::nullopt),
        value.captureId(),
        value.imageSha256(),
        value.relativeImagePath(),
        value.imageMime(),
        value.imageSizeBytes());
}

class FakeDeviceRepository final : public domain::IDeviceRepository {
public:
    void add(const std::string_view token, const std::string_view deviceId, const bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto digest = sha256(token);
        records_.emplace(
            digest.toHex(),
            domain::DeviceRecord(
                domain::DeviceId::parse(deviceId),
                "测试设备",
                digest,
                std::string(deviceId),
                enabled));
    }

    void failWith(const std::optional<domain::RepositoryFailure> failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = failure;
    }

    domain::RepositoryResult<std::optional<domain::DeviceRecord>> findByHttpTokenHash(
        const domain::Sha256Digest& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++lookupCalls_;
        if (failure_) {
            return *failure_;
        }
        const auto found = records_.find(tokenHash.toHex());
        return found == records_.end() ? std::optional<domain::DeviceRecord>{} :
                                         std::optional<domain::DeviceRecord>(found->second);
    }

    std::size_t lookupCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lookupCalls_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, domain::DeviceRecord, std::less<>> records_;
    std::optional<domain::RepositoryFailure> failure_;
    std::size_t lookupCalls_ = 0U;
};

class FakeRecognitionRepository final : public domain::IRecognitionRepository {
public:
    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> findByCapture(
        const domain::DeviceId& deviceId,
        const domain::CaptureId& captureId) override {
        if (blockFind_.load(std::memory_order_acquire)) {
            findEntered_.store(true, std::memory_order_release);
            while (blockFind_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        ++findByCaptureCalls_;
        if (barrierTarget_ != 0U && barrierArrivals_ < barrierTarget_) {
            ++barrierArrivals_;
            changed_.notify_all();
            changed_.wait(lock, [this] { return barrierArrivals_ >= barrierTarget_; });
            return std::optional<domain::RecognitionRecord>{};
        }
        if (findFailure_) {
            return *findFailure_;
        }
        const auto found = byCapture_.find(captureKey(deviceId, captureId));
        return found == byCapture_.end() ? std::optional<domain::RecognitionRecord>{} :
                                          std::optional<domain::RecognitionRecord>(found->second);
    }

    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> findById(
        const domain::RecognitionId& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = byId_.find(id.toString());
        return found == byId_.end() ? std::optional<domain::RecognitionRecord>{} :
                                     std::optional<domain::RecognitionRecord>(found->second);
    }

    domain::RepositoryResult<domain::RecognitionRecord> insertProcessing(
        const domain::NewRecognition& recognition) override {
        if (blockInsert_.load(std::memory_order_acquire)) {
            insertEntered_.store(true, std::memory_order_release);
            while (blockInsert_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++insertCalls_;
        if (throwOnInsert_) {
            throw std::runtime_error("synthetic insert failure");
        }
        if (insertFailure_) {
            return *insertFailure_;
        }
        if (primaryKeyConflictOnce_) {
            primaryKeyConflictOnce_ = false;
            const auto collisionCapture = domain::CaptureId::parse(
                "99999999-9999-4999-8999-999999999999");
            auto collision = processingRecord(domain::NewRecognition(
                recognition.recognitionId(),
                recognition.deviceId(),
                collisionCapture,
                recognition.imageSha256(),
                recognition.relativeImagePath(),
                recognition.imageMime(),
                recognition.imageSizeBytes(),
                recognition.capturedAtUtc(),
                recognition.startedAtUtc()));
            byId_.insert_or_assign(recognition.recognitionId().toString(), collision);
            ++insertConflicts_;
            return domain::RepositoryFailure::conflict;
        }
        const auto capture = captureKey(recognition.deviceId(), recognition.captureId());
        if (byCapture_.find(capture) != byCapture_.end() ||
            byId_.find(recognition.recognitionId().toString()) != byId_.end()) {
            ++insertConflicts_;
            return domain::RepositoryFailure::conflict;
        }
        auto record = processingRecord(recognition);
        byCapture_.emplace(capture, record);
        byId_.emplace(recognition.recognitionId().toString(), record);
        return record;
    }

    domain::RepositoryResult<domain::RecognitionRecord> finalize(
        const domain::FinalizeRecognition& recognition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = byId_.find(recognition.recognitionId.toString());
        if (found == byId_.end()) {
            return domain::RepositoryFailure::notFound;
        }
        const auto& current = found->second;
        const bool succeeded = recognition.outcome.isSuccess();
        auto snapshot = domain::RecognitionSnapshot(
            current.snapshot().recognitionId(),
            2U,
            current.snapshot().deviceId(),
            succeeded ? domain::RecognitionStatus::succeeded : domain::RecognitionStatus::failed,
            succeeded ? std::optional<domain::PlateNumber>(
                            recognition.outcome.plateNumber()) : std::nullopt,
            succeeded ? std::nullopt : std::optional<domain::RecognitionFailureCode>(
                                               domain::toRecognitionFailureCode(
                                                   recognition.outcome.failureCode())),
            succeeded ? std::nullopt : std::optional<std::string>("识别失败"),
            current.snapshot().capturedAt(),
            current.snapshot().startedAt(),
            recognition.completedAtUtc,
            recognition.durationMs);
        domain::RecognitionRecord finalRecord(
            std::move(snapshot),
            current.captureId(),
            current.imageSha256(),
            current.relativeImagePath(),
            current.imageMime(),
            current.imageSizeBytes());
        const auto capture = captureKey(current.snapshot().deviceId(), current.captureId());
        byCapture_.insert_or_assign(capture, finalRecord);
        found->second = finalRecord;
        return finalRecord;
    }

    domain::RepositoryResult<std::vector<domain::RecognitionRecord>> failInterruptedOnStartup(
        domain::UtcTimePoint) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<domain::PageResult<domain::RecognitionRecord>> queryHistory(
        const domain::HistoryFilter&,
        const domain::PageRequest&) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<std::unique_ptr<domain::IHistoryCursor>> openHistoryCursor(
        const domain::HistoryFilter&) override {
        return domain::RepositoryFailure::internal;
    }

    domain::RepositoryResult<domain::HistoryCursorResult> visitHistory(
        const domain::HistoryFilter&,
        const domain::HistoryVisitor&) override {
        return domain::RepositoryFailure::internal;
    }

    void setInsertFailure(const std::optional<domain::RepositoryFailure> failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        insertFailure_ = failure;
    }

    void setThrowOnInsert(const bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        throwOnInsert_ = enabled;
    }

    void conflictNextInsertOnRecognitionId() {
        std::lock_guard<std::mutex> lock(mutex_);
        primaryKeyConflictOnce_ = true;
    }

    void setFindFailure(const std::optional<domain::RepositoryFailure> failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        findFailure_ = failure;
    }

    void setInitialMissBarrier(const std::size_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        barrierTarget_ = target;
        barrierArrivals_ = 0U;
    }

    void blockFind() {
        findEntered_.store(false, std::memory_order_relaxed);
        blockFind_.store(true, std::memory_order_release);
    }

    void waitUntilFindEntered() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!findEntered_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_TRUE(findEntered_.load(std::memory_order_acquire));
    }

    void releaseFind() {
        blockFind_.store(false, std::memory_order_release);
    }

    void blockInsert() {
        insertEntered_.store(false, std::memory_order_relaxed);
        blockInsert_.store(true, std::memory_order_release);
    }

    void waitUntilInsertEntered() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!insertEntered_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_TRUE(insertEntered_.load(std::memory_order_acquire));
    }

    void releaseInsert() {
        blockInsert_.store(false, std::memory_order_release);
    }

    std::optional<domain::RecognitionRecord> recordById(
        const domain::RecognitionId& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = byId_.find(id.toString());
        return found == byId_.end() ? std::nullopt :
                                     std::optional<domain::RecognitionRecord>(found->second);
    }

    std::size_t insertCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return insertCalls_;
    }

    std::size_t insertConflicts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return insertConflicts_;
    }

    std::size_t recordCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return byId_.size();
    }

private:
    static std::string captureKey(
        const domain::DeviceId& deviceId,
        const domain::CaptureId& captureId) {
        return deviceId.value() + "|" + captureId.toString();
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, domain::RecognitionRecord, std::less<>> byCapture_;
    std::map<std::string, domain::RecognitionRecord, std::less<>> byId_;
    std::optional<domain::RepositoryFailure> insertFailure_;
    std::optional<domain::RepositoryFailure> findFailure_;
    std::size_t insertCalls_ = 0U;
    std::size_t insertConflicts_ = 0U;
    std::size_t findByCaptureCalls_ = 0U;
    std::size_t barrierTarget_ = 0U;
    std::size_t barrierArrivals_ = 0U;
    std::atomic<bool> blockFind_{false};
    std::atomic<bool> findEntered_{false};
    bool throwOnInsert_ = false;
    bool primaryKeyConflictOnce_ = false;
    std::atomic<bool> blockInsert_{false};
    std::atomic<bool> insertEntered_{false};
};

class TrackingImageStorage final : public domain::IImageStorage {
public:
    explicit TrackingImageStorage(const std::filesystem::path& root) : delegate_(root) {}

    domain::StorageResult<domain::StoredImage> saveAtomically(
        const domain::SaveImageCommand& command) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++saveCalls_;
        if (failure_) {
            return *failure_;
        }
        return delegate_.saveAtomically(command);
    }

    domain::StorageResult<domain::ImageFile> openForRead(
        const domain::RelativeImagePath& path) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return delegate_.openForRead(path);
    }

    void removeBestEffort(const domain::RelativeImagePath& path) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++removeCalls_;
        delegate_.removeBestEffort(path);
    }

    void failWith(const std::optional<domain::StorageFailure> failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = failure;
    }

    std::size_t saveCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return saveCalls_;
    }

    std::size_t removeCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return removeCalls_;
    }

private:
    mutable std::mutex mutex_;
    storage::PosixImageStorage delegate_;
    std::optional<domain::StorageFailure> failure_;
    std::size_t saveCalls_ = 0U;
    std::size_t removeCalls_ = 0U;
};

enum class PublishMode { accept, reject, throwException };

class FakeMqttPublisher final : public domain::IMqttPublisher {
public:
    FakeMqttPublisher(
        FakeRecognitionRepository& recognitions,
        TrackingImageStorage& images)
        : recognitions_(recognitions), images_(images) {}

    domain::PublishAttempt publishManagement(
        const domain::RecognitionSnapshot& snapshot) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++publishCalls_;
        const auto record = recognitions_.recordById(snapshot.recognitionId());
        if (record) {
            auto opened = images_.openForRead(record->relativeImagePath());
            observedCommittedAndReadable_ =
                std::holds_alternative<domain::ImageFile>(opened);
        }
        if (mode_ == PublishMode::throwException) {
            throw std::runtime_error("synthetic publisher failure");
        }
        return mode_ == PublishMode::reject ?
                   domain::PublishAttempt::rejected(domain::PublishFailure::notConnected) :
                   domain::PublishAttempt::accepted();
    }

    domain::PublishAttempt publishDeviceFinal(
        const domain::RecognitionSnapshot&,
        domain::GateAction) override {
        return domain::PublishAttempt::accepted();
    }

    void setMode(const PublishMode mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
    }

    std::size_t publishCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return publishCalls_;
    }

    bool observedCommittedAndReadable() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return observedCommittedAndReadable_;
    }

private:
    FakeRecognitionRepository& recognitions_;
    TrackingImageStorage& images_;
    mutable std::mutex mutex_;
    PublishMode mode_ = PublishMode::accept;
    std::size_t publishCalls_ = 0U;
    bool observedCommittedAndReadable_ = false;
};

class SequenceIdGenerator final
    : public services::recognition::acceptance::IRecognitionIdGenerator {
public:
    std::optional<domain::RecognitionId> next() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queued_.empty()) {
            auto result = queued_.front();
            queued_.pop_front();
            return result;
        }
        return recognitionId(next_++);
    }

    void push(const domain::RecognitionId& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        queued_.push_back(id);
    }

private:
    std::mutex mutex_;
    std::deque<domain::RecognitionId> queued_;
    std::uint64_t next_ = 1U;
};

class FixedClock final : public services::recognition::acceptance::IAcceptanceClock {
public:
    domain::UtcTimePoint nowUtc() override {
        return serialization::time::parseProtocolTime("2026-08-23T10:20:31.000+08:00");
    }
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
};

ParsedResponse parseResponse(const std::string& raw) {
    ParsedResponse response;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return response;
    }
    std::istringstream input(raw.substr(0U, headerEnd));
    std::string statusLine;
    std::getline(input, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    status >> version >> response.status;
    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers[lowercase(line.substr(0U, colon))] =
                trim(line.substr(colon + 1U));
        }
    }
    response.body = raw.substr(headerEnd + 4U);
    return response;
}

ParsedResponse exchange(
    const std::uint16_t port,
    const std::string& target,
    const std::string& body,
    const std::optional<std::string_view> token = kToken,
    const std::string_view contentType = "multipart/form-data; boundary=task017-boundary") {
    std::string request = "POST " + target + " HTTP/1.1\r\nHost: localhost\r\n";
    request += "Content-Type: " + std::string(contentType) + "\r\n";
    if (token) {
        request += "Authorization: Bearer " + std::string(*token) + "\r\n";
    }
    request += "Content-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n";
    request += body;

    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::error_code writeError;
    asio::write(socket, asio::buffer(request), writeError);
    if (writeError && writeError != asio::error::broken_pipe &&
        writeError != asio::error::connection_reset) {
        throw asio::system_error(writeError);
    }
    std::string raw;
    std::array<char, 16384> buffer{};
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error) {
            break;
        }
    }
    return parseResponse(raw);
}

std::string jpeg(const int blue = 20) {
    cv::Mat image(8, 12, CV_8UC3, cv::Scalar(blue, 40, 80));
    std::vector<unsigned char> bytes;
    if (!cv::imencode(".jpg", image, bytes)) {
        throw std::runtime_error("jpeg fixture encoding failed");
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string readAll(domain::ImageFile& file) {
    std::string result;
    result.resize(static_cast<std::size_t>(file.sizeBytes()));
    std::size_t offset = 0U;
    while (offset < result.size()) {
        const auto count = file.read(
            reinterpret_cast<std::uint8_t*>(result.data()) + offset,
            result.size() - offset);
        if (count == 0U) {
            break;
        }
        offset += count;
    }
    result.resize(offset);
    return result;
}

std::size_t regularFileCount(const std::filesystem::path& root) {
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

std::string multipart(
    const std::string_view image,
    const std::string_view captureId = kCaptureId,
    const std::string_view capturedAt = kCapturedAt,
    const std::string_view boundary = kBoundary) {
    const std::string delimiter = "--" + std::string(boundary);
    std::string body = delimiter +
                       "\r\nContent-Disposition: form-data; name=\"image\"; "
                       "filename=\"capture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    body.append(image.data(), image.size());
    body += "\r\n" + delimiter +
            "\r\nContent-Disposition: form-data; name=\"captureId\"\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n" +
            std::string(captureId);
    body += "\r\n" + delimiter +
            "\r\nContent-Disposition: form-data; name=\"capturedAt\"\r\n\r\n" +
            std::string(capturedAt);
    body += "\r\n" + delimiter + "--\r\n";
    return body;
}

nlohmann::json jsonBody(const ParsedResponse& response) {
    return nlohmann::json::parse(response.body);
}

void expectEnvelope(
    const ParsedResponse& response,
    const int status,
    const std::string_view code) {
    ASSERT_EQ(response.status, status);
    ASSERT_EQ(response.headers.at("content-type"), "application/json");
    const auto body = jsonBody(response);
    EXPECT_EQ(body.size(), 5U);
    EXPECT_EQ(body.at("success"), status == 200 || status == 202);
    EXPECT_EQ(body.at("code"), code);
    EXPECT_TRUE(body.at("message").is_string());
    EXPECT_NO_THROW(domain::Uuid::parse(body.at("requestId").get<std::string>()));
    if (status != 200 && status != 202) {
        EXPECT_TRUE(body.at("data").is_null());
    }
}

class DeviceUploadApiTest : public ::testing::Test {
protected:
    DeviceUploadApiTest()
        : images_(imageRoot_.path()),
          mqtt_(recognitions_, images_),
          queue_(64U),
          acceptance_(devices_, recognitions_, images_, mqtt_, queue_, ids_, clock_) {}

    void SetUp() override {
        devices_.add(kToken, kDeviceId, true);
        devices_.add(kDisabledToken, "device-disabled", false);
        server_ = std::make_unique<http::server::HttpServer>(
            0U, http::middleware::makeRandomRequestIdGenerator());
        controller_ =
            std::make_unique<http::controllers::device_recognition::DeviceRecognitionController>(
                acceptance_, validator_);
        controller_->registerRoutes(*server_);
        thread_ = std::thread([this] { server_->run(); });
        server_->waitUntilStarted();
        port_ = server_->port();
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        acceptance_.stopAcceptingAndWait();
        queue_.requestStop();
    }

    ParsedResponse upload(
        const std::string& body,
        const std::string& target = "/api/v1/devices/device-001/recognitions",
        const std::optional<std::string_view> token = kToken,
        const std::string_view contentType =
            "multipart/form-data; boundary=task017-boundary") const {
        return exchange(port_, target, body, token, contentType);
    }

    TemporaryDirectory imageRoot_;
    FakeDeviceRepository devices_;
    FakeRecognitionRepository recognitions_;
    TrackingImageStorage images_;
    FakeMqttPublisher mqtt_;
    queue::RecognitionTaskQueue queue_;
    SequenceIdGenerator ids_;
    FixedClock clock_;
    RecognitionAcceptanceService acceptance_;
    OpenCvUploadImageValidator validator_;
    std::unique_ptr<http::server::HttpServer> server_;
    std::unique_ptr<http::controllers::device_recognition::DeviceRecognitionController>
        controller_;
    std::thread thread_;
    std::uint16_t port_ = 0U;
};

TEST_F(DeviceUploadApiTest, FirstUploadIsExact202AndIdempotentAcrossProcessingAndFinal) {
    const auto body = multipart(jpeg());
    const auto first = upload(body);
    expectEnvelope(first, 202, "OK");
    const auto firstData = jsonBody(first).at("data");
    EXPECT_EQ(firstData.size(), 3U);
    EXPECT_EQ(firstData.at("captureId"), kCaptureId);
    EXPECT_EQ(firstData.at("status"), "PROCESSING");
    const auto id = domain::RecognitionId::parse(firstData.at("recognitionId").get<std::string>());
    EXPECT_EQ(recognitions_.recordCount(), 1U);
    EXPECT_EQ(recognitions_.insertCalls(), 1U);
    EXPECT_EQ(images_.saveCalls(), 1U);
    EXPECT_EQ(mqtt_.publishCalls(), 1U);
    EXPECT_TRUE(mqtt_.observedCommittedAndReadable());
    EXPECT_EQ(queue_.queueDepth(), 1U);

    const auto duplicate = upload(body);
    expectEnvelope(duplicate, 202, "OK");
    EXPECT_EQ(jsonBody(duplicate).at("data").at("recognitionId"), id.toString());
    EXPECT_EQ(recognitions_.insertCalls(), 1U);
    EXPECT_EQ(images_.saveCalls(), 1U);
    EXPECT_EQ(mqtt_.publishCalls(), 1U);
    EXPECT_EQ(queue_.queueDepth(), 1U);

    const auto completedAt =
        serialization::time::parseProtocolTime("2026-08-23T10:20:32.000+08:00");
    auto finalized = recognitions_.finalize(domain::FinalizeRecognition{
        id,
        domain::RecognitionOutcome::succeeded(domain::PlateNumber::parse("A12345")),
        completedAt,
        1000U});
    ASSERT_TRUE(std::holds_alternative<domain::RecognitionRecord>(finalized));
    const auto finalDuplicate = upload(body);
    expectEnvelope(finalDuplicate, 200, "OK");
    EXPECT_EQ(jsonBody(finalDuplicate).at("data").at("recognitionId"), id.toString());
    EXPECT_EQ(jsonBody(finalDuplicate).at("data").at("status"), "SUCCEEDED");

    const auto queued = queue_.take();
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(queued->recognitionId(), id);
    EXPECT_EQ(queue_.queueDepth(), 0U);
}

TEST_F(DeviceUploadApiTest, AuthenticationPathAndQueryUseFixedPriority) {
    const auto body = multipart(jpeg());
    expectEnvelope(upload(body, "/api/v1/devices/device-001/recognitions", std::nullopt),
                   401,
                   "DEVICE_UNAUTHORIZED");
    const std::string oversizedToken(257U, 'x');
    expectEnvelope(upload(body, "/api/v1/devices/device-001/recognitions", oversizedToken),
                   401,
                   "DEVICE_UNAUTHORIZED");
    expectEnvelope(upload(body, "/api/v1/devices/device-001/recognitions", "unknown-token"),
                   401,
                   "DEVICE_UNAUTHORIZED");
    const auto lookupsBeforeComma = devices_.lookupCalls();
    expectEnvelope(
        upload(body, "/api/v1/devices/device-001/recognitions", "invalid,token"),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(devices_.lookupCalls(), lookupsBeforeComma);
    const auto commaToken = acceptance_.authenticateToken("invalid,token");
    ASSERT_TRUE(std::holds_alternative<AcceptanceFailure>(commaToken));
    EXPECT_EQ(
        std::get<AcceptanceFailure>(commaToken),
        AcceptanceFailure::deviceUnauthorized);
    EXPECT_EQ(devices_.lookupCalls(), lookupsBeforeComma);
    expectEnvelope(upload(body,
                          "/api/v1/devices/device-001/recognitions",
                          kDisabledToken),
                   403,
                   "DEVICE_FORBIDDEN");
    expectEnvelope(upload(body,
                          "/api/v1/devices/device-disabled/recognitions",
                          kDisabledToken),
                   403,
                   "DEVICE_DISABLED");
    expectEnvelope(upload(body, "/api/v1/devices/bad%2Fid/recognitions"),
                   400,
                   "INVALID_REQUEST");
    expectEnvelope(upload(body, "/api/v1/devices/device-001/recognitions?debug=1"),
                   400,
                   "INVALID_REQUEST");

    devices_.failWith(domain::RepositoryFailure::unavailable);
    expectEnvelope(upload(body), 503, "DATABASE_UNAVAILABLE");
    devices_.failWith(domain::RepositoryFailure::internal);
    expectEnvelope(upload(body), 500, "INTERNAL_ERROR");
    EXPECT_EQ(images_.saveCalls(), 0U);
    EXPECT_EQ(recognitions_.insertCalls(), 0U);
}

TEST_F(DeviceUploadApiTest, StrictMultipartRejectsMalformedFormsAndPreservesBoundaryBytes) {
    const auto validJpeg = jpeg();
    const std::string delimiter = "--" + std::string(kBoundary);
    auto duplicatePart = multipart(validJpeg);
    const std::string closing = "\r\n" + delimiter + "--\r\n";
    const auto closingPosition = duplicatePart.rfind(closing);
    ASSERT_NE(closingPosition, std::string::npos);
    duplicatePart.insert(
        closingPosition,
        "\r\n" + delimiter +
            "\r\nContent-Disposition: form-data; name=\"captureId\"\r\n\r\n" +
            std::string(kCaptureId));
    const std::vector<std::string> invalidBodies = {
        "preamble" + multipart(validJpeg),
        multipart(validJpeg) + "epilogue",
        duplicatePart,
        delimiter + "\nContent-Disposition: form-data; name=\"image\"\n\n" +
            validJpeg + "\n" + delimiter + "--\n",
        delimiter +
            "\r\nContent-Disposition: form-data; name=\"image\"\r\n"
            "Content-Disposition: form-data; name=\"image\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n" +
            validJpeg + "\r\n" + delimiter + "--\r\n",
        delimiter +
            "\r\nContent-Disposition: form-data; name=\"image\"; "
            "name=\"image\"; filename=\"capture.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n" +
            validJpeg + "\r\n" + delimiter + "--\r\n",
        delimiter +
            "\r\nContent-Disposition: form-data; name=\"image\"\r\n X-Fold: bad\r\n\r\n" +
            validJpeg + "\r\n" + delimiter + "--\r\n",
        delimiter +
            "\r\nContent-Disposition: form-data; name=\"image\"\r\n"
            "Content-Transfer-Encoding: binary\r\n\r\n" +
            validJpeg + "\r\n" + delimiter + "--\r\n",
        delimiter +
            "\r\nContent-Disposition: form-data; name=\"unknown\"\r\n\r\nx\r\n" +
            delimiter + "--\r\n"};
    for (const auto& body : invalidBodies) {
        expectEnvelope(upload(body), 400, "INVALID_REQUEST");
    }

    auto embeddedBoundary = validJpeg;
    embeddedBoundary += "\r\n--" + std::string(kBoundary) + "X";
    embeddedBoundary += "\r\n--" + std::string(kBoundary) + "--X";
    const auto accepted = upload(multipart(embeddedBoundary));
    expectEnvelope(accepted, 202, "OK");
    EXPECT_EQ(images_.saveCalls(), 1U);
    EXPECT_EQ(queue_.queueDepth(), 1U);
    EXPECT_TRUE(queue_.take().has_value());
}

TEST_F(DeviceUploadApiTest, InvalidCaptureIdAndCapturedAtHaveNoAcceptanceSideEffects) {
    const auto imageBytes = jpeg();
    const std::vector<std::string_view> invalidCaptureIds = {
        "{11111111-2222-4333-8444-555555555555}",
        "11111111222243338444555555555555",
        "not-a-uuid"};
    for (const auto value : invalidCaptureIds) {
        SCOPED_TRACE(value);
        expectEnvelope(upload(multipart(imageBytes, value)), 400, "INVALID_REQUEST");
    }
    const std::vector<std::string_view> invalidCapturedAt = {
        "2026-08-23T10:20:30.123Z",
        "2026-08-23T02:20:30.123+00:00",
        "2026-08-23T10:20:30+08:00"};
    for (const auto value : invalidCapturedAt) {
        SCOPED_TRACE(value);
        expectEnvelope(
            upload(multipart(imageBytes, kCaptureId, value)),
            400,
            "INVALID_REQUEST");
    }
    EXPECT_EQ(images_.saveCalls(), 0U);
    EXPECT_EQ(recognitions_.insertCalls(), 0U);
    EXPECT_EQ(recognitions_.recordCount(), 0U);
    EXPECT_EQ(mqtt_.publishCalls(), 0U);
    EXPECT_EQ(queue_.queueDepth(), 0U);
    EXPECT_EQ(queue_.reservedCount(), 0U);
}

TEST_F(DeviceUploadApiTest, ImageAndRequestSizeFailuresAreDistinct) {
    std::string oversizedImage(storage::kMaximumCompressedImageBytes + 1U, 'x');
    oversizedImage[0] = static_cast<char>(0xFF);
    oversizedImage[1] = static_cast<char>(0xD8);
    oversizedImage[2] = static_cast<char>(0xFF);
    expectEnvelope(upload(multipart(oversizedImage)), 413, "IMAGE_TOO_LARGE");
    expectEnvelope(upload(multipart("not-an-image")), 400, "IMAGE_INVALID");

    std::string oversizedRequest(11U * 1024U * 1024U + 1U, 'x');
    expectEnvelope(upload(oversizedRequest), 413, "REQUEST_TOO_LARGE");
    EXPECT_EQ(recognitions_.insertCalls(), 0U);
    EXPECT_EQ(images_.saveCalls(), 0U);
}

TEST_F(DeviceUploadApiTest, QueueStorageAndDatabaseFailuresLeaveNoAcceptedResidue) {
    std::vector<queue::QueueReservation> reservations;
    reservations.reserve(queue_.capacity());
    for (std::size_t index = 0U; index < queue_.capacity(); ++index) {
        auto reservation = queue_.tryReserve();
        ASSERT_TRUE(reservation.has_value());
        reservations.push_back(std::move(*reservation));
    }
    expectEnvelope(upload(multipart(jpeg())), 503, "RECOGNITION_QUEUE_FULL");
    EXPECT_EQ(images_.saveCalls(), 0U);
    EXPECT_EQ(recognitions_.insertCalls(), 0U);
    reservations.clear();

    images_.failWith(domain::StorageFailure::writeFailed);
    expectEnvelope(upload(multipart(jpeg(),
                                    "11111111-2222-4333-8444-555555555556")),
                   500,
                   "IMAGE_STORAGE_ERROR");
    images_.failWith(domain::StorageFailure::internal);
    expectEnvelope(upload(multipart(jpeg(),
                                    "11111111-2222-4333-8444-555555555557")),
                   500,
                   "INTERNAL_ERROR");
    images_.failWith(std::nullopt);

    recognitions_.setInsertFailure(domain::RepositoryFailure::unavailable);
    expectEnvelope(upload(multipart(jpeg(),
                                    "11111111-2222-4333-8444-555555555558")),
                   503,
                   "DATABASE_UNAVAILABLE");
    EXPECT_GE(images_.removeCalls(), 1U);
    EXPECT_EQ(recognitions_.recordCount(), 0U);
    EXPECT_EQ(queue_.queueDepth(), 0U);
    EXPECT_EQ(queue_.reservedCount(), 0U);
}

TEST_F(DeviceUploadApiTest, RepositoryExceptionRemovesStoredImageAndLeavesNoAcceptedResidue) {
    recognitions_.setThrowOnInsert(true);

    expectEnvelope(upload(multipart(jpeg())), 500, "INTERNAL_ERROR");

    EXPECT_EQ(recognitions_.insertCalls(), 1U);
    EXPECT_EQ(recognitions_.recordCount(), 0U);
    EXPECT_EQ(images_.saveCalls(), 1U);
    EXPECT_EQ(images_.removeCalls(), 1U);
    EXPECT_EQ(mqtt_.publishCalls(), 0U);
    EXPECT_EQ(queue_.queueDepth(), 0U);
    EXPECT_EQ(queue_.reservedCount(), 0U);
}

TEST_F(DeviceUploadApiTest, ExistingFinalPathUsesNextIdWithoutOverwritingOriginalBytes) {
    const auto firstId = recognitionId(700U);
    const auto secondId = recognitionId(701U);
    ids_.push(firstId);
    ids_.push(secondId);
    const auto capturedAt = serialization::time::parseProtocolTime(kCapturedAt);
    const auto originalBytes = jpeg(31);
    auto originalValidation = validator_.validate(originalBytes);
    ASSERT_TRUE(std::holds_alternative<
                services::recognition::acceptance::ValidatedUploadImage>(
        originalValidation));
    const auto& originalImage = std::get<
        services::recognition::acceptance::ValidatedUploadImage>(originalValidation);
    auto originalStored = images_.saveAtomically(domain::SaveImageCommand(
        firstId,
        capturedAt,
        originalImage.format(),
        originalImage.bytes(),
        originalImage.digest()));
    ASSERT_TRUE(std::holds_alternative<domain::StoredImage>(originalStored));
    const auto originalPath =
        std::get<domain::StoredImage>(originalStored).relativePath();

    const auto response = upload(multipart(jpeg(32)));
    expectEnvelope(response, 202, "OK");
    EXPECT_EQ(
        jsonBody(response).at("data").at("recognitionId").get<std::string>(),
        secondId.toString());
    EXPECT_EQ(recognitions_.insertCalls(), 1U);
    EXPECT_EQ(recognitions_.recordCount(), 1U);
    EXPECT_EQ(mqtt_.publishCalls(), 1U);
    EXPECT_EQ(queue_.queueDepth(), 1U);
    EXPECT_EQ(regularFileCount(imageRoot_.path()), 2U);

    auto originalFile = images_.openForRead(originalPath);
    ASSERT_TRUE(std::holds_alternative<domain::ImageFile>(originalFile));
    auto& readable = std::get<domain::ImageFile>(originalFile);
    EXPECT_EQ(readAll(readable), originalBytes);
    const auto task = queue_.take();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->recognitionId(), secondId);
}

TEST_F(DeviceUploadApiTest, EightExistingFinalPathsExhaustCandidatesWithoutResidue) {
    const auto capturedAt = serialization::time::parseProtocolTime(kCapturedAt);
    const auto imageBytes = jpeg(41);
    auto validation = validator_.validate(imageBytes);
    ASSERT_TRUE(std::holds_alternative<
                services::recognition::acceptance::ValidatedUploadImage>(validation));
    const auto& image = std::get<
        services::recognition::acceptance::ValidatedUploadImage>(validation);
    for (std::uint64_t sequence = 800U; sequence < 808U; ++sequence) {
        const auto id = recognitionId(sequence);
        ids_.push(id);
        auto stored = images_.saveAtomically(domain::SaveImageCommand(
            id,
            capturedAt,
            image.format(),
            image.bytes(),
            image.digest()));
        ASSERT_TRUE(std::holds_alternative<domain::StoredImage>(stored));
    }

    expectEnvelope(upload(multipart(imageBytes)), 500, "INTERNAL_ERROR");
    EXPECT_EQ(images_.saveCalls(), 16U);
    EXPECT_EQ(images_.removeCalls(), 0U);
    EXPECT_EQ(regularFileCount(imageRoot_.path()), 8U);
    EXPECT_EQ(recognitions_.insertCalls(), 0U);
    EXPECT_EQ(recognitions_.recordCount(), 0U);
    EXPECT_EQ(mqtt_.publishCalls(), 0U);
    EXPECT_EQ(queue_.queueDepth(), 0U);
    EXPECT_EQ(queue_.reservedCount(), 0U);
}

TEST_F(DeviceUploadApiTest, PrimaryKeyOnlyConflictUsesNextCandidate) {
    const auto firstId = recognitionId(900U);
    const auto secondId = recognitionId(901U);
    ids_.push(firstId);
    ids_.push(secondId);
    recognitions_.conflictNextInsertOnRecognitionId();

    const auto response = upload(multipart(jpeg()));
    expectEnvelope(response, 202, "OK");
    EXPECT_EQ(
        jsonBody(response).at("data").at("recognitionId").get<std::string>(),
        secondId.toString());
    EXPECT_EQ(recognitions_.insertCalls(), 2U);
    EXPECT_EQ(recognitions_.insertConflicts(), 1U);
    EXPECT_EQ(recognitions_.recordCount(), 2U);
    EXPECT_EQ(images_.saveCalls(), 2U);
    EXPECT_EQ(images_.removeCalls(), 1U);
    EXPECT_EQ(regularFileCount(imageRoot_.path()), 1U);
    EXPECT_EQ(mqtt_.publishCalls(), 1U);
    EXPECT_EQ(queue_.queueDepth(), 1U);
    const auto task = queue_.take();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->recognitionId(), secondId);
}

TEST_F(DeviceUploadApiTest, CaptureConflictAndMqttFailuresDoNotCorruptAcceptance) {
    const auto first = upload(multipart(jpeg()));
    expectEnvelope(first, 202, "OK");
    expectEnvelope(upload(multipart(jpeg(21))), 409, "CAPTURE_ID_CONFLICT");
    expectEnvelope(upload(multipart(jpeg(), kCaptureId,
                                    "2026-08-23T10:20:30.124+08:00")),
                   409,
                   "CAPTURE_ID_CONFLICT");
    EXPECT_TRUE(queue_.take().has_value());

    mqtt_.setMode(PublishMode::reject);
    const auto rejected = upload(multipart(
        jpeg(), "11111111-2222-4333-8444-555555555559"));
    expectEnvelope(rejected, 202, "OK");
    EXPECT_TRUE(queue_.take().has_value());

    mqtt_.setMode(PublishMode::throwException);
    const auto threw = upload(multipart(
        jpeg(), "11111111-2222-4333-8444-555555555560"));
    expectEnvelope(threw, 202, "OK");
    EXPECT_TRUE(queue_.take().has_value());
    EXPECT_EQ(recognitions_.recordCount(), 3U);
}

TEST_F(DeviceUploadApiTest, ConcurrentFirstUploadsCreateOneRowFilePublishAndTask) {
    constexpr std::size_t kRequests = 16U;
    recognitions_.setInitialMissBarrier(3U);
    const auto body = multipart(jpeg());
    std::vector<std::future<ParsedResponse>> requests;
    requests.reserve(kRequests);
    for (std::size_t index = 0U; index < kRequests; ++index) {
        requests.push_back(std::async(std::launch::async, [this, body] { return upload(body); }));
    }

    std::optional<std::string> winnerId;
    for (auto& request : requests) {
        ASSERT_EQ(request.wait_for(10s), std::future_status::ready);
        const auto response = request.get();
        expectEnvelope(response, 202, "OK");
        const auto id = jsonBody(response).at("data").at("recognitionId").get<std::string>();
        if (!winnerId) {
            winnerId = id;
        }
        EXPECT_EQ(id, *winnerId);
    }
    EXPECT_EQ(recognitions_.recordCount(), 1U);
    EXPECT_EQ(recognitions_.insertCalls(), 3U);
    EXPECT_EQ(recognitions_.insertConflicts(), 2U);
    EXPECT_EQ(images_.saveCalls(), 3U);
    EXPECT_EQ(images_.removeCalls(), 2U);
    EXPECT_EQ(mqtt_.publishCalls(), 1U);
    EXPECT_EQ(queue_.queueDepth(), 1U);
    const auto task = queue_.take();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->recognitionId().toString(), *winnerId);
    EXPECT_EQ(queue_.reservedCount(), 0U);
}

TEST_F(DeviceUploadApiTest, StopAcceptingWaitsForInflightAndRejectsLaterWork) {
    const auto imageBytes = jpeg();
    auto validated = validator_.validate(imageBytes);
    ASSERT_TRUE(std::holds_alternative<
                services::recognition::acceptance::ValidatedUploadImage>(validated));
    recognitions_.blockFind();
    auto accepting = std::async(std::launch::async, [this, &validated] {
        return acceptance_.acceptUpload(
            AuthorizedDevice(domain::DeviceId::parse(kDeviceId)),
            domain::CaptureId::parse(kCaptureId),
            serialization::time::parseProtocolTime(kCapturedAt),
            std::get<services::recognition::acceptance::ValidatedUploadImage>(validated),
            domain::Uuid::parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
    });
    recognitions_.waitUntilFindEntered();

    auto stopping = std::async(std::launch::async, [this] {
        acceptance_.stopAcceptingAndWait();
    });
    EXPECT_EQ(stopping.wait_for(50ms), std::future_status::timeout);
    recognitions_.releaseFind();
    ASSERT_EQ(accepting.wait_for(2s), std::future_status::ready);
    const auto firstResult = accepting.get();
    ASSERT_TRUE(std::holds_alternative<AcceptanceFailure>(firstResult));
    EXPECT_EQ(std::get<AcceptanceFailure>(firstResult), AcceptanceFailure::serviceUnavailable);
    ASSERT_EQ(stopping.wait_for(2s), std::future_status::ready);
    stopping.get();
    EXPECT_FALSE(acceptance_.isAccepting());

    const auto later = acceptance_.acceptUpload(
        AuthorizedDevice(domain::DeviceId::parse(kDeviceId)),
        domain::CaptureId::parse(kCaptureId),
        serialization::time::parseProtocolTime(kCapturedAt),
        std::get<services::recognition::acceptance::ValidatedUploadImage>(validated),
        domain::Uuid::parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
    ASSERT_TRUE(std::holds_alternative<AcceptanceFailure>(later));
    EXPECT_EQ(std::get<AcceptanceFailure>(later), AcceptanceFailure::serviceUnavailable);
    EXPECT_EQ(queue_.reservedCount(), 0U);
}

TEST_F(DeviceUploadApiTest, StopAcceptingLetsReservedInflightCommitBeforeRejectingNewWork) {
    const auto imageBytes = jpeg();
    auto validated = validator_.validate(imageBytes);
    ASSERT_TRUE(std::holds_alternative<
                services::recognition::acceptance::ValidatedUploadImage>(validated));
    recognitions_.blockInsert();
    auto accepting = std::async(std::launch::async, [this, &validated] {
        return acceptance_.acceptUpload(
            AuthorizedDevice(domain::DeviceId::parse(kDeviceId)),
            domain::CaptureId::parse(kCaptureId),
            serialization::time::parseProtocolTime(kCapturedAt),
            std::get<services::recognition::acceptance::ValidatedUploadImage>(validated),
            domain::Uuid::parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
    });
    recognitions_.waitUntilInsertEntered();
    EXPECT_EQ(queue_.reservedCount(), 1U);
    EXPECT_EQ(images_.saveCalls(), 1U);

    auto stopping = std::async(std::launch::async, [this] {
        acceptance_.stopAcceptingAndWait();
    });
    EXPECT_EQ(stopping.wait_for(50ms), std::future_status::timeout);
    recognitions_.releaseInsert();
    ASSERT_EQ(accepting.wait_for(2s), std::future_status::ready);
    const auto accepted = accepting.get();
    ASSERT_TRUE(std::holds_alternative<AcceptanceSucceeded>(accepted));
    ASSERT_EQ(stopping.wait_for(2s), std::future_status::ready);
    stopping.get();

    EXPECT_EQ(recognitions_.recordCount(), 1U);
    EXPECT_EQ(mqtt_.publishCalls(), 1U);
    EXPECT_EQ(queue_.queueDepth(), 1U);
    EXPECT_EQ(queue_.reservedCount(), 0U);
    expectEnvelope(
        upload(multipart(
            imageBytes,
            "11111111-2222-4333-8444-555555555566")),
        503,
        "SERVICE_UNAVAILABLE");
    const auto task = queue_.take();
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(
        task->recognitionId(),
        std::get<AcceptanceSucceeded>(accepted).recognitionId);
}

}  // namespace
}  // namespace ocrservice
