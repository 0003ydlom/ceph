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

#ifndef __MDS_EFRAGMENT_H
#define __MDS_EFRAGMENT_H

#include "../LogEvent.h"
#include "EMetaBlob.h"

class EFragment : public LogEvent {
public:
  EMetaBlob metablob;
  inodeno_t ino;
  frag_t basefrag;
  int bits;         // positive for split (from basefrag), negative for merge (to basefrag)

  EFragment() : LogEvent(EVENT_FRAGMENT) { }
  EFragment(MDLog *mdlog, inodeno_t i, frag_t bf, int b) : 
    LogEvent(EVENT_FRAGMENT), metablob(mdlog), 
    ino(i), basefrag(bf), bits(b) { }
  void print(ostream& out) {
    out << "EFragment " << ino << " " << basefrag << " by " << bits << " " << metablob;
  }

  void encode(bufferlist &bl) const {
    ::encode(ino, bl);
    ::encode(basefrag, bl);
    ::encode(bits, bl);
    ::encode(metablob, bl);
  } 
  void decode(bufferlist::iterator &bl) {
    ::decode(ino, bl);
    ::decode(basefrag, bl);
    ::decode(bits, bl);
    ::decode(metablob, bl);
  }

  void replay(MDS *mds);
};

#endif
