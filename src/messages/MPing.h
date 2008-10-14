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



#ifndef __MPING_H
#define __MPING_H

#include "msg/Message.h"
#include "include/encoding.h"

class MPing : public Message {
 public:
  MPing() : Message(CEPH_MSG_PING) {}

  void decode_payload() { }
  void encode_payload() { }
  const char *get_type_name() { return "ping"; }
};

#endif
