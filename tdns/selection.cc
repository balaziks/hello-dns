#include "selection.hh"

map<ComboAddress, GlobalServerState> selection_cache;

struct SelectionException : public std::exception {
    const char * what () const throw ()
    {
    	return "Server selection could converge :(";
    }
};

transport Selection::get_transport() {
    double epsilon = 0.5;

    auto servers = get_from_cache(zonecut);

    vector<server> with_ip;
    vector<server> without_ip;

    // We tried to resolve this name but we failed so there is no point in trying again, I guess
    // https://en.wikipedia.org/wiki/Erase%E2%80%93remove_idiom
    servers.erase(remove_if(servers.begin(), servers.end(), [this](server s){return this->local_state[s].cantResolveName();}), servers.end());

    for(auto server : servers) {
        if(server.second == NO_IP) {
            without_ip.push_back(server);
        } else {
            with_ip.push_back(server);
        }
    }

    if (!servers.size()) {
        // No servers left. :(
        throw SelectionException();
    }

    random_device rd;
    mt19937 g(rd());

    std::uniform_real_distribution<> dis = std::uniform_real_distribution<>(0.0, 1.0);

    if(dis(g) > epsilon && with_ip.size()) {
        cout << "EXPLOIT!" << endl;

        // Shuffle to randomize order in the beginning (where all timeouts are MIN_TIMEOUT)
        // This can be replaced by adding random small values to timeout when inicializing GlobalState
        shuffle(with_ip.begin(), with_ip.end(), g);

        // Sort by local server state (now only errors) primarily (broken servers in the back) and by timeout secondarily
        stable_sort(with_ip.begin(), with_ip.end(), [](const server &a, const server &b) {
            return selection_cache[a.second].timeout < selection_cache[b.second].timeout;});
        stable_sort(with_ip.begin(), with_ip.end(), [this](const server &a, const server &b) {
            return this->local_state[a].errors < this->local_state[b].errors;});

        for (auto server : with_ip)
            cout << local_state[server].errors << " " << selection_cache[server.second].rtt_estimate/1000 << " ms\t\t" << server.first << "\t" << server.second.toString() << " " << endl;

        // Best RTT over servers with minimal number of errors
        server choice = with_ip.at(0);

        return {.name = choice.first,
                .address = choice.second,
                .TCP = doTCP,
                .timeout = selection_cache[choice.second].timeout,
               };
    } else {
        cout << "EXPLORE!" << endl;
        shuffle(servers.begin(), servers.end(), g);
        server choice = servers.at(0);
        return {.name = choice.first,
                .address = choice.second,
                .TCP = doTCP,
                .timeout = selection_cache[choice.second].timeout,
               };

    }
}

void Selection::success(transport choice) {
    return;
}

void Selection::timeout(transport choice) {
    selection_cache[choice.address].packet_lost();
}

void Selection::rtt(transport choice, int elapsed) {
    cout << "Updating " << choice.address.toString() << " with " << elapsed << endl;
    selection_cache[choice.address].update(elapsed);
}

void Selection::error(transport choice, SelectionError error) {
    switch (error)
    {
    case TIMEOUT:
        // we handle timeout separetly
        return;

    case TRUNCATED:
        // but TCP tres crashes on long TXT records for some reason
        doTCP = true;
        return;

    case CANT_RESOLVE_A:
        local_state[choice.getServer()].noA = true;
        return;

    case CANT_RESOLVE_AAAA:
        local_state[choice.getServer()].noAAAA = true;
        return;

    case FORMERROR:
        // we could do something with EDNS but we don't

    default:
        local_state[choice.getServer()].errors++;
    }


}

void Selection::resolve_ns(DNSName ns_name) {
    return;
}
