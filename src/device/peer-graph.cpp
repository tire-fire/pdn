#include "device/peer-graph.hpp"

#include <algorithm>

void PeerGraph::setSelfMac(const net::Mac& mac) {
    selfMac_ = mac;
}

bool PeerGraph::acceptBeacon(const BeaconRecord& beacon, unsigned long nowMs) {
    auto it = beaconsBySource_.find(beacon.source);
    if (it != beaconsBySource_.end() && it->second == beacon) {
        return false;
    }
    beaconsBySource_[beacon.source] = beacon;
    lastGraphChangeMs_ = nowMs;
    if (onGraphChanged_) onGraphChanged_();
    return true;
}

std::optional<BeaconRecord> PeerGraph::getBeacon(const net::Mac& source) const {
    auto it = beaconsBySource_.find(source);
    if (it == beaconsBySource_.end()) return std::nullopt;
    return it->second;
}

void PeerGraph::setSelfPeers(const net::Mac& inPeer,
                             const net::Mac& outPeer,
                             unsigned long nowMs) {
    if (selfInPeer_ == inPeer && selfOutPeer_ == outPeer) return;
    selfInPeer_ = inPeer;
    selfOutPeer_ = outPeer;
    lastGraphChangeMs_ = nowMs;
    if (onGraphChanged_) onGraphChanged_();
}

bool PeerGraph::hasMutualEdge(const net::Mac& a, const net::Mac& b) const {
    auto claims = [this](const net::Mac& from, const net::Mac& to) -> bool {
        if (from == selfMac_) {
            return selfInPeer_ == to || selfOutPeer_ == to;
        }
        auto it = beaconsBySource_.find(from);
        if (it == beaconsBySource_.end()) return false;
        return it->second.inPeer == to || it->second.outPeer == to;
    };
    return claims(a, b) && claims(b, a);
}

std::vector<net::Mac> PeerGraph::getChainMembers() const {
    // BFS from self over mutual edges. Candidate nodes are the known beacon
    // sources plus self.
    std::vector<net::Mac> members{selfMac_};
    std::vector<net::Mac> frontier{selfMac_};
    while (!frontier.empty()) {
        net::Mac current = frontier.back();
        frontier.pop_back();
        for (const auto& entry : beaconsBySource_) {
            const net::Mac& source = entry.first;
            if (std::find(members.begin(), members.end(), source) != members.end()) continue;
            if (hasMutualEdge(current, source)) {
                members.push_back(source);
                frontier.push_back(source);
            }
        }
    }
    return members;
}

size_t PeerGraph::countReachableExcludingSelf(const net::Mac& firstHop) const {
    if (firstHop == net::Mac{} || firstHop == selfMac_) return 0;
    // Self is pre-marked visited so traversal never crosses to the far side
    // of the chain. firstHop is counted directly (a confirmed direct peer);
    // expansion past it follows mutual edges only.
    std::vector<net::Mac> visited{selfMac_, firstHop};
    std::vector<net::Mac> frontier{firstHop};
    size_t count = 1;
    while (!frontier.empty()) {
        net::Mac current = frontier.back();
        frontier.pop_back();
        for (const auto& entry : beaconsBySource_) {
            const net::Mac& source = entry.first;
            if (std::find(visited.begin(), visited.end(), source) != visited.end()) continue;
            if (hasMutualEdge(current, source)) {
                visited.push_back(source);
                frontier.push_back(source);
                count++;
            }
        }
    }
    return count;
}

bool PeerGraph::isInLoop() const {
    // Self is in a cycle iff two distinct mutual neighbors of self are
    // connected to each other by a path that does not pass back through self.
    std::vector<net::Mac> neighbors;
    for (const auto& entry : beaconsBySource_) {
        if (hasMutualEdge(selfMac_, entry.first)) neighbors.push_back(entry.first);
    }
    if (neighbors.size() < 2) return false;

    auto reachableAvoidingSelf = [this](const net::Mac& start,
                                        const net::Mac& goal) -> bool {
        std::vector<net::Mac> visited{selfMac_, start};
        std::vector<net::Mac> frontier{start};
        while (!frontier.empty()) {
            net::Mac current = frontier.back();
            frontier.pop_back();
            if (current == goal) return true;
            for (const auto& entry : beaconsBySource_) {
                const net::Mac& source = entry.first;
                if (std::find(visited.begin(), visited.end(), source) != visited.end()) continue;
                if (hasMutualEdge(current, source)) {
                    if (source == goal) return true;
                    visited.push_back(source);
                    frontier.push_back(source);
                }
            }
        }
        return false;
    };

    for (size_t i = 0; i < neighbors.size(); ++i) {
        for (size_t j = i + 1; j < neighbors.size(); ++j) {
            if (reachableAvoidingSelf(neighbors[i], neighbors[j])) return true;
        }
    }
    return false;
}

bool PeerGraph::isTopologyStable(unsigned long nowMs) const {
    return nowMs - lastGraphChangeMs_ >= kTopologyStabilityMs;
}

