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

/* Object Store Device (OSD) Monitor
 */

#ifndef __OSDMONITOR_H
#define __OSDMONITOR_H

#include <map>
#include <set>
using namespace std;

#include "include/types.h"
#include "msg/Messenger.h"

#include "osd/OSDMap.h"

#include "PaxosService.h"

class Monitor;
class MOSDBoot;
class MMonCommand;

class OSDMonitor : public PaxosService {
public:
  OSDMap osdmap;

private:
  map<entity_inst_t, epoch_t> waiting_for_map;  // who -> start epoch

  // [leader]
  OSDMap::Incremental pending_inc;
  map<int,utime_t>    down_pending_out;  // osd down -> out

  map<int,double> osd_weight;

  // svc
public:  
  void create_initial();
private:
  bool update_from_paxos();
  void create_pending();  // prepare a new pending
  void encode_pending(bufferlist &bl);

  void committed();

  void handle_query(Message *m);
  bool preprocess_query(Message *m);  // true if processed.
  bool prepare_update(Message *m);
  bool should_propose(double &delay);

  // ...
  void send_to_waiting();     // send current map to waiters.
  void send_full(entity_inst_t dest);
  void send_incremental(entity_inst_t dest, epoch_t since);
  void bcast_latest_mds();
  void bcast_latest_osd();
  void bcast_full_osd();
 
  void handle_osd_getmap(class MOSDGetMap *m);

  bool preprocess_failure(class MOSDFailure *m);
  bool prepare_failure(class MOSDFailure *m);
  void _reported_failure(MOSDFailure *m);

  bool preprocess_boot(class MOSDBoot *m);
  bool prepare_boot(class MOSDBoot *m);
  void _booted(MOSDBoot *m);

  bool preprocess_in(class MOSDIn *m);
  bool prepare_in(class MOSDIn *m);
  void _in(MOSDIn *m);

  struct C_Booted : public Context {
    OSDMonitor *cmon;
    MOSDBoot *m;
    C_Booted(OSDMonitor *cm, MOSDBoot *m_) : 
      cmon(cm), m(m_) {}
    void finish(int r) {
      if (r >= 0)
	cmon->_booted(m);
      else
	cmon->dispatch((Message*)m);
    }
  };
  struct C_In : public Context {
    OSDMonitor *osdmon;
    MOSDIn *m;
    C_In(OSDMonitor *o, MOSDIn *mm) : osdmon(o), m(mm) {}
    void finish(int r) {
      osdmon->_in(m);
    }    
  };
  struct C_Reported : public Context {
    OSDMonitor *cmon;
    MOSDFailure *m;
    C_Reported(OSDMonitor *cm, MOSDFailure *m_) : 
      cmon(cm), m(m_) {}
    void finish(int r) {
      if (r >= 0)
	cmon->_reported_failure(m);
      else
	cmon->dispatch((Message*)m);
    }
  };

  bool preprocess_out(class MOSDOut *m);
  bool prepare_out(class MOSDOut *m);

 public:
  OSDMonitor(Monitor *mn, Paxos *p) : 
    PaxosService(mn, p) { }

  void tick();  // check state, take actions

  bool preprocess_command(MMonCommand *m);
  bool prepare_command(MMonCommand *m);

  void mark_all_down();

  void send_latest(entity_inst_t i, epoch_t start=0);

  void fake_osd_failure(int osd, bool down);
  void fake_osdmap_update();
  void fake_reorg();
};

#endif
