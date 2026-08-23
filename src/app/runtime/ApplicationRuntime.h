#pragma once

namespace ocrservice::app::runtime {

class IApplicationStages {
public:
    virtual ~IApplicationStages() = default;

    virtual void loadConfig() = 0;
    virtual void createBootstrapLogger() = 0;
    virtual void prepareRoots() = 0;
    virtual void createFinalLogger() = 0;
    virtual void createWorkerRuntimes() = 0;
    virtual void createConnectionPool() = 0;
    virtual void migrate() = 0;
    virtual void recoverInterrupted() = 0;
    virtual void assembleApplication() = 0;
    virtual void startWorkers() = 0;
    virtual void startHttp() = 0;
    virtual void waitForHttpReady() = 0;
    virtual void startMqtt() = 0;

    virtual void stopAcceptanceAndWait() noexcept = 0;
    virtual void stopHttp() noexcept = 0;
    virtual void joinHttp() noexcept = 0;
    virtual void requestQueueStop() noexcept = 0;
    virtual void joinWorkers() noexcept = 0;
    virtual void stopMqtt() noexcept = 0;
    virtual void closeConnectionPool() noexcept = 0;
    virtual void flushLoggers() noexcept = 0;
};

class ApplicationRuntime final {
public:
    explicit ApplicationRuntime(IApplicationStages& stages) noexcept;
    ~ApplicationRuntime();

    ApplicationRuntime(const ApplicationRuntime&) = delete;
    ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

    void start();
    void stop() noexcept;
    bool isRunning() const noexcept;

private:
    void rollback() noexcept;

    IApplicationStages& stages_;
    bool bootstrapLoggerCreated_ = false;
    bool finalLoggerCreated_ = false;
    bool poolCreated_ = false;
    bool applicationAssembled_ = false;
    bool workersStarted_ = false;
    bool httpStarted_ = false;
    bool mqttStarted_ = false;
    bool running_ = false;
    bool stopped_ = false;
};

}  // namespace ocrservice::app::runtime
