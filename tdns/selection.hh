#pragma once

#include <random>
#include <algorithm>
#include <chrono>
#include <thread>

#include "sclasses.hh"
#include "record-types.hh"
#include "ns_cache.hh"

#include "tres.hh"

using namespace std;

typedef pair<DNSName, ComboAddress> server;

const int MILLISECOND = 1000;
const int SECOND = 1000 * MILLISECOND;

const int MIN_TIMEOUT = 1 * MILLISECOND;
const int MAX_TIMEOUT = 12 * SECOND;

// This ought to be called as a thread and never join
static void do_resolve_ns(DNSName ns_name)
try
{
    DNSName name(ns_name);
    vector<DNSType> types = {DNSType::A, DNSType::AAAA};
    auto resolver = TDNSResolver(g_root);
    for (auto type : types) {
        auto ret = resolver.resolveAt(ns_name, type);
        if (ret.res.size()) {
            for(const auto& res : ret.res)
                addr_cache[ns_name].insert(getIP(res.rr));
        }
    }
}
catch(TooManyQueriesException& e)
{
  cerr << "Thread died after too many queries" << endl;
}
catch(exception& e)
{
  cerr << "Thread died: " << e.what() << endl;
}

struct GlobalServerState {
    int rtt_estimate = 0; // microseconds
    int rtt_variance = 376 * MILLISECOND / 4;
    int timeout = calculate_timeout();

    void update(int new_rtt) {
        int delta = rtt_estimate - new_rtt;
        rtt_estimate += delta/8;
        rtt_variance += (abs(delta) - rtt_variance) / 4;
        timeout = calculate_timeout();
    }

    void packet_lost() {
        // This will get more interesting when we accept for outstanding answers after retransmit
        // See https://github.com/NLnetLabs/unbound/blob/4bf9d124190470b8a46439f569f1e72457222930/util/rtt.c#L100 for example
        // But in tres we can't so we don't :)
        // So for now we just backoff exponentially

        timeout *= 2;
        if (timeout > MAX_TIMEOUT)
            timeout = MAX_TIMEOUT;

    }

    int calculate_timeout() {
        int to = rtt_estimate + 4 * rtt_variance;
        if (to < MIN_TIMEOUT)
            return MIN_TIMEOUT;
        if (to > MAX_TIMEOUT)
            return MAX_TIMEOUT;
        return to;
    }

    int get_timeout() {
        if (calculate_timeout() != timeout) {
            // we have backed off or fell back
            return timeout;
        }
        return rtt_estimate + 4 * rtt_variance;
    }
};

extern map<ComboAddress, GlobalServerState> selection_cache;

enum SelectionFeedback {
    SOCKET,
    CANT_RESOLVE_AAAA,
    CANT_RESOLVE_A,
    INVALID_ANSWER, // wrong ID or not an answer at all
    FORMERROR,
    TRUNCATED,
    TIMEOUT,
};

struct transport {
    DNSName name;
    ComboAddress address = NO_IP;
    bool TCP = true;
    int timeout;

    server getServer() {
        return make_pair(name, address);
    }
};

struct LocalServerState {
    unsigned int errors = 0;
    bool noA = false;
    bool noAAAA = false;

    bool cantResolveName() {
        return noA && noAAAA;
    }
};

class Selection {
public:
    Selection(DNSName zonecut, TDNSResolver* res) : zonecut(zonecut), resolver(res) {};

    transport get_transport();
    void success(transport choice);
    void timeout(transport choice);
    void rtt(transport choice, int elapsed);
    void error(transport choice, SelectionFeedback error);

private:
    void resolve_ns(DNSName ns_name) {
        std::thread t(do_resolve_ns, ns_name);
        t.detach();
    }
    bool doTCP = false;

    DNSName zonecut;
    TDNSResolver* resolver;
    map<server, LocalServerState> local_state;
};