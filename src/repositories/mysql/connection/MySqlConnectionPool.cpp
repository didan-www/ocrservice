#include "MySqlConnectionPool.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <cppconn/connection.h>
#include <cppconn/exception.h>
#include <cppconn/statement.h>
#include <mysql_driver.h>

namespace ocrservice::repositories::mysql::connection {
namespace {

std::unique_ptr<sql::Connection> connectOnce(const app::config::MySqlConfig& config) {
    sql::ConnectOptionsMap properties;
    properties["hostName"] = config.host;
    properties["port"] = static_cast<int>(config.port);
    properties["userName"] = config.user;
    properties["password"] = config.password;
    properties["schema"] = config.database;
    properties["OPT_CONNECT_TIMEOUT"] = 1;
    auto* const driver = sql::mysql::get_mysql_driver_instance();
    return std::unique_ptr<sql::Connection>(driver->connect(properties));
}

void configureSession(sql::Connection& connection, const std::string& database) {
    connection.setSchema(database);
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    (void)statement->execute("SET time_zone = '+00:00'");
}

}  // namespace

ConnectionPoolError::ConnectionPoolError()
    : std::runtime_error("MySQL connection pool operation failed") {}

struct MySqlConnectionPool::SharedState final {
    using Clock = std::chrono::steady_clock;

    SharedState(app::config::MySqlConfig suppliedConfig, ConnectionPoolOptions suppliedOptions)
        : config(std::move(suppliedConfig)), options(suppliedOptions) {
        if (config.host.empty() || config.database.empty() || config.user.empty() ||
            options.size == 0U || options.connectRetryBudget.count() <= 0 ||
            options.retryDelay.count() <= 0) {
            throw ConnectionPoolError();
        }
        const auto deadline = Clock::now() + options.connectRetryBudget;
        std::vector<std::unique_ptr<sql::Connection>> connections;
        connections.reserve(options.size);
        try {
            while (connections.size() < options.size) {
                connections.push_back(connectUntil(deadline));
            }
        } catch (...) {
            throw ConnectionPoolError();
        }
        idle = std::move(connections);
    }

    std::unique_ptr<sql::Connection> connectUntil(const Clock::time_point deadline) const {
        for (;;) {
            try {
                auto connection = connectOnce(config);
                configureSession(*connection, config.database);
                connection->setAutoCommit(true);
                return connection;
            } catch (const sql::SQLException&) {
            } catch (...) {
                throw ConnectionPoolError();
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                throw ConnectionPoolError();
            }
            std::this_thread::sleep_for(std::min(
                options.retryDelay,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
    }

    void returnConnection(std::unique_ptr<sql::Connection> connection) noexcept {
        const bool reusable = connection && resetConnection(*connection);
        {
            std::lock_guard lock(mutex);
            if (leased > 0U) {
                --leased;
            }
            if (!closed && reusable) {
                idle.push_back(std::move(connection));
            }
        }
        available.notify_all();
    }

    bool resetConnection(sql::Connection& connection) const {
        try {
            if (connection.isClosed()) {
                return false;
            }
            connection.rollback();
            connection.setAutoCommit(true);
            configureSession(connection, config.database);
            return true;
        } catch (...) {
            return false;
        }
    }

    void close() noexcept {
        std::vector<std::unique_ptr<sql::Connection>> connections;
        {
            std::lock_guard lock(mutex);
            if (closed) {
                return;
            }
            closed = true;
            connections = std::move(idle);
        }
        available.notify_all();
    }

    app::config::MySqlConfig config;
    ConnectionPoolOptions options;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::vector<std::unique_ptr<sql::Connection>> idle;
    std::size_t leased = 0U;
    bool replenishing = false;
    bool closed = false;
};

MySqlConnectionPool::Lease::Lease(
    std::shared_ptr<SharedState> state,
    std::unique_ptr<sql::Connection> connection)
    : state_(std::move(state)), connection_(std::move(connection)) {}

MySqlConnectionPool::Lease::~Lease() { release(); }

MySqlConnectionPool::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)),
      connection_(std::move(other.connection_)) {}

MySqlConnectionPool::Lease& MySqlConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        connection_ = std::move(other.connection_);
    }
    return *this;
}

sql::Connection& MySqlConnectionPool::Lease::connection() const {
    if (!connection_) {
        throw ConnectionPoolError();
    }
    return *connection_;
}

MySqlConnectionPool::Lease::operator bool() const noexcept {
    return connection_ != nullptr;
}

void MySqlConnectionPool::Lease::discard() noexcept {
    connection_.reset();
    if (state_) {
        state_->returnConnection(nullptr);
    }
    state_.reset();
}

void MySqlConnectionPool::Lease::release() noexcept {
    if (state_ && connection_) {
        state_->returnConnection(std::move(connection_));
    }
    state_.reset();
}

MySqlConnectionPool::MySqlConnectionPool(
    app::config::MySqlConfig config,
    ConnectionPoolOptions options)
    : state_(std::make_shared<SharedState>(std::move(config), options)) {}

MySqlConnectionPool::~MySqlConnectionPool() { close(); }

std::optional<MySqlConnectionPool::Lease> MySqlConnectionPool::acquire(
    const std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return std::nullopt;
    }
    const auto deadline = SharedState::Clock::now() + timeout;
    const auto state = state_;
    std::unique_lock lock(state->mutex);
    for (;;) {
        if (state->closed) {
            return std::nullopt;
        }
        if (!state->idle.empty()) {
            auto connection = std::move(state->idle.back());
            state->idle.pop_back();
            ++state->leased;
            return Lease(state, std::move(connection));
        }
        const std::size_t live = state->leased + (state->replenishing ? 1U : 0U);
        if (live < state->options.size && !state->replenishing) {
            state->replenishing = true;
            lock.unlock();
            std::unique_ptr<sql::Connection> replacement;
            try {
                replacement = state->connectUntil(deadline);
            } catch (...) {
            }
            lock.lock();
            state->replenishing = false;
            if (state->closed) {
                state->available.notify_all();
                return std::nullopt;
            }
            if (replacement) {
                ++state->leased;
                state->available.notify_all();
                return Lease(state, std::move(replacement));
            }
            state->available.notify_all();
            return std::nullopt;
        }
        if (state->available.wait_until(lock, deadline) == std::cv_status::timeout) {
            return std::nullopt;
        }
    }
}

void MySqlConnectionPool::close() noexcept { state_->close(); }

const std::string& MySqlConnectionPool::database() const noexcept {
    return state_->config.database;
}

std::size_t MySqlConnectionPool::configuredSize() const noexcept {
    return state_->options.size;
}

std::size_t MySqlConnectionPool::idleCount() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->idle.size();
}

bool MySqlConnectionPool::isClosed() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->closed;
}

}  // namespace ocrservice::repositories::mysql::connection
