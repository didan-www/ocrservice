#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "ServerConfig.h"

namespace sql {
class Connection;
}

namespace ocrservice::repositories::mysql::connection {

inline constexpr std::size_t kDefaultConnectionPoolSize = 6U;
inline constexpr std::chrono::seconds kDefaultConnectRetryBudget{60};

struct ConnectionPoolOptions final {
    std::size_t size = kDefaultConnectionPoolSize;
    std::chrono::milliseconds connectRetryBudget = kDefaultConnectRetryBudget;
    std::chrono::milliseconds retryDelay{250};
};

class ConnectionPoolError final : public std::runtime_error {
public:
    ConnectionPoolError();
};

class MySqlConnectionPool final {
private:
    struct SharedState;

public:
    class Lease final {
    public:
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        sql::Connection& connection() const;
        explicit operator bool() const noexcept;
        void discard() noexcept;

    private:
        friend class MySqlConnectionPool;
        Lease(
            std::shared_ptr<SharedState> state,
            std::unique_ptr<sql::Connection> connection);
        void release() noexcept;

        std::shared_ptr<SharedState> state_;
        std::unique_ptr<sql::Connection> connection_;
    };

    explicit MySqlConnectionPool(
        app::config::MySqlConfig config,
        ConnectionPoolOptions options = {});
    ~MySqlConnectionPool();
    MySqlConnectionPool(const MySqlConnectionPool&) = delete;
    MySqlConnectionPool& operator=(const MySqlConnectionPool&) = delete;

    std::optional<Lease> acquire(std::chrono::milliseconds timeout);
    void close() noexcept;

    const std::string& database() const noexcept;
    std::size_t configuredSize() const noexcept;
    std::size_t idleCount() const noexcept;
    bool isClosed() const noexcept;

private:
    std::shared_ptr<SharedState> state_;
};

}  // namespace ocrservice::repositories::mysql::connection
