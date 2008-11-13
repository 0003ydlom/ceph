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

#ifndef __MESSAGE_H
#define __MESSAGE_H
 
/* public message types */
#include "include/types.h"

// monitor internal
#define MSG_MON_ELECTION           60
#define MSG_MON_PAXOS              61

/* monitor <-> mon admin tool */
#define MSG_MON_COMMAND            50
#define MSG_MON_COMMAND_ACK        51

// osd internal
#define MSG_OSD_PING         70
#define MSG_OSD_BOOT         71
#define MSG_OSD_FAILURE      72
#define MSG_OSD_ALIVE        73
#define MSG_OSD_IN           74
#define MSG_OSD_OUT          75

#define MSG_OSD_SUBOP        76
#define MSG_OSD_SUBOPREPLY   77

#define MSG_OSD_PG_NOTIFY      80
#define MSG_OSD_PG_QUERY       81
#define MSG_OSD_PG_SUMMARY     82
#define MSG_OSD_PG_LOG         83
#define MSG_OSD_PG_REMOVE      84
#define MSG_OSD_PG_INFO        85

#define MSG_PGSTATS    86
#define MSG_PGSTATSACK 87

#define MSG_OSD_PG_CREATE      88
#define MSG_REMOVE_SNAPS 89



// *** MDS ***

#define MSG_MDS_RESOLVE            0x200
#define MSG_MDS_RESOLVEACK         0x201
#define MSG_MDS_CACHEREJOIN        0x202
#define MSG_MDS_DISCOVER           0x203
#define MSG_MDS_DISCOVERREPLY      0x204
#define MSG_MDS_INODEUPDATE  0x205
#define MSG_MDS_DIRUPDATE    0x206
#define MSG_MDS_CACHEEXPIRE  0x207
#define MSG_MDS_DENTRYUNLINK      0x208
#define MSG_MDS_FRAGMENTNOTIFY 0x209

#define MSG_MDS_LOCK             0x300
#define MSG_MDS_INODEFILECAPS    0x301

#define MSG_MDS_EXPORTDIRDISCOVER     0x449
#define MSG_MDS_EXPORTDIRDISCOVERACK  0x450
#define MSG_MDS_EXPORTDIRCANCEL       0x451
#define MSG_MDS_EXPORTDIRPREP         0x452
#define MSG_MDS_EXPORTDIRPREPACK      0x453
#define MSG_MDS_EXPORTDIRWARNING      0x454
#define MSG_MDS_EXPORTDIRWARNINGACK   0x455
#define MSG_MDS_EXPORTDIR             0x456
#define MSG_MDS_EXPORTDIRACK          0x457
#define MSG_MDS_EXPORTDIRNOTIFY       0x458
#define MSG_MDS_EXPORTDIRNOTIFYACK    0x459
#define MSG_MDS_EXPORTDIRFINISH       0x460

#define MSG_MDS_EXPORTCAPS            0x470
#define MSG_MDS_EXPORTCAPSACK         0x471

#define MSG_MDS_BEACON             90  // to monitor
#define MSG_MDS_SLAVE_REQUEST      91
#define MSG_MDS_TABLE_REQUEST      92

#define MSG_MDS_HEARTBEAT          0x500  // for mds load balancer



#include <stdlib.h>

#include <iostream>
#include <list>
using std::list;

#include <ext/hash_map>


#include "include/types.h"
#include "include/buffer.h"
#include "msg_types.h"




// ======================================================

// abstract Message class


class Message {
protected:
  ceph_msg_header  header;      // headerelope
  ceph_msg_footer  footer;
  bufferlist       payload;  // "front" unaligned blob
  bufferlist       data;     // data payload (page-alignment will be preserved where possible)
  
  utime_t recv_stamp;

  friend class Messenger;

public:
  Message() {
    memset(&header, 0, sizeof(header));
  };
  Message(int t) {
    memset(&header, 0, sizeof(header));
    header.type = t;
    header.priority = 0;  // undef
    header.data_off = 0;
  }
  virtual ~Message() { }

  ceph_msg_header &get_header() { return header; }
  void set_header(const ceph_msg_header &e) { header = e; }
  void set_footer(const ceph_msg_footer &e) { footer = e; }
  ceph_msg_footer &get_footer() { return footer; }

  void clear_payload() { payload.clear(); }
  bool empty_payload() { return payload.length() == 0; }
  bufferlist& get_payload() { return payload; }
  void set_payload(bufferlist& bl) { payload.claim(bl); }
  void copy_payload(const bufferlist& bl) { payload = bl; }

  void set_data(bufferlist &d) { data.claim(d); }
  void copy_data(const bufferlist &d) { data = d; }
  bufferlist& get_data() { return data; }
  off_t get_data_len() { return data.length(); }

  void set_recv_stamp(utime_t t) { recv_stamp = t; }
  utime_t get_recv_stamp() { return recv_stamp; }

  void calc_header_crc() {
    header.crc = crc32c_le(0, (unsigned char*)&header,
			   sizeof(header) - sizeof(header.crc));
  }
  void calc_front_crc() {
    footer.front_crc = payload.crc32c(0);
  }
  void calc_data_crc() {
    footer.data_crc = data.crc32c(0);
  }

  // type
  int get_type() { return header.type; }
  void set_type(int t) { header.type = t; }

  unsigned get_seq() { return header.seq; }
  void set_seq(unsigned s) { header.seq = s; }

  unsigned get_priority() { return header.priority; }
  void set_priority(__s16 p) { header.priority = p; }

  // source/dest
  entity_inst_t get_dest_inst() { return entity_inst_t(header.dst); }
  entity_name_t get_dest() { return entity_name_t(header.dst.name); }
  void set_dest_inst(entity_inst_t& inst) { header.dst = inst; }

  entity_inst_t get_source_inst() { return entity_inst_t(header.src); }
  entity_name_t get_source() { return entity_name_t(header.src.name); }
  entity_addr_t get_source_addr() { return entity_addr_t(header.src.addr); }
  void set_source_inst(entity_inst_t& inst) { header.src = inst; }

  entity_inst_t get_orig_source_inst() { return entity_inst_t(header.orig_src); }
  entity_name_t get_orig_source() { return entity_name_t(header.orig_src.name); }
  entity_addr_t get_orig_source_addr() { return entity_addr_t(header.orig_src.addr); }
  void set_orig_source_inst(entity_inst_t &i) { header.orig_src = i; }

  // virtual bits
  virtual void decode_payload() = 0;
  virtual void encode_payload() = 0;
  virtual const char *get_type_name() = 0;
  virtual void print(ostream& out) {
    out << get_type_name();
  }
  
};

extern Message *decode_message(ceph_msg_header &header, ceph_msg_footer& footer,
			       bufferlist& front, bufferlist& data);
inline ostream& operator<<(ostream& out, Message& m) {
  m.print(out);
  return out;
}

#endif
