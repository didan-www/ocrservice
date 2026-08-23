#include "ApplicationRuntime.h"

#include <stdexcept>

namespace ocrservice::app::runtime {

ApplicationRuntime::ApplicationRuntime(IApplicationStages& stages) noexcept : stages_(stages) {}

ApplicationRuntime::~ApplicationRuntime() { stop(); }

void ApplicationRuntime::start() {
    if (running_ || stopped_) {
        throw std::logic_error("application runtime cannot be started in its current state");
    }
    try {
        stages_.loadConfig();
        stages_.createBootstrapLogger();
        bootstrapLoggerCreated_ = true;
        stages_.prepareRoots();
        stages_.createFinalLogger();
        finalLoggerCreated_ = true;
        stages_.createWorkerRuntimes();
        stages_.createConnectionPool();
        poolCreated_ = true;
        stages_.migrate();
        stages_.recoverInterrupted();
        stages_.assembleApplication();
        applicationAssembled_ = true;
        stages_.startWorkers();
        workersStarted_ = true;
        stages_.startHttp();
        httpStarted_ = true;
        stages_.waitForHttpReady();
        stages_.startMqtt();
        mqttStarted_ = true;
        running_ = true;
    } catch (...) {
        rollback();
        throw;
    }
}

void ApplicationRuntime::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    if (applicationAssembled_) {
        stages_.stopAcceptanceAndWait();
    }
    if (httpStarted_) {
        stages_.stopHttp();
        stages_.joinHttp();
    }
    if (workersStarted_) {
        stages_.requestQueueStop();
        stages_.joinWorkers();
    }
    if (mqttStarted_) {
        stages_.stopMqtt();
    }
    if (poolCreated_) {
        stages_.closeConnectionPool();
    }
    if (finalLoggerCreated_ || bootstrapLoggerCreated_) {
        stages_.flushLoggers();
    }
    running_ = false;
}

bool ApplicationRuntime::isRunning() const noexcept { return running_; }

void ApplicationRuntime::rollback() noexcept {
    if (mqttStarted_) {
        stages_.stopMqtt();
    }
    if (httpStarted_) {
        stages_.stopHttp();
        stages_.joinHttp();
    }
    if (workersStarted_) {
        stages_.requestQueueStop();
        stages_.joinWorkers();
    }
    if (applicationAssembled_) {
        stages_.stopAcceptanceAndWait();
    }
    if (poolCreated_) {
        stages_.closeConnectionPool();
    }
    if (finalLoggerCreated_ || bootstrapLoggerCreated_) {
        stages_.flushLoggers();
    }
    stopped_ = true;
    running_ = false;
}

}  // namespace ocrservice::app::runtime
