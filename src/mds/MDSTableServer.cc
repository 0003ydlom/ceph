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

#include "MDSTableServer.h"
#include "MDS.h"
#include "MDLog.h"
#include "msg/Messenger.h"

#include "messages/MMDSTableRequest.h"
#include "events/ETableServer.h"

#define dout(x)  if (x <= g_conf.debug_mds) *_dout << dbeginl << g_clock.now() << " " << mds->messenger->get_myname() << ".tableserver(" << get_mdstable_name(table) << ") "
#define derr(x)  if (x <= g_conf.debug_mds) *_derr << dbeginl << g_clock.now() << " " << mds->messenger->get_myname() << ".tableserver(" << get_mdstable_name(table) << ") "


void MDSTableServer::handle_request(MMDSTableRequest *req)
{
  assert(req->op >= 0);
  switch (req->op) {
  case TABLE_OP_QUERY: return handle_query(req);
  case TABLE_OP_PREPARE: return handle_prepare(req);
  case TABLE_OP_COMMIT: return handle_commit(req);
  case TABLE_OP_ROLLBACK: return handle_rollback(req);
  default: assert(0);
  }
}

// prepare

void MDSTableServer::handle_prepare(MMDSTableRequest *req)
{
  dout(7) << "handle_prepare " << *req << dendl;
  int from = req->get_source().num();

  _prepare(req->bl, req->reqid, from);
  pending_for_mds[version].mds = from;
  pending_for_mds[version].reqid = req->reqid;
  pending_for_mds[version].tid = version;

  ETableServer *le = new ETableServer(table, TABLE_OP_PREPARE, req->reqid, from, version, version);
  le->mutation = req->bl;
  mds->mdlog->submit_entry(le, new C_Prepare(this, req, version));
}

void MDSTableServer::_prepare_logged(MMDSTableRequest *req, version_t tid)
{
  dout(7) << "_create_logged " << *req << " tid " << tid << dendl;
  MMDSTableRequest *reply = new MMDSTableRequest(table, TABLE_OP_AGREE, req->reqid, tid);
  mds->send_message_mds(reply, req->get_source().num());
  delete req;
}


// commit

void MDSTableServer::handle_commit(MMDSTableRequest *req)
{
  dout(7) << "handle_commit " << *req << dendl;

  version_t tid = req->tid;

  if (pending_for_mds.count(tid)) {
    _commit(tid);
    pending_for_mds.erase(tid);
    mds->mdlog->submit_entry(new ETableServer(table, TABLE_OP_COMMIT, 0, -1, tid, version));
    mds->mdlog->wait_for_sync(new C_Commit(this, req));
  }
  else if (tid <= version) {
    dout(0) << "got commit for tid " << tid << " <= " << version 
	    << ", already committed, sending ack." 
	    << dendl;
    _commit_logged(req);
  } 
  else {
    // wtf.
    dout(0) << "got commit for tid " << tid << " > " << version << dendl;
    assert(tid <= version);
  }
}

void MDSTableServer::_commit_logged(MMDSTableRequest *req)
{
  dout(7) << "_commit_logged, sending ACK" << dendl;
  MMDSTableRequest *reply = new MMDSTableRequest(table, TABLE_OP_ACK, req->reqid, req->tid);
  mds->send_message_mds(reply, req->get_source().num());
  delete req;
}

// ROLLBACK

void MDSTableServer::handle_rollback(MMDSTableRequest *req)
{
  dout(7) << "handle_rollback " << *req << dendl;
  _rollback(req->tid);
  delete req;
}


// recovery

void MDSTableServer::finish_recovery()
{
  dout(7) << "finish_recovery" << dendl;
  handle_mds_recovery(-1);  // resend agrees for everyone.
}

void MDSTableServer::handle_mds_recovery(int who)
{
  if (who >= 0)
    dout(7) << "handle_mds_recovery mds" << who << dendl;
  
  // resend agrees for recovered mds
  for (map<version_t,_pending>::iterator p = pending_for_mds.begin();
       p != pending_for_mds.end();
       p++) {
    if (who >= 0 && p->second.mds != who)
      continue;
    MMDSTableRequest *reply = new MMDSTableRequest(table, TABLE_OP_AGREE, p->second.reqid, p->second.tid);
    mds->send_message_mds(reply, p->second.mds);
  }
}
