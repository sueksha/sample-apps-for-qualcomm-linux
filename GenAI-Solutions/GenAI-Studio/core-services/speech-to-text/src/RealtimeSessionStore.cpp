// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "RealtimeSessionStore.hpp"

RealtimeStoreStats RealtimeSessionStore::snapshotStats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return RealtimeStoreStats{
        sessions_.size(),
        totalPendingBytesLocked()
    };
}

bool RealtimeSessionStore::tryInsertSession(RealtimeSessionState state,
                                            size_t max_sessions,
                                            int session_ttl_sec) {
    std::lock_guard<std::mutex> lk(mutex_);
    pruneExpiredSessionsLocked(session_ttl_sec, std::chrono::steady_clock::now());
    if (sessions_.size() >= max_sessions) {
        return false;
    }
    sessions_[state.id] = std::move(state);
    return true;
}

bool RealtimeSessionStore::eraseSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    return sessions_.erase(session_id) > 0;
}

RealtimeSessionStore::SessionLookup RealtimeSessionStore::withLiveSession(
    const std::string& session_id,
    int session_ttl_sec,
    const std::function<void(RealtimeSessionState&, uint64_t total_pending_before)>& fn) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return SessionLookup::kNotFound;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto age_sec =
        std::chrono::duration_cast<std::chrono::seconds>(now - it->second.created_at).count();
    if (age_sec > session_ttl_sec) {
        sessions_.erase(it);
        return SessionLookup::kExpired;
    }

    const uint64_t total_pending_before = totalPendingBytesLocked();
    fn(it->second, total_pending_before);
    return SessionLookup::kFound;
}

bool RealtimeSessionStore::withSessionIfExists(
    const std::string& session_id,
    const std::function<void(RealtimeSessionState&)>& fn) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    fn(it->second);
    return true;
}

void RealtimeSessionStore::prependPendingIfExists(
    const std::string& session_id,
    const std::vector<uint8_t>& pcm_prefix) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return;
    }
    auto& session = it->second;
    session.pending_pcm.insert(session.pending_pcm.begin(),
                               pcm_prefix.begin(),
                               pcm_prefix.end());
}

uint64_t RealtimeSessionStore::totalPendingBytesLocked() const {
    uint64_t total = 0;
    for (const auto& it : sessions_) {
        total += static_cast<uint64_t>(it.second.pending_pcm.size());
    }
    return total;
}

void RealtimeSessionStore::pruneExpiredSessionsLocked(
    int session_ttl_sec,
    std::chrono::steady_clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.created_at).count();
        if (age_sec > session_ttl_sec) {
            it = sessions_.erase(it);
            continue;
        }
        ++it;
    }
}
