#include <fstream>
#include <vector>
#include <map>
#include <stdexcept>
#include "sclasses.hh"
#include <signal.h>
#include <random>
#include "record-types.hh"
#include <thread>
#include <chrono>
#include "nlohmann/json.hpp"
#include <systemd/sd-daemon.h>

#include "ns_cache.hh"
#include "selection.hh"

#include "tres.hh"

/*!
   @file
   @brief Teachable resolver
*/

using namespace std;

ComboAddress ip4_src = ComboAddress("0.0.0.0:0");
ComboAddress ip6_src = ComboAddress("[::]:0");

int ip4_port = 1023;
int ip6_port = 1023;

int get_next_ip4_port() {
  ip4_port++;
  if (ip4_port >= 65535)
    ip4_port = 1024;
  return ip4_port;
}

int get_next_ip6_port() {
  ip6_port++;
  if (ip6_port >= 65535)
    ip6_port = 1024;
  return ip6_port;
}

//! this is a different kind of error: we KNOW your name does not exist
struct NxdomainException{};
//! Or if your type does not exist
struct NodataException{};

multimap<DNSName, ComboAddress> g_root;

/** This function guarantees that you will get an answer from this server. It will drop EDNS for you
    and eventually it will even fall back to TCP for you. If nothing works, an exception is thrown.
    Note that this function does not think about actual DNS errors, you get those back verbatim.
    Only the TC bit is checked.

    This function does check if the ID field of the response matches the query, but the caller should
    check qname and qtype.
*/
DNSMessageReader TDNSResolver::getResponse(const ComboAddress& server, const DNSName& dn, const DNSType& dt, double timeout, bool doTCP, int depth)
{
  std::string prefix(depth, ' ');
  prefix += dn.toString() + "|"+toString(dt)+" ";

  bool doEDNS=true;

  if(++d_numqueries > d_maxqueries) // there is the possibility our algorithm will loop
    throw TooManyQueriesException(); // and send out thousands of queries, so let's not

  DNSMessageWriter dmw(dn, dt);
  dmw.dh.rd = false;
  dmw.randomizeID();
  if(doEDNS)
    dmw.setEDNS(1500, false);  // no DNSSEC for now, 1500 byte buffer size
  string resp;

  if(doTCP) {
    Socket sock(server.sin4.sin_family, SOCK_STREAM);
    if (server.isIPv4()) {
      if (ip4_src != ComboAddress("0.0.0.0:0")) {
        ip4_src.setPort(get_next_ip4_port());
        SBind(sock, ip4_src);
      }
    } else {
      if (ip6_src != ComboAddress("[::]:0")) {
        ip6_src.setPort(get_next_ip6_port());
        SBind(sock, ip6_src);
      }
    }
    SConnect(sock, server);
    string ser = dmw.serialize();
    uint16_t len = htons(ser.length());
    string tmp((char*)&len, 2);
    SWrite(sock, tmp);
    SWrite(sock, ser);

    int err = waitForData(sock, &timeout);

    if( err <= 0) {
      if(!err) d_numtimeouts++;
      throw std::runtime_error("Error waiting for data from "+server.toStringWithPort()+": "+ (err ? string(strerror(errno)): string("Timeout")));
    }

    tmp=SRead(sock, 2);
    len = ntohs(*((uint16_t*)tmp.c_str()));

    // so yes, you need to check for a timeout here again!
    err = waitForData(sock, &timeout);

    if( err <= 0) {
      if(!err) d_numtimeouts++;
      throw std::runtime_error("Error waiting for data from "+server.toStringWithPort()+": "+ (err ? string(strerror(errno)): string("Timeout")));
    }
    // and even this is not good enough, an authoritative server could be trickling us bytes
    resp = SRead(sock, len);
  }
  else {
    Socket sock(server.sin4.sin_family, SOCK_DGRAM);
    if (server.isIPv4()) {
      if (ip4_src != ComboAddress("0.0.0.0:0")) {
        ip4_src.setPort(get_next_ip4_port());
        SBind(sock, ip4_src);
      }
    } else {
      if (ip6_src != ComboAddress("[::]:0")) {
        ip6_src.setPort(get_next_ip6_port());
        SBind(sock, ip6_src);
      }
    }
    SConnect(sock, server);
    SWrite(sock, dmw.serialize());

    int err = waitForData(sock, &timeout);

    // so one could simply retry on a timeout, but here we don't
    if( err <= 0) {

      if(!err) {
        d_numtimeouts++;
        throw SelectionFeedback(TIMEOUT);
      }
      throw SelectionFeedback(SOCKET);
      // throw std::runtime_error("Error waiting for data from "+server.toStringWithPort()+": "+ (err ? string(strerror(errno)): string("Timeout")));
    }
    ComboAddress ign=server;
    resp = SRecvfrom(sock, 65535, ign);
  }

  DNSMessageReader dmr;
  try {
    dmr = DNSMessageReader(resp);
  }
  catch (const std::runtime_error &) {
    // Parse error
    throw SelectionFeedback(INVALID_ANSWER);
  }
  if(dmr.dh.id != dmw.dh.id) {
    lstream() << prefix << "ID mismatch on answer" << endl;
    throw SelectionFeedback(INVALID_ANSWER);
  }
  if(!dmr.dh.qr) { // for security reasons, you really need this
    lstream() << prefix << "What we received was not a response, ignoring"<<endl;
    throw SelectionFeedback(INVALID_ANSWER);
  }
  if((RCode)dmr.dh.rcode == RCode::Formerr) { // XXX this should check that there is no OPT in the response
    lstream() << prefix <<"Got a Formerr"<<endl;
    throw SelectionFeedback(FORMERROR);
  }
  if(dmr.dh.tc) {
    lstream() << prefix <<"Got a truncated answer"<<endl;
    throw SelectionFeedback(TRUNCATED);
  }
  return dmr;
}


void TDNSResolver::dotQuery(const DNSName& auth, const DNSName& server)
{
  if(!d_dot) return;
  (*d_dot) << '"' << auth << "\" [shape=diamond]\n";
  (*d_dot) << '"' << auth << "\" -> \"" << server << "\" [ label = \" " << d_numqueries<<"\"]" << endl;
}

void TDNSResolver::dotAnswer(const DNSName& dn, const DNSType& rrdt, const DNSName& server)
{
  if(!d_dot) return;
  (*d_dot) <<"\"" << dn << "/"<<rrdt<<"\" [shape=box]\n";
  (*d_dot) << '"' << server << "\" -> \"" << dn << "/"<<rrdt<<"\""<<endl;
}

void TDNSResolver::dotCNAME(const DNSName& target, const DNSName& server, const DNSName& dn)
{
  if(!d_dot) return;
  (*d_dot) << '"' << target << "\" [shape=box]"<<endl;
  (*d_dot) << '"' << server << "\" -> \"" << dn << "/CNAME\" -> \"" << target <<"\"\n";
}

void TDNSResolver::dotDelegation(const DNSName& rrdn, const DNSName& server)
{
  if(!d_dot) return;
  (*d_dot) << '"' << rrdn << "\" [shape=diamond]\n";
  (*d_dot) << '"' << server << "\" -> \"" << rrdn << "\"" <<endl;
}

/** This attempts to look up the name dn with type dt. The depth parameter is for
    trace output.
    the 'auth' field describes the authority of the servers we will be talking to. Defaults to root ('believe everything')
    The multimap specifies the servers to try with. Defaults to a list of
    root-servers.
*/

TDNSResolver::ResolveResult TDNSResolver::resolveAt(const DNSName& dn, const DNSType& dt, int depth, const DNSName& auth)
{
  std::string prefix(depth, ' ');
  prefix += dn.toString() + "|"+toString(dt)+" ";

  ResolveResult ret;

  auto selection = Selection(auth, this);
  while(true) {
    transport choice = selection.get_transport();

    if (choice.address == NO_IP) {
      // resolve choice.name as it is needed
      for(const DNSType& qtype : {DNSType::A, DNSType::AAAA}) {
        try {
          auto result = resolveAt(choice.name, qtype, depth+1);
          lstream() << prefix<<"Got "<<result.res.size()<<" nameserver " << qtype <<" addresses, adding to cache"<<endl;
          for(const auto& res : result.res) {
            save_to_cache(auth, choice.name, getIP(res.rr));
          }
        }
        catch(std::exception& e)
        {
          lstream() << prefix <<"Failed to resolve name for "<<choice.name<<"|"<<qtype<<": "<<e.what()<<endl;

          if(qtype == DNSType::A)
            selection.error(choice, CANT_RESOLVE_A);
          else
            selection.error(choice, CANT_RESOLVE_AAAA);

          continue;
        }
        catch(...)
        {
          lstream() << prefix <<"Failed to resolve name for "<<choice.name<<"|"<<qtype<<endl;
          continue;
        }
      }
    } else {
      // send query to choice.address
      dotQuery(auth, choice.name);
      choice.address.sin4.sin_port = htons(53);

      try {
      lstream() << prefix<<"Sending to server "<<choice.name<<" on "<<choice.address.toString()<<endl;

      DNSMessageReader dmr;
      auto start = chrono::steady_clock::now();
      double timeout = 1.0 * choice.timeout / 1000000; // conversion to seconds
      cout << timeout << endl;
      try {
        dmr = getResponse(choice.address, dn, dt, timeout, choice.TCP, depth);
        auto finish = chrono::steady_clock::now();
        auto duration = chrono::duration_cast<chrono::microseconds>(finish-start);
        selection.success(choice);
        selection.rtt(choice, duration.count());
      }
      catch (SelectionFeedback e) {
        auto finish = chrono::steady_clock::now();
        auto duration = chrono::duration_cast<chrono::microseconds>(finish-start);
        cout << "============ error code " << e << endl;
        selection.error(choice, e);
        if (e != TIMEOUT && e != SOCKET) {
          // selection.rtt(choice, 0); // some kind of error on socket or timeout, no point in reporting rtt
        } else {
          selection.rtt(choice, duration.count());
        }
        continue;
      }

      DNSSection rrsection;
      uint32_t ttl;
      DNSName rrdn, newAuth;
      DNSType rrdt;

      dmr.getQuestion(rrdn, rrdt); // parse into rrdn and rrdt

      lstream() << prefix<<"Received a "<< dmr.size() << " byte response with RCode "<<(RCode)dmr.dh.rcode<<", qname " <<dn<<", qtype "<<dt<<", aa: "<<dmr.dh.aa << endl;
      if(rrdn != dn || dt != rrdt) {
        lstream() << prefix << "Got a response to a different question or different type than we asked for!"<<endl;
        continue; // see if another server wants to work with us
      }

      // in a real resolver, you must ignore NXDOMAIN in case of a CNAME. Because that is how the internet rolls.
      if((RCode)dmr.dh.rcode == RCode::Nxdomain) {
        lstream() << prefix<<"Got an Nxdomain, it does not exist"<<endl;
        throw NxdomainException();
      }
      else if((RCode)dmr.dh.rcode != RCode::Noerror) {
        throw std::runtime_error(string("Answer from authoritative server had an error: ") + toString((RCode)dmr.dh.rcode));
      }
      if(dmr.dh.aa) {
        lstream() << prefix<<"Answer says it is authoritative!"<<endl;
      }

      std::unique_ptr<RRGen> rr;
      set<DNSName> nsses;

      /* here we loop over records. Perhaps the answer is there, perhaps
         there is a CNAME we should follow, perhaps we get a delegation.
         And if we do get a delegation, there might even be useful glue */

      while(dmr.getRR(rrsection, rrdn, rrdt, ttl, rr)) {
        lstream() << prefix << rrsection<<" "<<rrdn<< " IN " << rrdt << " " << ttl << " " <<rr->toString()<<endl;
        if(dmr.dh.aa==1) { // authoritative answer. We trust this.
          if(rrsection == DNSSection::Answer && dn == rrdn && dt == rrdt) {
            lstream() << prefix<<"We got an answer to our question!"<<endl;
            dotAnswer(dn, rrdt, choice.name);
            ret.res.push_back({dn, ttl, std::move(rr)});
          }
          else if(dn == rrdn && rrdt == DNSType::CNAME) {
            DNSName target = dynamic_cast<CNAMEGen*>(rr.get())->d_name;
            ret.intermediate.push_back({dn, ttl, std::move(rr)}); // rr is DEAD now!
            lstream() << prefix<<"We got a CNAME to " << target <<", chasing"<<endl;
            dotCNAME(target, choice.name, dn);
            if(target.isPartOf(auth)) { // this points to something we consider this server auth for
              lstream() << prefix << "target " << target << " is within " << auth<<", harvesting from packet"<<endl;
              bool hadMatch=false;      // perhaps the answer is in this DNS message
              while(dmr.getRR(rrsection, rrdn, rrdt, ttl, rr)) {
                if(rrsection==DNSSection::Answer && rrdn == target && rrdt == dt) {
                  hadMatch=true;
                  ret.res.push_back({dn, ttl, std::move(rr)});
                }
              }
              if(hadMatch) {            // if it worked, great, otherwise actual chase
                lstream() << prefix << "in-message chase worked, we're done"<<endl;
                return ret;
              }
              else
                lstream() <<prefix<<"in-message chase not successful, will do new query for "<<target<<endl;
            }

            auto chaseres=resolveAt(target, dt, depth + 1);
            ret.res = std::move(chaseres.res);
            for(auto& rr : chaseres.intermediate)   // add up their intermediates to ours
              ret.intermediate.push_back(std::move(rr));
            return ret;
          }
        }
        else {
          // this picks up nameserver records. We check if glue records are within the authority
          // of what we approached this server for.
          if(rrsection == DNSSection::Authority && rrdt == DNSType::NS) {
            if(dn.isPartOf(rrdn))  {
              DNSName nsname = dynamic_cast<NSGen*>(rr.get())->d_name;

              if(!dmr.dh.aa && (newAuth != rrdn || nsses.empty())) {
                dotDelegation(rrdn, choice.name);
              }
              save_to_cache(rrdn, nsname, NO_IP);
              nsses.insert(nsname);
              newAuth = rrdn;
            }
            else
              lstream()<< prefix << "Authoritative server gave us NS record to which this query does not belong" <<endl;
          }
          else if(rrsection == DNSSection::Additional && nsses.count(rrdn) && (rrdt == DNSType::A || rrdt == DNSType::AAAA)) {
            // this only picks up addresses for NS records we've seen already
            // but that is ok: NS is in Authority section
            cout << "is" << rrdn << " part of " << auth << endl;
            if(rrdn.isPartOf(auth)) {
              save_to_cache(newAuth, rrdn, getIP(rr));
            }
            else
              lstream() << prefix << "Not accepting IP address of " << rrdn <<": out of authority of this server"<<endl;
          }
        }
      }
      if(!ret.res.empty()) {
        // the answer is in!
        lstream() << prefix<<"Done, returning "<<ret.res.size()<<" results, "<<ret.intermediate.size()<<" intermediate\n";
        return ret;
      }
      else if(dmr.dh.aa) {
        lstream() << prefix <<"No data response"<<endl;
        throw NodataException();
      }
      // we saved the delegation to cache, try the next query
      lstream() << prefix << "We got delegated to " << nsses.size() << " " << newAuth << " nameserver names " << endl;

      auto res2=resolveAt(dn, dt, depth+1, newAuth);
      if(!res2.res.empty())
        return res2;
      lstream() << prefix<<"The IP addresses we had did not provide a good answer"<<endl;
      }

      catch(std::exception& e) {
        lstream() << prefix <<"Error resolving: " << e.what() << endl;
      }
    }
  }

  // if we get here, we have no results for you.
  return ret;
}

//! This is a thread that will create an answer to the query in `dmr`
void processQuery(int sock, ComboAddress client, DNSMessageReader dmr)
try
{
  DNSName dn;
  DNSType dt;
  dmr.getQuestion(dn, dt);

  DNSMessageWriter dmw(dn, dt);
  dmw.dh.rd = dmr.dh.rd;
  dmw.dh.ra = true;
  dmw.dh.qr = true;
  dmw.dh.id = dmr.dh.id;

  TDNSResolver::ResolveResult res;
  TDNSResolver tdr(g_root);
  try {

    res = tdr.resolveAt(dn, dt);

    cout<<"Result of query for "<< dn <<"|"<<toString(dt)<<endl;
    for(const auto& r : res.intermediate) {
      cout<<r.name <<" "<<r.ttl<<" "<<r.rr->getType()<<" " << r.rr->toString()<<endl;
    }

    for(const auto& r : res.res) {
      cout<<r.name <<" "<<r.ttl<<" "<<r.rr->getType()<<" "<<r.rr->toString()<<endl;
    }
    cout<<"Result for "<< dn <<"|"<<toString(dt)<<" took "<<tdr.d_numqueries <<" queries"<<endl;
  }
  catch(NodataException& nd)
  {
    cout<<"No Data for "<< dn <<"|"<<toString(dt)<<" took "<<tdr.d_numqueries <<" queries"<<endl;
    SSendto(sock, dmw.serialize(), client);
    return;
  }
  catch(NxdomainException& nx)
  {
    cout<<"NXDOMAIN for "<< dn <<"|"<<toString(dt)<<" took "<<tdr.d_numqueries <<" queries"<<endl;
    dmw.dh.rcode = (int)RCode::Nxdomain;
    SSendto(sock, dmw.serialize(), client);
    return;
  }
  // Put in the CNAME chain
  for(const auto& rr : res.intermediate)
    dmw.putRR(DNSSection::Answer, rr.name, rr.ttl, rr.rr);
  for(const auto& rr : res.res) // and the actual answer
    dmw.putRR(DNSSection::Answer, rr.name, rr.ttl, rr.rr);
  string resp = dmw.serialize();
  SSendto(sock, resp, client); // and send it!
}
catch(TooManyQueriesException& e)
{
  cerr << "Thread died after too many queries" << endl;
}

catch(exception& e)
{
  cerr << "Thread died: " << e.what() << endl;
}

static nlohmann::json rrToJSON(const TDNSResolver::ResolveRR& r)
{
  nlohmann::json record;
  record["name"]=r.name.toString();
  record["ttl"]=r.ttl;
  record["type"]=toString(r.rr->getType());
  record["content"]=r.rr->toString();
  return record;
}


int main(int argc, char** argv)
try
{
  if(argc != 5 && argc != 6) {
    cerr<<"Syntax: tres name type ip4_src ip6_src hintsfile\n";
    cerr<<"Syntax: tres ip:port ip4_src ip6_src hintsfile\n";
    cerr<<"\n";
    cerr<<"When name and type are specified, tres looks up a DNS record.\n";
    cerr<<"types: A, NS, CNAME, SOA, PTR, MX, TXT, AAAA, ...\n";
    cerr<<"       see https://en.wikipedia.org/wiki/List_of_DNS_record_types\n";
    cerr<<"\n";
    cerr<<"When ip:port is specified, tres acts as a DNS server.\n";
    return(EXIT_FAILURE);
  }
  signal(SIGPIPE, SIG_IGN); // TCP, so we need this

  ip4_src = ComboAddress(argv[argc-3], 0);
  cout << "here" << endl;
  ip6_src = ComboAddress("["+(string) argv[argc-2]+"]:0");

  multimap<DNSName, ComboAddress> hints;

  // Hacky way to load hints from file
  ifstream hints_file(argv[argc-1]);

  if (hints_file.is_open()) {
    string line;
    vector<std::string> tokens;
    while (getline(hints_file, line)) {
      if (line[0] == ';')
        continue;
      vector<std::string> tokens;
      for (auto i = strtok(&line[0], " \t"); i != NULL; i = strtok(NULL, " \t"))
        tokens.push_back(i);
      if (tokens.size() >= 2) {
        auto name = tokens.front();
        auto ip = tokens.back();
        if (name != ".") {
          hints.insert({makeDNSName(name), ComboAddress(ip, 53)});
        }
      }
    }
  }

  // If it fails we use the default hardcoded ones
  if (hints.empty()) {
    hints = {{makeDNSName("a.root-servers.net"), ComboAddress("198.41.0.4", 53)},
             {makeDNSName("f.root-servers.net"), ComboAddress("192.5.5.241", 53)},
             {makeDNSName("k.root-servers.net"), ComboAddress("193.0.14.129", 53)},
    };
  }

  // retrieve the actual live root NSSET from the hints
  for(const auto& h : hints) {
    try {
      TDNSResolver tdr;
      // XXX if the root servers aren't available via UDP, tough luck
      DNSMessageReader dmr = tdr.getResponse(h.second, makeDNSName("."), DNSType::NS, 1.0);
      DNSSection rrsection;
      DNSName rrdn;
      DNSType rrdt;
      uint32_t ttl;
      std::unique_ptr<RRGen> rr;

      // XXX should check if response name and type match query
      // this assumes the root will only send us relevant NS records
      // we could check with the NS records if we wanted
      // but if a root wants to mess with us, it can
      while(dmr.getRR(rrsection, rrdn, rrdt, ttl, rr)) {
        if(rrdt == DNSType::A || rrdt == DNSType::AAAA) {
          g_root.insert({rrdn, getIP(rr)});
          save_to_cache(makeDNSName("."), rrdn, getIP(rr));
        }
      }
      break;
    }
    catch(...){}
  }

  cout<<"Retrieved . NSSET from hints, have "<<g_root.size()<<" addresses"<<endl;

  sd_notify(0, "READY=1");

  if(argc == 5) { // be a server
    ComboAddress local(argv[1], 53);
    Socket sock(local.sin4.sin_family, SOCK_DGRAM);
    SBind(sock, local);
    string packet;
    ComboAddress client;

    for(;;) {
      try {
        packet = SRecvfrom(sock, 1500, client);
        cout<<"Received packet from "<< client.toStringWithPort() << endl;
        DNSMessageReader dmr(packet);
        if(dmr.dh.qr) {
          cout << "Packet from " << client.toStringWithPort()<< " was not a query"<<endl;
          continue;
        }
        std::thread t(processQuery, (int)sock, client, dmr);
        t.detach();
      }
      catch(exception& e) {
        cout << "Processing packet from " << client.toStringWithPort() <<": "<<e.what() << endl;
      }
    }
  }

  // single shot operation
  DNSName dn = makeDNSName(argv[1]);
  DNSType dt = makeDNSType(argv[2]);


  TDNSResolver tdr(g_root);
  ostringstream logstream;
  ostringstream dotstream;
  tdr.setLog(logstream);
  tdr.setPlot(dotstream);

  auto start = chrono::steady_clock::now();

  int rc = EXIT_SUCCESS;

  nlohmann::json jres;
  jres["name"]=dn.toString();
  jres["type"]=toString(dt);
  jres["intermediate"]= nlohmann::json::array();
  jres["answer"]= nlohmann::json::array();
  try {

    auto res = tdr.resolveAt(dn, dt);

    jres["numqueries"]=tdr.d_numqueries;
    cout<<"Result of query for "<< dn <<"|"<<toString(dt)<< " ("<<res.intermediate.size()<<" intermediate, "<<res.res.size()<<" actual)\n";
    for(const auto& r : res.intermediate) {
      jres["intermediate"].push_back(rrToJSON(r));
      cout<<r.name <<" "<<r.ttl<<" "<<r.rr->getType()<<" " << r.rr->toString()<<endl;
    }

    for(const auto& r : res.res) {
      jres["answer"].push_back(rrToJSON(r));
      cout<<r.name <<" "<<r.ttl<<" "<<r.rr->getType()<<" "<<r.rr->toString()<<endl;
    }
    cout<<"Used "<<tdr.d_numqueries << " queries"<<endl;
    jres["rcode"]=0;
  }
  catch(NxdomainException& e)
  {
    cout<<argv[1]<<": name does not exist"<<endl;
    cout<<"Used "<<tdr.d_numqueries << " queries"<<endl;
    rc=EXIT_FAILURE;
    jres["rcode"]=3;
  }
  catch(NodataException& e)
  {
    cout<<argv[1]<< ": name does not have datatype requested"<<endl;
    cout<<"Used "<<tdr.d_numqueries << " queries"<<endl;
    rc=EXIT_FAILURE;
    jres["rcode"]=0;
  }
  catch(TooManyQueriesException& e)
  {
    cout<<argv[1]<< ": exceeded maximum number of queries (" << tdr.d_numqueries<<")"<<endl;
    rc= EXIT_FAILURE;

    jres["rcode"]=2;
  }
  jres["numqueries"]=tdr.d_numqueries;
  jres["numtimeouts"]=tdr.d_numtimeouts;
  jres["numformerrs"]=tdr.d_numformerrs;
  jres["trace"]=logstream.str();
  auto finish = chrono::steady_clock::now();
  auto msecs = chrono::duration_cast<chrono::milliseconds>(finish-start);

  jres["msec"]= msecs.count();
  {
    tdr.endPlot();

    ofstream tmpstr(dn.toString()+"dot");
    tmpstr << dotstream.str();
    tmpstr.flush();
  }

  FILE* dotfp = popen(string("dot -Tsvg < "+dn.toString()+"dot").c_str(), "r");
  if(!dotfp) {
    cerr << "popen failed: " << strerror(errno) <<endl;
  }
  else {
    char buffer[100000];
    int siz = fread(buffer, 1, sizeof(buffer), dotfp);
    //    unlink(string(dn.toString()+"dot").c_str());
    jres["dot"]=std::string(buffer, siz);
    pclose(dotfp);
  }
  cout << jres << endl;

  ofstream logfile(dn.toString()+"txt");
  logfile << logstream.str();

  std::vector<std::uint8_t> v_cbor = nlohmann::json::to_cbor(jres);
  FILE* out = fopen("cbor", "w");
  fwrite(&v_cbor[0], 1, v_cbor.size(), out);
  fclose(out);
  return rc;
}
catch(std::exception& e)
{
  cerr<<argv[1]<<": fatal error: "<<e.what()<<endl;
  return EXIT_FAILURE;
}
