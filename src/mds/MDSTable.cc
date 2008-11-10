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

#include "MDSTable.h"

#include "MDS.h"
#include "MDLog.h"

#include "osdc/Filer.h"

#include "include/types.h"

#include "config.h"


#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "mds" << mds->get_nodeid() << "." << table_name << ": "


class C_MT_Save : public Context {
  MDSTable *ida;
  version_t version;
public:
  C_MT_Save(MDSTable *i, version_t v) : ida(i), version(v) {}
  void finish(int r) {
    ida->save_2(version);
  }
};

void MDSTable::save(Context *onfinish, version_t v)
{
  if (v > 0 && v <= committing_version) {
    dout(10) << "save v " << version << " - already saving "
	     << committing_version << " >= needed " << v << dendl;
    waitfor_save[v].push_back(onfinish);
    return;
  }
  
  dout(10) << "save v " << version << dendl;
  assert(is_active());
  
  bufferlist bl;
  ::encode(version, bl);
  encode_state(bl);

  committing_version = version;

  if (onfinish)
    waitfor_save[version].push_back(onfinish);

  // write (async)
  SnapContext snapc;
  object_t oid(ino, 0);
  mds->objecter->write_full(oid,
			    mds->objecter->osdmap->file_to_object_layout(oid,
									 g_default_mds_dir_layout),
			    snapc,
			    bl, 0,
			    NULL, new C_MT_Save(this, version));
}

void MDSTable::save_2(version_t v)
{
  dout(10) << "save_2 v " << v << dendl;
  
  committed_version = v;
  
  list<Context*> ls;
  while (!waitfor_save.empty()) {
    if (waitfor_save.begin()->first > v) break;
    ls.splice(ls.end(), waitfor_save.begin()->second);
    waitfor_save.erase(waitfor_save.begin());
  }
  finish_contexts(ls,0);
}


void MDSTable::reset()
{
  init_inode();
  reset_state();
  state = STATE_ACTIVE;
}



// -----------------------

class C_MT_Load : public Context {
public:
  MDSTable *ida;
  Context *onfinish;
  bufferlist bl;
  C_MT_Load(MDSTable *i, Context *o) : ida(i), onfinish(o) {}
  void finish(int r) {
    ida->load_2(r, bl, onfinish);
  }
};

void MDSTable::load(Context *onfinish)
{ 
  dout(10) << "load" << dendl;

  init_inode();

  assert(is_undef());
  state = STATE_OPENING;

  C_MT_Load *c = new C_MT_Load(this, onfinish);
  object_t oid(ino, 0);
  mds->objecter->read(oid,
		      mds->objecter->osdmap->file_to_object_layout(oid,
								   g_default_mds_dir_layout),
		      0, 0, // whole object
		      &c->bl, 0, c);
}

void MDSTable::load_2(int r, bufferlist& bl, Context *onfinish)
{
  assert(is_opening());
  state = STATE_ACTIVE;

  if (r > 0) {
    dout(10) << "load_2 got " << bl.length() << " bytes" << dendl;
    bufferlist::iterator p = bl.begin();
    ::decode(version, p);
    committed_version = version;
    decode_state(p);
  }
  else {
    dout(10) << "load_2 found no table" << dendl;
    assert(0); // this shouldn't happen if mkfs finished.
    reset();   
  }

  if (onfinish) {
    onfinish->finish(0);
    delete onfinish;
  }
}
