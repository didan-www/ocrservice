#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "ApplicationRuntime.h"
#include "HealthController.h"
#include "HealthService.h"
#include "HttpServer.h"
#include "ProductionApplication.h"
#include "RecoveryPublisher.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::app::runtime::ApplicationRuntime;
using ocrservice::app::runtime::IApplicationStages;
using ocrservice::domain::CaptureId;
using ocrservice::domain::DeviceId;
using ocrservice::domain::GateAction;
using ocrservice::domain::ImageMime;
using ocrservice::domain::PublishAttempt;
using ocrservice::domain::PublishFailure;
using ocrservice::domain::RecognitionFailureCode;
using ocrservice::domain::RecognitionId;
using ocrservice::domain::RecognitionRecord;
using ocrservice::domain::RecognitionSnapshot;
using ocrservice::domain::RecognitionStatus;
using ocrservice::domain::Sha256Digest;
using ocrservice::domain::UtcTimePoint;
using ocrservice::domain::Uuid;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "ocrservice-application-lifecycle-XXXXXX")
                           .string();
        std::vector<char> writablePattern(pattern.begin(), pattern.end());
        writablePattern.push_back('\0');
        const char* const created = ::mkdtemp(writablePattern.data());
        if (created == nullptr) {
            throw std::runtime_error("temporary directory could not be created");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class ScopedEnvironment final {
public:
    ScopedEnvironment(std::string name, std::optional<std::string> value)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) {
            original_ = existing;
        }
        const int result = value ? ::setenv(name_.c_str(), value->c_str(), 1) :
                                   ::unsetenv(name_.c_str());
        if (result != 0) {
            throw std::runtime_error("test environment could not be configured");
        }
    }

    ~ScopedEnvironment() {
        if (original_) {
            ::setenv(name_.c_str(), original_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    std::optional<std::string> original_;
};

void writeConfig(
    const std::filesystem::path& path,
    const std::filesystem::path& imageRoot,
    const std::filesystem::path& logRoot,
    const std::filesystem::path& modelRoot) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("test configuration could not be created");
    }
    output << nlohmann::json{
        {"mqttPublicHost", "127.0.0.1"},
        {"imageRoot", imageRoot.string()},
        {"logRoot", logRoot.string()},
        {"modelRoot", modelRoot.string()},
    };
    if (!output) {
        throw std::runtime_error("test configuration could not be written");
    }
}

class ProductionEnvironment final {
public:
    ProductionEnvironment()
        : mysqlPassword_("MYSQL_PASSWORD", "test-mysql-password"),
          mqttServerPassword_("MQTT_SERVER_PASSWORD", "test-server-password"),
          mqttManagementPassword_(
              "MQTT_MANAGEMENT_PASSWORD", "test-management-password"),
          imageRoot_("IMAGE_ROOT", std::nullopt),
          logRoot_("LOG_ROOT", std::nullopt),
          modelRoot_("MODEL_ROOT", std::nullopt) {}

private:
    ScopedEnvironment mysqlPassword_;
    ScopedEnvironment mqttServerPassword_;
    ScopedEnvironment mqttManagementPassword_;
    ScopedEnvironment imageRoot_;
    ScopedEnvironment logRoot_;
    ScopedEnvironment modelRoot_;
};

class FakeStages final : public IApplicationStages {
public:
    explicit FakeStages(std::string failure = {}) : failure_(std::move(failure)) {}

#define START_STAGE(method) \
    void method() override { stage(#method); }
    START_STAGE(loadConfig)
    START_STAGE(createBootstrapLogger)
    START_STAGE(prepareRoots)
    START_STAGE(createFinalLogger)
    START_STAGE(createWorkerRuntimes)
    START_STAGE(createConnectionPool)
    START_STAGE(migrate)
    START_STAGE(recoverInterrupted)
    START_STAGE(assembleApplication)
    START_STAGE(startWorkers)
    START_STAGE(startHttp)
    START_STAGE(waitForHttpReady)
    START_STAGE(startMqtt)
#undef START_STAGE

#define STOP_STAGE(method) \
    void method() noexcept override { events.push_back(#method); }
    STOP_STAGE(stopAcceptanceAndWait)
    STOP_STAGE(stopHttp)
    STOP_STAGE(joinHttp)
    STOP_STAGE(requestQueueStop)
    STOP_STAGE(joinWorkers)
    STOP_STAGE(stopMqtt)
    STOP_STAGE(closeConnectionPool)
    STOP_STAGE(flushLoggers)
#undef STOP_STAGE

    std::vector<std::string> events;

private:
    void stage(const char* name) {
        events.emplace_back(name);
        if (failure_ == name) {
            throw std::runtime_error("injected startup failure");
        }
    }

    std::string failure_;
};

const std::vector<std::string> kStartup = {
    "loadConfig",
    "createBootstrapLogger",
    "prepareRoots",
    "createFinalLogger",
    "createWorkerRuntimes",
    "createConnectionPool",
    "migrate",
    "recoverInterrupted",
    "assembleApplication",
    "startWorkers",
    "startHttp",
    "waitForHttpReady",
    "startMqtt"};

TEST(ApplicationRuntimeTest, StartsAndStopsInExactRequiredOrder) {
    FakeStages stages;
    ApplicationRuntime runtime(stages);
    runtime.start();
    EXPECT_TRUE(runtime.isRunning());
    runtime.stop();
    runtime.stop();

    auto expected = kStartup;
    const std::vector<std::string> shutdown = {
        "stopAcceptanceAndWait",
        "stopHttp",
        "joinHttp",
        "requestQueueStop",
        "joinWorkers",
        "stopMqtt",
        "closeConnectionPool",
        "flushLoggers"};
    expected.insert(expected.end(), shutdown.begin(), shutdown.end());
    EXPECT_EQ(stages.events, expected);
    EXPECT_FALSE(runtime.isRunning());
}

TEST(ApplicationRuntimeTest, EveryStartupFailureRollsBackOnlyCompletedStages) {
    for (std::size_t failureIndex = 0U; failureIndex < kStartup.size(); ++failureIndex) {
        FakeStages stages(kStartup[failureIndex]);
        ApplicationRuntime runtime(stages);
        EXPECT_THROW(runtime.start(), std::runtime_error) << kStartup[failureIndex];

        std::vector<std::string> expected(
            kStartup.begin(), kStartup.begin() + static_cast<std::ptrdiff_t>(failureIndex + 1U));
        const bool bootstrap = failureIndex > 1U;
        const bool finalLogger = failureIndex > 3U;
        const bool pool = failureIndex > 5U;
        const bool assembled = failureIndex > 8U;
        const bool workers = failureIndex > 9U;
        const bool http = failureIndex > 10U;
        const bool mqtt = failureIndex > 12U;
        if (mqtt) {
            expected.push_back("stopMqtt");
        }
        if (http) {
            expected.push_back("stopHttp");
            expected.push_back("joinHttp");
        }
        if (workers) {
            expected.push_back("requestQueueStop");
            expected.push_back("joinWorkers");
        }
        if (assembled) {
            expected.push_back("stopAcceptanceAndWait");
        }
        if (pool) {
            expected.push_back("closeConnectionPool");
        }
        if (bootstrap || finalLogger) {
            expected.push_back("flushLoggers");
        }
        EXPECT_EQ(stages.events, expected) << kStartup[failureIndex];
        EXPECT_FALSE(runtime.isRunning());
        runtime.stop();
        EXPECT_EQ(stages.events, expected) << kStartup[failureIndex];
    }
}

TEST(ProductionApplicationTest, PrepareRootsCreatesWritableRootsWithoutProbeResidue) {
    ProductionEnvironment environment;
    TemporaryDirectory temporary;
    const auto imageRoot = temporary.path() / "images";
    const auto logRoot = temporary.path() / "logs";
    const auto modelRoot = temporary.path() / "models";
    ASSERT_TRUE(std::filesystem::create_directory(modelRoot));
    const auto configFile = temporary.path() / "server.json";
    writeConfig(configFile, imageRoot, logRoot, modelRoot);

    ocrservice::app::runtime::ProductionApplication application({configFile, {}});
    application.loadConfig();
    application.createBootstrapLogger();
    ASSERT_NO_THROW(application.prepareRoots());
    EXPECT_TRUE(std::filesystem::is_directory(imageRoot));
    EXPECT_TRUE(std::filesystem::is_empty(imageRoot));
    EXPECT_TRUE(std::filesystem::is_directory(logRoot));
    EXPECT_NO_THROW(application.createFinalLogger());
    application.flushLoggers();
    EXPECT_TRUE(std::filesystem::is_regular_file(logRoot / "ocrservice.jsonl"));
}

TEST(ProductionApplicationTest, PrepareRootsRejectsImageRootThatCannotCreateFiles) {
    ProductionEnvironment environment;
    TemporaryDirectory temporary;
    const auto logRoot = temporary.path() / "logs";
    const auto modelRoot = temporary.path() / "models";
    ASSERT_TRUE(std::filesystem::create_directory(modelRoot));
    const auto configFile = temporary.path() / "server.json";
    writeConfig(configFile, "/proc", logRoot, modelRoot);

    ocrservice::app::runtime::ProductionApplication application({configFile, {}});
    application.loadConfig();
    application.createBootstrapLogger();
    EXPECT_THROW(application.prepareRoots(), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(logRoot));
}

class FakeHealthSource final : public ocrservice::services::health::IHealthStateSource {
public:
    bool model = true;
    bool mysql = true;
    bool mqtt = true;
    std::uint64_t depth = 3U;
    std::uint64_t capacity = 20U;

    bool modelAvailable() const noexcept override { return model; }
    bool mysqlAvailable() noexcept override {
        ++mysqlProbes;
        return mysql;
    }
    bool mqttConnected() const noexcept override { return mqtt; }
    std::uint64_t queueDepth() const noexcept override { return depth; }
    std::uint64_t queueCapacity() const noexcept override { return capacity; }

    std::size_t mysqlProbes = 0U;
};

TEST(HealthServiceTest, ProducesOnlyValidUpAndDegradedSuccessStates) {
    FakeHealthSource source;
    ocrservice::services::health::HealthService service(source);

    auto up = service.check();
    ASSERT_TRUE(std::holds_alternative<ocrservice::serialization::json::HealthData>(up));
    EXPECT_EQ(
        std::get<ocrservice::serialization::json::HealthData>(up).status,
        ocrservice::serialization::json::HealthStatus::up);

    source.mqtt = false;
    auto degraded = service.check();
    ASSERT_TRUE(std::holds_alternative<ocrservice::serialization::json::HealthData>(degraded));
    const auto& data = std::get<ocrservice::serialization::json::HealthData>(degraded);
    EXPECT_EQ(data.status, ocrservice::serialization::json::HealthStatus::degraded);
    EXPECT_EQ(data.mqtt, ocrservice::serialization::json::ComponentStatus::down);
    EXPECT_EQ(source.mysqlProbes, 2U);
}

TEST(HealthServiceTest, ModelOrMysqlDownReturnsFailureWithoutPublicDownDto) {
    FakeHealthSource source;
    ocrservice::services::health::HealthService service(source);

    source.model = false;
    EXPECT_TRUE(std::holds_alternative<ocrservice::services::health::HealthFailure>(
        service.check()));
    EXPECT_EQ(source.mysqlProbes, 0U);

    source.model = true;
    source.mysql = false;
    EXPECT_TRUE(std::holds_alternative<ocrservice::services::health::HealthFailure>(
        service.check()));
    EXPECT_EQ(source.mysqlProbes, 1U);
}

class FixedRequestIds final : public ocrservice::http::middleware::IRequestIdGenerator {
public:
    Uuid next() override {
        std::array<std::uint8_t, 16> bytes{};
        bytes[6] = 0x40U;
        bytes[8] = 0x80U;
        return Uuid::v4(bytes);
    }
};

std::uint16_t reservePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw std::runtime_error("bind failed");
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        throw std::runtime_error("getsockname failed");
    }
    ::close(fd);
    return ntohs(address.sin_port);
}

std::string get(const std::uint16_t port, const std::string& path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw std::runtime_error("connect failed");
    }
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (::send(fd, request.data(), request.size(), 0) < 0) {
        ::close(fd);
        throw std::runtime_error("send failed");
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(fd);
    return response;
}

nlohmann::json responseJson(const std::string& response) {
    const auto separator = response.find("\r\n\r\n");
    if (separator == std::string::npos) {
        throw std::runtime_error("invalid HTTP response");
    }
    return nlohmann::json::parse(response.substr(separator + 4U));
}

TEST(HealthControllerTest, RealHttpRouteIsUnauthenticatedAndUsesExactEnvelopes) {
    FakeHealthSource source;
    source.mqtt = false;
    ocrservice::services::health::HealthService service(source);
    ocrservice::http::controllers::health::HealthController controller(service);
    const auto port = reservePort();
    ocrservice::http::server::HttpServer server(
        port, std::make_shared<FixedRequestIds>());
    controller.registerRoutes(server);
    std::thread running([&server] { server.run(); });
    server.waitUntilStarted();

    const auto degraded = get(port, "/health");
    EXPECT_NE(degraded.find("HTTP/1.1 200"), std::string::npos);
    const auto degradedJson = responseJson(degraded);
    EXPECT_EQ(degradedJson.at("success"), true);
    EXPECT_EQ(degradedJson.at("data").at("status"), "DEGRADED");
    EXPECT_EQ(degradedJson.at("data").size(), 6U);

    source.mysql = false;
    const auto failed = get(port, "/health");
    EXPECT_NE(failed.find("HTTP/1.1 503"), std::string::npos);
    const auto failedJson = responseJson(failed);
    const nlohmann::json expectedFailure = {
        {"success", false},
        {"code", "SERVICE_UNAVAILABLE"},
        {"message", "服务暂不可用"},
        {"requestId", "00000000-0000-4000-8000-000000000000"},
        {"data", nullptr},
    };
    EXPECT_EQ(failedJson, expectedFailure);

    server.stop();
    running.join();
}

RecognitionId recognitionId() {
    std::array<std::uint8_t, 16> bytes{};
    bytes[6] = 0x40U;
    bytes[8] = 0x80U;
    bytes[15] = 1U;
    return RecognitionId(Uuid::v4(bytes));
}

RecognitionRecord failedRecord() {
    return RecognitionRecord(
        RecognitionSnapshot(
            recognitionId(),
            2U,
            DeviceId::parse("device-001"),
            RecognitionStatus::failed,
            std::nullopt,
            RecognitionFailureCode::serverRestarted,
            "server restarted",
            UtcTimePoint(1000),
            UtcTimePoint(1100),
            UtcTimePoint(1200),
            100U),
        CaptureId(recognitionId().value()),
        Sha256Digest(std::array<std::uint8_t, 32>{}),
        ocrservice::domain::RelativeImagePath::parseGenerated("2026/08/23/image.jpg"),
        ImageMime::jpeg,
        10U);
}

class RecoveryPublisher final : public ocrservice::domain::IMqttPublisher {
public:
    PublishAttempt publishManagement(const RecognitionSnapshot&) override {
        events.emplace_back("management");
        if (throwManagement) {
            throw std::runtime_error("injected");
        }
        return PublishAttempt::rejected(PublishFailure::notConnected);
    }

    PublishAttempt publishDeviceFinal(const RecognitionSnapshot&, GateAction action) override {
        events.emplace_back("device");
        observedAction = action;
        return PublishAttempt::accepted();
    }

    bool throwManagement = false;
    std::vector<std::string> events;
    std::optional<GateAction> observedAction;
};

TEST(RecoveryPublisherTest, TopicsAreIndependentAndRecordsAreNeverRetried) {
    std::vector<RecognitionRecord> records;
    records.push_back(failedRecord());
    RecoveryPublisher publisher;
    publisher.throwManagement = true;

    ocrservice::app::runtime::publishRecoveredOnce(records, publisher);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(publisher.events, (std::vector<std::string>{"management", "device"}));
    ASSERT_TRUE(publisher.observedAction.has_value());
    EXPECT_EQ(*publisher.observedAction, GateAction::keepClosed);

    ocrservice::app::runtime::publishRecoveredOnce(records, publisher);
    EXPECT_EQ(publisher.events, (std::vector<std::string>{"management", "device"}));
}

}  // namespace
