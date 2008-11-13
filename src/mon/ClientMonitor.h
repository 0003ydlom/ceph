// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */
 
/*
 * The Client Monitor is used for traking the filesystem's clients.
 */

#ifndef __CLIENTMONITOR_H
#define __CLIENTMONITOR_H

#include <map>
#include <set>
using namespace std;

#include "include/types.h"
#include "msg/Messenger.h"

#include "mds/MDSMap.h"

#include "PaxosService.h"

class Monitor;
class Paxos;
class MClientMount;
class MClientUnmount;
class MMonCommand;


struct client_info_t {
  entity_addr_t addr;
  utime_t mount_time;
  
  void encode(bufferlist& bl) const {
    ::encode(addr, bl);
    ::encode(mount_time, bl);
  }
  void decode(bufferlist::iterator& bl) {
    ::decode(addr, bl);
    ::decode(mount_time, bl);
  }
};
WRITE_CLASS_ENCODER(client_info_t)


class ClientMonitor : public PaxosService {
public:

  struct Incremental {
    version_t version;
    uint32_t next_client;
    map<int32_t, client_info_t> mount;
    set<int32_t> unmount;
    
    Incremental() : version(0), next_client() {}

    bool is_empty() { return mount.empty() && unmount.empty(); }
    void add_mount(uint32_t client, client_info_t& info) {
      next_client = MAX(next_client, client+1);
      mount[client] = info;
    }
    void add_unmount(uint32_t client) {
      assert(client < next_client);
      if (mount.count(client))
	mount.erase(client);
      else
	unmount.insert(client);
    }
    
    void encode(bufferlist &bl) const {
      ::encode(version, bl);
      ::encode(next_client, bl);
      ::encode(mount, bl);
      ::encode(unmount, bl);
    }
    void decode(bufferlist::iterator &bl) {
      ::decode(version, bl);
      ::decode(next_client, bl);
      ::decode(mount, bl);
      ::decode(unmount, bl);
    }
  };

  struct Map {
    version_t version;
    uint32_t next_client;
    map<uint32_t,client_info_t> client_info;
    map<entity_addr_t,uint32_t> addr_client;  // reverse map

    Map() : version(0), next_client(0) {}

    void reverse() {
      addr_client.clear();
      for (map<uint32_t,client_info_t>::iterator p = client_info.begin();
	   p != client_info.end();
	   ++p) {
	addr_client[p->second.addr] = p->first;
      }
    }
    void apply_incremental(Incremental &inc) {
      assert(inc.version == version+1);
      version = inc.version;
      next_client = inc.next_client;
      for (map<int32_t,client_info_t>::iterator p = inc.mount.begin();
	   p != inc.mount.end();
	   ++p) {
	client_info[p->first] = p->second;
	addr_client[p->second.addr] = p->first;
      }
	
      for (set<int32_t>::iterator p = inc.unmount.begin();
	   p != inc.unmount.end();
	   ++p) {
	assert(client_info.count(*p));
	addr_client.erase(client_info[*p].addr);
	client_info.erase(*p);
      }
    }

    void encode(bufferlist &bl) const {
      ::encode(version, bl);
      ::encode(next_client, bl);
      ::encode(client_info, bl);
    }
    void decode(bufferlist::iterator &bl) {
      ::decode(version, bl);
      ::decode(next_client, bl);
      ::decode(client_info, bl);
      reverse();
    }
  };

  class C_Mounted : public Context {
    ClientMonitor *cmon;
    int client;
    MClientMount *m;
  public:
    C_Mounted(ClientMonitor *cm, int c, MClientMount *m_) : 
      cmon(cm), client(c), m(m_) {}
    void finish(int r) {
      if (r >= 0)
	cmon->_mounted(client, m);
      else
	cmon->dispatch((Message*)m);
    }
  };

  class C_Unmounted : public Context {
    ClientMonitor *cmon;
    MClientUnmount *m;
  public:
    C_Unmounted(ClientMonitor *cm, MClientUnmount *m_) : 
      cmon(cm), m(m_) {}
    void finish(int r) {
      if (r >= 0)
	cmon->_unmounted(m);
      else
	cmon->dispatch((Message*)m);
    }
  };


  Map client_map;

private:
  // leader
  Incremental pending_inc;

  void create_initial();
  bool update_from_paxos();
  void create_pending();  // prepare a new pending
  void encode_pending(bufferlist &bl);  // propose pending update to peers

  void committed();

  void _mounted(int c, MClientMount *m);
  void _unmounted(MClientUnmount *m);
 
  bool preprocess_query(Message *m);  // true if processed.
  bool prepare_update(Message *m);

  bool preprocess_command(MMonCommand *m);  // true if processed.
  bool prepare_command(MMonCommand *m);

 public:
  ClientMonitor(Monitor *mn, Paxos *p) : PaxosService(mn, p) { }
  
  void tick();  // check state, take actions

};

#endif
