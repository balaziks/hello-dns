#pragma once

#include <random>
#include <algorithm>

#include "sclasses.hh"
#include "record-types.hh"
#include "ns_cache.hh"



using namespace std;

struct GlobalState {
    unsigned int rtt_estimate;
};

extern map<ComboAddress, GlobalState> selection_cache;

enum SelectionError {
    GENERAL,
    CANT_RESOLVE_AAAA,
    CANT_RESOLVE_A,
};

struct transport {
    DNSName name;
    ComboAddress address = NO_IP;
    bool TCP = false;
    unsigned int timeout;
};

struct LocalState {
    unsigned int udp_tries = 0;
    unsigned int tcp_tries = 0;
};

typedef pair<DNSName, ComboAddress> server;

class Selection {
public:
    Selection(DNSName zonecut) : zonecut(zonecut) {};

    transport get_transport();
    void success(transport choice);
    void timeout(transport choice);
    void rtt(transport choice, unsigned int elapsed);
    void error(transport choice, SelectionError error);

private:
    void resolve_ns(DNSName ns_name); //void for now

    DNSName zonecut;
    map<server, LocalState> local_state;
};
