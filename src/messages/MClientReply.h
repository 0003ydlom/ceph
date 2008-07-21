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


#ifndef __MCLIENTREPLY_H
#define __MCLIENTREPLY_H

#include "include/types.h"
#include "MClientRequest.h"

#include "msg/Message.h"

#include <vector>
using namespace std;

/***
 *
 * MClientReply - container message for MDS reply to a client's MClientRequest
 *
 * key fields:
 *  long tid - transaction id, so the client can match up with pending request
 *  int result - error code, or fh if it was open
 *
 * for most requests:
 *  trace is a vector of InodeStat's tracing from root to the file/dir/whatever
 *  the operation referred to, so that the client can update it's info about what
 *  metadata lives on what MDS.
 *
 * for readdir replies:
 *  dir_contents is a vector of InodeStat*'s.  
 * 
 * that's mostly it, i think!
 *
 */


struct LeaseStat {
  // this matches ceph_mds_reply_lease
  __u16 mask;
  __u32 duration_ms;  
  void encode(bufferlist &bl) const {
    ::encode(mask, bl);
    ::encode(duration_ms, bl);
  }
  void decode(bufferlist::iterator &bl) {
    ::decode(mask, bl);
    ::decode(duration_ms, bl);
  }
};
WRITE_CLASS_ENCODER(LeaseStat)

struct DirStat {
  // mds distribution hints
  frag_t frag;
  __s32 auth;
  set<__s32> dist;
  
  DirStat() : auth(CDIR_AUTH_PARENT) {}
  DirStat(bufferlist::iterator& p) {
    decode(p);
  }

  void encode(bufferlist& bl) {
    ::encode(frag, bl);
    ::encode(auth, bl);
    ::encode(dist, bl);
  }
  void decode(bufferlist::iterator& p) {
    ::decode(frag, p);
    ::decode(auth, p);
    ::decode(dist, p);
  }

  // see CDir::encode_dirstat for encoder.
};

struct InodeStat {
  vinodeno_t vino;
  version_t version;
  ceph_file_layout layout;
  utime_t ctime, mtime, atime;
  unsigned mode, uid, gid, nlink, rdev;
  loff_t size, max_size;
  version_t time_warp_seq;

  frag_info_t dirstat;
  nest_info_t rstat;
  
  string  symlink;   // symlink content (if symlink)
  fragtree_t dirfragtree;
  map<string, bufferptr> xattrs;

 public:
  InodeStat() {}
  InodeStat(bufferlist::iterator& p) {
    decode(p);
  }

  void decode(bufferlist::iterator &p) {
    struct ceph_mds_reply_inode e;
    ::decode(e, p);
    vino.ino = inodeno_t(e.ino);
    vino.snapid = snapid_t(e.snapid);
    version = e.version;
    layout = e.layout;
    ctime.decode_timeval(&e.ctime);
    mtime.decode_timeval(&e.mtime);
    atime.decode_timeval(&e.atime);
    time_warp_seq = e.time_warp_seq;
    mode = e.mode;
    uid = e.uid;
    gid = e.gid;
    nlink = e.nlink;
    size = e.size;
    max_size = e.max_size;
    rdev = e.rdev;

    memset(&dirstat, 0, sizeof(dirstat));
    dirstat.nfiles = e.files;
    dirstat.nsubdirs = e.subdirs;

    rstat.rctime.decode_timeval(&e.rctime);
    rstat.rbytes = e.rbytes;
    rstat.rfiles = e.rfiles;
    rstat.rsubdirs = e.rsubdirs;

    int n = e.fragtree.nsplits;
    while (n) {
      ceph_frag_tree_split s;
      ::decode(s, p);
      dirfragtree._splits[(__u32)s.frag] = s.by;
      n--;
    }
    ::decode(symlink, p);

    bufferlist xbl;
    ::decode(xbl, p);
    if (xbl.length()) {
      bufferlist::iterator q = xbl.begin();
      ::decode(xattrs, q);
    }
  }
  
  // see CInode::encode_inodestat for encoder.
};


class MClientReply : public Message {
  // reply data
  struct ceph_mds_reply_head st;
  bufferlist trace_bl;
  bufferlist dir_bl;
public:
  bufferlist snapbl;

 public:
  long get_tid() { return st.tid; }
  int get_op() { return st.op; }

  void set_mdsmap_epoch(epoch_t e) { st.mdsmap_epoch = e; }
  epoch_t get_mdsmap_epoch() { return st.mdsmap_epoch; }

  int get_result() { return (__s32)(__u32)st.result; }

  unsigned get_file_caps() { return st.file_caps; }
  unsigned get_file_caps_seq() { return st.file_caps_seq; }
  unsigned get_file_caps_mseq() { return st.file_caps_mseq; }
  //uint64_t get_file_data_version() { return st.file_data_version; }
  
  void set_result(int r) { st.result = r; }
  void set_file_caps(unsigned char c) { st.file_caps = c; }
  void set_file_caps_seq(capseq_t s) { st.file_caps_seq = s; }
  void set_file_caps_mseq(capseq_t s) { st.file_caps_mseq = s; }
  //void set_file_data_version(uint64_t v) { st.file_data_version = v; }

  MClientReply() {}
  MClientReply(MClientRequest *req, int result = 0) : 
    Message(CEPH_MSG_CLIENT_REPLY) {
    memset(&st, 0, sizeof(st));
    this->st.tid = req->get_tid();
    this->st.op = req->get_op();
    this->st.result = result;
  }
  const char *get_type_name() { return "creply"; }
  void print(ostream& o) {
    o << "client_reply(" << env.dst.name << "." << st.tid;
    o << " = " << get_result();
    if (get_result() <= 0)
      o << " " << strerror(-get_result());
    o << ")";
  }

  // serialization
  virtual void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(st, p);
    ::decode(snapbl, p);
    ::decode(trace_bl, p);
    ::decode(dir_bl, p);
    assert(p.end());
  }
  virtual void encode_payload() {
    ::encode(st, payload);
    ::encode(snapbl, payload);
    ::encode(trace_bl, payload);
    ::encode(dir_bl, payload);
  }


  // dir contents
  void set_dir_bl(bufferlist& bl) {
    dir_bl.claim(bl);
  }
  bufferlist &get_dir_bl() {
    return dir_bl;
  }

  // trace
  void set_trace(bufferlist& bl) {
    trace_bl.claim(bl);
  }
  bufferlist& get_trace_bl() {
    return trace_bl;
  }
};

#endif
