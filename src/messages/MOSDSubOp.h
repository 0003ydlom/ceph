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


#ifndef __MOSDSUBOP_H
#define __MOSDSUBOP_H

#include "msg/Message.h"
#include "osd/osd_types.h"

/*
 * OSD sub op - for internal ops on pobjects between primary and replicas(/stripes/whatever)
 */

class MOSDSubOp : public Message {
public:
  epoch_t map_epoch;
  
  // metadata from original request
  osd_reqid_t reqid;
  
  // subop
  pg_t pgid;
  pobject_t poid;
  
  bool is_modify, wants_reply;
  vector<ceph_osd_op> ops;

  // subop metadata
  tid_t rep_tid;
  eversion_t version, old_version;
  uint32_t inc_lock;

  SnapSet snapset;
  SnapContext snapc;
  
  // piggybacked osd/og state
  eversion_t pg_trim_to;   // primary->replica: trim to here
  osd_peer_stat_t peer_stat;

  map<string,bufferptr> attrset;

  interval_set<__u64> data_subset;
  map<pobject_t, interval_set<__u64> > clone_subsets;

 virtual void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(map_epoch, p);
    ::decode(reqid, p);
    ::decode(pgid, p);
    ::decode(poid, p);
    ::decode(ops, p);
    ::decode(rep_tid, p);
    ::decode(version, p);
    ::decode(old_version, p);
    ::decode(inc_lock, p);
    ::decode(snapset, p);
    ::decode(snapc, p);
    ::decode(pg_trim_to, p);
    ::decode(peer_stat, p);
    ::decode(attrset, p);
    ::decode(data_subset, p);
    ::decode(clone_subsets, p);
  }

  virtual void encode_payload() {
    ::encode(map_epoch, payload);
    ::encode(reqid, payload);
    ::encode(pgid, payload);
    ::encode(poid, payload);
    ::encode(ops, payload);
    ::encode(rep_tid, payload);
    ::encode(version, payload);
    ::encode(old_version, payload);
    ::encode(inc_lock, payload);
    ::encode(snapset, payload);
    ::encode(snapc, payload);
    ::encode(pg_trim_to, payload);
    ::encode(peer_stat, payload);
    ::encode(attrset, payload);
    ::encode(data_subset, payload);
    ::encode(clone_subsets, payload);
    if (ops.size())
      header.data_off = ops[0].offset;
    else
      header.data_off = 0;
  }


  MOSDSubOp(osd_reqid_t r, pg_t p, pobject_t po, vector<ceph_osd_op>& o, bool wr,
	    epoch_t mape, tid_t rtid, unsigned il, eversion_t v) :
    Message(MSG_OSD_SUBOP),
    map_epoch(mape),
    reqid(r),
    pgid(p),
    poid(po),
    wants_reply(wr),
    ops(o),
    rep_tid(rtid),
    version(v),
    inc_lock(il)
  {
    memset(&peer_stat, 0, sizeof(peer_stat));
  }
  MOSDSubOp() {}

  const char *get_type_name() { return "osd_sub_op"; }
  void print(ostream& out) {
    out << "osd_sub_op(" << reqid
	<< " " << poid
	<< " " << ops
	<< " v " << version
	<< " snapset=" << snapset << " snapc=" << snapc;    
    if (!data_subset.empty()) out << " subset " << data_subset;
    out << ")";
  }
};


#endif
