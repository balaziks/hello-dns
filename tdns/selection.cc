#include "selection.hh"

map<ComboAddress, GlobalState> selection_cache;

struct SelectionException : public std::exception {
    const char * what () const throw ()
    {
    	return "Server selection could converge :(";
    }
};

transport Selection::get_transport() {
    auto servers = get_from_cache(zonecut);

    if (!servers.size()) {
        throw SelectionException();
    }

    vector<server> with_ip;
    vector<server> without_ip;

    for(auto server : servers) {
        if(server.second == NO_IP) {
            without_ip.push_back(server);
        } else {
            with_ip.push_back(server);
        }
    }

    random_device rd;
    mt19937 g(rd());

    if(with_ip.size()) {
        // choose randomly for now
        shuffle(with_ip.begin(), with_ip.end(), g);

        server choice = with_ip.at(0);

        // Also resolve some other NS name (does nothing for now)
        if (without_ip.size()) {
            shuffle(without_ip.begin(), without_ip.end(), g);
            resolve_ns(without_ip.at(0).first);
        }

        local_state[choice].udp_tries++;

        return {.name = choice.first,
                .address = choice.second,
                .TCP = false,
                .timeout = 200
               };
    } else {
        // We have no NS with IP
        shuffle(without_ip.begin(), without_ip.end(), g);
        server choice = without_ip.at(0);
        return {.name = choice.first,
                .address = NO_IP,
                .TCP = false,
                .timeout = 200,
               };

    }
}

void Selection::success(transport choice) {
    return;
}

void Selection::timeout(transport choice) {
    return;
}

void Selection::rtt(transport choice, unsigned int elapsed) {
    return;
}

void Selection::error(transport choice, SelectionError error) {
    return;
}

void Selection::resolve_ns(DNSName ns_name) {
    return;
}
