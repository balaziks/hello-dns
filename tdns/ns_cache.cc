#include "ns_cache.hh"

using namespace std;

map<DNSName, set<DNSName>> ns_cache;
map<DNSName, set<ComboAddress>> addr_cache;

void save_to_cache(DNSName zonecut, DNSName ns_name, ComboAddress address) {
    cout << "saving " << zonecut << "\t" << ns_name << "\t" << address.toString() << endl;
    ns_cache[zonecut].insert(ns_name);
    if (address != NO_IP) {
        addr_cache[ns_name].insert(address);
    }
}

vector<pair<DNSName, ComboAddress>> get_from_cache(DNSName zonecut) {
    cout << "getting " << zonecut << endl;
    vector<pair<DNSName, ComboAddress>> servers;

    for(auto ns_name : ns_cache[zonecut]) {
        if (addr_cache[ns_name].empty()) {
            servers.push_back(make_pair(ns_name, NO_IP));
        } else {
            for(auto address : addr_cache[ns_name]) {
                servers.push_back(make_pair(ns_name, address));
            }
        }
    }

    return servers;
}

bool is_cached(DNSName ns_name) {
    return !addr_cache[ns_name].empty();
}