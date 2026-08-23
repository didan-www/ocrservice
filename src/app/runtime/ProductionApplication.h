#pragma once

#include <filesystem>
#include <memory>

#include "ApplicationRuntime.h"

namespace ocrservice::app::runtime {

struct ProductionPaths final {
    std::filesystem::path configFile = "/app/config/server.json";
    std::filesystem::path migrations = "/app/migrations";
};

class ProductionApplication final : public IApplicationStages {
public:
    explicit ProductionApplication(ProductionPaths paths = {});
    ~ProductionApplication() override;

    ProductionApplication(const ProductionApplication&) = delete;
    ProductionApplication& operator=(const ProductionApplication&) = delete;

    void logStartupFailure() noexcept;

    void loadConfig() override;
    void createBootstrapLogger() override;
    void prepareRoots() override;
    void createFinalLogger() override;
    void createWorkerRuntimes() override;
    void createConnectionPool() override;
    void migrate() override;
    void recoverInterrupted() override;
    void assembleApplication() override;
    void startWorkers() override;
    void startHttp() override;
    void waitForHttpReady() override;
    void startMqtt() override;
    void stopAcceptanceAndWait() noexcept override;
    void stopHttp() noexcept override;
    void joinHttp() noexcept override;
    void requestQueueStop() noexcept override;
    void joinWorkers() noexcept override;
    void stopMqtt() noexcept override;
    void closeConnectionPool() noexcept override;
    void flushLoggers() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ocrservice::app::runtime
