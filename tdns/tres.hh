#pragma once

#include <map>
#include <vector>
#include "sclasses.hh"
#include "record-types.hh"

using namespace std;

extern multimap<DNSName, ComboAddress> g_root;


/** Helper function that extracts a useable IP address from an
    A or AAAA resource record. Returns sin_family == 0 if it didn't work */
static ComboAddress getIP(const std::unique_ptr<RRGen>& rr)
{
  ComboAddress ret;
  ret.sin4.sin_family = 0;
  if(auto ptr = dynamic_cast<AGen*>(rr.get()))
    ret=ptr->getIP();
  else if(auto ptr = dynamic_cast<AAAAGen*>(rr.get()))
    ret=ptr->getIP();

  ret.sin4.sin_port = htons(53);
  return ret;
}

struct TooManyQueriesException{};


class TDNSResolver
{
public:

  TDNSResolver(multimap<DNSName, ComboAddress>& root) : d_root(root)
  {}
  TDNSResolver()
  {}

  //! This describes a single resource record returned
  struct ResolveRR
  {
    DNSName name;
    uint32_t ttl;
    std::unique_ptr<RRGen> rr;
  };

  //! This is the end result of our resolving work
  struct ResolveResult
  {
    vector<ResolveRR> res; //!< what you asked for
    vector<ResolveRR> intermediate; //!< a CNAME chain that gets you there
    void clear()
    {
      res.clear();
      intermediate.clear();
    }
  };

  ResolveResult resolveAt(const DNSName& dn, const DNSType& dt, int depth=0, const DNSName& auth={});

  void setPlot(ostream& fs)
  {
    d_dot = &fs;
    (*d_dot) << "digraph { "<<endl;
  }

  void endPlot()
  {
    if(d_dot)
      (*d_dot) << "}\n";
  }

  void setLog(ostream& fs)
  {
    d_log = &fs;
  }

  ~TDNSResolver()
  {
  }
  DNSMessageReader getResponse(const ComboAddress& server, const DNSName& dn, const DNSType& dt, double timeout, bool doTCP = false, int depth=0);
private:
  void dotQuery(const DNSName& auth, const DNSName& server);
  void dotAnswer(const DNSName& dn, const DNSType& rrdt, const DNSName& server);
  void dotCNAME(const DNSName& target, const DNSName& server, const DNSName& dn);
  void dotDelegation(const DNSName& rrdn, const DNSName& server);
  multimap<DNSName, ComboAddress> d_root;
  unsigned int d_maxqueries{100};

  bool d_skipIPv6{false};
  ostream* d_dot{nullptr};
  ostream* d_log{nullptr};
  ostream& lstream()
  {
    return d_log ? *d_log : cout;
  }

public:
  unsigned int d_numqueries{0};
  unsigned int d_numtimeouts{0};
  unsigned int d_numformerrs{0};

};
