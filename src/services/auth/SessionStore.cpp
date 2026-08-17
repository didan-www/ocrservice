#include "SessionStore.h"

#include <utility>

namespace ocrservice::services::auth {

bool SessionStore::replace(std::string token, AuthSession session) {
    auto clientKey = session.clientId.toString();
    StoredSession stored{std::move(session), std::move(clientKey)};
    std::string reverseToken(token);
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessionsByToken_.find(token) != sessionsByToken_.end()) {
        return false;
    }
    auto inserted = sessionsByToken_.emplace(std::move(token), std::move(stored));
    try {
        auto reverse = tokenByClient_.find(inserted.first->second.clientKey);
        if (reverse == tokenByClient_.end()) {
            tokenByClient_.emplace(inserted.first->second.clientKey, std::move(reverseToken));
            return true;
        }
        reverse->second.swap(reverseToken);
        sessionsByToken_.erase(reverseToken);
        return true;
    } catch (...) {
        sessionsByToken_.erase(inserted.first);
        throw;
    }
}

SessionLookup SessionStore::lookup(
    const std::string_view token,
    const domain::UtcTimePoint nowUtc) {
    std::lock_guard<std::mutex> lock(mutex_);
    return inspectLocked(token, nowUtc, false);
}

SessionLookup SessionStore::revoke(
    const std::string_view token,
    const domain::UtcTimePoint nowUtc) {
    std::lock_guard<std::mutex> lock(mutex_);
    return inspectLocked(token, nowUtc, true);
}

std::size_t SessionStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionsByToken_.size();
}

SessionLookup SessionStore::inspectLocked(
    const std::string_view token,
    const domain::UtcTimePoint nowUtc,
    const bool revokeValid) {
    const auto found = sessionsByToken_.find(std::string(token));
    if (found == sessionsByToken_.end()) {
        return {SessionLookupStatus::invalid, std::nullopt};
    }
    if (nowUtc >= found->second.session.expiresAt) {
        eraseLocked(found);
        return {SessionLookupStatus::expired, std::nullopt};
    }
    auto session = found->second.session;
    if (revokeValid) {
        eraseLocked(found);
    }
    return {SessionLookupStatus::valid, std::move(session)};
}

void SessionStore::eraseLocked(const TokenIndex::iterator session) {
    const auto reverse = tokenByClient_.find(session->second.clientKey);
    if (reverse != tokenByClient_.end() && reverse->second == session->first) {
        tokenByClient_.erase(reverse);
    }
    sessionsByToken_.erase(session);
}

}  // namespace ocrservice::services::auth
