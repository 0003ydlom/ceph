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


#include "MDS.h"
#include "MDCache.h"
#include "Locker.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"

#include "MDLog.h"
#include "MDSMap.h"

#include "include/filepath.h"

#include "events/EString.h"
#include "events/EUpdate.h"
#include "events/EOpen.h"

#include "msg/Messenger.h"

#include "messages/MGenericMessage.h"
#include "messages/MDiscover.h"
#include "messages/MDiscoverReply.h"

#include "messages/MDirUpdate.h"

#include "messages/MInodeFileCaps.h"

#include "messages/MLock.h"
#include "messages/MClientLease.h"
#include "messages/MDentryUnlink.h"

#include "messages/MClientRequest.h"
#include "messages/MClientReply.h"
#include "messages/MClientFileCaps.h"

#include "messages/MMDSSlaveRequest.h"

#include <errno.h>
#include <assert.h>

#include "config.h"

#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".locker "



void Locker::dispatch(Message *m)
{

  switch (m->get_type()) {

    // inter-mds locking
  case MSG_MDS_LOCK:
    handle_lock((MLock*)m);
    break;
    // inter-mds caps
  case MSG_MDS_INODEFILECAPS:
    handle_inode_file_caps((MInodeFileCaps*)m);
    break;

    // client sync
  case CEPH_MSG_CLIENT_FILECAPS:
    handle_client_file_caps((MClientFileCaps*)m);
    break;
  case CEPH_MSG_CLIENT_LEASE:
    handle_client_lease((MClientLease*)m);
    break;
    
  default:
    assert(0);
  }
}


void Locker::send_lock_message(SimpleLock *lock, int msg)
{
  for (map<int,int>::iterator it = lock->get_parent()->replicas_begin(); 
       it != lock->get_parent()->replicas_end(); 
       it++) {
    if (mds->mdsmap->get_state(it->first) < MDSMap::STATE_REJOIN) 
      continue;
    MLock *m = new MLock(lock, msg, mds->get_nodeid());
    mds->send_message_mds(m, it->first);
  }
}

void Locker::send_lock_message(SimpleLock *lock, int msg, const bufferlist &data)
{
  for (map<int,int>::iterator it = lock->get_parent()->replicas_begin(); 
       it != lock->get_parent()->replicas_end(); 
       it++) {
    if (mds->mdsmap->get_state(it->first) < MDSMap::STATE_REJOIN) 
      continue;
    MLock *m = new MLock(lock, msg, mds->get_nodeid());
    m->set_data(data);
    mds->send_message_mds(m, it->first);
  }
}








bool Locker::acquire_locks(MDRequest *mdr,
			   set<SimpleLock*> &rdlocks,
			   set<SimpleLock*> &wrlocks,
			   set<SimpleLock*> &xlocks)
{
  if (mdr->done_locking) {
    dout(10) << "acquire_locks " << *mdr << " -- done locking" << dendl;    
    return true;  // at least we had better be!
  }
  dout(10) << "acquire_locks " << *mdr << dendl;

  set<SimpleLock*, SimpleLock::ptr_lt> sorted;  // sort everything we will lock
  set<SimpleLock*> mustpin = xlocks;            // items to authpin

  // xlocks
  for (set<SimpleLock*>::iterator p = xlocks.begin(); p != xlocks.end(); ++p) {
    dout(20) << " must xlock " << **p << " " << *(*p)->get_parent() << dendl;
    sorted.insert(*p);

    // augment xlock with a versionlock?
    if ((*p)->get_type() > CEPH_LOCK_IVERSION) {
      // inode version lock?
      CInode *in = (CInode*)(*p)->get_parent();
      if (mdr->is_master()) {
	// master.  wrlock versionlock so we can pipeline inode updates to journal.
	wrlocks.insert(&in->versionlock);
      } else {
	// slave.  exclusively lock the inode version (i.e. block other journal updates)
	xlocks.insert(&in->versionlock);
	sorted.insert(&in->versionlock);
      }
    }
  }

  // wrlocks
  for (set<SimpleLock*>::iterator p = wrlocks.begin(); p != wrlocks.end(); ++p) {
    dout(20) << " must wrlock " << **p << " " << *(*p)->get_parent() << dendl;
    sorted.insert(*p);
    if ((*p)->get_parent()->is_auth())
      mustpin.insert(*p);
    else if ((*p)->get_type() == CEPH_LOCK_IDIR &&
	     !(*p)->get_parent()->is_auth() && !((ScatterLock*)(*p))->can_wrlock()) { // we might have to request a scatter
      dout(15) << " will also auth_pin " << *(*p)->get_parent() << " in case we need to request a scatter" << dendl;
      mustpin.insert(*p);
    }
  }

  // rdlocks
  for (set<SimpleLock*>::iterator p = rdlocks.begin();
	 p != rdlocks.end();
       ++p) {
    dout(20) << " must rdlock " << **p << " " << *(*p)->get_parent() << dendl;
    sorted.insert(*p);
  }

 
  // AUTH PINS
  map<int, set<MDSCacheObject*> > mustpin_remote;  // mds -> (object set)
  
  // can i auth pin them all now?
  for (set<SimpleLock*>::iterator p = mustpin.begin();
       p != mustpin.end();
       ++p) {
    MDSCacheObject *object = (*p)->get_parent();

    dout(10) << " must authpin " << *object << dendl;

    if (mdr->is_auth_pinned(object)) 
      continue;
    
    if (!object->is_auth()) {
      if (object->is_ambiguous_auth()) {
	// wait
	dout(10) << " ambiguous auth, waiting to authpin " << *object << dendl;
	object->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, new C_MDS_RetryRequest(mdcache, mdr));
	mds->locker->drop_locks(mdr);
	mdr->drop_local_auth_pins();
	return false;
      }
      mustpin_remote[object->authority().first].insert(object);
      continue;
    }
    if (!object->can_auth_pin()) {
      // wait
      dout(10) << " can't auth_pin (freezing?), waiting to authpin " << *object << dendl;
      object->add_waiter(MDSCacheObject::WAIT_UNFREEZE, new C_MDS_RetryRequest(mdcache, mdr));
      mds->locker->drop_locks(mdr);
      mdr->drop_local_auth_pins();
      return false;
    }
  }

  // ok, grab local auth pins
  for (set<SimpleLock*>::iterator p = mustpin.begin();
       p != mustpin.end();
       ++p) {
    MDSCacheObject *object = (*p)->get_parent();
    if (mdr->is_auth_pinned(object)) {
      dout(10) << " already auth_pinned " << *object << dendl;
    } else if (object->is_auth()) {
      dout(10) << " auth_pinning " << *object << dendl;
      mdr->auth_pin(object);
    }
  }

  // request remote auth_pins
  if (!mustpin_remote.empty()) {
    for (map<int, set<MDSCacheObject*> >::iterator p = mustpin_remote.begin();
	 p != mustpin_remote.end();
	 ++p) {
      dout(10) << "requesting remote auth_pins from mds" << p->first << dendl;
      
      MMDSSlaveRequest *req = new MMDSSlaveRequest(mdr->reqid, MMDSSlaveRequest::OP_AUTHPIN);
      for (set<MDSCacheObject*>::iterator q = p->second.begin();
	   q != p->second.end();
	   ++q) {
	dout(10) << " req remote auth_pin of " << **q << dendl;
	MDSCacheObjectInfo info;
	(*q)->set_object_info(info);
	req->get_authpins().push_back(info);      
	mdr->pin(*q);
      }
      mds->send_message_mds(req, p->first);

      // put in waiting list
      assert(mdr->more()->waiting_on_slave.count(p->first) == 0);
      mdr->more()->waiting_on_slave.insert(p->first);
    }
    return false;
  }

  // acquire locks.
  // make sure they match currently acquired locks.
  set<SimpleLock*, SimpleLock::ptr_lt>::iterator existing = mdr->locks.begin();
  for (set<SimpleLock*, SimpleLock::ptr_lt>::iterator p = sorted.begin();
       p != sorted.end();
       ++p) {

    // already locked?
    if (existing != mdr->locks.end() && *existing == *p) {
      // right kind?
      SimpleLock *have = *existing;
      existing++;
      if (xlocks.count(*p) && mdr->xlocks.count(*p)) {
	dout(10) << " already xlocked " << *have << " " << *have->get_parent() << dendl;
      }
      else if (wrlocks.count(*p) && mdr->wrlocks.count(*p)) {
	dout(10) << " already wrlocked " << *have << " " << *have->get_parent() << dendl;
      }
      else if (rdlocks.count(*p) && mdr->rdlocks.count(*p)) {
	dout(10) << " already rdlocked " << *have << " " << *have->get_parent() << dendl;
      }
      else assert(0);
      continue;
    }
    
    // hose any stray locks
    while (existing != mdr->locks.end()) {
      SimpleLock *stray = *existing;
      existing++;
      dout(10) << " unlocking out-of-order " << *stray << " " << *stray->get_parent() << dendl;
      if (mdr->xlocks.count(stray)) 
	xlock_finish(stray, mdr);
      else if (mdr->wrlocks.count(stray))
	wrlock_finish(stray, mdr);
      else
	rdlock_finish(stray, mdr);
    }
      
    // lock
    if (xlocks.count(*p)) {
      if (!xlock_start(*p, mdr)) 
	return false;
      dout(10) << " got xlock on " << **p << " " << *(*p)->get_parent() << dendl;
    } else if (wrlocks.count(*p)) {
      if (!wrlock_start(*p, mdr)) 
	return false;
      dout(10) << " got wrlock on " << **p << " " << *(*p)->get_parent() << dendl;
    } else {
      if (!rdlock_start(*p, mdr)) 
	return false;
      dout(10) << " got rdlock on " << **p << " " << *(*p)->get_parent() << dendl;
    }
  }
    
  // any extra unneeded locks?
  while (existing != mdr->locks.end()) {
    SimpleLock *stray = *existing;
    existing++;
    dout(10) << " unlocking extra " << *stray << " " << *stray->get_parent() << dendl;
    if (mdr->xlocks.count(stray))
      xlock_finish(stray, mdr);
    else if (mdr->wrlocks.count(stray))
      wrlock_finish(stray, mdr);
    else
      rdlock_finish(stray, mdr);
  }

  return true;
}


void Locker::drop_locks(Mutation *mut)
{
  // leftover locks
  while (!mut->xlocks.empty()) 
    xlock_finish(*mut->xlocks.begin(), mut);
  while (!mut->rdlocks.empty()) 
    rdlock_finish(*mut->rdlocks.begin(), mut);
  while (!mut->wrlocks.empty()) 
    wrlock_finish(*mut->wrlocks.begin(), mut);
}


// generics

void Locker::eval_gather(SimpleLock *lock)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IFILE:
    return file_eval_gather((FileLock*)lock);
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_eval_gather((ScatterLock*)lock);
  default:
    return simple_eval_gather(lock);
  }
}

bool Locker::rdlock_start(SimpleLock *lock, MDRequest *mut)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IFILE:
    return file_rdlock_start((FileLock*)lock, mut);
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_rdlock_start((ScatterLock*)lock, mut);
  default:
    return simple_rdlock_start(lock, mut);
  }
}

void Locker::rdlock_finish(SimpleLock *lock, Mutation *mut)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IFILE:
    return file_rdlock_finish((FileLock*)lock, mut);
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_rdlock_finish((ScatterLock*)lock, mut);
  default:
    return simple_rdlock_finish(lock, mut);
  }
}

bool Locker::wrlock_start(SimpleLock *lock, MDRequest *mut)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_wrlock_start((ScatterLock*)lock, mut);
  case CEPH_LOCK_IVERSION:
    return local_wrlock_start((LocalLock*)lock, mut);
    //case CEPH_LOCK_IFILE:
    //return file_wrlock_start((ScatterLock*)lock, mut);
  default:
    assert(0); 
    return false;
  }
}

void Locker::wrlock_finish(SimpleLock *lock, Mutation *mut)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_wrlock_finish((ScatterLock*)lock, mut);
  case CEPH_LOCK_IVERSION:
    return local_wrlock_finish((LocalLock*)lock, mut);
  case CEPH_LOCK_IFILE:
    return file_wrlock_finish((FileLock*)lock, mut);
  default:
    assert(0);
  }
}

bool Locker::xlock_start(SimpleLock *lock, MDRequest *mut)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IFILE:
    return file_xlock_start((FileLock*)lock, mut);
  case CEPH_LOCK_IVERSION:
    return local_xlock_start((LocalLock*)lock, mut);
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_xlock_start((ScatterLock*)lock, mut);
  default:
    return simple_xlock_start(lock, mut);
  }
}

void Locker::xlock_finish(SimpleLock *lock, Mutation *mut)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IFILE:
    return file_xlock_finish((FileLock*)lock, mut);
  case CEPH_LOCK_IVERSION:
    return local_xlock_finish((LocalLock*)lock, mut);
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    return scatter_xlock_finish((ScatterLock*)lock, mut);
  default:
    return simple_xlock_finish(lock, mut);
  }
}



/** rejoin_set_state
 * @lock the lock 
 * @s the new state
 * @waiters list for anybody waiting on this lock
 */
void Locker::rejoin_set_state(SimpleLock *lock, int s, list<Context*>& waiters)
{
  if (!lock->is_stable()) {
    lock->set_state(s);
    lock->get_parent()->auth_unpin();
  } else {
    lock->set_state(s);
  }
  lock->take_waiting(SimpleLock::WAIT_ALL, waiters);
}




// file i/o -----------------------------------------

version_t Locker::issue_file_data_version(CInode *in)
{
  dout(7) << "issue_file_data_version on " << *in << dendl;
  return in->inode.file_data_version;
}

struct C_Locker_FileUpdate_finish : public Context {
  Locker *locker;
  CInode *in;
  Mutation *mut;
  bool share;
  C_Locker_FileUpdate_finish(Locker *l, CInode *i, Mutation *m, bool e=false) : 
    locker(l), in(i), mut(m), share(e) {
    in->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    locker->file_update_finish(in, mut, share);
  }
};

void Locker::file_update_finish(CInode *in, Mutation *mut, bool share)
{
  dout(10) << "file_update_finish on " << *in << dendl;
  in->pop_and_dirty_projected_inode(mut->ls);
  in->put(CInode::PIN_PTRWAITER);

  mut->pop_and_dirty_projected_inodes();
  mut->pop_and_dirty_projected_fnodes();
  drop_locks(mut);
  
  if (share && in->is_auth() && in->filelock.is_stable())
    share_inode_max_size(in);
}

Capability* Locker::issue_new_caps(CInode *in,
				   int mode,
				   Session *session)
{
  dout(7) << "issue_new_caps for mode " << mode << " on " << *in << dendl;
  
  // my needs
  assert(session->inst.name.is_client());
  int my_client = session->inst.name.num();
  int my_want = ceph_caps_for_mode(mode);

  // register a capability
  Capability *cap = in->get_client_cap(my_client);
  if (!cap) {
    // new cap
    cap = in->add_client_cap(my_client, in);
    session->touch_cap(cap);
    cap->set_wanted(my_want);
    cap->set_suppress(true); // suppress file cap messages for new cap (we'll bundle with the open() reply)
  } else {
    // make sure it wants sufficient caps
    if (my_want & ~cap->wanted()) {
      // augment wanted caps for this client
      cap->set_wanted(cap->wanted() | my_want);
    }
  }

  int before = cap->pending();

  if (in->is_auth()) {
    // [auth] twiddle mode?
    if (in->filelock.is_stable()) 
      file_eval(&in->filelock);
  } else {
    // [replica] tell auth about any new caps wanted
    request_inode_file_caps(in);
  }

  // issue caps (pot. incl new one)
  issue_caps(in);  // note: _eval above may have done this already...

  // re-issue whatever we can
  cap->issue(cap->pending());
  cap->set_last_open();

  // twiddle file_data_version?
  int now = cap->pending();
  if ((before & CEPH_CAP_WRBUFFER) == 0 &&
      (now & CEPH_CAP_WRBUFFER)) {
    in->inode.file_data_version++;
    dout(7) << " incrementing file_data_version, now " << in->inode.file_data_version << " for " << *in << dendl;
  }

  return cap;
}




bool Locker::issue_caps(CInode *in)
{
  // allowed caps are determined by the lock mode.
  int all_allowed = in->filelock.caps_allowed();
  dout(7) << "issue_caps filelock allows=" << cap_string(all_allowed) 
          << " on " << *in << dendl;

  // count conflicts with
  int nissued = 0;        

  bool sizemtime_is_projected = false;
  if (&in->inode != in->get_projected_inode() &&
      (in->inode.size != in->get_projected_inode()->size ||
       in->inode.mtime != in->get_projected_inode()->mtime)) {
    dout(10) << " new size|mtime is projected" << dendl;
    sizemtime_is_projected = true;
  }

  // should we increase max_size?
  if (!in->is_dir() && (all_allowed & CEPH_CAP_WR) && in->is_auth())
    check_inode_max_size(in);

  // client caps
  for (map<int, Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    Capability *cap = it->second;
    if (cap->is_stale())
      continue;

    // do not issue _new_ bits when size|mtime is projected
    int allowed = all_allowed;
    int careful = CEPH_CAP_EXCL|CEPH_CAP_WRBUFFER|CEPH_CAP_RDCACHE;
    int pending = cap->pending();
    if (sizemtime_is_projected)
      allowed &= ~careful | pending;   // only allow "careful" bits if already issued
    dout(20) << " all_allowed " << cap_string(all_allowed) 
	     << " pending " << cap_string(pending) 
	     << " allowed " << cap_string(allowed) 
	     << " wanted " << cap_string(cap->wanted())
	     << dendl;

    if (cap->pending() != (cap->wanted() & allowed)) {
      // issue
      nissued++;

      int before = cap->pending();
      long seq = cap->issue(cap->wanted() & allowed);
      int after = cap->pending();

      // twiddle file_data_version?
      if (!(before & CEPH_CAP_WRBUFFER) &&
          (after & CEPH_CAP_WRBUFFER)) {
        dout(7) << "   incrementing file_data_version for " << *in << dendl;
        in->inode.file_data_version++;
      }

      if (seq > 0 && 
          !cap->is_suppress()) {
        dout(7) << "   sending MClientFileCaps to client" << it->first
		<< " seq " << cap->get_last_seq()
		<< " new pending " << cap_string(cap->pending()) << " was " << cap_string(before) 
		<< dendl;
        mds->send_message_client(new MClientFileCaps(CEPH_CAP_OP_GRANT,
						     in->inode,
						     cap->get_last_seq(),
						     cap->pending(),
						     cap->wanted()),
				 it->first);
      }
    }
  }

  return (nissued == 0);  // true if no re-issued, no callbacks
}

void Locker::issue_truncate(CInode *in)
{
  dout(7) << "issue_truncate on " << *in << dendl;
  
  for (map<int, Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    Capability *cap = it->second;
    mds->send_message_client(new MClientFileCaps(CEPH_CAP_OP_TRUNC,
						 in->inode,
						 cap->get_last_seq(),
						 cap->pending(),
						 cap->wanted()),
			     it->first);
  }

  // should we increase max_size?
  if (in->is_auth() && !in->is_dir())
    check_inode_max_size(in);
}

void Locker::revoke_stale_caps(Session *session)
{
  dout(10) << "revoke_stale_caps for " << session->inst.name << dendl;
  
  for (xlist<Capability*>::iterator p = session->caps.begin(); !p.end(); ++p) {
    Capability *cap = *p;
    cap->set_stale(true);
    CInode *in = cap->get_inode();
    int issued = cap->issued();
    if (issued) {
      dout(10) << " revoking " << cap_string(issued) << " on " << *in << dendl;      
      cap->revoke();
      in->state_set(CInode::STATE_NEEDSRECOVER);
      if (!in->filelock.is_stable())
	file_eval_gather(&in->filelock);
      if (in->is_auth()) {
	if (in->filelock.is_stable())
	  file_eval(&in->filelock);
      } else {
	request_inode_file_caps(in);
      }
    } else {
      dout(10) << " nothing issued on " << *in << dendl;
    }
  }
}

void Locker::resume_stale_caps(Session *session)
{
  dout(10) << "resume_stale_caps for " << session->inst.name << dendl;

  for (xlist<Capability*>::iterator p = session->caps.begin(); !p.end(); ++p) {
    Capability *cap = *p;
    CInode *in = cap->get_inode();
    if (cap->is_stale()) {
      dout(10) << " clearing stale flag on " << *in << dendl;
      cap->set_stale(false);
      if (in->is_auth() && in->filelock.is_stable())
	file_eval(&in->filelock);
      else
	issue_caps(in);
    }
  }
}

class C_MDL_RequestInodeFileCaps : public Context {
  Locker *locker;
  CInode *in;
public:
  C_MDL_RequestInodeFileCaps(Locker *l, CInode *i) : locker(l), in(i) {
    in->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    in->put(CInode::PIN_PTRWAITER);
    if (!in->is_auth())
      locker->request_inode_file_caps(in);
  }
};

void Locker::request_inode_file_caps(CInode *in)
{
  assert(!in->is_auth());

  int wanted = in->get_caps_wanted();
  if (wanted != in->replica_caps_wanted) {

    if (wanted == 0) {
      if (in->replica_caps_wanted_keep_until > g_clock.recent_now()) {
        // ok, release them finally!
        in->replica_caps_wanted_keep_until.sec_ref() = 0;
        dout(7) << "request_inode_file_caps " << cap_string(wanted)
                 << " was " << cap_string(in->replica_caps_wanted) 
                 << " no keeping anymore " 
                 << " on " << *in 
                 << dendl;
      }
      else if (in->replica_caps_wanted_keep_until.sec() == 0) {
        in->replica_caps_wanted_keep_until = g_clock.recent_now();
        in->replica_caps_wanted_keep_until.sec_ref() += 2;
        
        dout(7) << "request_inode_file_caps " << cap_string(wanted)
                 << " was " << cap_string(in->replica_caps_wanted) 
                 << " keeping until " << in->replica_caps_wanted_keep_until
                 << " on " << *in 
                 << dendl;
        return;
      } else {
        // wait longer
        return;
      }
    } else {
      in->replica_caps_wanted_keep_until.sec_ref() = 0;
    }
    assert(!in->is_auth());

    // wait for single auth
    if (in->is_ambiguous_auth()) {
      in->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, 
		     new C_MDL_RequestInodeFileCaps(this, in));
      return;
    }

    int auth = in->authority().first;
    dout(7) << "request_inode_file_caps " << cap_string(wanted)
            << " was " << cap_string(in->replica_caps_wanted) 
            << " on " << *in << " to mds" << auth << dendl;
    assert(!in->is_auth());

    in->replica_caps_wanted = wanted;

    if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN)
      mds->send_message_mds(new MInodeFileCaps(in->ino(), mds->get_nodeid(),
					       in->replica_caps_wanted),
			    auth);
  } else {
    in->replica_caps_wanted_keep_until.sec_ref() = 0;
  }
}

void Locker::handle_inode_file_caps(MInodeFileCaps *m)
{
  // nobody should be talking to us during recovery.
  assert(mds->is_rejoin() || mds->is_active() || mds->is_stopping());

  // ok
  CInode *in = mdcache->get_inode(m->get_ino());
  assert(in);
  assert(in->is_auth());

  if (mds->is_rejoin() &&
      in->is_rejoining()) {
    dout(7) << "handle_inode_file_caps still rejoining " << *in << ", dropping " << *m << dendl;
    delete m;
    return;
  }

  
  dout(7) << "handle_inode_file_caps replica mds" << m->get_from() << " wants caps " << cap_string(m->get_caps()) << " on " << *in << dendl;

  if (m->get_caps())
    in->mds_caps_wanted[m->get_from()] = m->get_caps();
  else
    in->mds_caps_wanted.erase(m->get_from());

  if (in->filelock.is_stable())
    try_file_eval(&in->filelock);  // ** may or may not be auth_pinned **
  delete m;
}


class C_MDL_CheckMaxSize : public Context {
  Locker *locker;
  CInode *in;
public:
  C_MDL_CheckMaxSize(Locker *l, CInode *i) : locker(l), in(i) {
    in->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    in->put(CInode::PIN_PTRWAITER);
    if (in->is_auth())
      locker->check_inode_max_size(in);
  }
};


bool Locker::check_inode_max_size(CInode *in, bool forcewrlock)
{
  assert(in->is_auth());
  if (!forcewrlock && !in->filelock.can_wrlock()) {
    // try again later
    in->filelock.add_waiter(SimpleLock::WAIT_STABLE, new C_MDL_CheckMaxSize(this, in));
    dout(10) << "check_inode_max_size can't wrlock, waiting on " << *in << dendl;
    return false;    
  }

  inode_t *latest = in->get_projected_inode();
  uint64_t new_max = latest->max_size;

  if ((in->get_caps_wanted() & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) == 0)
    new_max = 0;
  else if ((latest->size << 1) >= latest->max_size)
    new_max = latest->max_size ? (latest->max_size << 1):in->get_layout_size_increment();
  
  if (new_max == latest->max_size)// && !force_journal)
    return false;  // no change.

  dout(10) << "check_inode_max_size " << latest->max_size << " -> " << new_max
	   << " on " << *in << dendl;

  Mutation *mut = new Mutation;
  mut->ls = mds->mdlog->get_current_segment();
    
  inode_t *pi = in->project_inode();
  pi->version = in->pre_dirty();
  pi->max_size = new_max;
  EOpen *le = new EOpen(mds->mdlog);
  le->metablob.add_dir_context(in->get_parent_dir());
  le->metablob.add_primary_dentry(in->parent, true, 0, pi);
  le->add_ino(in->ino());
  mut->ls->open_files.push_back(&in->xlist_open_file);
  mds->mdlog->submit_entry(le, new C_Locker_FileUpdate_finish(this, in, mut, true));
  file_wrlock_force(&in->filelock, mut);  // wrlock for duration of journal
  return true;
}


void Locker::share_inode_max_size(CInode *in)
{
  /*
   * only share if currently issued a WR cap.  if client doesn't have it,
   * file_max doesn't matter, and the client will get it if/when they get
   * the cap later.
   */
  dout(10) << "share_inode_max_size on " << *in << dendl;
  for (map<int,Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    const int client = it->first;
    Capability *cap = it->second;
    if (cap->pending() & CEPH_CAP_WR) {
      dout(10) << "share_inode_max_size with client" << client << dendl;
      mds->send_message_client(new MClientFileCaps(CEPH_CAP_OP_GRANT,
						   in->inode,
						   cap->get_last_seq(),
						   cap->pending(),
						   cap->wanted()),
			       client);
    }
  }
}


/*
 * note: we only get these from the client if
 * - we are calling back previously issued caps (fewer than the client previously had)
 * - or if the client releases (any of) its caps on its own
 */
void Locker::handle_client_file_caps(MClientFileCaps *m)
{
  int client = m->get_source().num();
  CInode *in = mdcache->get_inode(m->get_ino());
  Capability *cap = 0;
  if (in) 
    cap = in->get_client_cap(client);

  if (!in || !cap) {
    if (!in) {
      dout(7) << "handle_client_file_caps on unknown ino " << m->get_ino() << ", dropping" << dendl;
    } else {
      dout(7) << "handle_client_file_caps no cap for client" << client << " on " << *in << dendl;
    }
    delete m;
    return;
  } 
  
  assert(cap);

  // filter wanted based on what we could ever give out (given auth/replica status)
  int wanted = m->get_wanted() & in->filelock.caps_allowed_ever();
  
  dout(7) << "handle_client_file_caps seq " << m->get_seq() 
          << " confirms caps " << cap_string(m->get_caps()) 
          << " wants " << cap_string(wanted)
          << " from client" << client
          << " on " << *in 
          << dendl;  
  
  // confirm caps
  int had = cap->confirm_receipt(m->get_seq(), m->get_caps());
  int has = cap->confirmed();
  dout(10) << "client had " << cap_string(had) << ", has " << cap_string(has) << dendl;

  // update wanted
  if (cap->wanted() != wanted) {
    if (m->get_seq() < cap->get_last_open()) {
      /* this is awkward.
	 client may be trying to release caps (i.e. inode closed, etc.) by setting reducing wanted
	 set.
	 but it may also be opening the same filename, not sure that it'll map to the same inode.
	 so, we don't want wanted reductions to clobber mds's notion of wanted unless we're
	 sure the client has seen all the latest caps.
      */
      dout(10) << "handle_client_file_caps ignoring wanted " << cap_string(m->get_wanted())
		<< " bc seq " << m->get_seq() << " < last open " << cap->get_last_open() << dendl;
    } else if (wanted == 0) {
      // outright release?
      dout(7) << " cap for client" << client << " is now null, removing from " << *in << dendl;
      in->remove_client_cap(client);

      if (!in->is_any_caps())
	in->xlist_open_file.remove_myself();  // unpin logsegment
      if (!in->is_auth())
	request_inode_file_caps(in);
    } else {
      cap->set_wanted(wanted);
    }
  }

  inode_t *latest = in->get_projected_inode();

  utime_t atime = m->get_atime();
  utime_t mtime = m->get_mtime();
  utime_t ctime = m->get_ctime();
  uint64_t size = m->get_size();

  // atime|mtime|size?
  bool had_or_has_wr = (had|has) & CEPH_CAP_WR;
  bool excl = (had|has) & CEPH_CAP_EXCL;
  bool dirty_atime = false;
  bool dirty_mtime = false;
  bool dirty_ctime = false;
  bool dirty_size = false;
  if (had_or_has_wr || excl) {
    if (mtime > latest->mtime || (excl && mtime != latest->mtime)) 
      dirty_mtime = true;
    if (ctime > latest->ctime)
      dirty_ctime = true;
    if (size > latest->size) 
      dirty_size = true;
  }
  if (excl && atime != latest->atime)
    dirty_atime = true;
  bool dirty = dirty_atime || dirty_mtime || dirty_ctime || dirty_size;
  
  // increase or zero max_size?
  bool change_max = false;
  uint64_t new_max = latest->max_size;

  if (in->is_auth()) {
    if (latest->max_size && (wanted & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) == 0) {
      change_max = true;
      new_max = 0;
    }
    else if ((wanted & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER|CEPH_CAP_WREXTEND)) &&
	(size << 1) >= latest->max_size) {
      dout(10) << "wr caps wanted, and size " << size
	       << " *2 >= max " << latest->max_size << ", increasing" << dendl;
      change_max = true;
      new_max = latest->max_size ? (latest->max_size << 1):in->get_layout_size_increment();
    }
    if ((wanted & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER|CEPH_CAP_WREXTEND)) &&
	m->get_max_size() > new_max) {
      dout(10) << "client requests file_max " << m->get_max_size()
	       << " > max " << latest->max_size << dendl;
      change_max = true;
      new_max = (m->get_max_size() << 1) & ~(in->get_layout_size_increment() - 1);
    }

    if (change_max && !in->filelock.can_wrlock()) {
      dout(10) << "want to change file_max, but lock won't allow it; will retry" << dendl;
      check_inode_max_size(in);  // this will fail, and schedule a waiter.
      change_max = false;
    }
  }

  if ((dirty || change_max) &&
      !in->is_base()) {              // FIXME.. what about root inode mtime/atime?
    EUpdate *le = new EUpdate(mds->mdlog, "size|max_size|mtime|ctime|atime update");
    inode_t *pi = in->project_inode();
    pi->version = in->pre_dirty();
    if (change_max) {
      dout(7) << " max_size " << pi->max_size << " -> " << new_max << dendl;
      pi->max_size = new_max;
    }    
    if (dirty_mtime) {
      dout(7) << "  mtime " << pi->mtime << " -> " << mtime
	      << " for " << *in << dendl;
      pi->mtime = mtime;
    }
    if (dirty_ctime) {
      dout(7) << "  ctime " << pi->ctime << " -> " << ctime
	      << " for " << *in << dendl;
      pi->ctime = ctime;
    }
    if (dirty_size) {
      dout(7) << "  size " << pi->size << " -> " << size
	      << " for " << *in << dendl;
      pi->size = size;
      pi->dirstat.rbytes = size;
    }
    if (dirty_atime) {
      dout(7) << "  atime " << pi->atime << " -> " << atime
	      << " for " << *in << dendl;
      pi->atime = atime;
    }
    if (excl && pi->time_warp_seq < m->get_time_warp_seq()) {
      dout(7) << "  time_warp_seq " << pi->time_warp_seq << " -> " << m->get_time_warp_seq()
	      << " for " << *in << dendl;
      pi->time_warp_seq = m->get_time_warp_seq();
    }
    Mutation *mut = new Mutation;
    mut->ls = mds->mdlog->get_current_segment();
    file_wrlock_force(&in->filelock, mut);  // wrlock for duration of journal
    predirty_nested(mut, &le->metablob, in, 0, true, false);
    le->metablob.add_dir_context(in->get_parent_dir());
    le->metablob.add_primary_dentry(in->parent, true, 0, pi);
    mds->mdlog->submit_entry(le, new C_Locker_FileUpdate_finish(this, in, mut, change_max));
  }

  // reevaluate, waiters
  if (!in->filelock.is_stable())
    file_eval_gather(&in->filelock);
  else if (in->is_auth())
    file_eval(&in->filelock);
  
  delete m;
}


void Locker::handle_client_lease(MClientLease *m)
{
  dout(10) << "handle_client_lease " << *m << dendl;

  assert(m->get_source().is_client());
  int client = m->get_source().num();

  CInode *in = mdcache->get_inode(m->get_ino());
  if (!in) {
    dout(7) << "handle_client_lease don't have ino " << m->get_ino() << dendl;
    delete m;
    return;
  }
  CDentry *dn = 0;
  MDSCacheObject *p;
  if (m->get_mask() & CEPH_LOCK_DN) {
    frag_t fg = in->pick_dirfrag(m->dname);
    CDir *dir = in->get_dirfrag(fg);
    if (dir) 
      p = dn = dir->lookup(m->dname);
    if (!dn) {
      dout(7) << "handle_client_lease don't have dn " << m->get_ino() << " " << m->dname << dendl;
      delete m;
      return;
    }
  } else {
    p = in;
  }
  dout(10) << " on " << *p << dendl;

  // replica and lock
  ClientLease *l = p->get_client_lease(client);
  if (!l) {
    dout(7) << "handle_client_lease didn't have lease for client" << client << " of " << *p << dendl;
    delete m;
    return;
  } 

  switch (m->get_action()) {
  case CEPH_MDS_LEASE_RELEASE:
    {
      dout(7) << "handle_client_lease client" << client
	      << " release mask " << m->get_mask()
	      << " on " << *p << dendl;
      int left = p->remove_client_lease(l, l->mask, this);
      dout(10) << " remaining mask is " << left << " on " << *p << dendl;
    }
    break;

  case CEPH_MDS_LEASE_RENEW:
  default:
    assert(0); // implement me
    break;
  }

  delete m;
}



void Locker::_issue_client_lease(MDSCacheObject *p, int mask, int pool, int client,
				 bufferlist &bl, utime_t now, Session *session)
{
  if (mask) {
    ClientLease *l = p->add_client_lease(client, mask);
    session->touch_lease(l);

    now += mdcache->client_lease_durations[pool];
    mdcache->touch_client_lease(l, pool, now);
  }

  LeaseStat e;
  e.mask = mask;
  e.duration_ms = (int)(1000 * mdcache->client_lease_durations[pool]);
  ::encode(e, bl);
}
  


int Locker::issue_client_lease(CInode *in, int client, 
			       bufferlist &bl, utime_t now, Session *session)
{
  int mask = CEPH_LOCK_INO;
  int pool = 1;   // fixme.. do something smart!
  if (in->authlock.can_lease()) mask |= CEPH_LOCK_IAUTH;
  if (in->linklock.can_lease()) mask |= CEPH_LOCK_ILINK;
  if (in->is_dir()) {
    if (in->dirlock.can_lease()) mask |= CEPH_LOCK_IDIR;
  } else {
    if (in->filelock.can_lease()) mask |= CEPH_LOCK_IFILE;
  }
  if (in->xattrlock.can_lease()) mask |= CEPH_LOCK_IXATTR;
  //if (in->nestedlock.can_lease()) mask |= CEPH_LOCK_INESTED;

  _issue_client_lease(in, mask, pool, client, bl, now, session);
  return mask;
}

int Locker::issue_client_lease(CDentry *dn, int client,
			       bufferlist &bl, utime_t now, Session *session)
{
  int pool = 1;   // fixme.. do something smart!

  // is it necessary?
  // -> dont issue per-dentry lease if a dir lease is possible, or
  //    if the client is holding EXCL|RDCACHE caps.
  int mask = 0;
  CInode *diri = dn->get_dir()->get_inode();
  if (!diri->dirlock.can_lease() &&
      (diri->get_client_cap_pending(client) & (CEPH_CAP_EXCL|CEPH_CAP_RDCACHE)) == 0 &&
      dn->lock.can_lease())
    mask |= CEPH_LOCK_DN;

  _issue_client_lease(dn, mask, pool, client, bl, now, session);
  return mask;
}


void Locker::revoke_client_leases(SimpleLock *lock)
{
  int n = 0;
  for (hash_map<int, ClientLease*>::iterator p = lock->get_parent()->client_lease_map.begin();
       p != lock->get_parent()->client_lease_map.end();
       p++) {
    ClientLease *l = p->second;
    
    if ((l->mask & lock->get_type()) == 0)
      continue;
    
    n++;
    if (lock->get_type() == CEPH_LOCK_DN) {
      CDentry *dn = (CDentry*)lock->get_parent();
      int mask = CEPH_LOCK_DN;

      // i should also revoke the dir ICONTENT lease, if they have it!
      CInode *diri = dn->get_dir()->get_inode();
      if (diri->get_client_lease_mask(l->client) & CEPH_LOCK_ICONTENT)
	mask |= CEPH_LOCK_ICONTENT;

      mds->send_message_client(new MClientLease(CEPH_MDS_LEASE_REVOKE,
						mask,
						dn->get_dir()->ino(),
						dn->get_name()),
			       l->client);
    } else {
      CInode *in = (CInode*)lock->get_parent();
      mds->send_message_client(new MClientLease(CEPH_MDS_LEASE_REVOKE,
						lock->get_type(),
						in->ino()),
			       l->client);
    }
  }
  assert(n == lock->get_num_client_lease());
}


// nested ---------------------------------------------------------------

void Locker::predirty_nested(Mutation *mut, EMetaBlob *blob,
			     CInode *in, CDir *parent,
			     bool primary_dn, bool do_parent, int linkunlink)
{
  dout(10) << "predirty_nested "
	   << (do_parent ? "do_parent_mtime ":"")
	   << "linkunlink=" <<  linkunlink
	   << (primary_dn ? "primary_dn ":"remote_dn ")
	   << " " << *in << dendl;

  if (!parent)
    parent = in->get_projected_parent_dn()->get_dir();

  inode_t *curi = in->get_projected_inode();

  __s64 drbytes = 1, drfiles = 0, drsubdirs = 0;
  utime_t rctime;

  // build list of inodes to wrlock, dirty, and update
  list<CInode*> lsi;
  CInode *cur = in;
  while (parent) {
    assert(cur->is_auth());
    assert(parent->is_auth());
    
    // opportunistically adjust parent dirfrag
    CInode *pin = parent->get_inode();

    if (do_parent) {
      assert(mut->wrlocks.count(&pin->dirlock));
      //assert(mut->wrlocks.count(&pin->nestedlock));
    }

    // inode -> dirfrag
    mut->add_projected_fnode(parent);

    fnode_t *pf = parent->project_fnode();
    pf->version = parent->pre_dirty();

    if (do_parent) {
      dout(10) << "predirty_nested updating mtime/size on " << *parent << dendl;
      pf->fragstat.mtime = mut->now;
      if (linkunlink) {
	if (in->is_dir())
	  pf->fragstat.nsubdirs += linkunlink;
	else
	  pf->fragstat.nfiles += linkunlink;
      }
    }
    if (primary_dn) {
      if (linkunlink == 0) {
	drbytes = curi->dirstat.rbytes - curi->accounted_dirstat.rbytes;
	drfiles = curi->dirstat.rfiles - curi->accounted_dirstat.rfiles;
	drsubdirs = curi->dirstat.rsubdirs - curi->accounted_dirstat.rsubdirs;
      } else if (linkunlink < 0) {
	drbytes = 0 - curi->accounted_dirstat.rbytes;
	drfiles = 0 - curi->accounted_dirstat.rfiles;
	drsubdirs = 0 - curi->accounted_dirstat.rsubdirs;
      } else {
	drbytes = curi->dirstat.rbytes;
	drfiles = curi->dirstat.rfiles;
	drsubdirs = curi->dirstat.rsubdirs;
      }
      rctime = MAX(curi->ctime, curi->dirstat.rctime);

      dout(10) << "predirty_nested delta "
	       << drbytes << " bytes / " << drfiles << " files / " << drsubdirs << " subdirs for " 
	       << *parent << dendl;
      pf->fragstat.rbytes += drbytes;
      pf->fragstat.rfiles += drfiles;
      pf->fragstat.rsubdirs += drsubdirs;
      pf->fragstat.rctime = rctime;
    
      curi->accounted_dirstat = curi->dirstat;
    } else {
      dout(10) << "predirty_nested no delta (remote dentry) in " << *parent << dendl;
      assert(!in->is_dir());
      pf->fragstat.rfiles += linkunlink;
    }


    // stop?
    if (pin->is_base())
      break;

    bool stop = false;
    if (mut->wrlocks.count(&pin->dirlock) == 0 &&
	!scatter_wrlock_try(&pin->dirlock, mut)) {
      dout(10) << "predirty_nested can't wrlock " << pin->dirlock << " on " << *pin << dendl;
      stop = true;
    }
    if (!pin->is_auth() || pin->is_ambiguous_auth()) {
      dout(10) << "predirty_nested !auth or ambig on " << *pin << dendl;
      stop = true;
    }
    if (stop) {
      dout(10) << "predirty_nested stop.  marking dirty dirfrag/scatterlock on " << *pin << dendl;
      mut->add_updated_scatterlock(&pin->dirlock);
      mut->ls->dirty_dirfrag_nested.push_back(&pin->xlist_dirty_dirfrag_dir);
      break;
    }

    // dirfrag -> diri
    mut->add_projected_inode(pin);
    lsi.push_back(pin);

    inode_t *pi = pin->project_inode();
    pi->version = pin->pre_dirty();
    pi->dirstat.version++;
    pi->dirstat.take_diff(pf->fragstat, pf->accounted_fragstat);

    // next parent!
    cur = pin;
    curi = pi;
    parent = cur->get_projected_parent_dn()->get_dir();
    linkunlink = 0;
    do_parent = false;
    primary_dn = true;
  }

  // now, stick it in the blob
  assert(parent->is_auth());
  blob->add_dir_context(parent);
  blob->add_dir(parent, true);

  for (list<CInode*>::iterator p = lsi.begin();
       p != lsi.end();
       p++) {
    CInode *cur = *p;
    inode_t *pi = cur->get_projected_inode();
    blob->add_primary_dentry(cur->get_projected_parent_dn(), true, 0, pi);
  }
}



// locks ----------------------------------------------------------------

SimpleLock *Locker::get_lock(int lock_type, MDSCacheObjectInfo &info) 
{
  switch (lock_type) {
  case CEPH_LOCK_DN:
    {
      // be careful; info.dirfrag may have incorrect frag; recalculate based on dname.
      CInode *diri = mdcache->get_inode(info.dirfrag.ino);
      frag_t fg;
      CDir *dir = 0;
      CDentry *dn = 0;
      if (diri) {
	fg = diri->pick_dirfrag(info.dname);
	dir = diri->get_dirfrag(fg);
	if (dir) 
	  dn = dir->lookup(info.dname);
      }
      if (!dn) {
	dout(7) << "get_lock don't have dn " << info.dirfrag.ino << " " << info.dname << dendl;
	return 0;
      }
      return &dn->lock;
    }

  case CEPH_LOCK_IAUTH:
  case CEPH_LOCK_ILINK:
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IFILE:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_IXATTR:
  case CEPH_LOCK_INESTED:
    {
      CInode *in = mdcache->get_inode(info.ino);
      if (!in) {
	dout(7) << "get_lock don't have ino " << info.ino << dendl;
	return 0;
      }
      switch (lock_type) {
      case CEPH_LOCK_IAUTH: return &in->authlock;
      case CEPH_LOCK_ILINK: return &in->linklock;
      case CEPH_LOCK_IDFT: return &in->dirfragtreelock;
      case CEPH_LOCK_IFILE: return &in->filelock;
      case CEPH_LOCK_IDIR: return &in->dirlock;
      case CEPH_LOCK_IXATTR: return &in->xattrlock;
      case CEPH_LOCK_INESTED: return &in->nestedlock;
      }
    }

  default:
    dout(7) << "get_lock don't know lock_type " << lock_type << dendl;
    assert(0);
    break;
  }

  return 0;  
}


void Locker::handle_lock(MLock *m)
{
  // nobody should be talking to us during recovery.
  assert(mds->is_rejoin() || mds->is_active() || mds->is_stopping());

  SimpleLock *lock = get_lock(m->get_lock_type(), m->get_object_info());
  if (!lock) {
    dout(10) << "don't have object " << m->get_object_info() << ", must have trimmed, dropping" << dendl;
    delete m;
    return;
  }

  switch (lock->get_type()) {
  case CEPH_LOCK_DN:
  case CEPH_LOCK_IAUTH:
  case CEPH_LOCK_ILINK:
    handle_simple_lock(lock, m);
    break;
    
  case CEPH_LOCK_IFILE:
    handle_file_lock((FileLock*)lock, m);
    break;
    
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IDIR:
  case CEPH_LOCK_INESTED:
    handle_scatter_lock((ScatterLock*)lock, m);
    break;

  default:
    dout(7) << "handle_lock got otype " << m->get_lock_type() << dendl;
    assert(0);
    break;
  }
}
 




// ==========================================================================
// simple lock

void Locker::handle_simple_lock(SimpleLock *lock, MLock *m)
{
  int from = m->get_asker();
  
  if (mds->is_rejoin()) {
    if (lock->get_parent()->is_rejoining()) {
      dout(7) << "handle_simple_lock still rejoining " << *lock->get_parent()
	      << ", dropping " << *m << dendl;
      delete m;
      return;
    }
  }

  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK);
    lock->decode_locked_state(m->get_data());
    lock->set_state(LOCK_SYNC);
    lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);

    // special case: trim replica no-longer-null dentry?
    if (lock->get_type() == CEPH_LOCK_DN) {
      CDentry *dn = (CDentry*)lock->get_parent();
      if (dn->is_null() && m->get_data().length() > 0) {
	dout(10) << "handle_simple_lock replica dentry null -> non-null, must trim " 
		 << *dn << dendl;
	assert(dn->get_num_ref() == 0);
	map<int, MCacheExpire*> expiremap;
	mdcache->trim_dentry(dn, expiremap);
	mdcache->send_expire_messages(expiremap);
      }
    }
    break;
    
  case LOCK_AC_LOCK:
    assert(lock->get_state() == LOCK_SYNC);
    //||           lock->get_state() == LOCK_GLOCKR);
    
    // wait for readers to finish?
    if (lock->is_rdlocked()) {
      dout(7) << "handle_simple_lock has reader, waiting before ack on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      lock->set_state(LOCK_GLOCKR);
    } else {
      // update lock and reply
      lock->set_state(LOCK_LOCK);
      mds->send_message_mds(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid()), from);
    }
    break;


    // -- auth --
  case LOCK_AC_LOCKACK:
    assert(lock->get_state() == LOCK_GLOCKR);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    if (lock->is_gathering()) {
      dout(7) << "handle_simple_lock " << *lock << " on " << *lock->get_parent() << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_simple_lock " << *lock << " on " << *lock->get_parent() << " from " << from
	      << ", last one" << dendl;
      simple_eval_gather(lock);
    }
    break;

  }

  delete m;
}

/* unused, currently.

class C_Locker_SimpleEval : public Context {
  Locker *locker;
  SimpleLock *lock;
public:
  C_Locker_SimpleEval(Locker *l, SimpleLock *lk) : locker(l), lock(lk) {}
  void finish(int r) {
    locker->try_simple_eval(lock);
  }
};

void Locker::try_simple_eval(SimpleLock *lock)
{
  // unstable and ambiguous auth?
  if (!lock->is_stable() &&
      lock->get_parent()->is_ambiguous_auth()) {
    dout(7) << "simple_eval not stable and ambiguous auth, waiting on " << *lock->get_parent() << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    lock->get_parent()->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, new C_Locker_SimpleEval(this, lock));
    return;
  }

  if (!lock->get_parent()->is_auth()) {
    dout(7) << "try_simple_eval not auth for " << *lock->get_parent() << dendl;
    return;
  }

  if (!lock->get_parent()->can_auth_pin()) {
    dout(7) << "try_simple_eval can't auth_pin, waiting on " << *lock->get_parent() << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    lock->get_parent()->add_waiter(MDSCacheObject::WAIT_UNFREEZE, new C_Locker_SimpleEval(this, lock));
    return;
  }

  if (lock->is_stable())
    simple_eval(lock);
}
*/

void Locker::simple_eval_gather(SimpleLock *lock)
{
  dout(10) << "simple_eval_gather " << *lock << " on " << *lock->get_parent() << dendl;

  // finished gathering?
  if (lock->get_state() == LOCK_GLOCKR &&
      !lock->is_gathering() &&
      lock->get_num_client_lease() == 0 &&
      !lock->is_rdlocked()) {
    dout(7) << "simple_eval finished gather on " << *lock << " on " << *lock->get_parent() << dendl;

    // replica: tell auth
    if (!lock->get_parent()->is_auth()) {
      int auth = lock->get_parent()->authority().first;
      if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN) 
	mds->send_message_mds(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid()), 
			      lock->get_parent()->authority().first);
    }
    
    lock->set_state(LOCK_LOCK);
    lock->finish_waiters(SimpleLock::WAIT_STABLE|SimpleLock::WAIT_WR);

    if (lock->get_parent()->is_auth()) {
      lock->get_parent()->auth_unpin();

      // re-eval?
      simple_eval(lock);
    }
  }
}

void Locker::simple_eval(SimpleLock *lock)
{
  dout(10) << "simple_eval " << *lock << " on " << *lock->get_parent() << dendl;

  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  if (lock->get_parent()->is_frozen()) return;

  // stable -> sync?
  if (!lock->is_xlocked() &&
      lock->get_state() != LOCK_SYNC &&
      !lock->is_waiter_for(SimpleLock::WAIT_WR)) {
    dout(7) << "simple_eval stable, syncing " << *lock 
	    << " on " << *lock->get_parent() << dendl;
    simple_sync(lock);
  }
  
}


// mid

void Locker::simple_sync(SimpleLock *lock)
{
  dout(7) << "simple_sync on " << *lock << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());
  
  // check state
  if (lock->get_state() == LOCK_SYNC)
    return; // already sync
  assert(lock->get_state() == LOCK_LOCK);

  // sync.
  if (lock->get_parent()->is_replicated()) {
    // hard data
    bufferlist data;
    lock->encode_locked_state(data);
    
    // bcast to replicas
    send_lock_message(lock, LOCK_AC_SYNC, data);
  }
  
  // change lock
  lock->set_state(LOCK_SYNC);
  
  // waiters?
  lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);
}

void Locker::simple_lock(SimpleLock *lock)
{
  dout(7) << "simple_lock on " << *lock << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());
  
  // check state
  if (lock->get_state() == LOCK_LOCK) return;
  assert(lock->get_state() == LOCK_SYNC);
  
  if (lock->get_parent()->is_replicated() ||
      lock->get_num_client_lease() ||
      lock->is_rdlocked()) {
    // bcast to mds replicas
    send_lock_message(lock, LOCK_AC_LOCK);

    // bcast to client replicas
    revoke_client_leases(lock);
    
    // change lock
    lock->set_state(LOCK_GLOCKR);
    lock->init_gather();
    lock->get_parent()->auth_pin();
  } else {
    lock->set_state(LOCK_LOCK);
  }
}


// top

bool Locker::simple_rdlock_try(SimpleLock *lock, Context *con)
{
  dout(7) << "simple_rdlock_try on " << *lock << " on " << *lock->get_parent() << dendl;  

  // can read?  grab ref.
  if (lock->can_rdlock(0)) 
    return true;
  
  // wait!
  dout(7) << "simple_rdlock_try waiting on " << *lock << " on " << *lock->get_parent() << dendl;
  if (con) lock->add_waiter(SimpleLock::WAIT_RD, con);
  return false;
}

bool Locker::simple_rdlock_start(SimpleLock *lock, MDRequest *mut)
{
  dout(7) << "simple_rdlock_start  on " << *lock << " on " << *lock->get_parent() << dendl;  

  // can read?  grab ref.
  if (lock->can_rdlock(mut)) {
    lock->get_rdlock();
    mut->rdlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  }
  
  // wait!
  dout(7) << "simple_rdlock_start waiting on " << *lock << " on " << *lock->get_parent() << dendl;
  lock->add_waiter(SimpleLock::WAIT_RD, new C_MDS_RetryRequest(mdcache, mut));
  return false;
}

void Locker::simple_rdlock_finish(SimpleLock *lock, Mutation *mut)
{
  // drop ref
  lock->put_rdlock();
  if (mut) {
    mut->rdlocks.erase(lock);
    mut->locks.erase(lock);
  }

  dout(7) << "simple_rdlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;
  
  // last one?
  if (!lock->is_rdlocked())
    simple_eval_gather(lock);
}

bool Locker::simple_xlock_start(SimpleLock *lock, MDRequest *mut)
{
  dout(7) << "simple_xlock_start  on " << *lock << " on " << *lock->get_parent() << dendl;

  // xlock by me?
  if (lock->is_xlocked() &&
      lock->get_xlocked_by() == mut) 
    return true;

  // auth?
  if (lock->get_parent()->is_auth()) {
    // auth

    // lock.
    if (lock->get_state() == LOCK_SYNC)
      simple_lock(lock);

    // already locked?
    if (lock->get_state() == LOCK_LOCK) {
      if (lock->is_xlocked()) {
	// by someone else.
	lock->add_waiter(SimpleLock::WAIT_WR, new C_MDS_RetryRequest(mdcache, mut));
	return false;
      }

      // xlock.
      lock->get_xlock(mut);
      mut->xlocks.insert(lock);
      mut->locks.insert(lock);
      return true;
    } else {
      // wait for lock
      lock->add_waiter(SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
      return false;
    }
  } else {
    // replica
    // this had better not be a remote xlock attempt!
    assert(!mut->slave_request);

    // wait for single auth
    if (lock->get_parent()->is_ambiguous_auth()) {
      lock->get_parent()->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, 
				     new C_MDS_RetryRequest(mdcache, mut));
      return false;
    }

    // send lock request
    int auth = lock->get_parent()->authority().first;
    mut->more()->slaves.insert(auth);
    MMDSSlaveRequest *r = new MMDSSlaveRequest(mut->reqid, MMDSSlaveRequest::OP_XLOCK);
    r->set_lock_type(lock->get_type());
    lock->get_parent()->set_object_info(r->get_object_info());
    mds->send_message_mds(r, auth);
    
    // wait
    lock->add_waiter(SimpleLock::WAIT_REMOTEXLOCK, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }
}


void Locker::simple_xlock_finish(SimpleLock *lock, Mutation *mut)
{
  dout(7) << "simple_xlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;

  // drop ref
  assert(lock->can_xlock(mut));
  lock->put_xlock();
  assert(mut);
  mut->xlocks.erase(lock);
  mut->locks.erase(lock);

  // remote xlock?
  if (!lock->get_parent()->is_auth()) {
    // tell auth
    dout(7) << "simple_xlock_finish releasing remote xlock on " << *lock->get_parent()  << dendl;
    int auth = lock->get_parent()->authority().first;
    if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN) {
      MMDSSlaveRequest *slavereq = new MMDSSlaveRequest(mut->reqid, MMDSSlaveRequest::OP_UNXLOCK);
      slavereq->set_lock_type(lock->get_type());
      lock->get_parent()->set_object_info(slavereq->get_object_info());
      mds->send_message_mds(slavereq, auth);
    }
  }

  // others waiting?
  lock->finish_waiters(SimpleLock::WAIT_STABLE |
		       SimpleLock::WAIT_WR | 
		       SimpleLock::WAIT_RD, 0); 

  // eval?
  if (lock->get_parent()->is_auth())
    simple_eval(lock);
}



// dentry specific helpers

/** dentry_can_rdlock_trace
 * see if we can _anonymously_ rdlock an entire trace.  
 * if not, and req is specified, wait and retry that message.
 */
bool Locker::dentry_can_rdlock_trace(vector<CDentry*>& trace) 
{
  // verify dentries are rdlockable.
  // we do this because
  // - we're being less aggressive about locks acquisition, and
  // - we're not acquiring the locks in order!
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) {
    CDentry *dn = *it;
    if (!dn->lock.can_rdlock(0)) {
      dout(10) << "can_rdlock_trace can't rdlock " << *dn << dendl;
      return false;
    }
  }
  return true;
}

void Locker::dentry_anon_rdlock_trace_start(vector<CDentry*>& trace)
{
  // grab dentry rdlocks
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) {
    dout(10) << "dentry_anon_rdlock_trace_start rdlocking " << (*it)->lock << " " << **it << dendl;
    (*it)->lock.get_rdlock();
  }
}


void Locker::dentry_anon_rdlock_trace_finish(vector<CDentry*>& trace)
{
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) 
    simple_rdlock_finish(&(*it)->lock, 0);
}



// ==========================================================================
// scatter lock

bool Locker::scatter_rdlock_start(ScatterLock *lock, MDRequest *mut)
{
  dout(7) << "scatter_rdlock_start  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  

  // read on stable scattered replica?  
  if (lock->get_state() == LOCK_SCATTER &&
      !lock->get_parent()->is_auth()) {
    dout(7) << "scatter_rdlock_start  scatterlock read on a stable scattered replica, fw to auth" << dendl;
    mdcache->request_forward(mut, lock->get_parent()->authority().first);
    return false;
  }

  // pre-twiddle?
  if (lock->get_state() == LOCK_SCATTER &&
      lock->get_parent()->is_auth() &&
      !lock->get_parent()->is_replicated() &&
      !lock->is_wrlocked()) 
    scatter_sync(lock);

  // can rdlock?
  if (lock->can_rdlock(mut)) {
    lock->get_rdlock();
    mut->rdlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  }

  // wait for read.
  lock->add_waiter(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));

  // initiate sync or tempsync?
  if (lock->is_stable() &&
      lock->get_parent()->is_auth()) {
    if (lock->get_parent()->is_replicated())
      scatter_tempsync(lock);
    else
      scatter_sync(lock);
  }

  return false;
}

void Locker::scatter_rdlock_finish(ScatterLock *lock, Mutation *mut)
{
  dout(7) << "scatter_rdlock_finish  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->put_rdlock();
  if (mut) {
    mut->rdlocks.erase(lock);
    mut->locks.erase(lock);
  }
  
  scatter_eval_gather(lock);
}


bool Locker::scatter_wrlock_try(ScatterLock *lock, Mutation *mut)
{
  // pre-twiddle?
  if (lock->get_parent()->is_auth() &&
      !lock->get_parent()->is_replicated() &&
      !lock->is_rdlocked() &&
      !lock->is_xlocked() &&
      lock->get_num_client_lease() == 0 &&
      lock->get_state() == LOCK_SYNC) 
    lock->set_state(LOCK_SCATTER);
  //scatter_scatter(lock);

  // can wrlock?
  if (lock->can_wrlock()) {
    lock->get_wrlock();
    mut->wrlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  }

  return false;
}

bool Locker::scatter_wrlock_start(ScatterLock *lock, MDRequest *mut)
{
  dout(7) << "scatter_wrlock_start  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  
  if (scatter_wrlock_try(lock, mut))
    return true;

  // wait for write.
  lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, 
		   new C_MDS_RetryRequest(mdcache, mut));
  
  // initiate scatter or lock?
  if (lock->is_stable()) {
    if (lock->get_parent()->is_auth()) {
      // auth.  scatter or lock?
      if (((CInode*)lock->get_parent())->has_subtree_root_dirfrag()) 
	scatter_scatter(lock);
      else
	scatter_lock(lock);
    } else {
      // replica.
      // auth should be auth_pinned (see acquire_locks wrlock weird mustpin case).
      int auth = lock->get_parent()->authority().first;
      dout(10) << "requesting scatter from auth on " 
	       << *lock << " on " << *lock->get_parent() << dendl;
      mds->send_message_mds(new MLock(lock, LOCK_AC_REQSCATTER, mds->get_nodeid()), auth);
    }
  }

  return false;
}

void Locker::scatter_wrlock_finish(ScatterLock *lock, Mutation *mut)
{
  dout(7) << "scatter_wrlock_finish  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->put_wrlock();
  if (mut) {
    mut->wrlocks.erase(lock);
    mut->locks.erase(lock);
  }
  
  scatter_eval_gather(lock);
}


bool Locker::scatter_xlock_start(ScatterLock *lock, MDRequest *mut)
{
  dout(7) << "file_xlock_start on " << *lock << " on " << *lock->get_parent() << dendl;

  assert(lock->get_parent()->is_auth());  // remote scatter xlock not implemented

  // already xlocked by me?
  if (lock->get_xlocked_by() == mut)
    return true;

  // can't write?
  if (!lock->can_xlock(mut)) {
    
    // auth
    if (!lock->can_xlock_soon()) {
      if (!lock->is_stable()) {
	dout(7) << "scatter_xlock_start on auth, waiting for stable on " << *lock << " on " << *lock->get_parent() << dendl;
	lock->add_waiter(SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
	return false;
      }
      
      // initiate lock 
      scatter_lock(lock);
      
      // fall-thru to below.
    }
  } 
  
  // check again
  if (lock->can_xlock(mut)) {
    assert(lock->get_parent()->is_auth());
    lock->get_xlock(mut);
    mut->locks.insert(lock);
    mut->xlocks.insert(lock);
    return true;
  } else {
    dout(7) << "scatter_xlock_start on auth, waiting for write on " << *lock << " on " << *lock->get_parent() << dendl;
    lock->add_waiter(SimpleLock::WAIT_WR, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }
}

void Locker::scatter_xlock_finish(ScatterLock *lock, Mutation *mut)
{
  dout(7) << "scatter_xlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;

  // drop ref
  assert(lock->can_xlock(mut));
  lock->put_xlock();
  mut->locks.erase(lock);
  mut->xlocks.erase(lock);

  assert(lock->get_parent()->is_auth());  // or implement remote xlocks

  // others waiting?
  lock->finish_waiters(SimpleLock::WAIT_STABLE | 
		       SimpleLock::WAIT_WR | 
		       SimpleLock::WAIT_RD, 0); 

  if (lock->get_parent()->is_auth())
    scatter_eval(lock);
}


class C_Locker_ScatterEval : public Context {
  Locker *locker;
  ScatterLock *lock;
public:
  C_Locker_ScatterEval(Locker *l, ScatterLock *lk) : locker(l), lock(lk) {
    lock->get_parent()->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    lock->get_parent()->put(CInode::PIN_PTRWAITER);
    locker->try_scatter_eval(lock);
  }
};


void Locker::try_scatter_eval(ScatterLock *lock)
{
  // unstable and ambiguous auth?
  if (!lock->is_stable() &&
      lock->get_parent()->is_ambiguous_auth()) {
    dout(7) << "try_scatter_eval not stable and ambiguous auth, waiting on " << *lock->get_parent() << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    lock->get_parent()->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, new C_Locker_ScatterEval(this, lock));
    return;
  }

  if (!lock->get_parent()->is_auth()) {
    dout(7) << "try_scatter_eval not auth for " << *lock->get_parent() << dendl;
    return;
  }

  if (!lock->get_parent()->can_auth_pin()) {
    dout(7) << "try_scatter_eval can't auth_pin, waiting on " << *lock->get_parent() << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    lock->get_parent()->add_waiter(MDSCacheObject::WAIT_UNFREEZE, new C_Locker_ScatterEval(this, lock));
    return;
  }

  if (lock->is_stable())
    scatter_eval(lock);
}


void Locker::scatter_eval_gather(ScatterLock *lock)
{
  dout(10) << "scatter_eval_gather " << *lock << " on " << *lock->get_parent() << dendl;

  if (!lock->get_parent()->is_auth()) {
    // REPLICA

    if (lock->get_state() == LOCK_GLOCKC &&
	!lock->is_wrlocked()) {
      dout(10) << "scatter_eval no wrlocks, acking lock" << dendl;
      int auth = lock->get_parent()->authority().first;
      if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN) {
	bufferlist data;
	lock->encode_locked_state(data);
	mds->send_message_mds(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid(), data), auth);
      }
      lock->set_state(LOCK_LOCK);
    }
    
  } else {
    // AUTH

    // glocks|glockt -> lock?
    if ((lock->get_state() == LOCK_GLOCKS || 
	 lock->get_state() == LOCK_GLOCKT) &&
	!lock->is_gathering() &&
	lock->get_num_client_lease() == 0 &&
	!lock->is_rdlocked()) {
      dout(7) << "scatter_eval finished lock gather/un-rdlock on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      lock->set_state(LOCK_LOCK);
      lock->finish_waiters(ScatterLock::WAIT_XLOCK|ScatterLock::WAIT_STABLE);
      lock->get_parent()->auth_unpin();
    }
    
    // glockc -> lock?
    else if (lock->get_state() == LOCK_GLOCKC &&
	     !lock->is_gathering() &&
	     !lock->is_wrlocked()) {
      if (lock->is_updated()) {
	scatter_writebehind(lock);
      } else {
	dout(7) << "scatter_eval finished lock gather/un-wrlock on " << *lock
	      << " on " << *lock->get_parent() << dendl;
	lock->set_state(LOCK_LOCK);
	lock->finish_waiters(ScatterLock::WAIT_XLOCK|ScatterLock::WAIT_STABLE);
	lock->get_parent()->auth_unpin();
      }
    }

    // gSyncL -> sync?
    else if (lock->get_state() == LOCK_GSYNCL &&
	     !lock->is_wrlocked()) {
      dout(7) << "scatter_eval finished sync un-wrlock on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      if (lock->get_parent()->is_replicated()) {
	// encode and bcast
	bufferlist data;
	lock->encode_locked_state(data);
	send_lock_message(lock, LOCK_AC_SYNC, data);
      }
      lock->set_state(LOCK_SYNC);
      lock->finish_waiters(ScatterLock::WAIT_RD|ScatterLock::WAIT_STABLE);
      lock->get_parent()->auth_unpin();
    }

    // gscattert|gscatters -> scatter?
    else if ((lock->get_state() == LOCK_GSCATTERT ||
	      lock->get_state() == LOCK_GSCATTERS) &&
	     !lock->is_gathering() &&
	     lock->get_num_client_lease() == 0 &&
	     !lock->is_rdlocked()) {
      dout(7) << "scatter_eval finished scatter un-rdlock(/gather) on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      if (lock->get_parent()->is_replicated()) {
	// encode and bcast
	bufferlist data;
	lock->encode_locked_state(data);
	send_lock_message(lock, LOCK_AC_SCATTER, data);
      }
      lock->set_state(LOCK_SCATTER);
      lock->finish_waiters(ScatterLock::WAIT_WR|ScatterLock::WAIT_STABLE);      
      lock->get_parent()->auth_unpin();
    }

    // gTempsyncC|gTempsyncL -> tempsync
    else if ((lock->get_state() == LOCK_GTEMPSYNCC ||
	      lock->get_state() == LOCK_GTEMPSYNCL) &&
	     !lock->is_gathering() &&
	     !lock->is_wrlocked()) {
      if (lock->is_updated()) {
	scatter_writebehind(lock);
      } else {
	dout(7) << "scatter_eval finished tempsync gather/un-wrlock on " << *lock
		<< " on " << *lock->get_parent() << dendl;
	lock->set_state(LOCK_TEMPSYNC);
	lock->finish_waiters(ScatterLock::WAIT_RD|ScatterLock::WAIT_STABLE);
	lock->get_parent()->auth_unpin();
      }
    }


    // re-eval?
    if (lock->is_stable()) // && lock->get_parent()->can_auth_pin())
      scatter_eval(lock);
  }
}

void Locker::scatter_writebehind(ScatterLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(10) << "scatter_writebehind " << in->inode.mtime << " on " << *lock << " on " << *in << dendl;

  // hack:
  if (in->is_base()) {
    dout(10) << "scatter_writebehind just clearing updated flag for base inode " << *in << dendl;
    lock->clear_updated();
    scatter_eval_gather(lock);
    return;
  }

  // journal write-behind.
  inode_t *pi = in->project_inode();
  pi->mtime = in->inode.mtime;   // make sure an intermediate version isn't goofing us up
  pi->version = in->pre_dirty();

  lock->get_parent()->finish_scatter_gather_update(lock->get_type());
  
  EUpdate *le = new EUpdate(mds->mdlog, "scatter writebehind");
  le->metablob.add_dir_context(in->get_parent_dn()->get_dir());
  le->metablob.add_primary_dentry(in->get_parent_dn(), true, 0, pi);
  
  mds->mdlog->submit_entry(le);
  mds->mdlog->wait_for_sync(new C_Locker_ScatterWB(this, lock, mds->mdlog->get_current_segment()));
}

void Locker::scatter_writebehind_finish(ScatterLock *lock, LogSegment *ls)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(10) << "scatter_writebehind_finish on " << *lock << " on " << *in << dendl;
  in->pop_and_dirty_projected_inode(ls);
  lock->clear_updated();
  scatter_eval_gather(lock);
}

void Locker::scatter_eval(ScatterLock *lock)
{
  dout(10) << "scatter_eval " << *lock << " on " << *lock->get_parent() << dendl;

  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  if (lock->get_parent()->is_frozen()) return;

  CInode *in = (CInode*)lock->get_parent();
  if (in->has_subtree_root_dirfrag() && !in->is_base()) {
    // i _should_ be scattered.
    if (!lock->is_rdlocked() &&
	!lock->is_xlocked() &&
	lock->get_state() != LOCK_SCATTER) {
      dout(10) << "scatter_eval no rdlocks|xlocks, am subtree root inode, scattering" << dendl;
      scatter_scatter(lock);
      autoscattered.push_back(&lock->xlistitem_autoscattered);
    }
  } else {
    // i _should_ be sync.
    lock->xlistitem_autoscattered.remove_myself(); 
    if (!lock->is_wrlocked() &&
	!lock->is_xlocked() &&
	lock->get_state() != LOCK_SYNC) {
      dout(10) << "scatter_eval no wrlocks|xlocks, not subtree root inode, syncing" << dendl;
      scatter_sync(lock);
    }
  }
}

void Locker::note_autoscattered(ScatterLock *lock)
{
  dout(10) << "note_autoscattered " << *lock << " on " << *lock->get_parent() << dendl;
  autoscattered.push_back(&lock->xlistitem_autoscattered);
}


/*
 * this is called by LogSegment::try_to_trim() when trying to 
 * flush dirty scattered data (e.g. inode->dirlock mtime) back
 * to the auth node.
 */
void Locker::scatter_try_unscatter(ScatterLock *lock, Context *c)
{
  dout(10) << "scatter_try_unscatter " << *lock << " on " << *lock->get_parent() << dendl;
  assert(!lock->get_parent()->is_auth());
  assert(!lock->get_parent()->is_ambiguous_auth());

  // request unscatter?
  int auth = lock->get_parent()->authority().first;
  if (lock->get_state() == LOCK_SCATTER &&
      mds->mdsmap->get_state(auth) >= MDSMap::STATE_ACTIVE) 
    mds->send_message_mds(new MLock(lock, LOCK_AC_REQUNSCATTER, mds->get_nodeid()), auth);
  
  // wait...
  lock->add_waiter(SimpleLock::WAIT_STABLE, c);
}


void Locker::scatter_sync(ScatterLock *lock)
{
  dout(10) << "scatter_sync " << *lock
	   << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  switch (lock->get_state()) {
  case LOCK_SYNC:
    return;    // already sync.

  case LOCK_TEMPSYNC:
    break;    // just do it.

  case LOCK_LOCK:
    if (lock->is_wrlocked() || lock->is_xlocked()) {
      lock->set_state(LOCK_GSYNCL);
      lock->get_parent()->auth_pin();
      return;
    }
    break; // do it.

  case LOCK_SCATTER:
    // lock first.  this is the slow way, incidentally.
    if (lock->get_parent()->is_replicated()) {
      send_lock_message(lock, LOCK_AC_LOCK);
      lock->init_gather();
    } else {
      if (!lock->is_wrlocked()) {
	break; // do it now, we're fine
      }
    }
    lock->set_state(LOCK_GLOCKC);
    lock->get_parent()->auth_pin();
    return;

  default:
    assert(0);
  }
  
  // do sync
  if (lock->get_parent()->is_replicated()) {
    // encode and bcast
    bufferlist data;
    lock->encode_locked_state(data);
    send_lock_message(lock, LOCK_AC_SYNC, data);
  }

  lock->set_state(LOCK_SYNC);
  lock->finish_waiters(ScatterLock::WAIT_RD|ScatterLock::WAIT_STABLE);
}

void Locker::scatter_scatter(ScatterLock *lock)
{
  dout(10) << "scatter_scatter " << *lock
	   << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());
  
  lock->set_last_scatter(g_clock.now());

  switch (lock->get_state()) {
  case LOCK_SYNC:
    if (!lock->is_rdlocked() &&
	!lock->get_parent()->is_replicated() &&
	!lock->get_num_client_lease())
      break; // do it
    if (lock->get_parent()->is_replicated()) {
      send_lock_message(lock, LOCK_AC_LOCK);
      lock->init_gather();
    }
    revoke_client_leases(lock);
    lock->set_state(LOCK_GSCATTERS);
    lock->get_parent()->auth_pin();
    return;

  case LOCK_LOCK:
    if (lock->is_xlocked())
      return;  // sorry
    break; // do it.

  case LOCK_SCATTER:
    return; // did it.

  case LOCK_TEMPSYNC:
    if (lock->is_rdlocked()) {
      lock->set_state(LOCK_GSCATTERT);
      lock->get_parent()->auth_pin();
      return;
    }
    break; // do it

  default: 
    assert(0);
  }

  // do scatter
  if (lock->get_parent()->is_replicated()) {
    // encode and bcast
    bufferlist data;
    lock->encode_locked_state(data);
    send_lock_message(lock, LOCK_AC_SCATTER, data);
  } 
  lock->set_state(LOCK_SCATTER);
  lock->finish_waiters(ScatterLock::WAIT_WR|ScatterLock::WAIT_STABLE);
}

void Locker::scatter_lock(ScatterLock *lock)
{
  dout(10) << "scatter_lock " << *lock
	   << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  switch (lock->get_state()) {
  case LOCK_SYNC:
    if (!lock->is_rdlocked() &&
	!lock->get_parent()->is_replicated() &&
	!lock->get_num_client_lease())
      break; // do it.

    if (lock->get_parent()->is_replicated()) {
      send_lock_message(lock, LOCK_AC_LOCK);
      lock->init_gather();
    } 
    revoke_client_leases(lock);  
    lock->set_state(LOCK_GLOCKS);
    lock->get_parent()->auth_pin();
    return;

  case LOCK_LOCK:
    return; // done.

  case LOCK_SCATTER:
    if (!lock->is_wrlocked() &&
	!lock->get_parent()->is_replicated()) {
      break; // do it.
    }

    if (lock->get_parent()->is_replicated()) {
      send_lock_message(lock, LOCK_AC_LOCK);
      lock->init_gather();
    }
    lock->set_state(LOCK_GLOCKC);
    lock->get_parent()->auth_pin();
    return;

  case LOCK_TEMPSYNC:
    if (lock->is_rdlocked()) {
      lock->set_state(LOCK_GLOCKT);
      lock->get_parent()->auth_pin();
      return;
    }
    break; // do it.
  }

  // do lock
  lock->set_state(LOCK_LOCK);
  lock->finish_waiters(ScatterLock::WAIT_XLOCK|ScatterLock::WAIT_WR|ScatterLock::WAIT_STABLE);
}

void Locker::scatter_tempsync(ScatterLock *lock)
{
  dout(10) << "scatter_tempsync " << *lock
	   << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  switch (lock->get_state()) {
  case LOCK_SYNC:
    assert(0);   // this shouldn't happen

  case LOCK_LOCK:
    if (lock->is_wrlocked() ||
	lock->is_xlocked()) {
      lock->set_state(LOCK_GTEMPSYNCL);
      lock->get_parent()->auth_pin();
      return;
    }
    break; // do it.

  case LOCK_SCATTER:
    if (!lock->is_wrlocked() &&
	!lock->get_parent()->is_replicated()) {
      break; // do it.
    }
    
    if (lock->get_parent()->is_replicated()) {
      send_lock_message(lock, LOCK_AC_LOCK);
      lock->init_gather();
    }
    lock->set_state(LOCK_GTEMPSYNCC);
    lock->get_parent()->auth_pin();
    return;

  case LOCK_TEMPSYNC:
    return; // done
  }
  
  // do tempsync
  lock->set_state(LOCK_TEMPSYNC);
  lock->finish_waiters(ScatterLock::WAIT_RD|ScatterLock::WAIT_STABLE);
}




void Locker::handle_scatter_lock(ScatterLock *lock, MLock *m)
{
  int from = m->get_asker();
  dout(10) << "handle_scatter_lock " << *m << " on " << *lock << " on " << *lock->get_parent() << dendl;
  
  if (mds->is_rejoin()) {
    if (lock->get_parent()->is_rejoining()) {
      dout(7) << "handle_scatter_lock still rejoining " << *lock->get_parent()
	      << ", dropping " << *m << dendl;
      delete m;
      return;
    }
  }

  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK);
    lock->set_state(LOCK_SYNC);
    lock->decode_locked_state(m->get_data());
    lock->clear_updated();
    lock->finish_waiters(ScatterLock::WAIT_RD|ScatterLock::WAIT_STABLE);
    break;

  case LOCK_AC_LOCK:
    assert(lock->get_state() == LOCK_SCATTER ||
	   lock->get_state() == LOCK_SYNC);

    // wait for wrlocks to close?
    if (lock->is_wrlocked()) {
      assert(lock->get_state() == LOCK_SCATTER);
      dout(7) << "handle_scatter_lock has wrlocks, waiting on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      lock->set_state(LOCK_GLOCKC);
    } else if (lock->is_rdlocked()) {
      assert(lock->get_state() == LOCK_SYNC);
      dout(7) << "handle_scatter_lock has rdlocks, waiting on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      lock->set_state(LOCK_GLOCKS);
    } else {
      dout(7) << "handle_scatter_lock has no rd|wrlocks, sending lockack for " << *lock
	      << " on " << *lock->get_parent() << dendl;
      
      // encode and reply
      bufferlist data;
      lock->encode_locked_state(data);
      mds->send_message_mds(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid(), data), from);
      lock->set_state(LOCK_LOCK);
    }
    break;

  case LOCK_AC_SCATTER:
    assert(lock->get_state() == LOCK_LOCK);
    lock->decode_locked_state(m->get_data());
    lock->clear_updated();
    lock->set_state(LOCK_SCATTER);
    lock->finish_waiters(ScatterLock::WAIT_WR|ScatterLock::WAIT_STABLE);
    break;

    // -- for auth --
  case LOCK_AC_LOCKACK:
    assert(lock->get_state() == LOCK_GLOCKS ||
	   lock->get_state() == LOCK_GLOCKC ||
	   lock->get_state() == LOCK_GSCATTERS ||
	   lock->get_state() == LOCK_GTEMPSYNCC);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    lock->decode_locked_state(m->get_data());
    
    if (lock->is_gathering()) {
      dout(7) << "handle_scatter_lock " << *lock << " on " << *lock->get_parent()
	      << " from " << from << ", still gathering " << lock->get_gather_set()
	      << dendl;
    } else {
      dout(7) << "handle_scatter_lock " << *lock << " on " << *lock->get_parent()
	      << " from " << from << ", last one" 
	      << dendl;
      scatter_eval_gather(lock);
    }
    break;

  case LOCK_AC_REQSCATTER:
    if (lock->is_stable()) {
      /* NOTE: we can do this _even_ if !can_auth_pin (i.e. freezing)
       *  because the replica should be holding an auth_pin if they're
       *  doing this (and thus, we are freezing, not frozen, and indefinite
       *  starvation isn't an issue).
       */
      dout(7) << "handle_scatter_lock got scatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      scatter_scatter(lock);
    } else {
      dout(7) << "handle_scatter_lock ignoring scatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
    }
    break;

  case LOCK_AC_REQUNSCATTER:
    if (!lock->is_stable()) {
      dout(7) << "handle_scatter_lock ignoring now-unnecessary unscatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
    } else if (lock->get_parent()->can_auth_pin()) {
      dout(7) << "handle_scatter_lock got unscatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      scatter_lock(lock);
    } else {
      dout(7) << "handle_scatter_lock DROPPING unscatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      /* FIXME: if we can't auth_pin here, this request is effectively lost... */
    }
  }

  delete m;
}



void Locker::scatter_unscatter_autoscattered()
{
  /* 
   * periodically unscatter autoscattered locks
   */

  dout(10) << "scatter_unscatter_autoscattered" << dendl;
  
  utime_t now = g_clock.now();
  int n = autoscattered.size();
  while (!autoscattered.empty()) {
    ScatterLock *lock = autoscattered.front();
    
    // stop?
    if (lock->get_state() == LOCK_SCATTER &&
	now - lock->get_last_scatter() < 10.0) 
      break;
    
    autoscattered.pop_front();

    if (lock->get_state() == LOCK_SCATTER &&
	lock->get_parent()->is_replicated()) {
      if (((CInode*)lock->get_parent())->is_frozen() ||
	  ((CInode*)lock->get_parent())->is_freezing()) {
	// hrm.. requeue.
	dout(10) << "last_scatter " << lock->get_last_scatter() 
		 << ", now " << now << ", but frozen|freezing, requeueing" << dendl;
	autoscattered.push_back(&lock->xlistitem_autoscattered);	
      } else {
	dout(10) << "last_scatter " << lock->get_last_scatter() 
		 << ", now " << now << ", locking" << dendl;
	scatter_lock(lock);
      }
    }
    if (--n == 0) break;
  }
}



// ==========================================================================
// local lock


bool Locker::local_wrlock_start(LocalLock *lock, MDRequest *mut)
{
  dout(7) << "local_wrlock_start  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  
  if (lock->can_wrlock()) {
    lock->get_wrlock();
    mut->wrlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  } else {
    lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }
}

void Locker::local_wrlock_finish(LocalLock *lock, Mutation *mut)
{
  dout(7) << "local_wrlock_finish  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->put_wrlock();
  mut->wrlocks.erase(lock);
  mut->locks.erase(lock);
}

bool Locker::local_xlock_start(LocalLock *lock, MDRequest *mut)
{
  dout(7) << "local_xlock_start  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  
  if (lock->is_xlocked_by_other(mut)) {
    lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }

  lock->get_xlock(mut);
  mut->xlocks.insert(lock);
  mut->locks.insert(lock);
  return true;
}

void Locker::local_xlock_finish(LocalLock *lock, Mutation *mut)
{
  dout(7) << "local_xlock_finish  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->put_xlock();
  mut->xlocks.erase(lock);
  mut->locks.erase(lock);

  lock->finish_waiters(SimpleLock::WAIT_STABLE | 
		       SimpleLock::WAIT_WR | 
		       SimpleLock::WAIT_RD);
}



// ==========================================================================
// file lock


bool Locker::file_rdlock_start(FileLock *lock, MDRequest *mut)
{
  dout(7) << "file_rdlock_start " << *lock << " on " << *lock->get_parent() << dendl;

  // can read?  grab ref.
  if (lock->can_rdlock(mut)) {
    lock->get_rdlock();
    mut->rdlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  }
  
  // can't read, and replicated.
  if (lock->can_rdlock_soon()) {
    // wait
    dout(7) << "file_rdlock_start can_rdlock_soon " << *lock << " on " << *lock->get_parent() << dendl;
  } else {    
    if (lock->get_parent()->is_auth()) {
      // auth

      // FIXME or qsync?

      if (lock->is_stable()) {
        file_lock(lock);     // lock, bc easiest to back off ... FIXME
	
        if (lock->can_rdlock(mut)) {
          lock->get_rdlock();
	  mut->rdlocks.insert(lock);
	  mut->locks.insert(lock);
          
          lock->finish_waiters(SimpleLock::WAIT_STABLE);
          return true;
        }
      } else {
        dout(7) << "file_rdlock_start waiting until stable on " << *lock << " on " << *lock->get_parent() << dendl;
        lock->add_waiter(SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
        return false;
      }
    } else {
      // replica
      if (lock->is_stable()) {
	
        // fw to auth
	CInode *in = (CInode*)lock->get_parent();
        int auth = in->authority().first;
        dout(7) << "file_rdlock_start " << *lock << " on " << *lock->get_parent() << " on replica and async, fw to auth " << auth << dendl;
        assert(auth != mds->get_nodeid());
        mdcache->request_forward(mut, auth);
        return false;
        
      } else {
        // wait until stable
        dout(7) << "inode_file_rdlock_start waiting until stable on " << *lock << " on " << *lock->get_parent() << dendl;
        lock->add_waiter(SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
        return false;
      }
    }
  }
  
  // wait
  dout(7) << "file_rdlock_start waiting on " << *lock << " on " << *lock->get_parent() << dendl;
  lock->add_waiter(SimpleLock::WAIT_RD, new C_MDS_RetryRequest(mdcache, mut));
        
  return false;
}



void Locker::file_rdlock_finish(FileLock *lock, Mutation *mut)
{
  dout(7) << "rdlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;

  // drop ref
  lock->put_rdlock();
  mut->rdlocks.erase(lock);
  mut->locks.erase(lock);

  if (!lock->is_rdlocked()) {
    if (!lock->is_stable())
      file_eval_gather(lock);
    else if (lock->get_parent()->is_auth())
      file_eval(lock);
  }
}

bool Locker::file_wrlock_force(FileLock *lock, Mutation *mut)
{
  dout(7) << "file_wrlock_force  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->get_wrlock(true);
  mut->wrlocks.insert(lock);
  mut->locks.insert(lock);
  return true;

  /*
  if (lock->can_wrlock()) {
    lock->get_wrlock();
    mut->wrlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  } else {
    lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    return false;
    }*/
}

void Locker::file_wrlock_finish(FileLock *lock, Mutation *mut)
{
  dout(7) << "wrlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;
  lock->put_wrlock();
  if (mut) {
    mut->wrlocks.erase(lock);
    mut->locks.erase(lock);
  }

  if (!lock->is_wrlocked())
    file_eval_gather(lock);
}


bool Locker::file_xlock_start(FileLock *lock, MDRequest *mut)
{
  dout(7) << "file_xlock_start on " << *lock << " on " << *lock->get_parent() << dendl;

  assert(lock->get_parent()->is_auth());  // remote file xlock not implemented

  // already xlocked by me?
  if (lock->get_xlocked_by() == mut)
    return true;

  // can't write?
  if (!lock->can_xlock(mut)) {
    
    // auth
    if (!lock->can_xlock_soon()) {
      if (!lock->is_stable()) {
	dout(7) << "file_xlock_start on auth, waiting for stable on " << *lock << " on " << *lock->get_parent() << dendl;
	lock->add_waiter(SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
	return false;
      }
      
      // initiate lock 
      file_lock(lock);
      
      // fall-thru to below.
    }
  } 
  
  // check again
  if (lock->can_xlock(mut)) {
    assert(lock->get_parent()->is_auth());
    lock->get_xlock(mut);
    mut->locks.insert(lock);
    mut->xlocks.insert(lock);
    return true;
  } else {
    dout(7) << "file_xlock_start on auth, waiting for write on " << *lock << " on " << *lock->get_parent() << dendl;
    lock->add_waiter(SimpleLock::WAIT_WR, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }
}


void Locker::file_xlock_finish(FileLock *lock, Mutation *mut)
{
  dout(7) << "file_xlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;

  // drop ref
  assert(lock->can_xlock(mut));
  lock->put_xlock();
  mut->locks.erase(lock);
  mut->xlocks.erase(lock);

  assert(lock->get_parent()->is_auth());  // or implement remote xlocks

  // others waiting?
  lock->finish_waiters(SimpleLock::WAIT_STABLE | 
		       SimpleLock::WAIT_WR | 
		       SimpleLock::WAIT_RD, 0); 

  if (lock->get_parent()->is_auth())
    file_eval(lock);
}


/*
 * ...
 *
 * also called after client caps are acked to us
 * - checks if we're in unstable sfot state and can now move on to next state
 * - checks if soft state should change (eg bc last writer closed)
 */
class C_Locker_FileEval : public Context {
  Locker *locker;
  FileLock *lock;
public:
  C_Locker_FileEval(Locker *l, FileLock *lk) : locker(l), lock(lk) {
    lock->get_parent()->get(CInode::PIN_PTRWAITER);    
  }
  void finish(int r) {
    lock->get_parent()->put(CInode::PIN_PTRWAITER);
    locker->try_file_eval(lock);
  }
};

void Locker::try_file_eval(FileLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();

  // unstable and ambiguous auth?
  if (!lock->is_stable() &&
      in->is_ambiguous_auth()) {
    dout(7) << "try_file_eval not stable and ambiguous auth, waiting on " << *in << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    in->add_waiter(CInode::WAIT_SINGLEAUTH, new C_Locker_FileEval(this, lock));
    return;
  }

  if (!lock->get_parent()->is_auth()) {
    dout(7) << "try_file_eval not auth for " << *lock->get_parent() << dendl;
    return;
  }

  if (!lock->get_parent()->can_auth_pin()) {
    dout(7) << "try_file_eval can't auth_pin, waiting on " << *in << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    in->add_waiter(CInode::WAIT_UNFREEZE, new C_Locker_FileEval(this, lock));
    return;
  }

  if (lock->is_stable())
    file_eval(lock);
}



void Locker::file_eval_gather(FileLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  int issued = in->get_caps_issued();
 
  dout(7) << "file_eval_gather issued " << cap_string(issued)
	  << " vs " << cap_string(lock->caps_allowed())
	  << " on " << *lock << " on " << *lock->get_parent()
	  << dendl;

  if (lock->is_stable())
    return;  // nothing for us to do here!
  
  // [auth] finished gather?
  if (in->is_auth() &&
      !lock->is_gathering() &&
      !lock->is_wrlocked() &&
      lock->get_num_client_lease() == 0 &&
      ((issued & ~lock->caps_allowed()) == 0)) {

    if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
      dout(7) << "file_eval_gather finished gather, but need to recover" << dendl;
      mds->mdcache->queue_file_recover(in);
      mds->mdcache->do_file_recover();
    }
    if (in->state_test(CInode::STATE_RECOVERING)) {
      dout(7) << "file_eval_gather finished gather, but still recovering" << dendl;
      return;
    }

    dout(7) << "file_eval_gather finished gather" << dendl;
    
    switch (lock->get_state()) {
      // to lock
    case LOCK_GLOCKR:
    case LOCK_GLOCKM:
    case LOCK_GLOCKL:
      lock->set_state(LOCK_LOCK);
      
      // waiters
      lock->get_rdlock();
      lock->finish_waiters(SimpleLock::WAIT_STABLE|SimpleLock::WAIT_WR|SimpleLock::WAIT_RD);
      lock->put_rdlock();
      lock->get_parent()->auth_unpin();
      break;
      
      // to mixed
    case LOCK_GMIXEDR:
      lock->set_state(LOCK_MIXED);
      lock->finish_waiters(SimpleLock::WAIT_STABLE);
      lock->get_parent()->auth_unpin();
      break;

    case LOCK_GMIXEDL:
      lock->set_state(LOCK_MIXED);
      
      if (in->is_replicated()) {
	// data
	bufferlist softdata;
	lock->encode_locked_state(softdata);
        
	// bcast to replicas
	send_lock_message(lock, LOCK_AC_MIXED, softdata);
      }
      
      lock->finish_waiters(SimpleLock::WAIT_STABLE);
      lock->get_parent()->auth_unpin();
      break;

      // to loner
    case LOCK_GLONERR:
      lock->set_state(LOCK_LONER);
      lock->finish_waiters(SimpleLock::WAIT_STABLE);
      lock->get_parent()->auth_unpin();
      break;

    case LOCK_GLONERM:
      lock->set_state(LOCK_LONER);
      lock->finish_waiters(SimpleLock::WAIT_STABLE);
      lock->get_parent()->auth_unpin();
      break;
      
      // to sync
    case LOCK_GSYNCL:
    case LOCK_GSYNCM:
      lock->set_state(LOCK_SYNC);
      
      { // bcast data to replicas
	bufferlist softdata;
	lock->encode_locked_state(softdata);
          
	send_lock_message(lock, LOCK_AC_SYNC, softdata);
      }
      
      // waiters
      lock->get_rdlock();
      lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);
      lock->put_rdlock();
      lock->get_parent()->auth_unpin();
      break;
      
    default: 
      assert(0);
    }

    issue_caps(in);

    // stable re-eval?
    if (lock->is_stable())    //&& lock->get_parent()->can_auth_pin())
      file_eval(lock);
  }
  
  // [replica] finished caps gather?
  if (!in->is_auth() &&
      lock->get_num_client_lease() == 0 &&
      ((issued & ~lock->caps_allowed()) == 0)) {
    switch (lock->get_state()) {
    case LOCK_GMIXEDR:
      { 
	lock->set_state(LOCK_MIXED);
	
	// ack
	MLock *reply = new MLock(lock, LOCK_AC_MIXEDACK, mds->get_nodeid());
	mds->send_message_mds(reply, in->authority().first);
      }
      break;

    case LOCK_GLOCKR:
      {
        lock->set_state(LOCK_LOCK);
        
        // ack
        MLock *reply = new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid());
        mds->send_message_mds(reply, in->authority().first);
      }
      break;

    default:
      assert(0);
    }
  }


}

void Locker::file_eval(FileLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  int wanted = in->get_caps_wanted();
  bool loner = in->is_loner_cap();
  dout(7) << "file_eval wanted=" << cap_string(wanted)
	  << "  filelock=" << *lock << " on " << *lock->get_parent()
	  << "  loner=" << loner
	  << dendl;

  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  // not xlocked!
  if (lock->is_xlocked() || lock->get_parent()->is_frozen()) return;
  
  // * -> loner?
  if (!lock->is_rdlocked() &&
      !lock->is_waiter_for(SimpleLock::WAIT_WR) &&
      (wanted & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) &&
      loner &&
      lock->get_state() != LOCK_LONER) {
    dout(7) << "file_eval stable, bump to loner " << *lock << " on " << *lock->get_parent() << dendl;
    file_loner(lock);
  }

  // * -> mixed?
  else if ((!lock->is_rdlocked() &&
	    !lock->is_waiter_for(SimpleLock::WAIT_WR) &&
	    (wanted & CEPH_CAP_RD) &&
	    (wanted & CEPH_CAP_WR) &&
	    !(loner && lock->get_state() == LOCK_LONER) &&
	    lock->get_state() != LOCK_MIXED) ||
	   (!loner && in->is_any_nonstale_caps() && lock->get_state() == LOCK_LONER)) {
    dout(7) << "file_eval stable, bump to mixed " << *lock << " on " << *lock->get_parent() << dendl;
    file_mixed(lock);
  }
  
  // * -> sync?
  else if (!in->filelock.is_waiter_for(SimpleLock::WAIT_WR) &&
	   !(wanted & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) &&
	   //((wanted & CEPH_CAP_RD) || 
	    //in->is_replicated() || 
	    //lock->get_num_client_lease() || 
	   //(!loner && lock->get_state() == LOCK_LONER)) &&
	   !(loner && lock->get_state() == LOCK_LONER) &&       // leave loner in loner state
	   lock->get_state() != LOCK_SYNC) {
    dout(7) << "file_eval stable, bump to sync " << *lock << " on " << *lock->get_parent() << dendl;
    file_sync(lock);
  }
  
  // * -> lock?  (if not replicated or open)
/*
  else if (!in->is_replicated() &&
	   wanted == 0 &&
	   lock->get_num_client_lease() == 0 && 
	   lock->get_state() != LOCK_LOCK) {
    file_lock(lock);
  }
*/

  else
    issue_caps(in);
}


// mid

bool Locker::file_sync(FileLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(7) << "file_sync " << *lock << " on " << *lock->get_parent() << dendl;  

  assert(in->is_auth());
  assert(lock->is_stable());

  if (lock->get_state() == LOCK_LOCK) {
    if (in->is_replicated()) {
      bufferlist softdata;
      lock->encode_locked_state(softdata);
      send_lock_message(lock, LOCK_AC_SYNC, softdata);
    }
    
    // change lock
    lock->set_state(LOCK_SYNC);
    issue_caps(in);    // reissue caps
    return true;
  } else {
    // gather?
    switch (lock->get_state()) {
    case LOCK_MIXED: lock->set_state(LOCK_GSYNCM); break;
    case LOCK_LONER: lock->set_state(LOCK_GSYNCL); break;
    default: assert(0);
    }

    int gather = 0;
    if (in->is_replicated()) {
      bufferlist softdata;
      lock->encode_locked_state(softdata);
      send_lock_message(lock, LOCK_AC_SYNC, softdata);
      lock->init_gather();
      gather++;
    }
    int issued = in->get_caps_issued();
    if (issued & ~lock->caps_allowed()) {
      issue_caps(in);
      gather++;
    }
    if (lock->is_wrlocked())
      gather++;
    if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
      mds->mdcache->queue_file_recover(in);
      mds->mdcache->do_file_recover();
      gather++;
    }

    if (gather)
      lock->get_parent()->auth_pin();
    else {
      lock->set_state(LOCK_SYNC);
      issue_caps(in);
      return true;
    }
  }

  return false;
}



void Locker::file_lock(FileLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(7) << "file_lock " << *lock << " on " << *lock->get_parent() << dendl;  

  assert(in->is_auth());
  assert(lock->is_stable());

  // gather?
  switch (lock->get_state()) {
  case LOCK_SYNC: lock->set_state(LOCK_GLOCKR); break;
  case LOCK_MIXED: lock->set_state(LOCK_GLOCKM); break;
  case LOCK_LONER: lock->set_state(LOCK_GLOCKL); break;
  default: assert(0);
  }

  int gather = 0;
  if (in->is_replicated()) {
    send_lock_message(lock, LOCK_AC_LOCK);
    lock->init_gather();
    gather++;
  }
  if (lock->get_num_client_lease()) {
    revoke_client_leases(lock);
    gather++;
  }
  int issued = in->get_caps_issued();
  if (issued & ~lock->caps_allowed()) {
    issue_caps(in);
    gather++;
  }
  if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
    mds->mdcache->queue_file_recover(in);
    mds->mdcache->do_file_recover();
    gather++;
  }

  if (gather)
    lock->get_parent()->auth_pin();
  else
    lock->set_state(LOCK_LOCK);
}


void Locker::file_mixed(FileLock *lock)
{
  dout(7) << "file_mixed " << *lock << " on " << *lock->get_parent() << dendl;  

  CInode *in = (CInode*)lock->get_parent();
  assert(in->is_auth());
  assert(lock->is_stable());

  if (lock->get_state() == LOCK_LOCK) {
    if (in->is_replicated()) {
      // data
      bufferlist softdata;
      lock->encode_locked_state(softdata);
      
      // bcast to replicas
      send_lock_message(lock, LOCK_AC_MIXED, softdata);
    }

    // change lock
    lock->set_state(LOCK_MIXED);
    issue_caps(in);
  } else {
    // gather?
    switch (lock->get_state()) {
    case LOCK_SYNC: lock->set_state(LOCK_GMIXEDR); break;
    case LOCK_LONER: lock->set_state(LOCK_GMIXEDL); break;
    default: assert(0);
    }

    int gather = 0;
    if (in->is_replicated()) {
      send_lock_message(lock, LOCK_AC_MIXED);
      lock->init_gather();
      gather++;
    }
    if (lock->get_num_client_lease()) {
      revoke_client_leases(lock);
      gather++;
    }
    int issued = in->get_caps_issued();
    if (issued & ~lock->caps_allowed()) {
      issue_caps(in);
      gather++;
    }
    if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
      mds->mdcache->queue_file_recover(in);
      mds->mdcache->do_file_recover();
      gather++;
    }

    if (gather)
      lock->get_parent()->auth_pin();
    else {
      lock->set_state(LOCK_MIXED);
      issue_caps(in);
    }
  }
}


void Locker::file_loner(FileLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(7) << "file_loner " << *lock << " on " << *lock->get_parent() << dendl;  

  assert(in->is_auth());
  assert(lock->is_stable());

  assert(in->count_nonstale_caps() == 1 && in->mds_caps_wanted.empty());
  
  if (lock->get_state() == LOCK_LOCK) {
    // change lock.  ignore replicas; they don't know about LONER.
    lock->set_state(LOCK_LONER);
    issue_caps(in);
  } else {
    switch (lock->get_state()) {
    case LOCK_SYNC: lock->set_state(LOCK_GLONERR); break;
    case LOCK_MIXED: lock->set_state(LOCK_GLONERM); break;
    default: assert(0);
    }
    int gather = 0;

    if (in->is_replicated()) {
      send_lock_message(lock, LOCK_AC_LOCK);
      lock->init_gather();
      gather++;
    }
    if (lock->get_num_client_lease()) {
      revoke_client_leases(lock);
      gather++;
    }
    if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
      mds->mdcache->queue_file_recover(in);
      mds->mdcache->do_file_recover();
      gather++;
    }
 
    if (gather)
      lock->get_parent()->auth_pin();
    else {
      lock->set_state(LOCK_LONER);
      issue_caps(in);
    }
  }
}



// messenger

void Locker::handle_file_lock(FileLock *lock, MLock *m)
{
  CInode *in = (CInode*)lock->get_parent();
  int from = m->get_asker();

  if (mds->is_rejoin()) {
    if (in->is_rejoining()) {
      dout(7) << "handle_file_lock still rejoining " << *in
	      << ", dropping " << *m << dendl;
      delete m;
      return;
    }
  }


  dout(7) << "handle_file_lock a=" << m->get_action() << " from " << from << " " 
	  << *in << " filelock=" << *lock << dendl;  
  
  int issued = in->get_caps_issued();

  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK ||
           lock->get_state() == LOCK_MIXED);
    
    lock->decode_locked_state(m->get_data());
    lock->set_state(LOCK_SYNC);
    
    // no need to reply.
    
    // waiters
    lock->get_rdlock();
    lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);
    lock->put_rdlock();
    file_eval_gather(lock);
    break;
    
  case LOCK_AC_LOCK:
    assert(lock->get_state() == LOCK_SYNC ||
           lock->get_state() == LOCK_MIXED);
    
    lock->set_state(LOCK_GLOCKR);
    
    // call back caps?
    if (issued & CEPH_CAP_RD) {
      dout(7) << "handle_file_lock client readers, gathering caps on " << *in << dendl;
      issue_caps(in);
      break;
    }
    else if (lock->is_rdlocked()) {
      dout(7) << "handle_file_lock rdlocked, waiting before ack on " << *in << dendl;
      break;
    } 
    
    // nothing to wait for, lock and ack.
    {
      lock->set_state(LOCK_LOCK);
      
      MLock *reply = new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid());
      mds->send_message_mds(reply, from);
    }
    break;
    
  case LOCK_AC_MIXED:
    assert(lock->get_state() == LOCK_SYNC ||
           lock->get_state() == LOCK_LOCK);
    
    if (lock->get_state() == LOCK_SYNC) {
      // MIXED
      if (issued & CEPH_CAP_RD) {
        // call back client caps
        lock->set_state(LOCK_GMIXEDR);
        issue_caps(in);
        break;
      } else {
        // no clients, go straight to mixed
        lock->set_state(LOCK_MIXED);

        // ack
        MLock *reply = new MLock(lock, LOCK_AC_MIXEDACK, mds->get_nodeid());
        mds->send_message_mds(reply, from);
      }
    } else {
      // LOCK
      lock->set_state(LOCK_MIXED);
      
      // no ack needed.
    }

    issue_caps(in);
    
    // waiters
    lock->finish_waiters(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE);
    file_eval_gather(lock);
    break;

 
    

    // -- auth --
  case LOCK_AC_LOCKACK:
    assert(lock->get_state() == LOCK_GLOCKR ||
           lock->get_state() == LOCK_GLOCKM ||
           lock->get_state() == LOCK_GLONERM ||
           lock->get_state() == LOCK_GLONERR);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);

    if (lock->is_gathering()) {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from
	      << ", last one" << dendl;
      file_eval_gather(lock);
    }
    break;
    
  case LOCK_AC_SYNCACK:
    assert(lock->get_state() == LOCK_GSYNCM);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    /* not used currently
    {
      // merge data  (keep largest size, mtime, etc.)
      int off = 0;
      in->decode_merge_file_state(m->get_data(), off);
    }
    */

    if (lock->is_gathering()) {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from
	      << ", last one" << dendl;
      file_eval_gather(lock);
    }
    break;

  case LOCK_AC_MIXEDACK:
    assert(lock->get_state() == LOCK_GMIXEDR);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    if (lock->is_gathering()) {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from
	      << ", last one" << dendl;
      file_eval_gather(lock);
    }
    break;


  default:
    assert(0);
  }  
  
  delete m;
}






