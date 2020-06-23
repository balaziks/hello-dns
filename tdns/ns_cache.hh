#pragma once

#include <map>
#include <vector>
#include "sclasses.hh"
#include "record-types.hh"


using namespace std;

const auto NO_IP = ComboAddress();

extern map<DNSName, set<DNSName>> ns_cache;
extern map<DNSName, set<ComboAddress>> addr_cache;

void save_to_cache(DNSName zonecut, DNSName ns_name, ComboAddress address = NO_IP);
bool is_cached(DNSName ns_name);
vector<pair<DNSName, ComboAddress>> get_from_cache(DNSName zonecut);
