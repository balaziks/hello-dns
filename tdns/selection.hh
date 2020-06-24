#pragma once

#include <random>
#include <algorithm>
#include <chrono>

#include "sclasses.hh"
#include "record-types.hh"
#include "ns_cache.hh"



using namespace std;

typedef pair<DNSName, ComboAddress> server;

const unsigned int MILLISECOND = 1000;
const unsigned int SECOND = 1000 * MILLISECOND;

const int DEFAULT_TIMEOUT = 200 * MILLISECOND;

// These should definitely be configurable, for now taken from Unbound
const int MIN_TIMEOUT = 50 * MILLISECOND;
const int MAX_TIMEOUT = 12 * SECOND;

struct GlobalServerState { // Models an exponencial moving average (no decaying)
    int rtt_estimate = 0; // microseconds
    int rtt_variance = 0;
    int timeout = DEFAULT_TIMEOUT;
    bool backed_off = false;
    chrono::time_point<chrono::steady_clock> last_update = chrono::time_point<chrono::steady_clock>();

    void update(unsigned int new_rtt) {
        auto now = chrono::steady_clock::now();
        if(rtt_estimate == 0) {
            rtt_estimate = new_rtt;
            last_update = now;
        } else {
            chrono::duration<float> time_delta = now - last_update; // in seconds
            cout << "time since last update " << time_delta.count() << endl;

            // Factor calculation from PowerDNS
            double factor = expf(-time_delta.count())/2.0f; // factor -> 0.5 as time_delta -> 0+
            cout << "updating with factor " << factor;
            last_update = now;
            cout << " from " << rtt_estimate/1000 << " ms ";
            int old_estimate = rtt_estimate;
            rtt_estimate = rtt_estimate * factor + new_rtt * (1 - factor);
            cout << " to " << rtt_estimate/1000 << " ms" << endl;
            int rtt_delta = old_estimate - rtt_estimate;
            rtt_variance = (1 - factor) * (rtt_variance + factor * factor * rtt_delta * rtt_delta);

            update_timeout();
        }
    }

    int update_timeout() {
        // Timeout calculation from RFC6298
        int timeout = rtt_estimate + 4 * rtt_variance;

        if (timeout < MIN_TIMEOUT) {
            return MIN_TIMEOUT;
        }
        if (timeout > MAX_TIMEOUT) {
            return MAX_TIMEOUT;
        }
        return timeout;
    }

    void packet_lost() {
        // This will get more interesting when we accept for outstanding answers after retransmit
        // But in tres we can't so we don't :)
        // So for now we just backoff exponentially
        backed_off = true;

        int new_timeout = timeout * 2;
        if (new_timeout > MAX_TIMEOUT)
            timeout = MAX_TIMEOUT;
        else
            timeout = new_timeout;
    }


};

extern map<ComboAddress, GlobalServerState> selection_cache;

enum SelectionError {
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
    Selection(DNSName zonecut) : zonecut(zonecut) {};

    transport get_transport();
    void success(transport choice);
    void timeout(transport choice);
    void rtt(transport choice, int elapsed);
    void error(transport choice, SelectionError error);

private:
    void resolve_ns(DNSName ns_name); //void for now

    bool doTCP = false;

    DNSName zonecut;
    map<server, LocalServerState> local_state;
};
