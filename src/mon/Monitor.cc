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

// TODO: missing run() method, which creates the two main timers, refreshTimer and readTimer

#include "Monitor.h"

#include "osd/OSDMap.h"

#include "MonitorStore.h"

#include "msg/Message.h"
#include "msg/Messenger.h"

#include "messages/MPing.h"
#include "messages/MPingAck.h"
#include "messages/MMonMap.h"
#include "messages/MMonGetMap.h"
#include "messages/MGenericMessage.h"
#include "messages/MMonCommand.h"
#include "messages/MMonCommandAck.h"

#include "messages/MMonPaxos.h"

#include "common/Timer.h"
#include "common/Clock.h"

#include "OSDMonitor.h"
#include "MDSMonitor.h"
#include "ClientMonitor.h"
#include "PGMonitor.h"

#include "config.h"

#define  dout(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) *_dout << dbeginl << g_clock.now() << " mon" << whoami << (is_starting() ? (const char*)"(starting)":(is_leader() ? (const char*)"(leader)":(is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << " "
#define  derr(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) *_derr << dbeginl << g_clock.now() << " mon" << whoami << (is_starting() ? (const char*)"(starting)":(is_leader() ? (const char*)"(leader)":(is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << " "

Monitor::Monitor(int w, MonitorStore *s, Messenger *m, MonMap *map) :
  whoami(w), 
  messenger(m),
  monmap(map),
  timer(lock), tick_timer(0),
  store(s),
  
  state(STATE_STARTING), stopping(false),
  
  elector(this, w),
  mon_epoch(0), 
  leader(0),
  
  paxos_mdsmap(this, w, PAXOS_MDSMAP),
  paxos_osdmap(this, w, PAXOS_OSDMAP),
  paxos_clientmap(this, w, PAXOS_CLIENTMAP),
  paxos_pgmap(this, w, PAXOS_PGMAP),
  
  osdmon(0), mdsmon(0), clientmon(0)
{
  osdmon = new OSDMonitor(this, &paxos_osdmap);
  mdsmon = new MDSMonitor(this, &paxos_mdsmap);
  clientmon = new ClientMonitor(this, &paxos_clientmap);
  pgmon = new PGMonitor(this, &paxos_pgmap);
}

Monitor::~Monitor()
{
  delete osdmon;
  delete mdsmon;
  delete clientmon;
  delete pgmon;
  delete messenger;
}

void Monitor::init()
{
  lock.Lock();
  
  dout(1) << "init fsid " << monmap->fsid << dendl;
  
  // init paxos
  paxos_osdmap.init();
  paxos_mdsmap.init();
  paxos_clientmap.init();
  paxos_pgmap.init();
  
  // i'm ready!
  messenger->set_dispatcher(this);
  
  // start ticker
  reset_tick();
  
  // call election?
  if (monmap->size() > 1) {
    assert(monmap->size() != 2); 
    call_election();
  } else {
    // we're standalone.
    set<int> q;
    q.insert(whoami);
    win_election(1, q);
  }
  
  lock.Unlock();
}

void Monitor::shutdown()
{
  dout(1) << "shutdown" << dendl;
  
  elector.shutdown();
  
  // clean up
  osdmon->shutdown();
  mdsmon->shutdown();
  clientmon->shutdown();
  pgmon->shutdown();

  // cancel all events
  cancel_tick();
  timer.cancel_all();
  timer.join();  
  
  // die.
  messenger->shutdown();
}


void Monitor::call_election()
{
  if (monmap->size() == 1) return;
  
  dout(10) << "call_election" << dendl;
  state = STATE_STARTING;
  
  // tell paxos
  paxos_mdsmap.election_starting();
  paxos_osdmap.election_starting();
  paxos_clientmap.election_starting();
  paxos_pgmap.election_starting();
  
  // call a new election
  elector.call_election();
}

void Monitor::win_election(epoch_t epoch, set<int>& active) 
{
  state = STATE_LEADER;
  leader = whoami;
  mon_epoch = epoch;
  quorum = active;
  dout(10) << "win_election, epoch " << mon_epoch << " quorum is " << quorum << dendl;
  
  // init paxos
  paxos_mdsmap.leader_init();
  paxos_osdmap.leader_init();
  paxos_clientmap.leader_init();
  paxos_pgmap.leader_init();
  
  // init
  pgmon->election_finished();  // hack: before osdmon, for osd->pg kick works ok
  osdmon->election_finished();
  mdsmon->election_finished();
  clientmon->election_finished();
} 

void Monitor::lose_election(epoch_t epoch, set<int> &q, int l) 
{
  state = STATE_PEON;
  mon_epoch = epoch;
  leader = l;
  quorum = q;
  dout(10) << "lose_election, epoch " << mon_epoch << " leader is mon" << leader
	   << " quorum is " << quorum << dendl;
  
  // init paxos
  paxos_mdsmap.peon_init();
  paxos_osdmap.peon_init();
  paxos_clientmap.peon_init();
  paxos_pgmap.peon_init();
  
  // init
  osdmon->election_finished();
  mdsmon->election_finished();
  clientmon->election_finished();
  pgmon->election_finished();
}

void Monitor::handle_command(MMonCommand *m)
{
  if (!ceph_fsid_equal(&m->fsid, &monmap->fsid)) {
    dout(0) << "handle_command on fsid " << m->fsid << " != " << monmap->fsid << dendl;
    reply_command(m, -EPERM, "wrong fsid");
    return;
  }

  // first time we've seen it?
  if (m->inst.addr.ipaddr.sin_addr.s_addr == htonl(INADDR_ANY)) {
    m->inst = m->get_source_inst();
    m->clear_payload();
  }

  dout(0) << "handle_command " << *m << dendl;
  string rs;
  if (!m->cmd.empty()) {
    if (m->cmd[0] == "mds") {
      mdsmon->dispatch(m);
      return;
    }
    if (m->cmd[0] == "osd") {
      osdmon->dispatch(m);
      return;
    }
    if (m->cmd[0] == "pg") {
      pgmon->dispatch(m);
      return;
    }
    if (m->cmd[0] == "stop") {
      shutdown();
      reply_command(m, 0, "stopping");
      return;
    }
    if (m->cmd[0] == "stop_cluster") {
      stop_cluster();
      reply_command(m, 0, "initiating cluster shutdown");
      return;
    }
    rs = "unrecognized subsystem";
  } else 
    rs = "no command";

  reply_command(m, -EINVAL, rs);
}

void Monitor::reply_command(MMonCommand *m, int rc, const string &rs)
{
  bufferlist rdata;
  reply_command(m, rc, rs, rdata);
}

void Monitor::reply_command(MMonCommand *m, int rc, const string &rs, bufferlist& rdata)
{
  MMonCommandAck *reply = new MMonCommandAck(rc, rs);
  reply->set_data(rdata);
  messenger->send_message(reply, m->inst);
  delete m;
}

void Monitor::stop_cluster()
{
  dout(0) << "stop_cluster -- initiating shutdown" << dendl;
  stopping = true;
  mdsmon->do_stop();
}


void Monitor::dispatch(Message *m)
{
  lock.Lock();
  {
    switch (m->get_type()) {
      
      // misc
    case CEPH_MSG_MON_GET_MAP:
      handle_mon_get_map((MMonGetMap*)m);
      break;

    case CEPH_MSG_PING_ACK:
      handle_ping_ack((MPingAck*)m);
      break;
      
    case CEPH_MSG_SHUTDOWN:
      if (m->get_source().is_osd()) 
	osdmon->dispatch(m);
      else
	handle_shutdown(m);
      break;
      
    case MSG_MON_COMMAND:
      handle_command((MMonCommand*)m);
      break;


      // OSDs
    case CEPH_MSG_OSD_GETMAP:
    case MSG_OSD_FAILURE:
    case MSG_OSD_BOOT:
    case MSG_OSD_IN:
    case MSG_OSD_OUT:
    case MSG_OSD_ALIVE:
      osdmon->dispatch(m);
      break;

      
      // MDSs
    case MSG_MDS_BEACON:
    case CEPH_MSG_MDS_GETMAP:
      mdsmon->dispatch(m);
      break;

      // clients
    case CEPH_MSG_CLIENT_MOUNT:
    case CEPH_MSG_CLIENT_UNMOUNT:
      clientmon->dispatch(m);
      break;

      // pg
    case CEPH_MSG_STATFS:
    case MSG_PGSTATS:
      pgmon->dispatch(m);
      break;


      // paxos
    case MSG_MON_PAXOS:
      {
	MMonPaxos *pm = (MMonPaxos*)m;

	// sanitize
	if (pm->epoch > mon_epoch) 
	  call_election();
	if (pm->epoch != mon_epoch) {
	  delete pm;
	  break;
	}

	// send it to the right paxos instance
	switch (pm->machine_id) {
	case PAXOS_OSDMAP:
	  paxos_osdmap.dispatch(m);
	  break;
	case PAXOS_MDSMAP:
	  paxos_mdsmap.dispatch(m);
	  break;
	case PAXOS_CLIENTMAP:
	  paxos_clientmap.dispatch(m);
	  break;
	case PAXOS_PGMAP:
	  paxos_pgmap.dispatch(m);
	  break;
	default:
	  assert(0);
	}
      }
      break;

      // elector messages
    case MSG_MON_ELECTION:
      elector.dispatch(m);
      break;

      
    default:
      dout(0) << "unknown message " << m << " " << *m << " from " << m->get_source_inst() << dendl;
      assert(0);
    }
  }
  lock.Unlock();
}

void Monitor::handle_mon_get_map(MMonGetMap *m)
{
  dout(10) << "handle_mon_get_map" << dendl;
  bufferlist bl;
  monmap->encode(bl);
  messenger->send_message(new MMonMap(bl), m->get_source_inst());
  delete m;
}


void Monitor::handle_shutdown(Message *m)
{
  assert(m->get_source().is_mon());
  if (m->get_source().num() == get_leader()) {
    dout(1) << "shutdown from leader " << m->get_source() << dendl;

    if (is_leader()) {
      // stop osds.
      set<int32_t> ls;
      osdmon->osdmap.get_all_osds(ls);
      for (set<int32_t>::iterator it = ls.begin(); it != ls.end(); it++) {
	if (osdmon->osdmap.is_down(*it)) continue;
	dout(10) << "sending shutdown to osd" << *it << dendl;
	messenger->send_message(new MGenericMessage(CEPH_MSG_SHUTDOWN),
				osdmon->osdmap.get_inst(*it));
      }
      osdmon->mark_all_down();
      
      // monitors too.
      for (unsigned i=0; i<monmap->size(); i++)
	if ((int)i != whoami)
	  messenger->send_message(new MGenericMessage(CEPH_MSG_SHUTDOWN), 
				  monmap->get_inst(i));
    }

    shutdown();
  } else {
    dout(1) << "ignoring shutdown from non-leader " << m->get_source() << dendl;
  }
  delete m;
}

void Monitor::handle_ping_ack(MPingAck *m)
{
  // ...
  
  delete m;
}




/************ TICK ***************/

class C_Mon_Tick : public Context {
  Monitor *mon;
public:
  C_Mon_Tick(Monitor *m) : mon(m) {}
  void finish(int r) {
    mon->tick();
  }
};

void Monitor::cancel_tick()
{
  if (tick_timer) timer.cancel_event(tick_timer);
}

void Monitor::reset_tick()
{
  cancel_tick();
  tick_timer = new C_Mon_Tick(this);
  timer.add_event_after(g_conf.mon_tick_interval, tick_timer);
}


void Monitor::tick()
{
  tick_timer = 0;

  // ok go.
  dout(11) << "tick" << dendl;
  
  osdmon->tick();
  mdsmon->tick();
  pgmon->tick();
  
  // next tick!
  reset_tick();
}





/*
 * this is the closest thing to a traditional 'mkfs' for ceph.
 * initialize the monitor state machines to their initial values.
 */
int Monitor::mkfs()
{
  // create it
  int err = store->mkfs();
  if (err < 0) {
    cerr << "error " << err << " " << strerror(err) << std::endl;
    exit(1);
  }
  
  store->put_int(whoami, "whoami", 0);

  bufferlist monmapbl;
  monmap->encode(monmapbl);
  store->put_bl_ss(monmapbl, "monmap", 0);

  list<PaxosService*> services;
  services.push_back(osdmon);
  services.push_back(mdsmon);
  services.push_back(clientmon);
  services.push_back(pgmon);
  for (list<PaxosService*>::iterator p = services.begin(); 
       p != services.end();
       p++) {
    PaxosService *svc = *p;
    dout(10) << "initializing " << svc->get_machine_name() << dendl;
    svc->paxos->init();
    svc->create_pending();
    svc->create_initial();

    // commit to paxos
    bufferlist bl;
    svc->encode_pending(bl);
    store->put_bl_sn(bl, svc->get_machine_name(), 1);
    store->put_int(1, svc->get_machine_name(), "last_committed");
  }

  return 0;
}


