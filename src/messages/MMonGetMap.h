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

#ifndef __MMONGETMAP_H
#define __MMONGETMAP_H

#include "msg/Message.h"

#include "include/types.h"
#include "include/encodable.h"

class MMonGetMap : public Message {
 public:
  MMonGetMap() : Message(CEPH_MSG_MON_GET_MAP) { }

  const char *get_type_name() { return "mongetmap"; }
  
  void encode_payload() { }
  void decode_payload() { }
};

#endif
