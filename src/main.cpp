#include <csignal>
#include <pthread.h>

#include "ApplicationRuntime.h"
#include "ProductionApplication.h"

namespace {

bool blockTerminationSignals(sigset_t& signals) noexcept {
    return sigemptyset(&signals) == 0 && sigaddset(&signals, SIGINT) == 0 &&
           sigaddset(&signals, SIGTERM) == 0 &&
           pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
}

}  // namespace

int main(const int argc, char**) {
    if (argc != 1) {
        return 64;
    }

    sigset_t terminationSignals{};
    if (!blockTerminationSignals(terminationSignals)) {
        return 1;
    }

    ocrservice::app::runtime::ProductionApplication application;
    ocrservice::app::runtime::ApplicationRuntime runtime(application);
    try {
        runtime.start();
    } catch (...) {
        application.logStartupFailure();
        return 1;
    }

    int receivedSignal = 0;
    if (sigwait(&terminationSignals, &receivedSignal) != 0) {
        runtime.stop();
        return 1;
    }
    runtime.stop();
    return receivedSignal == SIGINT || receivedSignal == SIGTERM ? 0 : 1;
}
