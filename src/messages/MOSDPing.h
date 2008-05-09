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

#ifndef __MOSDPING_H
#define __MOSDPING_H

#include "common/Clock.h"

#include "msg/Message.h"
#include "osd/osd_types.h"


class MOSDPing : public Message {
 public:
  epoch_t map_epoch;
  bool ack;
  osd_peer_stat_t peer_stat;

  MOSDPing(epoch_t e, osd_peer_stat_t& ps, bool a=false) : 
    Message(MSG_OSD_PING), map_epoch(e), ack(a), peer_stat(ps) { }
  MOSDPing() {}

  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(map_epoch, p);
    ::decode(ack, p);
    ::decode(peer_stat, p);
  }
  void encode_payload() {
    ::encode(map_epoch, payload);
    ::encode(ack, payload);
    ::encode(peer_stat, payload);
  }

  const char *get_type_name() { return "osd_ping"; }
};

#endif
