#include "ProductionApplication.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "AccessListController.h"
#include "AccessListService.h"
#include "AuthController.h"
#include "AuthService.h"
#include "AuthTypes.h"
#include "BcryptVerifier.h"
#include "ClientController.h"
#include "CsvService.h"
#include "DeviceRecognitionController.h"
#include "HealthController.h"
#include "HealthService.h"
#include "HistoryService.h"
#include "HttpServer.h"
#include "LoggerFactory.h"
#include "MySqlConnectionPool.h"
#include "MySqlMigrator.h"
#include "MySqlRepositories.h"
#include "OnnxPlateRecognizer.h"
#include "OpaqueTokenGenerator.h"
#include "PahoMqttPublisher.h"
#include "PosixImageStorage.h"
#include "RecognitionAcceptanceService.h"
#include "RecognitionController.h"
#include "RecognitionTaskQueue.h"
#include "RecognitionWorker.h"
#include "RecoveryPublisher.h"
#include "ServerConfig.h"

namespace ocrservice::app::runtime {
namespace {

using services::recognition::worker::OpenCvStoredImageDecoder;
using services::recognition::worker::RecognitionWorker;
using services::recognition::worker::SystemWorkerClock;

domain::UtcTimePoint nowUtc() {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return domain::UtcTimePoint(milliseconds.count());
}

void ensureDirectory(const std::filesystem::path& path, const bool create) {
    if (path.empty()) {
        throw std::runtime_error("required directory path is empty");
    }
    std::error_code error;
    auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("required directory could not be inspected");
    }
    if (!std::filesystem::exists(status)) {
        if (!create || !std::filesystem::create_directories(path, error) || error) {
            throw std::runtime_error("required directory could not be created");
        }
        status = std::filesystem::symlink_status(path, error);
    }
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        throw std::runtime_error("required path is not a directory");
    }
}

void verifyWritableDirectory(const std::filesystem::path& path) {
    auto pattern = (path / ".ocrservice-write-check-XXXXXX").string();
    std::vector<char> writablePattern(pattern.begin(), pattern.end());
    writablePattern.push_back('\0');

    const int descriptor = ::mkstemp(writablePattern.data());
    if (descriptor < 0) {
        throw std::runtime_error("required directory is not writable");
    }

    const std::filesystem::path probePath(writablePattern.data());
    if (::unlink(writablePattern.data()) != 0) {
        const int unlinkError = errno;
        ::close(descriptor);
        std::error_code cleanupError;
        std::filesystem::remove(probePath, cleanupError);
        errno = unlinkError;
        throw std::runtime_error("required directory write probe could not be removed");
    }

    constexpr char probeByte = '\0';
    ssize_t bytesWritten = 0;
    do {
        bytesWritten = ::write(descriptor, &probeByte, sizeof(probeByte));
    } while (bytesWritten < 0 && errno == EINTR);

    int syncResult = 0;
    if (bytesWritten == static_cast<ssize_t>(sizeof(probeByte))) {
        do {
            syncResult = ::fsync(descriptor);
        } while (syncResult != 0 && errno == EINTR);
    }
    const int closeResult = ::close(descriptor);
    if (bytesWritten != static_cast<ssize_t>(sizeof(probeByte)) || syncResult != 0 ||
        closeResult != 0) {
        throw std::runtime_error("required directory is not writable");
    }
}

void logNoThrow(
    const std::shared_ptr<logging::JsonLinesLogger>& logger,
    const logging::LogLevel level,
    const char* event,
    const char* code) noexcept {
    if (!logger) {
        return;
    }
    try {
        logger->log(
            level,
            logging::LogEvent("application_runtime", event, code, "system"));
    } catch (...) {
    }
}

class RuntimeHealthSource final : public services::health::IHealthStateSource {
public:
    RuntimeHealthSource(
        const std::atomic<bool>& modelReady,
        repositories::mysql::connection::MySqlConnectionPool& pool,
        mqtt::PahoMqttPublisher& mqttPublisher,
        queue::RecognitionTaskQueue& queue)
        : modelReady_(modelReady),
          pool_(pool),
          mqttPublisher_(mqttPublisher),
          queue_(queue) {}

    bool modelAvailable() const noexcept override { return modelReady_.load(); }
    bool mysqlAvailable() noexcept override { return pool_.ping(); }
    bool mqttConnected() const noexcept override { return mqttPublisher_.isConnected(); }
    std::uint64_t queueDepth() const noexcept override {
        return static_cast<std::uint64_t>(queue_.queueDepth());
    }
    std::uint64_t queueCapacity() const noexcept override {
        return static_cast<std::uint64_t>(queue_.capacity());
    }

private:
    const std::atomic<bool>& modelReady_;
    repositories::mysql::connection::MySqlConnectionPool& pool_;
    mqtt::PahoMqttPublisher& mqttPublisher_;
    queue::RecognitionTaskQueue& queue_;
};

struct WorkerRuntime final {
    WorkerRuntime(
        const config::ServerConfig& config,
        const std::filesystem::path& yolo,
        const std::filesystem::path& lpr)
        : recognizer(
              yolo,
              lpr,
              static_cast<float>(config.model.yoloConfidence),
              static_cast<float>(config.model.yoloNmsIou)) {}

    model::OnnxPlateRecognizer recognizer;
    OpenCvStoredImageDecoder decoder;
    SystemWorkerClock clock;
    std::unique_ptr<RecognitionWorker> worker;
};

}  // namespace

class ProductionApplication::Impl final {
public:
    explicit Impl(ProductionPaths suppliedPaths) : paths(std::move(suppliedPaths)) {}

    ProductionPaths paths;
    std::optional<config::ServerConfig> config;
    std::shared_ptr<logging::JsonLinesLogger> bootstrapLogger;
    std::shared_ptr<logging::JsonLinesLogger> logger;
    std::atomic<bool> modelReady{false};
    std::vector<std::unique_ptr<WorkerRuntime>> workerRuntimes;
    std::unique_ptr<repositories::mysql::connection::MySqlConnectionPool> pool;
    std::unique_ptr<repositories::mysql::repositories::MySqlRecognitionRepository>
        recognitionRepository;
    std::unique_ptr<repositories::mysql::repositories::MySqlAdminUserRepository>
        adminRepository;
    std::unique_ptr<repositories::mysql::repositories::MySqlDeviceRepository>
        deviceRepository;
    std::unique_ptr<repositories::mysql::repositories::MySqlAccessListRepository>
        accessListRepository;
    std::shared_ptr<std::vector<domain::RecognitionRecord>> recovered;
    std::unique_ptr<storage::PosixImageStorage> images;
    std::unique_ptr<queue::RecognitionTaskQueue> queue;
    std::unique_ptr<mqtt::PahoMqttPublisher> mqttPublisher;
    std::unique_ptr<RuntimeHealthSource> healthSource;
    std::unique_ptr<services::health::HealthService> healthService;

    std::unique_ptr<security::BcryptVerifier> passwordVerifier;
    std::unique_ptr<security::OpenSslRandomByteSource> tokenRandom;
    std::unique_ptr<security::OpaqueTokenGenerator> tokenGenerator;
    std::unique_ptr<services::auth::SystemAuthClock> authClock;
    std::unique_ptr<services::auth::SessionStore> sessions;
    std::unique_ptr<services::auth::AuthService> authService;
    std::unique_ptr<services::history::HistoryService> historyService;
    std::unique_ptr<services::csv::CsvService> csvService;
    std::unique_ptr<services::access_list::SystemAccessListClock> accessListClock;
    std::unique_ptr<services::access_list::AccessListService> accessListService;
    std::unique_ptr<services::recognition::acceptance::OpenSslRecognitionIdGenerator>
        recognitionIds;
    std::unique_ptr<services::recognition::acceptance::SystemAcceptanceClock> acceptanceClock;
    std::unique_ptr<services::recognition::acceptance::OpenCvUploadImageValidator>
        uploadValidator;
    std::unique_ptr<services::recognition::acceptance::RecognitionAcceptanceService>
        acceptanceService;

    std::shared_ptr<http::middleware::StructuredAccessLogSink> accessLog;
    std::unique_ptr<http::server::HttpServer> httpServer;
    std::unique_ptr<http::controllers::auth::AuthController> authController;
    std::unique_ptr<http::controllers::client::ClientController> clientController;
    std::unique_ptr<http::controllers::recognition::RecognitionController>
        recognitionController;
    std::unique_ptr<http::controllers::access_list::AccessListController>
        accessListController;
    std::unique_ptr<http::controllers::device_recognition::DeviceRecognitionController>
        deviceController;
    std::unique_ptr<http::controllers::health::HealthController> healthController;

    std::vector<std::thread> workerThreads;
    std::thread httpThread;
};

ProductionApplication::ProductionApplication(ProductionPaths paths)
    : impl_(std::make_unique<Impl>(std::move(paths))) {}

ProductionApplication::~ProductionApplication() {
    stopAcceptanceAndWait();
    stopHttp();
    joinHttp();
    requestQueueStop();
    joinWorkers();
    stopMqtt();
    closeConnectionPool();
    flushLoggers();
}

void ProductionApplication::logStartupFailure() noexcept {
    const auto& activeLogger = impl_->logger ? impl_->logger : impl_->bootstrapLogger;
    logNoThrow(
        activeLogger,
        logging::LogLevel::critical,
        "application_start_failed",
        "STARTUP_FAILED");
}

void ProductionApplication::loadConfig() {
    impl_->config = config::ServerConfigLoader::loadFromProcess(impl_->paths.configFile);
}

void ProductionApplication::createBootstrapLogger() {
    impl_->bootstrapLogger = logging::LoggerFactory::createConsole("ocrservice-bootstrap");
}

void ProductionApplication::prepareRoots() {
    if (!impl_->config) {
        throw std::logic_error("configuration is not loaded");
    }
    ensureDirectory(impl_->config->storage.imageRoot, true);
    verifyWritableDirectory(impl_->config->storage.imageRoot);
    ensureDirectory(impl_->config->storage.logRoot, true);
    ensureDirectory(impl_->config->storage.modelRoot, false);
}

void ProductionApplication::createFinalLogger() {
    impl_->logger = logging::LoggerFactory::createRotatingFile(
        "ocrservice", impl_->config->storage.logRoot);
    impl_->logger->log(
        logging::LogLevel::info,
        logging::LogEvent(
            "application_runtime",
            "configuration_loaded",
            "OK",
            "system",
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            config::ServerConfigLoader::summarize(*impl_->config)));
}

void ProductionApplication::createWorkerRuntimes() {
    const auto yolo = impl_->config->storage.modelRoot / "yolov8_plate.onnx";
    const auto lpr = impl_->config->storage.modelRoot / "lprnet.onnx";
    impl_->workerRuntimes.reserve(impl_->config->recognition.workers);
    for (std::uint32_t index = 0U; index < impl_->config->recognition.workers; ++index) {
        impl_->workerRuntimes.push_back(
            std::make_unique<WorkerRuntime>(*impl_->config, yolo, lpr));
    }
    impl_->modelReady.store(true);
}

void ProductionApplication::createConnectionPool() {
    impl_->pool =
        std::make_unique<repositories::mysql::connection::MySqlConnectionPool>(
            impl_->config->mysql);
}

void ProductionApplication::migrate() {
    repositories::mysql::migration::MySqlMigrator migrator(
        *impl_->pool, impl_->paths.migrations);
    migrator.migrate();
}

void ProductionApplication::recoverInterrupted() {
    impl_->recognitionRepository =
        std::make_unique<repositories::mysql::repositories::MySqlRecognitionRepository>(
            *impl_->pool);
    auto result = impl_->recognitionRepository->failInterruptedOnStartup(nowUtc());
    auto* records = std::get_if<std::vector<domain::RecognitionRecord>>(&result);
    if (records == nullptr) {
        throw std::runtime_error("startup recovery failed");
    }
    impl_->recovered = std::make_shared<std::vector<domain::RecognitionRecord>>(
        std::move(*records));
}

void ProductionApplication::assembleApplication() {
    auto& cfg = *impl_->config;
    impl_->adminRepository =
        std::make_unique<repositories::mysql::repositories::MySqlAdminUserRepository>(
            *impl_->pool);
    impl_->deviceRepository =
        std::make_unique<repositories::mysql::repositories::MySqlDeviceRepository>(
            *impl_->pool);
    impl_->accessListRepository =
        std::make_unique<repositories::mysql::repositories::MySqlAccessListRepository>(
            *impl_->pool);
    impl_->images = std::make_unique<storage::PosixImageStorage>(cfg.storage.imageRoot);
    impl_->queue = std::make_unique<queue::RecognitionTaskQueue>(
        static_cast<std::size_t>(cfg.recognition.queueCapacity));

    const auto recovered = impl_->recovered;
    const auto logger = impl_->logger;
    impl_->mqttPublisher = std::make_unique<mqtt::PahoMqttPublisher>(
        cfg.mqtt,
        impl_->logger,
        [recovered, logger](domain::IMqttPublisher& publisher) noexcept {
            publishRecoveredOnce(*recovered, publisher, logger);
        });

    impl_->passwordVerifier = std::make_unique<security::BcryptVerifier>();
    impl_->tokenRandom = std::make_unique<security::OpenSslRandomByteSource>();
    impl_->tokenGenerator =
        std::make_unique<security::OpaqueTokenGenerator>(*impl_->tokenRandom);
    impl_->authClock = std::make_unique<services::auth::SystemAuthClock>();
    impl_->sessions = std::make_unique<services::auth::SessionStore>();
    impl_->authService = std::make_unique<services::auth::AuthService>(
        *impl_->adminRepository,
        *impl_->passwordVerifier,
        *impl_->tokenGenerator,
        *impl_->authClock,
        *impl_->sessions,
        std::chrono::seconds(cfg.session.tokenTtlSeconds));
    impl_->historyService = std::make_unique<services::history::HistoryService>(
        *impl_->recognitionRepository, *impl_->images);
    impl_->csvService = std::make_unique<services::csv::CsvService>(
        *impl_->recognitionRepository);
    impl_->accessListClock =
        std::make_unique<services::access_list::SystemAccessListClock>();
    impl_->accessListService = std::make_unique<services::access_list::AccessListService>(
        *impl_->accessListRepository, *impl_->accessListClock);
    impl_->recognitionIds = std::make_unique<
        services::recognition::acceptance::OpenSslRecognitionIdGenerator>();
    impl_->acceptanceClock =
        std::make_unique<services::recognition::acceptance::SystemAcceptanceClock>();
    impl_->uploadValidator =
        std::make_unique<services::recognition::acceptance::OpenCvUploadImageValidator>();
    impl_->acceptanceService = std::make_unique<
        services::recognition::acceptance::RecognitionAcceptanceService>(
        *impl_->deviceRepository,
        *impl_->recognitionRepository,
        *impl_->images,
        *impl_->mqttPublisher,
        *impl_->queue,
        *impl_->recognitionIds,
        *impl_->acceptanceClock,
        impl_->logger);

    impl_->healthSource = std::make_unique<RuntimeHealthSource>(
        impl_->modelReady, *impl_->pool, *impl_->mqttPublisher, *impl_->queue);
    impl_->healthService =
        std::make_unique<services::health::HealthService>(*impl_->healthSource);

    for (auto& runtime : impl_->workerRuntimes) {
        runtime->worker = std::make_unique<RecognitionWorker>(
            *impl_->queue,
            *impl_->recognitionRepository,
            *impl_->images,
            runtime->recognizer,
            *impl_->mqttPublisher,
            runtime->clock,
            runtime->decoder,
            impl_->logger);
    }

    impl_->accessLog =
        std::make_shared<http::middleware::StructuredAccessLogSink>(impl_->logger);
    impl_->httpServer = std::make_unique<http::server::HttpServer>(
        cfg.http.port,
        http::middleware::makeRandomRequestIdGenerator(),
        impl_->accessLog);
    impl_->authController =
        std::make_unique<http::controllers::auth::AuthController>(
            *impl_->authService, cfg.mqtt);
    impl_->clientController =
        std::make_unique<http::controllers::client::ClientController>(*impl_->authService);
    impl_->recognitionController =
        std::make_unique<http::controllers::recognition::RecognitionController>(
            *impl_->authService, *impl_->historyService, *impl_->csvService);
    impl_->accessListController =
        std::make_unique<http::controllers::access_list::AccessListController>(
            *impl_->authService, *impl_->accessListService);
    impl_->deviceController =
        std::make_unique<http::controllers::device_recognition::DeviceRecognitionController>(
            *impl_->acceptanceService, *impl_->uploadValidator);
    impl_->healthController =
        std::make_unique<http::controllers::health::HealthController>(
            *impl_->healthService);

    impl_->authController->registerRoutes(*impl_->httpServer);
    impl_->clientController->registerRoutes(*impl_->httpServer);
    impl_->recognitionController->registerRoutes(*impl_->httpServer);
    impl_->accessListController->registerRoutes(*impl_->httpServer);
    impl_->deviceController->registerRoutes(*impl_->httpServer);
    impl_->healthController->registerRoutes(*impl_->httpServer);
}

void ProductionApplication::startWorkers() {
    impl_->workerThreads.reserve(impl_->workerRuntimes.size());
    try {
        for (const auto& runtime : impl_->workerRuntimes) {
            impl_->workerThreads.emplace_back([worker = runtime->worker.get()] { worker->run(); });
        }
    } catch (...) {
        impl_->queue->requestStop();
        joinWorkers();
        throw;
    }
}

void ProductionApplication::startHttp() {
    impl_->httpThread = std::thread([this] {
        try {
            impl_->httpServer->run();
        } catch (...) {
            logNoThrow(
                impl_->logger,
                logging::LogLevel::error,
                "http_server_failed",
                "HTTP_SERVER_FAILED");
        }
    });
}

void ProductionApplication::waitForHttpReady() {
    impl_->httpServer->waitUntilStarted();
}

void ProductionApplication::startMqtt() {
    try {
        impl_->mqttPublisher->start();
    } catch (...) {
        impl_->mqttPublisher->stop();
        throw;
    }
}

void ProductionApplication::stopAcceptanceAndWait() noexcept {
    if (impl_->acceptanceService) {
        try {
            impl_->acceptanceService->stopAcceptingAndWait();
        } catch (...) {
        }
    }
}

void ProductionApplication::stopHttp() noexcept {
    if (impl_->httpServer) {
        impl_->httpServer->stop();
    }
}

void ProductionApplication::joinHttp() noexcept {
    if (impl_->httpThread.joinable()) {
        impl_->httpThread.join();
    }
}

void ProductionApplication::requestQueueStop() noexcept {
    if (impl_->queue) {
        impl_->queue->requestStop();
    }
}

void ProductionApplication::joinWorkers() noexcept {
    for (auto& thread : impl_->workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    impl_->workerThreads.clear();
}

void ProductionApplication::stopMqtt() noexcept {
    if (impl_->mqttPublisher) {
        impl_->mqttPublisher->stop();
    }
}

void ProductionApplication::closeConnectionPool() noexcept {
    if (impl_->pool) {
        impl_->pool->close();
    }
}

void ProductionApplication::flushLoggers() noexcept {
    if (impl_->logger) {
        try {
            impl_->logger->flush();
        } catch (...) {
        }
    }
    if (impl_->bootstrapLogger) {
        try {
            impl_->bootstrapLogger->flush();
        } catch (...) {
        }
    }
}

}  // namespace ocrservice::app::runtime
