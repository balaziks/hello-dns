#include <map>
#include <vector>
#include "sclasses.hh"
#include "record-types.hh"


using namespace std;

auto ns_cache = map<DNSName, set<DNSName>>();
auto addr_cache = map<DNSName, set<ComboAddress>>();

auto save_to_cache(DNSName zonecut, DNSName ns_name, ComboAddress address = ComboAddress()) {
    cout << "saving " << zonecut << "\t" << ns_name << "\t" << address.toString() << endl;
    ns_cache[zonecut].insert(ns_name);
    if (address != ComboAddress()) {
        addr_cache[ns_name].insert(address);
    }
}

auto get_from_cache(DNSName zonecut) {
    cout << "getting " << zonecut << endl;
    vector<pair<DNSName, ComboAddress>> servers;

    for(auto ns_name : ns_cache[zonecut]) {
        for(auto address : addr_cache[ns_name]) {
            servers.push_back(make_pair(ns_name, address));
        }
    }

    return servers;
}

auto is_cached(DNSName ns_name) {
    return !addr_cache[ns_name].empty();
}
