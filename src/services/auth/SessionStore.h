#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "AuthTypes.h"

namespace ocrservice::services::auth {

enum class SessionLookupStatus { valid, invalid, expired };

struct SessionLookup final {
    SessionLookupStatus status;
    std::optional<AuthSession> session;
};

class SessionStore final {
public:
    bool replace(std::string token, AuthSession session);
    SessionLookup lookup(std::string_view token, domain::UtcTimePoint nowUtc);
    SessionLookup revoke(std::string_view token, domain::UtcTimePoint nowUtc);
    std::size_t size() const;

private:
    struct StoredSession final {
        AuthSession session;
        std::string clientKey;
    };

    using TokenIndex = std::unordered_map<std::string, StoredSession>;

    SessionLookup inspectLocked(
        std::string_view token,
        domain::UtcTimePoint nowUtc,
        bool revokeValid);
    void eraseLocked(TokenIndex::iterator session);

    mutable std::mutex mutex_;
    TokenIndex sessionsByToken_;
    std::unordered_map<std::string, std::string> tokenByClient_;
};

}  // namespace ocrservice::services::auth
