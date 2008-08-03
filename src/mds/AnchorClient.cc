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

#include "AnchorClient.h"
#include "MDSMap.h"
#include "LogSegment.h"
#include "MDS.h"
#include "msg/Messenger.h"

#include "messages/MMDSTableRequest.h"

#include "config.h"

#define dout(x)  if (x <= g_conf.debug_mds) *_dout << dbeginl << g_clock.now() << " " << mds->messenger->get_myname() << ".anchorclient "
#define derr(x)  if (x <= g_conf.debug_mds) *_derr << dbeginl << g_clock.now() << " " << mds->messenger->get_myname() << ".anchorclient "



// LOOKUPS

void AnchorClient::handle_query_result(class MMDSTableRequest *m)
{
  dout(10) << "handle_anchor_reply " << *m << dendl;

  inodeno_t ino;
  vector<Anchor> trace;

  bufferlist::iterator p = m->bl.begin();
  ::decode(ino, p);

  assert(pending_lookup.count(ino));
  ::decode(*pending_lookup[ino].trace, p);
  Context *onfinish = pending_lookup[ino].onfinish;
  pending_lookup.erase(ino);
  
  if (onfinish) {
    onfinish->finish(0);
    delete onfinish;
  }
}

void AnchorClient::resend_queries()
{
  // resend any pending lookups.
  for (hash_map<inodeno_t, _pending_lookup>::iterator p = pending_lookup.begin();
       p != pending_lookup.end();
       p++) {
    dout(10) << "resending lookup on " << p->first << dendl;
    _lookup(p->first);
  }
}

void AnchorClient::lookup(inodeno_t ino, vector<Anchor>& trace, Context *onfinish)
{
  assert(pending_lookup.count(ino) == 0);
  _pending_lookup& l = pending_lookup[ino];
  l.onfinish = onfinish;
  l.trace = &trace;
  _lookup(ino);
}

void AnchorClient::_lookup(inodeno_t ino)
{
  MMDSTableRequest *req = new MMDSTableRequest(table, TABLE_OP_QUERY, 0, 0);
  ::encode(ino, req->bl);
  mds->send_message_mds(req, mds->mdsmap->get_tableserver());
}


// FRIENDLY PREPARE

void AnchorClient::prepare_create(inodeno_t ino, vector<Anchor>& trace, 
				  version_t *patid, Context *onfinish)
{
  dout(10) << "prepare_create " << ino << " " << trace << dendl;
  bufferlist bl;
  __u32 op = TABLE_OP_CREATE;
  ::encode(op, bl);
  ::encode(ino, bl);
  ::encode(trace, bl);
  _prepare(bl, patid, onfinish);
}

void AnchorClient::prepare_destroy(inodeno_t ino, 
				   version_t *patid, Context *onfinish)
{
  dout(10) << "prepare_destroy " << ino << dendl;
  bufferlist bl;
  __u32 op = TABLE_OP_DESTROY;
  ::encode(op, bl);
  ::encode(ino, bl);
  _prepare(bl, patid, onfinish);
}


void AnchorClient::prepare_update(inodeno_t ino, vector<Anchor>& trace, 
				  version_t *patid, Context *onfinish)
{
  dout(10) << "prepare_update " << ino << " " << trace << dendl;
  bufferlist bl;
  __u32 op = TABLE_OP_UPDATE;
  ::encode(op, bl);
  ::encode(ino, bl);
  ::encode(trace, bl);
  _prepare(bl, patid, onfinish);
}
