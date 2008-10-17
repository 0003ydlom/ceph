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

#include "config.h"
#include "events/EString.h"
#include "events/ESubtreeMap.h"
#include "events/ESession.h"
#include "events/ESessions.h"

#include "events/EMetaBlob.h"

#include "events/EUpdate.h"
#include "events/ESlaveUpdate.h"
#include "events/EOpen.h"
#include "events/ECommitted.h"

#include "events/EPurgeFinish.h"

#include "events/EExport.h"
#include "events/EImportStart.h"
#include "events/EImportFinish.h"
#include "events/EFragment.h"

#include "events/ETableClient.h"
#include "events/ETableServer.h"


#include "LogSegment.h"

#include "MDS.h"
#include "MDLog.h"
#include "MDCache.h"
#include "Server.h"
#include "Migrator.h"

#include "InoTable.h"
#include "MDSTableClient.h"
#include "MDSTableServer.h"

#include "Locker.h"


#include "config.h"

#define  dout(l)    if (l<=g_conf.debug_mds || l <= g_conf.debug_mds_log || l <= g_conf.debug_mds_log_expire) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".journal "
#define  derr(l)    if (l<=g_conf.debug_mds || l <= g_conf.debug_mds_log || l <= g_conf.debug_mds_log_expire) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".journal "


// -----------------------
// LogSegment

class C_MDL_RetryExpireSegment : public Context {
public:
  MDS *mds;
  LogSegment *ls;
  C_MDL_RetryExpireSegment(MDS *m, LogSegment *l) : mds(m), ls(l) {}
  void finish(int r) {
    ls->try_to_expire(mds);
  }
};

C_Gather *LogSegment::try_to_expire(MDS *mds)
{
  C_Gather *gather = 0;

  set<CDir*> commit;

  dout(6) << "LogSegment(" << offset << ").try_to_expire" << dendl;

  // commit dirs
  for (xlist<CDir*>::iterator p = new_dirfrags.begin(); !p.end(); ++p) {
    dout(20) << " new_dirfrag " << **p << dendl;
    assert((*p)->is_auth());
    commit.insert(*p);
  }
  for (xlist<CDir*>::iterator p = dirty_dirfrags.begin(); !p.end(); ++p) {
    dout(20) << " dirty_dirfrag " << **p << dendl;
    assert((*p)->is_auth());
    commit.insert(*p);
  }
  for (xlist<CDentry*>::iterator p = dirty_dentries.begin(); !p.end(); ++p) {
    dout(20) << " dirty_dentry " << **p << dendl;
    assert((*p)->is_auth());
    commit.insert((*p)->get_dir());
  }
  for (xlist<CInode*>::iterator p = dirty_inodes.begin(); !p.end(); ++p) {
    dout(20) << " dirty_inode " << **p << dendl;
    assert((*p)->is_auth());
    commit.insert((*p)->get_parent_dn()->get_dir());
  }

  if (!commit.empty()) {
    if (!gather) gather = new C_Gather;
    
    for (set<CDir*>::iterator p = commit.begin();
	 p != commit.end();
	 ++p) {
      CDir *dir = *p;
      assert(dir->is_auth());
      if (dir->can_auth_pin()) {
	dout(15) << "try_to_expire committing " << *dir << dendl;
	dir->commit(0, gather->new_sub());
      } else {
	dout(15) << "try_to_expire waiting for unfreeze on " << *dir << dendl;
	dir->add_waiter(CDir::WAIT_UNFREEZE, gather->new_sub());
      }
    }
  }

  // master ops with possibly uncommitted slaves
  for (set<metareqid_t>::iterator p = uncommitted_masters.begin();
       p != uncommitted_masters.end();
       p++) {
    dout(10) << "try_to_expire waiting for slaves to ack commit on " << *p << dendl;
    if (!gather) gather = new C_Gather;
    mds->mdcache->wait_for_uncommitted_master(*p, gather->new_sub());
  }

  // nudge scatterlocks
  for (xlist<CInode*>::iterator p = dirty_dirfrag_dir.begin(); !p.end(); ++p) {
    CInode *in = *p;
    dout(10) << "try_to_expire waiting for dirlock flush on " << *in << dendl;
    if (!gather) gather = new C_Gather;
    mds->locker->scatter_nudge(&in->dirlock, gather->new_sub());
  }
  for (xlist<CInode*>::iterator p = dirty_dirfrag_dirfragtree.begin(); !p.end(); ++p) {
    CInode *in = *p;
    dout(10) << "try_to_expire waiting for dirfragtreelock flush on " << *in << dendl;
    if (!gather) gather = new C_Gather;
    mds->locker->scatter_nudge(&in->dirfragtreelock, gather->new_sub());
  }
  for (xlist<CInode*>::iterator p = dirty_dirfrag_nest.begin(); !p.end(); ++p) {
    CInode *in = *p;
    dout(10) << "try_to_expire waiting for nest flush on " << *in << dendl;
    if (!gather) gather = new C_Gather;
    mds->locker->scatter_nudge(&in->nestlock, gather->new_sub());
  }

  // open files
  if (!open_files.empty()) {
    assert(!mds->mdlog->is_capped()); // hmm FIXME
    EOpen *le = 0;
    LogSegment *ls = mds->mdlog->get_current_segment();
    for (xlist<CInode*>::iterator p = open_files.begin(); !p.end(); ++p) {
      CInode *in = *p;
      dout(20) << "try_to_expire requeueing open file " << *in << dendl;
      if (!le) le = new EOpen(mds->mdlog);
      le->add_clean_inode(in);
      ls->open_files.push_back(&in->xlist_open_file);
    }
    if (le) {
      if (!gather) gather = new C_Gather;
      mds->mdlog->submit_entry(le, gather->new_sub());
      dout(10) << "try_to_expire waiting for open files to rejournal" << dendl;
    }
  }

  // slave updates
  for (xlist<MDSlaveUpdate*>::iterator p = slave_updates.begin(); !p.end(); ++p) {
    MDSlaveUpdate *su = *p;
    dout(10) << "try_to_expire waiting on slave update " << su << dendl;
    assert(su->waiter == 0);
    if (!gather) gather = new C_Gather;
    su->waiter = gather->new_sub();
  }

  // idalloc
  if (inotablev > mds->inotable->get_committed_version()) {
    dout(10) << "try_to_expire saving inotable table, need " << inotablev
	      << ", committed is " << mds->inotable->get_committed_version()
	      << " (" << mds->inotable->get_committing_version() << ")"
	      << dendl;
    if (!gather) gather = new C_Gather;
    mds->inotable->save(gather->new_sub(), inotablev);
  }

  // sessionmap
  if (sessionmapv > mds->sessionmap.committed) {
    dout(10) << "try_to_expire saving sessionmap, need " << sessionmapv 
	      << ", committed is " << mds->sessionmap.committed
	      << " (" << mds->sessionmap.committing << ")"
	      << dendl;
    if (!gather) gather = new C_Gather;
    mds->sessionmap.save(gather->new_sub(), sessionmapv);
  }

  // pending commit atids
  for (map<int, hash_set<version_t> >::iterator p = pending_commit_tids.begin();
       p != pending_commit_tids.end();
       ++p) {
    MDSTableClient *client = mds->get_table_client(p->first);
    for (hash_set<version_t>::iterator q = p->second.begin();
	 q != p->second.end();
	 ++q) {
      if (!gather) gather = new C_Gather;
      dout(10) << "try_to_expire " << get_mdstable_name(p->first) << " transaction " << *q 
	       << " pending commit (not yet acked), waiting" << dendl;
      assert(!client->has_committed(*q));
      client->wait_for_ack(*q, gather->new_sub());
    }
  }
  
  // table servers
  for (map<int, version_t>::iterator p = tablev.begin();
       p != tablev.end();
       p++) {
    MDSTableServer *server = mds->get_table_server(p->first);
    if (p->second > server->get_committed_version()) {
      dout(10) << "try_to_expire waiting for " << get_mdstable_name(p->first) 
	       << " to save, need " << p->second << dendl;
      if (!gather) gather = new C_Gather;
      server->save(gather->new_sub());
    }
  }

  // FIXME client requests...?
  // audit handling of anchor transactions?

  // once we are otherwise trimmable, make sure journal is fully safe on disk.
  if (!gather) {
    if (!trimmable_at)
      trimmable_at = mds->mdlog->get_write_pos();

    if (trimmable_at <= mds->mdlog->get_safe_pos()) {
      dout(6) << "LogSegment(" << offset << ").try_to_expire trimmable at " << trimmable_at
	      << " <= " << mds->mdlog->get_safe_pos() << dendl;
    } else {
      dout(6) << "LogSegment(" << offset << ").try_to_expire trimmable at " << trimmable_at
	      << " > " << mds->mdlog->get_safe_pos()
	      << ", waiting for safe journal flush" << dendl;
      if (!gather) gather = new C_Gather;
      mds->mdlog->wait_for_safe(gather->new_sub());
    }
  }

  if (gather) {
    dout(6) << "LogSegment(" << offset << ").try_to_expire waiting" << dendl;
  } else {
    dout(6) << "LogSegment(" << offset << ").try_to_expire success" << dendl;
  }
  return gather;
}



#undef dout
#undef derr
#define  dout(l)    if (l<=g_conf.debug_mds || l <= g_conf.debug_mds_log) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".journal "
#define  derr(l)    if (l<=g_conf.debug_mds || l <= g_conf.debug_mds_log) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".journal "


// -----------------------
// EString

void EString::replay(MDS *mds)
{
  dout(10) << "EString.replay " << event << dendl; 
}



// -----------------------
// EMetaBlob

EMetaBlob::EMetaBlob(MDLog *mdlog) :
  last_subtree_map(mdlog->get_last_segment_offset()),
  my_offset(mdlog->get_write_pos()) 
{
}

void EMetaBlob::update_segment(LogSegment *ls)
{
  // atids?
  //for (list<version_t>::iterator p = atids.begin(); p != atids.end(); ++p)
  //  ls->pending_commit_atids[*p] = ls;
  // -> handled directly by AnchorClient

  // dirty inode mtimes
  // -> handled directly by Server.cc, replay()

  // alloc table update?
  if (!allocated_inos.empty())
    ls->inotablev = inotablev;

  // truncated inodes
  // -> handled directly by Server.cc

  // client requests
  //  note the newest request per client
  //if (!client_reqs.empty())
    //    ls->last_client_tid[client_reqs.rbegin()->client] = client_reqs.rbegin()->tid);
}

void EMetaBlob::replay(MDS *mds, LogSegment *logseg)
{
  dout(10) << "EMetaBlob.replay " << lump_map.size() << " dirlumps" << dendl;

  if (!logseg) logseg = _segment;
  assert(logseg);

  // walk through my dirs (in order!)
  for (list<dirfrag_t>::iterator lp = lump_order.begin();
       lp != lump_order.end();
       ++lp) {
    dout(10) << "EMetaBlob.replay dir " << *lp << dendl;
    dirlump &lump = lump_map[*lp];

    // the dir 
    CDir *dir = mds->mdcache->get_dirfrag(*lp);
    if (!dir) {
      // hmm.  do i have the inode?
      CInode *diri = mds->mdcache->get_inode((*lp).ino);
      if (!diri) {
	if ((*lp).ino == MDS_INO_ROOT) {
	  diri = mds->mdcache->create_root_inode();
	  dout(10) << "EMetaBlob.replay created root " << *diri << dendl;
	} else if (MDS_INO_IS_STRAY((*lp).ino)) {
	  int whose = (*lp).ino - MDS_INO_STRAY_OFFSET;
	  diri = mds->mdcache->create_stray_inode(whose);
	  dout(10) << "EMetaBlob.replay created stray " << *diri << dendl;
	} else {
	  dout(0) << "EMetaBlob.replay missing dir ino  " << (*lp).ino << dendl;
	  assert(0);
	}
      }
      // create the dirfrag
      dir = diri->get_or_open_dirfrag(mds->mdcache, (*lp).frag);

      if ((*lp).ino < MDS_INO_BASE) 
	mds->mdcache->adjust_subtree_auth(dir, CDIR_AUTH_UNKNOWN);

      dout(10) << "EMetaBlob.replay added dir " << *dir << dendl;  
    }
    dir->set_version( lump.fnode.version );
    if (lump.is_dirty()) {
      dir->_mark_dirty(logseg);
      dir->get_inode()->dirlock.set_updated();
    }

    if (lump.is_complete())
      dir->mark_complete();
    
    // decode bits
    lump._decode_bits();

    // full dentry+inode pairs
    for (list<fullbit>::iterator p = lump.get_dfull().begin();
	 p != lump.get_dfull().end();
	 p++) {
      CDentry *dn = dir->lookup(p->dn, p->dnlast);
      if (!dn) {
	dn = dir->add_null_dentry(p->dn, p->dnfirst, p->dnlast);
	dn->set_version(p->dnv);
	if (p->dirty) dn->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay added " << *dn << dendl;
      } else {
	dn->set_version(p->dnv);
	if (p->dirty) dn->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay for [" << p->dnfirst << "," << p->dnlast << "] had " << *dn << dendl;
	dn->first = p->dnfirst;
	assert(dn->last == p->dnlast);
      }

      CInode *in = mds->mdcache->get_inode(p->inode.ino, p->dnlast);
      if (!in) {
	in = new CInode(mds->mdcache, true, p->dnfirst, p->dnlast);
	in->inode = p->inode;
	in->xattrs = p->xattrs;
	if (in->inode.is_dir()) {
	  in->dirfragtree = p->dirfragtree;
	  /*
	   * we can do this before linking hte inode bc the split_at would
	   * be a no-op.. we have no children (namely open snaprealms) to
	   * divy up 
	   */
	  in->decode_snap_blob(p->snapbl);  
	}
	if (in->inode.is_symlink()) in->symlink = p->symlink;
	mds->mdcache->add_inode(in);
	if (!dn->is_null()) {
	  if (dn->is_primary())
	    dout(-10) << "EMetaBlob.replay FIXME had dentry linked to wrong inode " << *dn 
		     << " " << *dn->get_inode()
		     << " should be " << p->inode.ino
		     << dendl;
	  dir->unlink_inode(dn);
	  //assert(0); // hrm!  fallout from sloppy unlink?  or?  hmmm FIXME investigate further
	}
	dir->link_primary_inode(dn, in);
	if (p->dirty) in->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay added " << *in << dendl;
      } else {
	if (dn->get_inode() != in && in->get_parent_dn()) {
	  dout(10) << "EMetaBlob.replay unlinking " << *in << dendl;
	  in->get_parent_dn()->get_dir()->unlink_inode(in->get_parent_dn());
	}
	if (in->get_parent_dn() && in->inode.anchored != p->inode.anchored)
	  in->get_parent_dn()->adjust_nested_anchors( (int)p->inode.anchored - (int)in->inode.anchored );
	in->inode = p->inode;
	in->xattrs = p->xattrs;
	if (in->inode.is_dir()) {
	  in->dirfragtree = p->dirfragtree;
	  in->decode_snap_blob(p->snapbl);
	}
	if (in->inode.is_symlink()) in->symlink = p->symlink;
	if (p->dirty) in->_mark_dirty(logseg);
	if (dn->get_inode() != in) {
	  if (!dn->is_null())  // note: might be remote.  as with stray reintegration.
	    dir->unlink_inode(dn);
	  dir->link_primary_inode(dn, in);
	  dout(10) << "EMetaBlob.replay linked " << *in << dendl;
	} else {
	  dout(10) << "EMetaBlob.replay for [" << p->dnfirst << "," << p->dnlast << "] had " << *in << dendl;
	}
   	in->first = p->dnfirst;
      }
    }

    // remote dentries
    for (list<remotebit>::iterator p = lump.get_dremote().begin();
	 p != lump.get_dremote().end();
	 p++) {
      CDentry *dn = dir->lookup(p->dn, p->dnlast);
      if (!dn) {
	dn = dir->add_remote_dentry(p->dn, p->ino, p->d_type, p->dnfirst, p->dnlast);
	dn->set_version(p->dnv);
	if (p->dirty) dn->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay added " << *dn << dendl;
      } else {
	if (!dn->is_null()) {
	  dout(10) << "EMetaBlob.replay unlinking " << *dn << dendl;
	  dir->unlink_inode(dn);
	}
	dn->set_remote(p->ino, p->d_type);
	dn->set_version(p->dnv);
	if (p->dirty) dn->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay for [" << p->dnfirst << "," << p->dnlast << "] had " << *dn << dendl;
	dn->first = p->dnfirst;
	assert(dn->last == p->dnlast);
      }
    }

    // null dentries
    for (list<nullbit>::iterator p = lump.get_dnull().begin();
	 p != lump.get_dnull().end();
	 p++) {
      CDentry *dn = dir->lookup(p->dn, p->dnfirst);
      if (!dn) {
	dn = dir->add_null_dentry(p->dn, p->dnfirst, p->dnlast);
	dn->set_version(p->dnv);
	if (p->dirty) dn->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay added " << *dn << dendl;
      } else {
	dn->first = p->dnfirst;
	if (!dn->is_null()) {
	  dout(10) << "EMetaBlob.replay unlinking " << *dn << dendl;
	  dir->unlink_inode(dn);
	}
	dn->set_version(p->dnv);
	if (p->dirty) dn->_mark_dirty(logseg);
	dout(10) << "EMetaBlob.replay had " << *dn << dendl;
	assert(dn->last == p->dnlast);
      }
    }
  }

  // table client transactions
  for (list<pair<__u8,version_t> >::iterator p = table_tids.begin();
       p != table_tids.end();
       ++p) {
    dout(10) << "EMetaBlob.replay noting " << get_mdstable_name(p->first)
	     << " transaction " << p->second << dendl;
    MDSTableClient *client = mds->get_table_client(p->first);
    client->got_journaled_agree(p->second, logseg);
    logseg->pending_commit_tids[p->first].insert(p->second);
  }

  // allocated_inos
  if (!allocated_inos.empty()) {
    if (mds->inotable->get_version() >= inotablev) {
      dout(10) << "EMetaBlob.replay inotable tablev " << inotablev
	       << " <= table " << mds->inotable->get_version() << dendl;
    } else {
      for (list<inodeno_t>::iterator p = allocated_inos.begin();
	   p != allocated_inos.end();
	   ++p) {
	dout(10) << " EMetaBlob.replay inotable " << *p << " tablev " << inotablev
		 << " - 1 == table " << mds->inotable->get_version() << dendl;
	assert(inotablev-1 == mds->inotable->get_version());
	
	inodeno_t ino = mds->inotable->alloc_id();
	assert(ino == *p);       // this should match.
      }	
      assert(inotablev == mds->inotable->get_version());
    }
  }

  // truncated inodes
  for (list< triple<inodeno_t,uint64_t,uint64_t> >::iterator p = truncated_inodes.begin();
       p != truncated_inodes.end();
       ++p) {
    CInode *in = mds->mdcache->get_inode(p->first);
    assert(in);
    dout(10) << "EMetaBlob.replay will purge truncated " 
	     << p->third << " -> " << p->second
	     << " on " << *in << dendl;
    mds->mdcache->add_recovered_purge(in, p->second, p->third, logseg);
  }

  // client requests
  for (list<metareqid_t>::iterator p = client_reqs.begin();
       p != client_reqs.end();
       ++p)
    if (p->name.is_client())
      mds->sessionmap.add_completed_request(*p);


  // update segment
  update_segment(logseg);
}

// -----------------------
// ESession

void ESession::update_segment()
{
  _segment->sessionmapv = cmapv;
}

void ESession::replay(MDS *mds)
{
  if (mds->sessionmap.version >= cmapv) {
    dout(10) << "ESession.replay sessionmap " << mds->sessionmap.version 
	     << " >= " << cmapv << ", noop" << dendl;

    // hrm, this isn't very pretty.
    if (!open)
      mds->sessionmap.trim_completed_requests(client_inst.name, 0);

  } else {
    dout(10) << "ESession.replay sessionmap " << mds->sessionmap.version
	     << " < " << cmapv << " " << (open ? "open":"close") << dendl;
    mds->sessionmap.projected = ++mds->sessionmap.version;
    assert(mds->sessionmap.version == cmapv);
    if (open) {
      Session *session = mds->sessionmap.get_or_add_session(client_inst);
      session->last_cap_renew = g_clock.now();
      mds->sessionmap.set_state(session, Session::STATE_OPEN);
    } else {
      Session *session = mds->sessionmap.get_session(client_inst.name);
      if (session)
	mds->sessionmap.remove_session(session);
    }
  }
  _segment->sessionmapv = cmapv;
}

void ESessions::update_segment()
{
  _segment->sessionmapv = cmapv;
}

void ESessions::replay(MDS *mds)
{
  if (mds->sessionmap.version >= cmapv) {
    dout(10) << "ESessions.replay sessionmap " << mds->sessionmap.version
	     << " >= " << cmapv << ", noop" << dendl;
  } else {
    dout(10) << "ESessions.replay sessionmap " << mds->sessionmap.version
	     << " < " << cmapv << dendl;
    mds->sessionmap.open_sessions(client_map);
    assert(mds->sessionmap.version == cmapv);
    mds->sessionmap.projected = mds->sessionmap.version;
  }
  _segment->sessionmapv = cmapv;
}




void ETableServer::update_segment()
{
  _segment->tablev[table] = version;
}

void ETableServer::replay(MDS *mds)
{
  MDSTableServer *server = mds->get_table_server(table);
  if (server->get_version() >= version) {
    dout(10) << "ETableServer.replay " << get_mdstable_name(table)
	     << " " << get_mdstableserver_opname(op)
	     << " event " << version
	     << " <= table " << server->get_version() << dendl;
    return;
  }
  
  dout(10) << " ETableServer.replay " << get_mdstable_name(table)
	   << " " << get_mdstableserver_opname(op)
	   << " event " << version << " - 1 == table " << server->get_version() << dendl;
  assert(version-1 == server->get_version());
  
  switch (op) {
  case TABLESERVER_OP_PREPARE:
    server->_prepare(mutation, reqid, bymds);
    break;
  case TABLESERVER_OP_COMMIT:
    server->_commit(tid);
    break;
  case TABLESERVER_OP_SERVER_UPDATE:
    server->_server_update(mutation);
    break;
  default:
    assert(0);
  }
  
  assert(version == server->get_version());
}


void ETableClient::replay(MDS *mds)
{
  dout(10) << " ETableClient.replay " << get_mdstable_name(table)
	   << " op " << get_mdstableserver_opname(op)
	   << " tid " << tid << dendl;
    
  MDSTableClient *client = mds->get_table_client(table);
  assert(op == TABLESERVER_OP_ACK);
  client->got_journaled_ack(tid);
}


// -----------------------
// ESnap
/*
void ESnap::update_segment()
{
  _segment->tablev[TABLE_SNAP] = version;
}

void ESnap::replay(MDS *mds)
{
  if (mds->snaptable->get_version() >= version) {
    dout(10) << "ESnap.replay event " << version
	     << " <= table " << mds->snaptable->get_version() << dendl;
    return;
  } 
  
  dout(10) << " ESnap.replay event " << version
	   << " - 1 == table " << mds->snaptable->get_version() << dendl;
  assert(version-1 == mds->snaptable->get_version());

  if (create) {
    version_t v;
    snapid_t s = mds->snaptable->create(snap.dirino, snap.name, snap.stamp, &v);
    assert(s == snap.snapid);
  } else {
    mds->snaptable->remove(snap.snapid);
  }

  assert(version == mds->snaptable->get_version());
}
*/



// -----------------------
// EUpdate

void EUpdate::update_segment()
{
  metablob.update_segment(_segment);

  if (had_slaves)
    _segment->uncommitted_masters.insert(reqid);
}

void EUpdate::replay(MDS *mds)
{
  metablob.replay(mds, _segment);

  if (had_slaves) {
    dout(10) << "EUpdate.replay " << reqid << " had slaves, expecting a matching ECommitted" << dendl;
    _segment->uncommitted_masters.insert(reqid);
    set<int> slaves;
    mds->mdcache->add_uncommitted_master(reqid, _segment, slaves);
  }
}


// ------------------------
// EOpen

void EOpen::update_segment()
{
  // ??
}

void EOpen::replay(MDS *mds)
{
  dout(10) << "EOpen.replay " << dendl;
  metablob.replay(mds, _segment);

  // note which segments inodes belong to, so we don't have to start rejournaling them
  for (list<inodeno_t>::iterator p = inos.begin();
       p != inos.end();
       p++) {
    CInode *in = mds->mdcache->get_inode(*p);
    assert(in); 
    _segment->open_files.push_back(&in->xlist_open_file);
  }
}


// -----------------------
// ECommitted

void ECommitted::replay(MDS *mds)
{
  if (mds->mdcache->uncommitted_masters.count(reqid)) {
    dout(10) << "ECommitted.replay " << reqid << dendl;
    mds->mdcache->uncommitted_masters[reqid].ls->uncommitted_masters.erase(reqid);
    mds->mdcache->uncommitted_masters.erase(reqid);
  } else {
    dout(10) << "ECommitted.replay " << reqid << " -- didn't see original op" << dendl;
  }
}



// -----------------------
// ESlaveUpdate

void ESlaveUpdate::replay(MDS *mds)
{
  switch (op) {
  case ESlaveUpdate::OP_PREPARE:
    dout(10) << "ESlaveUpdate.replay prepare " << reqid << " for mds" << master 
	     << ": applying commit, saving rollback info" << dendl;
    assert(mds->mdcache->uncommitted_slave_updates[master].count(reqid) == 0);
    commit.replay(mds, _segment);
    mds->mdcache->uncommitted_slave_updates[master][reqid] = 
      new MDSlaveUpdate(origop, rollback, _segment->slave_updates);
    break;

  case ESlaveUpdate::OP_COMMIT:
    if (mds->mdcache->uncommitted_slave_updates[master].count(reqid)) {
      dout(10) << "ESlaveUpdate.replay commit " << reqid << " for mds" << master << dendl;
      delete mds->mdcache->uncommitted_slave_updates[master][reqid];
      mds->mdcache->uncommitted_slave_updates[master].erase(reqid);
      if (mds->mdcache->uncommitted_slave_updates[master].empty())
	mds->mdcache->uncommitted_slave_updates.erase(master);
    } else {
      dout(10) << "ESlaveUpdate.replay commit " << reqid << " for mds" << master 
	       << ": ignoring, no previously saved prepare" << dendl;
    }
    break;

  case ESlaveUpdate::OP_ROLLBACK:
    if (mds->mdcache->uncommitted_slave_updates[master].count(reqid)) {
      dout(10) << "ESlaveUpdate.replay abort " << reqid << " for mds" << master
	       << ": applying rollback commit blob" << dendl;
      assert(mds->mdcache->uncommitted_slave_updates[master].count(reqid));
      commit.replay(mds, _segment);
      delete mds->mdcache->uncommitted_slave_updates[master][reqid];
      mds->mdcache->uncommitted_slave_updates[master].erase(reqid);
      if (mds->mdcache->uncommitted_slave_updates[master].empty())
	mds->mdcache->uncommitted_slave_updates.erase(master);
    } else {
      dout(10) << "ESlaveUpdate.replay abort " << reqid << " for mds" << master 
	       << ": ignoring, no previously saved prepare" << dendl;
    }
    break;

  default:
    assert(0);
  }
}


// -----------------------
// ESubtreeMap

void ESubtreeMap::replay(MDS *mds) 
{
  // suck up the subtree map?
  if (mds->mdcache->is_subtrees()) {
    dout(10) << "ESubtreeMap.replay -- ignoring, already have import map" << dendl;
    return;
  }

  dout(10) << "ESubtreeMap.replay -- reconstructing (auth) subtree spanning tree" << dendl;
  
  // first, stick the spanning tree in my cache
  //metablob.print(*_dout);
  metablob.replay(mds, _segment);
  
  // restore import/export maps
  for (map<dirfrag_t, vector<dirfrag_t> >::iterator p = subtrees.begin();
       p != subtrees.end();
       ++p) {
    CDir *dir = mds->mdcache->get_dirfrag(p->first);
    mds->mdcache->adjust_bounded_subtree_auth(dir, p->second, mds->get_nodeid());
  }
  
  mds->mdcache->show_subtrees();
}



// -----------------------
// EFragment

void EFragment::replay(MDS *mds)
{
  dout(10) << "EFragment.replay " << ino << " " << basefrag << " by " << bits << dendl;
  
  CInode *in = mds->mdcache->get_inode(ino);
  assert(in);

  list<CDir*> resultfrags;
  list<Context*> waiters;
  mds->mdcache->adjust_dir_fragments(in, basefrag, bits, resultfrags, waiters, true);

  metablob.replay(mds, _segment);
}



// -----------------------
// EPurgeFinish


void EPurgeFinish::update_segment()
{
  // ** update purge lists?
}

void EPurgeFinish::replay(MDS *mds)
{
  dout(10) << "EPurgeFinish.replay " << ino << " " << oldsize << " -> " << newsize << dendl;
  CInode *in = mds->mdcache->get_inode(ino);
  assert(in);
  mds->mdcache->remove_recovered_purge(in, newsize, oldsize);
}





// =========================================================================

// -----------------------
// EExport

void EExport::replay(MDS *mds)
{
  dout(10) << "EExport.replay " << base << dendl;
  metablob.replay(mds, _segment);
  
  CDir *dir = mds->mdcache->get_dirfrag(base);
  assert(dir);
  
  set<CDir*> realbounds;
  for (set<dirfrag_t>::iterator p = bounds.begin();
       p != bounds.end();
       ++p) {
    CDir *bd = mds->mdcache->get_dirfrag(*p);
    assert(bd);
    realbounds.insert(bd);
  }

  // adjust auth away
  mds->mdcache->adjust_bounded_subtree_auth(dir, realbounds, pair<int,int>(CDIR_AUTH_UNKNOWN, CDIR_AUTH_UNKNOWN));
  mds->mdcache->try_subtree_merge(dir);
}


// -----------------------
// EImportStart

void EImportStart::update_segment()
{
  _segment->sessionmapv = cmapv;
}

void EImportStart::replay(MDS *mds)
{
  dout(10) << "EImportStart.replay " << base << dendl;
  //metablob.print(*_dout);
  metablob.replay(mds, _segment);

  // put in ambiguous import list
  mds->mdcache->add_ambiguous_import(base, bounds);

  // open client sessions?
  if (mds->sessionmap.version >= cmapv) {
    dout(10) << "EImportStart.replay sessionmap " << mds->sessionmap.version 
	     << " >= " << cmapv << ", noop" << dendl;
  } else {
    dout(10) << "EImportStart.replay sessionmap " << mds->sessionmap.version 
	     << " < " << cmapv << dendl;
    map<__u32,entity_inst_t> cm;
    bufferlist::iterator blp = client_map.begin();
    ::decode(cm, blp);
    mds->sessionmap.open_sessions(cm);
    assert(mds->sessionmap.version == cmapv);
    mds->sessionmap.projected = mds->sessionmap.version;
  }
  _segment->sessionmapv = cmapv;
}

// -----------------------
// EImportFinish

void EImportFinish::replay(MDS *mds)
{
  if (mds->mdcache->have_ambiguous_import(base)) {
    dout(10) << "EImportFinish.replay " << base << " success=" << success << dendl;
    if (success) 
      mds->mdcache->finish_ambiguous_import(base);
    else
      mds->mdcache->cancel_ambiguous_import(base);
  } else {
    dout(10) << "EImportFinish.replay " << base << " success=" << success
	     << ", predates my subtree_map start point, ignoring" 
	     << dendl;
    // verify that?
  }
}





