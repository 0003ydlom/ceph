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



// unix-ey fs stuff
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/statvfs.h>


#include <iostream>
using namespace std;


// ceph stuff
#include "Client.h"

#include "messages/MStatfs.h"
#include "messages/MStatfsReply.h"

#include "messages/MMonMap.h"

#include "messages/MClientMount.h"
#include "messages/MClientUnmount.h"
#include "messages/MClientSession.h"
#include "messages/MClientReconnect.h"
#include "messages/MClientRequest.h"
#include "messages/MClientRequestForward.h"
#include "messages/MClientReply.h"
#include "messages/MClientFileCaps.h"
#include "messages/MClientLease.h"

#include "messages/MGenericMessage.h"

#include "messages/MMDSGetMap.h"
#include "messages/MMDSMap.h"

#include "osdc/Filer.h"
#include "osdc/Objecter.h"
#include "osdc/ObjectCacher.h"

#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/Logger.h"



#include "config.h"

#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_client) *_dout << dbeginl << g_clock.now() << " client" << whoami /*<< "." << pthread_self() */ << " "

#define  tout       if (g_conf.client_trace) traceout


// static logger
Mutex client_logger_lock;
LogType client_logtype;
Logger  *client_logger = 0;




ostream& operator<<(ostream &out, Inode &in)
{
  out << in.inode.ino << "("
      << " cap_refs=" << in.cap_refs
      << " open=" << in.open_by_mode
      << " ref=" << in.ref
      << ")";
  return out;
}



// cons/des

Client::Client(Messenger *m, MonMap *mm) : timer(client_lock)
{
  // which client am i?
  whoami = m->get_myname().num();
  monmap = mm;
  
  tick_event = 0;

  mounted = false;
  mounters = 0;
  mount_timeout_event = 0;
  unmounting = false;

  last_tid = 0;
  unsafe_sync_write = 0;

  mdsmap = 0;

  cwd = filepath(1);  // root directory

  // 
  root = 0;

  lru.lru_set_max(g_conf.client_cache_size);

  // file handles
  free_fd_set.insert(10, 1<<30);

  // set up messengers
  messenger = m;

  // osd interfaces
  osdmap = new OSDMap();     // initially blank.. see mount()
  objecter = new Objecter(messenger, monmap, osdmap, client_lock);
  objecter->set_client_incarnation(0);  // client always 0, for now.
  objectcacher = new ObjectCacher(objecter, client_lock);
  filer = new Filer(objecter);
}


Client::~Client() 
{
  tear_down_cache();

  if (objectcacher) { 
    delete objectcacher; 
    objectcacher = 0; 
  }

  if (filer) { delete filer; filer = 0; }
  if (objecter) { delete objecter; objecter = 0; }
  if (osdmap) { delete osdmap; osdmap = 0; }
  if (mdsmap) { delete mdsmap; mdsmap = 0; }

  if (messenger) { delete messenger; messenger = 0; }
}


void Client::tear_down_cache()
{
  // fd's
  for (hash_map<int, Fh*>::iterator it = fd_map.begin();
       it != fd_map.end();
       it++) {
    Fh *fh = it->second;
    dout(1) << "tear_down_cache forcing close of fh " << it->first << " ino " << fh->inode->inode.ino << dendl;
    put_inode(fh->inode);
    delete fh;
  }
  fd_map.clear();

  // caps!
  // *** FIXME ***

  // empty lru
  lru.lru_set_max(0);
  trim_cache();
  assert(lru.lru_get_size() == 0);

  // close root ino
  assert(inode_map.size() <= 1);
  if (root && inode_map.size() == 1) {
    delete root;
    root = 0;
    inode_map.clear();
  }

  assert(inode_map.empty());
}



// debug crapola

void Client::dump_inode(Inode *in, set<Inode*>& did)
{
  dout(1) << "dump_inode: inode " << in->ino() << " ref " << in->ref << " dir " << in->dir << dendl;

  if (in->dir) {
    dout(1) << "  dir size " << in->dir->dentries.size() << dendl;
    //for (hash_map<const char*, Dentry*, hash<const char*>, eqstr>::iterator it = in->dir->dentries.begin();
    for (hash_map<nstring, Dentry*>::iterator it = in->dir->dentries.begin();
         it != in->dir->dentries.end();
         it++) {
      dout(1) << "    dn " << it->first << " ref " << it->second->ref << dendl;
      dump_inode(it->second->inode, did);
    }
  }
}

void Client::dump_cache()
{
  set<Inode*> did;

  if (root) dump_inode(root, did);

  for (hash_map<inodeno_t, Inode*>::iterator it = inode_map.begin();
       it != inode_map.end();
       it++) {
    if (did.count(it->second)) continue;
    
    dout(1) << "dump_cache: inode " << it->first
            << " ref " << it->second->ref 
            << " dir " << it->second->dir << dendl;
    if (it->second->dir) {
      dout(1) << "  dir size " << it->second->dir->dentries.size() << dendl;
    }
  }
 
}


void Client::init() 
{
  Mutex::Locker lock(client_lock);

  tick();

  // do logger crap only once per process.
  static bool did_init = false;
  if (did_init) return;
  did_init = true;
  
  // logger?
  client_logger_lock.Lock();
  if (client_logger == 0) {
    client_logtype.add_inc("lsum");
    client_logtype.add_inc("lnum");
    client_logtype.add_inc("lwsum");
    client_logtype.add_inc("lwnum");
    client_logtype.add_inc("lrsum");
    client_logtype.add_inc("lrnum");
    client_logtype.add_inc("trsum");
    client_logtype.add_inc("trnum");
    client_logtype.add_inc("wrlsum");
    client_logtype.add_inc("wrlnum");
    client_logtype.add_inc("lstatsum");
    client_logtype.add_inc("lstatnum");
    client_logtype.add_inc("ldirsum");
    client_logtype.add_inc("ldirnum");
    client_logtype.add_inc("readdir");
    client_logtype.add_inc("stat");
    client_logtype.add_avg("owrlat");
    client_logtype.add_avg("ordlat");
    client_logtype.add_inc("owr");
    client_logtype.add_inc("ord");
    
    char s[80];
    char hostname[80];
    gethostname(hostname, 79);
    sprintf(s,"clients.%s.%d", hostname, getpid());
    client_logger = new Logger(s, &client_logtype);
  }
  client_logger_lock.Unlock();

}

void Client::shutdown() 
{
  dout(1) << "shutdown" << dendl;
  messenger->shutdown();
}




// ===================
// metadata cache stuff

void Client::trim_cache()
{
  unsigned last = 0;
  while (lru.lru_get_size() != last) {
    last = lru.lru_get_size();

    if (lru.lru_get_size() <= lru.lru_get_max())  break;

    // trim!
    Dentry *dn = (Dentry*)lru.lru_expire();
    if (!dn) break;  // done
    
    dout(15) << "trim_cache unlinking dn " << dn->name 
	     << " in dir " << hex << dn->dir->parent_inode->inode.ino 
	     << dendl;
    unlink(dn);
  }

  // hose root?
  if (lru.lru_get_size() == 0 && root && root->ref == 0 && inode_map.size() == 1) {
    dout(15) << "trim_cache trimmed root " << root << dendl;
    delete root;
    root = 0;
    inode_map.clear();
  }
}


void Client::update_inode_file_bits(Inode *in,
				    __u64 size,
				    utime_t ctime,
				    utime_t mtime,
				    utime_t atime,
				    int issued,
				    __u64 time_warp_seq)
{
  bool warn = false;

  // be careful with size, mtime, atime
  if (issued & CEPH_CAP_EXCL) {
    if (ctime > in->inode.ctime) 
      in->inode.ctime = ctime;
    if (time_warp_seq > in->inode.time_warp_seq)
      dout(0) << "WARNING: " << *in << " mds time_warp_seq "
	      << time_warp_seq << " > " << in->inode.time_warp_seq << dendl;
  } else if (issued & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) {
    if (size > in->inode.size) {
      in->inode.size = size;
      in->reported_size = size;
    }
    if (time_warp_seq > in->inode.time_warp_seq) {
      in->inode.ctime = ctime;
      in->inode.mtime = mtime;
      in->inode.atime = atime;
      in->inode.time_warp_seq = time_warp_seq;
    } else if (time_warp_seq == in->inode.time_warp_seq) {
      if (ctime > in->inode.ctime) 
	in->inode.ctime = ctime;
      if (mtime > in->inode.mtime) 
	in->inode.mtime = mtime;
      if (atime > in->inode.atime)
	in->inode.atime = atime;
    } else
      warn = true;
  } else {
    in->inode.size = size;
    in->reported_size = size;
    if (time_warp_seq >= in->inode.time_warp_seq) {
      in->inode.ctime = ctime;
      in->inode.mtime = mtime;
      in->inode.atime = atime;
      in->inode.time_warp_seq = time_warp_seq;
    } else
      warn = true;
  }
  if (warn) {
    dout(0) << *in << " mds time_warp_seq "
	    << in->inode.time_warp_seq << " -> "
	    << time_warp_seq
	    << dendl;
  }
}

void Client::update_inode(Inode *in, InodeStat *st, LeaseStat *lease, utime_t from)
{
  utime_t ttl = from;
  ttl += (float)lease->duration_ms * 1000.0;

  dout(12) << "update_inode mask " << lease->mask << " ttl " << ttl << dendl;

  if (lease->mask & CEPH_STAT_MASK_INODE) {
    in->inode.ino = st->ino;
    in->inode.layout = st->layout;
    in->inode.rdev = st->rdev;

    in->inode.mode = st->mode;
    in->inode.uid = st->uid;
    in->inode.gid = st->gid;

    in->inode.nlink = st->nlink;
    in->inode.anchored = false;  /* lie */

    in->dirfragtree = st->dirfragtree;  // FIXME look at the mask!
    in->xattrs.swap(st->xattrs);
    in->inode.dirstat = st->dirstat;

    in->inode.ctime = st->ctime;
    in->inode.max_size = st->max_size;  // right?

    update_inode_file_bits(in, st->size, st->ctime, st->mtime, st->atime, in->caps_issued(), st->time_warp_seq);
  }

  if (lease->mask && ttl > in->lease_ttl) {
    in->lease_ttl = ttl;
    in->lease_mask = lease->mask;
    in->lease_mds = from;
  }

  // symlink?
  if (in->inode.is_symlink()) {
    if (!in->symlink) 
      in->symlink = new string;
    *(in->symlink) = st->symlink;
  }
}



/*
 * insert_dentry_inode - insert + link a single dentry + inode into the metadata cache.
 */
Inode* Client::insert_dentry_inode(Dir *dir, const string& dname, LeaseStat *dlease, 
				   InodeStat *ist, LeaseStat *ilease, 
				   utime_t from)
{
  int dmask = dlease->mask;
  utime_t dttl = from;
  dttl += (float)dlease->duration_ms * 1000.0;

  Dentry *dn = NULL;
  if (dir->dentries.count(dname))
    dn = dir->dentries[dname];

  dout(12) << "insert_dentry_inode " << dname << " ino " << ist->ino
           << "  size " << ist->size
           << "  mtime " << ist->mtime
	   << " dmask " << dmask
	   << " in dir " << dir->parent_inode->inode.ino
           << dendl;
  
  if (dn) {
    if (dn->inode->inode.ino == ist->ino) {
      touch_dn(dn);
      dout(12) << " had dentry " << dname
               << " with correct ino " << dn->inode->inode.ino
               << dendl;
    } else {
      dout(12) << " had dentry " << dname
               << " with WRONG ino " << dn->inode->inode.ino
               << dendl;
      unlink(dn, true);
      dn = NULL;
    }
  }
  
  if (!dn) {
    // have inode linked elsewhere?  -> unlink and relink!
    if (inode_map.count(ist->ino)) {
      Inode *in = inode_map[ist->ino];
      assert(in);

      if (in->dn) {
        dout(12) << " had ino " << in->inode.ino
                 << " not linked or linked at the right position, relinking"
                 << dendl;
        dn = relink_inode(dir, dname, in);
      } else {
        // link
        dout(12) << " had ino " << in->inode.ino
                 << " unlinked, linking" << dendl;
        dn = link(dir, dname, in);
      }
    }
  }

  if (!dn) {
    Inode *in = new Inode(ist->ino, &ist->layout);
    inode_map[ist->ino] = in;
    dn = link(dir, dname, in);
    dout(12) << " new dentry+node with ino " << ist->ino << dendl;
  } 

  assert(dn && dn->inode);

  if (dlease->mask & CEPH_LOCK_DN) {
    utime_t ttl = from;
    ttl += (float)dlease->duration_ms * 1000.0;
    if (ttl > dn->lease_ttl) {
      dout(10) << "got dentry lease on " << dname << " ttl " << ttl << dendl;
      dn->lease_ttl = ttl;
      dn->lease_mds = from;
    }
  }

  update_inode(dn->inode, ist, ilease, from);

  return dn->inode;
}

/** update_inode_dist
 *
 * update MDS location cache for a single inode
 */
void Client::update_dir_dist(Inode *in, DirStat *dst)
{
  // auth
  in->dir_auth = -1;
  if (dst->frag == frag_t()) {
    in->dir_auth = dst->auth;
  } else {
    dout(20) << "got dirfrag map for " << in->inode.ino << " frag " << dst->frag << " to mds " << dst->auth << dendl;
    in->fragmap[dst->frag] = dst->auth;
  }

  // replicated
  in->dir_replicated = !dst->dist.empty();  // FIXME that's just one frag!
  
  // dist
  /*
  if (!st->dirfrag_dist.empty()) {   // FIXME
    set<int> dist = st->dirfrag_dist.begin()->second;
    if (dist.empty() && !in->dir_contacts.empty())
      dout(9) << "lost dist spec for " << in->inode.ino 
              << " " << dist << dendl;
    if (!dist.empty() && in->dir_contacts.empty()) 
      dout(9) << "got dist spec for " << in->inode.ino 
              << " " << dist << dendl;
    in->dir_contacts = dist;
  }
  */
}


/** insert_trace
 *
 * insert a trace from a MDS reply into the cache.
 */
Inode* Client::insert_trace(MClientReply *reply, utime_t from)
{

  bufferlist::iterator p = reply->get_trace_bl().begin();
  if (p.end()) {
    dout(10) << "insert_trace -- no trace" << dendl;
    return NULL;
  }

  __u16 numi, numd;
  ::decode(numi, p);
  ::decode(numd, p);
  dout(10) << "insert_trace got " << numi << " inodes, " << numd << " dentries" << dendl;

  // decode
  LeaseStat ilease[numi];
  InodeStat ist[numi];
  DirStat dst[numd];
  string dname[numd];
  LeaseStat dlease[numd];

  if (numi == 0)
    return NULL;

  int ileft = numi;
  int dleft = numd;
  if (numi == numd)
    goto dentry;

 inode:
  if (!ileft) goto done;
  ileft--;
  ist[ileft].decode(p);
  ::decode(ilease[ileft], p);

 dentry:
  if (!dleft) goto done;
  dleft--;
  ::decode(dname[dleft], p);
  ::decode(dlease[dleft], p);
  dst[dleft].decode(p);
  goto inode;

 done:
  
  // insert into cache --
  // first inode
  Inode *curi = 0;
  inodeno_t ino = ist[0].ino;
  if (!root && ino == 1) {
    curi = root = new Inode(ino, &ist[0].layout);
    dout(10) << "insert_trace new root is " << root << dendl;
    inode_map[ino] = root;
    root->dir_auth = 0;
  }
  if (!curi) {
    assert(inode_map.count(ino));
    curi = inode_map[ino];
  }
  update_inode(curi, &ist[0], &ilease[0], from);

  for (unsigned i=0; i<numd; i++) {
    Dir *dir = curi->open_dir();

    // in?
    if (i+1 == numi) {
      dout(10) << "insert_trace " << dname[i] << " mask " << dlease[i].mask
	       << " -- NULL dentry caching not supported yet, IMPLEMENT ME" << dendl;
      //insert_null_dentry(dir, *pdn, *pdnmask, ttl);  // fixme
      break;
    }
    
    curi = insert_dentry_inode(dir, dname[i], &dlease[i], &ist[i+1], &ilease[i+1], from);
    update_dir_dist(curi, &dst[i]);  // dir stat info is attached to inode...
  }
  assert(p.end());

  return curi;
}



/*
 * bleh, dentry vs inode semantics here are sloppy
 */
Dentry *Client::lookup(const filepath& path)
{
  dout(14) << "lookup " << path << dendl;

  Inode *cur;
  if (path.get_ino()) {
    if (inode_map.count(path.get_ino()))
      cur = inode_map[path.get_ino()];
    else
      return NULL;
  } else
    cur = root;
  if (!cur) return NULL;

  Dentry *dn = 0;
  for (unsigned i=0; i<path.depth(); i++) {
    dout(14) << " seg " << i << " = " << path[i] << dendl;
    if (cur->inode.is_dir() && cur->dir) {
      // dir, we can descend
      Dir *dir = cur->dir;
      if (dir->dentries.count(path[i])) {
        dn = dir->dentries[path[i]];
        dout(14) << " hit dentry " << path[i] << " inode is " << dn->inode
		 << " ttl " << dn->inode->lease_ttl << dendl;
      } else {
        dout(14) << " dentry " << path[i] << " dne" << dendl;
        return NULL;
      }
      cur = dn->inode;
      assert(cur);
    } else {
      return NULL;  // not a dir
    }
  }

  if (!dn) 
    dn = cur->dn;
  if (dn) {
    dout(11) << "lookup '" << path << "' found " << dn->name << " inode " << dn->inode->inode.ino
	     << " ttl " << dn->inode->lease_ttl << dendl;
  }

  return dn;
}

// -------

int Client::choose_target_mds(MClientRequest *req) 
{
  int mds = 0;
    
  // find deepest known prefix
  Inode *diri = root;   // the deepest known containing dir
  Inode *item = 0;      // the actual item... if we know it
  int missing_dn = -1;  // which dn we miss on (if we miss)
  
  unsigned depth = req->get_filepath().depth();
  unsigned i;
  for (i=0; i<depth; i++) {
    // dir?
    if (diri && diri->inode.is_dir() && diri->dir) {
      Dir *dir = diri->dir;
      
      // do we have the next dentry?
      if (dir->dentries.count( req->get_filepath()[i] ) == 0) {
	missing_dn = i;  // no.
	break;
      }
      
      dout(7) << " have path seg " << i << " on " << diri->dir_auth << " ino " << diri->inode.ino << " " << req->get_filepath()[i] << dendl;
      
      if (i == depth-1) {  // last one!
	item = dir->dentries[ req->get_filepath()[i] ]->inode;
	break;
      } 
      
      // continue..
      diri = dir->dentries[ req->get_filepath()[i] ]->inode;
      assert(diri);
    } else {
      missing_dn = i;
      break;
    }
  }
  
  // pick mds
  if (!diri || g_conf.client_use_random_mds) {
    // no root info, pick a random MDS
    mds = mdsmap->get_random_in_mds();
    dout(10) << "random mds" << mds << dendl;
    if (mds < 0) mds = 0;

    if (0) {
      mds = 0;
      dout(0) << "hack: sending all requests to mds" << mds << dendl;
    }
  } else {
    if (req->auth_is_best()) {
      // pick the actual auth (as best we can)
      if (item) {
	mds = item->authority();
      } else {
	mds = diri->authority(req->get_filepath()[missing_dn]);
      }
    } else {
      // balance our traffic!
      mds = diri->pick_replica(mdsmap); // for the _inode_
      dout(20) << "for " << req->get_filepath() << " diri " << diri->inode.ino << " rep " 
	      << diri->dir_contacts
	      << " mds" << mds << dendl;
    }
  }
  dout(20) << "mds is " << mds << dendl;
  
  return mds;
}



MClientReply *Client::make_request(MClientRequest *req, 
				   int uid, int gid, 
				   Inode **ppin, utime_t *pfrom, int use_mds)
{
  // time the call
  utime_t start = g_clock.real_now();
  
  bool nojournal = false;
  int op = req->get_op();
  if (op == CEPH_MDS_OP_STAT ||
      op == CEPH_MDS_OP_LSTAT ||
      op == CEPH_MDS_OP_READDIR ||
      op == CEPH_MDS_OP_OPEN)
    nojournal = true;


  // -- request --
  // assign a unique tid
  tid_t tid = ++last_tid;
  req->set_tid(tid);

  if (!mds_requests.empty()) 
    req->set_oldest_client_tid(mds_requests.begin()->first);
  else
    req->set_oldest_client_tid(tid); // this one is the oldest.

  // make note
  MetaRequest request(req, tid);
  mds_requests[tid] = &request;

  if (uid < 0) {
    uid = geteuid();
    gid = getegid();
  }
  request.uid = uid;
  request.gid = gid;
  req->set_caller_uid(uid);
  req->set_caller_gid(gid);

  // encode payload now, in case we have to resend (in case of mds failure)
  req->encode_payload();
  request.request_payload = req->get_payload();

  // note idempotency
  request.idempotent = req->is_idempotent();

  // hack target mds?
  if (use_mds >= 0)
    request.resend_mds = use_mds;

  // set up wait cond
  Cond cond;
  request.caller_cond = &cond;
  
  while (1) {
    // choose mds
    int mds;
    // force use of a particular mds?
    if (request.resend_mds >= 0) {
      mds = request.resend_mds;
      request.resend_mds = -1;
      dout(10) << "target resend_mds specified as mds" << mds << dendl;
    } else {
      mds = choose_target_mds(req);
      if (mds >= 0) {
	dout(10) << "chose target mds" << mds << " based on hierarchy" << dendl;
      } else {
	mds = mdsmap->get_random_in_mds();
	if (mds < 0) mds = 0;  // hrm.
	dout(10) << "chose random target mds" << mds << " for lack of anything better" << dendl;
      }
    }
    
    // open a session?
    if (mds_sessions.count(mds) == 0) {
      Cond cond;

      if (!mdsmap->is_active(mds)) {
	dout(10) << "no address for mds" << mds << ", requesting new mdsmap" << dendl;
	int mon = monmap->pick_mon();
	messenger->send_message(new MMDSGetMap(monmap->fsid, mdsmap->get_epoch()+1),
				monmap->get_inst(mon));
	waiting_for_mdsmap.push_back(&cond);
	cond.Wait(client_lock);

	if (!mdsmap->is_active(mds)) {
	  dout(10) << "hmm, still have no address for mds" << mds << ", trying a random mds" << dendl;
	  request.resend_mds = mdsmap->get_random_in_mds();
	  continue;
	}
      }	
      
      if (waiting_for_session.count(mds) == 0) {
	dout(10) << "opening session to mds" << mds << dendl;
	messenger->send_message(new MClientSession(CEPH_SESSION_REQUEST_OPEN),
				mdsmap->get_inst(mds));
      }
      
      // wait
      waiting_for_session[mds].push_back(&cond);
      while (waiting_for_session.count(mds)) {
	dout(10) << "waiting for session to mds" << mds << " to open" << dendl;
	cond.Wait(client_lock);
      }
    }

    // send request.
    send_request(&request, mds);

    // wait for signal
    dout(20) << "awaiting kick on " << &cond << dendl;
    cond.Wait(client_lock);
    
    // did we get a reply?
    if (request.reply) 
      break;
  }

  // got it!
  MClientReply *reply = request.reply;
  
  // kick dispatcher (we've got it!)
  assert(request.dispatch_cond);
  request.dispatch_cond->Signal();
  dout(20) << "sendrecv kickback on tid " << tid << " " << request.dispatch_cond << dendl;
  
  // clean up.
  mds_requests.erase(tid);


  // insert trace
  utime_t from = request.sent_stamp;
  if (pfrom) 
    *pfrom = from;
  Inode *in = insert_trace(reply, from);
  if (ppin)
    *ppin = in;

  // -- log times --
  if (client_logger) {
    utime_t lat = g_clock.real_now();
    lat -= start;
    dout(20) << "lat " << lat << dendl;
    client_logger->finc("lsum",(double)lat);
    client_logger->inc("lnum");

    if (nojournal) {
      client_logger->finc("lrsum",(double)lat);
      client_logger->inc("lrnum");
    } else {
      client_logger->finc("lwsum",(double)lat);
      client_logger->inc("lwnum");
    }
    
    if (op == CEPH_MDS_OP_STAT) {
      client_logger->finc("lstatsum",(double)lat);
      client_logger->inc("lstatnum");
    }
    else if (op == CEPH_MDS_OP_READDIR) {
      client_logger->finc("ldirsum",(double)lat);
      client_logger->inc("ldirnum");
    }

  }

  return reply;
}


void Client::handle_client_session(MClientSession *m) 
{
  dout(10) << "handle_client_session " << *m << dendl;
  int from = m->get_source().num();

  switch (m->op) {
  case CEPH_SESSION_OPEN:
    assert(mds_sessions.count(from) == 0);
    mds_sessions[from].seq = 0;
    break;

  case CEPH_SESSION_CLOSE:
    mds_sessions.erase(from);
    // FIXME: kick requests (hard) so that they are redirected.  or fail.
    break;

  case CEPH_SESSION_RENEWCAPS:
    mds_sessions[from].cap_ttl = 
      mds_sessions[from].last_cap_renew_request + mdsmap->get_session_timeout();
    break;

  case CEPH_SESSION_STALE:
    // FIXME
    break;

  default:
    assert(0);
  }

  // kick waiting threads
  signal_cond_list(waiting_for_session[from]);
  waiting_for_session.erase(from);

  delete m;
}


void Client::send_request(MetaRequest *request, int mds)
{
  MClientRequest *r = request->request;
  if (!r) {
    // make a new one
    dout(10) << "send_request rebuilding request " << request->tid
	     << " for mds" << mds << dendl;
    r = new MClientRequest;
    r->copy_payload(request->request_payload);
    r->decode_payload();
    r->set_retry_attempt(request->retry_attempt);
  }
  request->request = 0;

  r->set_mdsmap_epoch(mdsmap->get_epoch());

  if (request->mds.empty()) {
    request->sent_stamp = g_clock.now();
    dout(20) << "send_request set sent_stamp to " << request->sent_stamp << dendl;
  }

  dout(10) << "send_request " << *r << " to mds" << mds << dendl;
  messenger->send_message(r, mdsmap->get_inst(mds));
  
  request->mds.insert(mds);
}

void Client::handle_client_request_forward(MClientRequestForward *fwd)
{
  tid_t tid = fwd->get_tid();

  if (mds_requests.count(tid) == 0) {
    dout(10) << "handle_client_request_forward no pending request on tid " << tid << dendl;
    delete fwd;
    return;
  }

  MetaRequest *request = mds_requests[tid];
  assert(request);

  // reset retry counter
  request->retry_attempt = 0;

  if (!fwd->must_resend() && 
      mds_sessions.count(fwd->get_dest_mds())) {
    // dest mds has a session, and request was forwarded for us.

    // note new mds set.
    if (request->num_fwd < fwd->get_num_fwd()) {
      // there are now exactly two mds's whose failure should trigger a resend
      // of this request.
      request->mds.clear();
      request->mds.insert(fwd->get_source().num());
      request->mds.insert(fwd->get_dest_mds());
      request->num_fwd = fwd->get_num_fwd();
      dout(10) << "handle_client_request tid " << tid
	       << " fwd " << fwd->get_num_fwd() 
	       << " to mds" << fwd->get_dest_mds() 
	       << ", mds set now " << request->mds
	       << dendl;
    } else {
      dout(10) << "handle_client_request tid " << tid
	       << " previously forwarded to mds" << fwd->get_dest_mds() 
	       << ", mds still " << request->mds
		 << dendl;
    }
  } else {
    // request not forwarded, or dest mds has no session.
    // resend.
    dout(10) << "handle_client_request tid " << tid
	     << " fwd " << fwd->get_num_fwd() 
	     << " to mds" << fwd->get_dest_mds() 
	     << ", non-idempotent, resending to " << fwd->get_dest_mds()
	     << dendl;

    request->mds.clear();
    request->num_fwd = fwd->get_num_fwd();
    request->resend_mds = fwd->get_dest_mds();
    request->caller_cond->Signal();
  }

  delete fwd;
}

void Client::handle_client_reply(MClientReply *reply)
{
  tid_t tid = reply->get_tid();

  if (mds_requests.count(tid) == 0) {
    dout(10) << "handle_client_reply no pending request on tid " << tid << dendl;
    delete reply;
    return;
  }
  MetaRequest *request = mds_requests[tid];
  assert(request);

  // store reply
  request->reply = reply;

  // wake up waiter
  request->caller_cond->Signal();

  // wake for kick back
  Cond cond;
  request->dispatch_cond = &cond;
  while (mds_requests.count(tid)) {
    dout(20) << "handle_client_reply awaiting kickback on tid " << tid << " " << &cond << dendl;
    cond.Wait(client_lock);
  }
}


// ------------------------
// incoming messages

void Client::dispatch(Message *m)
{
  client_lock.Lock();

  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
    handle_mon_map((MMonMap*)m);
    break;

    // osd
  case CEPH_MSG_OSD_OPREPLY:
    objecter->handle_osd_op_reply((MOSDOpReply*)m);
    break;

  case CEPH_MSG_OSD_MAP:
    objecter->handle_osd_map((class MOSDMap*)m);
    if (!mounted) mount_cond.Signal();
    break;
    
    // mounting and mds sessions
  case CEPH_MSG_MDS_MAP:
    handle_mds_map((MMDSMap*)m);
    break;
  case CEPH_MSG_CLIENT_UNMOUNT:
    handle_unmount(m);
    break;
  case CEPH_MSG_CLIENT_SESSION:
    handle_client_session((MClientSession*)m);
    break;

    // requests
  case CEPH_MSG_CLIENT_REQUEST_FORWARD:
    handle_client_request_forward((MClientRequestForward*)m);
    break;
  case CEPH_MSG_CLIENT_REPLY:
    handle_client_reply((MClientReply*)m);
    break;

  case CEPH_MSG_CLIENT_FILECAPS:
    handle_file_caps((MClientFileCaps*)m);
    break;
  case CEPH_MSG_CLIENT_LEASE:
    handle_lease((MClientLease*)m);
    break;

  case CEPH_MSG_STATFS_REPLY:
    handle_statfs_reply((MStatfsReply*)m);
    break;

  default:
    dout(10) << "dispatch doesn't recognize message type " << m->get_type() << dendl;
    assert(0);  // fail loudly
    break;
  }

  // unmounting?
  if (unmounting) {
    dout(10) << "unmounting: trim pass, size was " << lru.lru_get_size() 
             << "+" << inode_map.size() << dendl;
    trim_cache();
    if (lru.lru_get_size() == 0 && inode_map.empty()) {
      dout(10) << "unmounting: trim pass, cache now empty, waking unmount()" << dendl;
      mount_cond.Signal();
    } else {
      dout(10) << "unmounting: trim pass, size still " << lru.lru_get_size() 
               << "+" << inode_map.size() << dendl;
      dump_cache();      
    }
  }

  client_lock.Unlock();
}


void Client::handle_mds_map(MMDSMap* m)
{
  int frommds = -1;
  if (m->get_source().is_mds())
    frommds = m->get_source().num();

  if (mdsmap == 0) {
    mdsmap = new MDSMap;

    assert(m->get_source().is_mon());
    whoami = m->get_dest().num();
    messenger->reset_myname(entity_name_t::CLIENT(whoami));
    dout(1) << "handle_mds_map i am now " << m->get_dest() << dendl;
    
    mount_cond.Signal();  // mount might be waiting for this.
  } 

  if (m->get_epoch() < mdsmap->get_epoch()) {
    dout(1) << "handle_mds_map epoch " << m->get_epoch() << " is older than our "
	    << mdsmap->get_epoch() << dendl;
    delete m;
    return;
  }  

  dout(1) << "handle_mds_map epoch " << m->get_epoch() << dendl;
  mdsmap->decode(m->get_encoded());
  
  // send reconnect?
  if (frommds >= 0 && 
      mdsmap->get_state(frommds) == MDSMap::STATE_RECONNECT) {
    send_reconnect(frommds);
  }

  // kick requests?
  if (frommds >= 0 &&
      mdsmap->get_state(frommds) == MDSMap::STATE_ACTIVE) {
    kick_requests(frommds);
    //failed_mds.erase(from);
  }

  // kick any waiting threads
  list<Cond*> ls;
  ls.swap(waiting_for_mdsmap);
  signal_cond_list(ls);

  delete m;
}

void Client::send_reconnect(int mds)
{
  dout(10) << "send_reconnect to mds" << mds << dendl;

  MClientReconnect *m = new MClientReconnect;

  if (mds_sessions.count(mds)) {
    // i have an open session.
    for (hash_map<inodeno_t, Inode*>::iterator p = inode_map.begin();
	 p != inode_map.end();
	 p++) {
      Inode *in = p->second;
      if (in->caps.count(mds)) {
	dout(10) << " caps on " << p->first
		 << " " << cap_string(in->caps[mds].issued)
		 << " wants " << cap_string(in->caps_wanted())
		 << dendl;
	in->caps[mds].seq = 0;  // reset seq.
	m->add_inode_caps(p->first,    // ino
			  in->caps_wanted(), // wanted
			  in->caps[mds].issued,     // issued
			  in->inode.size, in->inode.mtime, in->inode.atime);
	filepath path;
	in->make_path(path);
	dout(10) << " path on " << p->first << " is " << path << dendl;
	m->add_inode_path(p->first, path.get_path());
      }
      if (in->exporting_mds == mds) {
	dout(10) << " clearing exporting_caps on " << p->first << dendl;
	in->exporting_mds = -1;
	in->exporting_issued = 0;
	in->exporting_mseq = 0;
      }
    }

    // reset my cap seq number
    mds_sessions[mds].seq = 0;
  } else {
    dout(10) << " i had no session with this mds";
    m->closed = true;
  }

  messenger->send_message(m, mdsmap->get_inst(mds));
}


void Client::kick_requests(int mds)
{
  dout(10) << "kick_requests for mds" << mds << dendl;

  for (map<tid_t, MetaRequest*>::iterator p = mds_requests.begin();
       p != mds_requests.end();
       ++p) 
    if (p->second->mds.count(mds)) {
      p->second->retry_attempt++;   // inc retry counter
      send_request(p->second, mds);
    }
}



/************
 * leases
 */

void Client::handle_lease(MClientLease *m)
{
  dout(10) << "handle_lease " << *m << dendl;

  assert(m->get_action() == CEPH_MDS_LEASE_REVOKE);

  int mds = m->get_source().num();
  mds_sessions[mds].seq++;

  Inode *in;
  if (inode_map.count(m->get_ino()) == 0) {
    dout(10) << " don't have inode " << m->get_ino() << dendl;
    goto revoke;
  }
  in = inode_map[m->get_ino()];

  if (m->get_mask() & CEPH_LOCK_DN) {
    if (!in->dir || in->dir->dentries.count(m->dname) == 0) {
      dout(10) << " don't have dir|dentry " << m->get_ino() << "/" << m->dname <<dendl;
      goto revoke;
    }
    Dentry *dn = in->dir->dentries[m->dname];
    dout(10) << " revoked DN lease on " << dn << dendl;
    dn->lease_mds = -1;
  } 
  if (m->get_mask() & in->lease_mask) {
    int newmask = in->lease_mask & ~m->get_mask();
    dout(10) << " revoked inode lease on " << in->ino() 
	     << " mask " << in->lease_mask << " -> " << newmask << dendl;
    in->lease_mask = newmask;
  }
  
 revoke:
  messenger->send_message(new MClientLease(CEPH_MDS_LEASE_RELEASE,
					   m->get_mask(), m->get_ino(), m->dname),
			  m->get_source_inst());
  delete m;
}


void Client::release_lease(Inode *in, Dentry *dn, int mask)
{
  int havemask = 0;
  int mds = -1;
  utime_t now = g_clock.now();
  string dname;

  // inode?
  if (in->lease_mds >= 0 && (in->lease_mask & mask) && now < in->lease_ttl) {
    havemask |= (in->lease_mask & mask);
    mds = in->lease_mds;
  }

  // dentry?
  if (dn && dn->lease_mds >= 0 && now < in->lease_ttl) {
    havemask |= CEPH_LOCK_DN;
    mds = dn->lease_mds;
    dname = dn->name;
  }

  if (mds >= 0 && mdsmap->is_up(mds)) {
    dout(10) << "release_lease mds" << mds << " mask " << mask
	     << " on " << in->ino() << " " << dname << dendl;
    messenger->send_message(new MClientLease(CEPH_MDS_LEASE_RELEASE, mask, in->ino(), dname),
			    mdsmap->get_inst(mds));
  }
}



/****
 * caps
 */



void Inode::get_cap_ref(int cap)
{
  int n = 0;
  while (cap) {
    if (cap & 1) {
      int c = 1 << n;
      cap_refs[c]++;
      //cout << "inode " << *this << " get " << cap_string(c) << " " << (cap_refs[c]-1) << " -> " << cap_refs[c] << std::endl;
    }
    cap >>= 1;
    n++;
  }
}

bool Inode::put_cap_ref(int cap)
{
  bool last = false;
  int n = 0;
  while (cap) {
    if (cap & 1) {
      int c = 1 << n;
      if (--cap_refs[c] == 0)
	last = true;      
      //cout << "inode " << *this << " put " << cap_string(c) << " " << (cap_refs[c]+1) << " -> " << cap_refs[c] << std::endl;
    }
    cap >>= 1;
    n++;
  }
  return last;
}

void Client::put_cap_ref(Inode *in, int cap)
{
  if (in->put_cap_ref(cap)) {
    check_caps(in);
    signal_cond_list(in->waitfor_commit);
  }
}

void Client::check_caps(Inode *in)
{
  int wanted = in->caps_wanted();
  int used = in->caps_used();

  dout(10) << "check_caps on " << *in
	   << " wanted " << cap_string(wanted)
	   << " used " << cap_string(used)
	   << dendl;
  
  if (in->caps.empty())
    return;   // guard if at end of func
  
  map<int,InodeCap>::iterator next;
  for (map<int,InodeCap>::iterator it = in->caps.begin();
       it != in->caps.end();
       it = next) {
    next = it;
    next++;

    InodeCap &cap = it->second;
    int revoking = cap.implemented & ~cap.issued;
    
    if (in->wanted_max_size > in->inode.max_size &&
	in->wanted_max_size > in->requested_max_size)
      goto ack;

    /* completed revocation? */
    if (revoking && (revoking && used) == 0) {
      dout(10) << "completed revocation of " << (cap.implemented & ~cap.issued) << dendl;
      goto ack;
    }

    /* approaching file_max? */
    if ((cap.issued & CEPH_CAP_WR) &&
	(in->inode.size << 1) >= in->inode.max_size &&
	(in->reported_size << 1) < in->inode.max_size) {
      dout(10) << "size approaching max_size" << dendl;
      goto ack;
    }

    if ((cap.issued & ~wanted) == 0)
      continue;     /* nothing extra, all good */

    /*
    if (time_before(jiffies, ci->i_hold_caps_until)) {
    // delaying cap release for a bit
    dout(30, "delaying cap release\n");
    continue;
    }
    */
    
  ack:
    MClientFileCaps *m = new MClientFileCaps(CEPH_CAP_OP_ACK,
					     in->inode, 
                                             it->second.seq,
                                             it->second.issued,
                                             wanted,
					     0);
    in->reported_size = in->inode.size;
    m->set_max_size(in->wanted_max_size);
    in->requested_max_size = in->wanted_max_size;
    messenger->send_message(m, mdsmap->get_inst(it->first));
    if (wanted == 0)
      mds_sessions[it->first].num_caps--;
  }

  if (wanted == 0) {
    dout(10) << "last caps on " << *in << dendl;
    in->caps.clear();
    put_inode(in);
  }
}



void Client::wait_on_list(list<Cond*>& ls)
{
  Cond cond;
  ls.push_back(&cond);
  cond.Wait(client_lock);
}

void Client::signal_cond_list(list<Cond*>& ls)
{
  for (list<Cond*>::iterator it = ls.begin(); it != ls.end(); it++)
    (*it)->Signal();
  ls.clear();
}



// flush dirty data (from objectcache)

void Client::_release(Inode *in, bool checkafter)
{
  if (in->cap_refs[CEPH_CAP_RDCACHE]) {
    objectcacher->release_set(in->inode.ino);
    if (checkafter)
      put_cap_ref(in, CEPH_CAP_RDCACHE);
    else 
      in->put_cap_ref(CEPH_CAP_RDCACHE);
  }
}

struct C_Flush : public Context {
  Client *client;
  Inode *in;
  bool checkafter;
  C_Flush(Client *c, Inode *i, bool ch) : client(c), in(i), checkafter(ch) {}
  void finish(int r) {
    client->_flushed(in, checkafter);
  }
};

void Client::_flush(Inode *in, bool checkafter)
{
  dout(10) << "_flush " << *in << dendl;
  if (in->cap_refs[CEPH_CAP_WRBUFFER] == 1) {
    in->get_cap_ref(CEPH_CAP_WRBUFFER);  // for the (one!) waiter

    Context *c = new C_Flush(this,  in, checkafter);
    bool safe = objectcacher->commit_set(in->inode.ino, c);
    if (safe) {
      c->finish(0);
      delete c;
    }
  }
}

void Client::_flushed(Inode *in, bool checkafter)
{
  dout(10) << "_flushed " << *in << dendl;
  assert(in->cap_refs[CEPH_CAP_WRBUFFER] == 2);

  // release clean pages too, if we dont hold RDCACHE reference
  if (in->cap_refs[CEPH_CAP_RDCACHE] == 0)
    objectcacher->release_set(in->inode.ino);

  put_cap_ref(in, CEPH_CAP_WRBUFFER);
  if (checkafter)
    put_cap_ref(in, CEPH_CAP_WRBUFFER);
  else
    in->put_cap_ref(CEPH_CAP_WRBUFFER);
}



/** handle_file_caps
 * handle caps update from mds.  including mds to mds caps transitions.
 * do not block.
 */
void Client::add_update_inode_cap(Inode *in, int mds, unsigned issued, unsigned seq, unsigned mseq)
{
  if (!in->caps.count(mds)) {
    mds_sessions[mds].num_caps++;
    if (in->caps.empty())
      in->get();
    if (in->exporting_mds == mds) {
      dout(10) << " clearing exporting_caps on " << mds << dendl;
      in->exporting_mds = -1;
      in->exporting_issued = 0;
      in->exporting_mseq = 0;
    }
  }
  unsigned old_caps = in->caps[mds].issued;
  InodeCap &cap = in->caps[mds];

  cap.issued |= issued;
  cap.implemented |= issued;
  cap.seq = seq;
  cap.mseq = mseq;

  if (issued & ~old_caps)
    signal_cond_list(in->waitfor_caps);
}

void Client::remove_cap(Inode *in, int mds)
{
  assert(in->caps.count(mds));
  in->caps.erase(mds);
  if (in->caps.empty())
    put_inode(in);
}

void Client::handle_file_caps(MClientFileCaps *m)
{
  int mds = m->get_source().num();

  m->clear_payload();  // for if/when we send back to MDS

  // note push seq increment
  if (mds_sessions.count(mds) == 0) 
    dout(0) << "got file_caps without session from mds" << mds << " msg " << *m << dendl;
  //assert(mds_sessions.count(mds));   // HACK FIXME SOON
  mds_sessions[mds].seq++;

  Inode *in = 0;
  if (inode_map.count(m->get_ino())) in = inode_map[ m->get_ino() ];
  if (!in) {
    /*
     * this can happen if we release caps, and then trim the inode from our cache,
     * but are racing with, e.g., an mds callback.
     */
    dout(5) << "handle_file_caps don't have ino " << m->get_ino() << dendl;
    delete m;
    return;
  }
  
  if (m->get_op() == CEPH_CAP_OP_IMPORT) {
    // add/update it
    add_update_inode_cap(in, mds, m->get_caps(), m->get_seq(), m->get_mseq());

    if (in->exporting_mseq < m->get_mseq()) {
      dout(5) << "handle_file_caps ino " << m->get_ino() << " mseq " << m->get_mseq()
	      << " IMPORT from mds" << mds << ", clearing exporting_issued " << cap_string(in->exporting_issued) 
	      << " mseq " << in->exporting_mseq << dendl;
      in->exporting_issued = 0;
      in->exporting_mseq = 0;
      in->exporting_mds = -1;
    } else {
      dout(5) << "handle_file_caps ino " << m->get_ino() << " mseq " << m->get_mseq()
	      << " IMPORT from mds" << mds << ", keeping exporting_issued " << cap_string(in->exporting_issued) 
	      << " mseq " << in->exporting_mseq << " by mds" << in->exporting_mds << dendl;
    }
    delete m;
    return;
  }

  if (m->get_op() == CEPH_CAP_OP_EXPORT) {
    // note?
    bool found_higher_mseq = false;
    InodeCap *cap = 0;
    for (map<int,InodeCap>::iterator p = in->caps.begin();
	 p != in->caps.end();
	 p++) {
      if (p->first == mds) {
	cap = &p->second;
	continue;
      }
      if (p->second.mseq > m->get_mseq()) {
	found_higher_mseq = true;
	dout(5) << "handle_file_caps ino " << m->get_ino() << " mseq " << m->get_mseq() 
		<< " EXPORT from mds" << mds
		<< ", but mds" << p->first << " has higher mseq " << p->second.mseq << dendl;
	break;
      }
    }
    if (cap && !found_higher_mseq) {
      dout(5) << "handle_file_caps ino " << m->get_ino() << " mseq " << m->get_mseq() 
	      << " EXPORT from mds" << mds
	      << ", setting exporting_issued " << cap_string(cap->issued) << dendl;
      in->exporting_issued = cap->issued;
      in->exporting_mseq = m->get_mseq();
      in->exporting_mds = mds;
      remove_cap(in, mds);
    }

    delete m;
    return;
  }



  // don't have cap?   
  //   (it may be that we've reopened the file locally,
  //    and thus want caps, but don't have a cap yet.
  //    we should never reply to a cap out of turn.)
  if (in->caps.count(mds) == 0) {
    // silently drop.
    dout(5) << "handle_file_caps on ino " << m->get_ino() 
            << " seq " << m->get_seq() 
            << " " << cap_string(m->get_caps()) 
            << ", don't have this cap, silently dropping." << dendl;
    delete m;
    return;
  }
  
  // ok!
  InodeCap &cap = in->caps[mds];


  // truncate?
  if (m->get_op() == CEPH_CAP_OP_TRUNC) {
    dout(10) << "handle_file_caps TRUNC on ino " << in->ino()
	     << " size " << in->inode.size << " -> " << m->get_size()
	     << dendl;
    // trim filecache?
    if (g_conf.client_oc &&
	m->get_size() < in->inode.size) {
      // map range to objects
      list<ObjectExtent> ls;
      filer->file_to_extents(in->inode.ino, &in->inode.layout, 
			     m->get_size(), in->inode.size - m->get_size(),
			     ls);
      objectcacher->truncate_set(in->inode.ino, ls);
    }

    in->reported_size = in->inode.size = m->get_size(); 
    delete m;
    return;
  }

  cap.seq = m->get_seq();

  // don't want it?
  int wanted = in->caps_wanted();
  if (wanted == 0) {
    dout(5) << "handle_file_caps on ino " << m->get_ino() 
            << " seq " << m->get_seq() 
            << " " << cap_string(m->get_caps()) 
            << ", which we don't want caps for, releasing." << dendl;
    m->set_op(CEPH_CAP_OP_ACK);
    m->set_caps(0);
    m->set_wanted(0);
    messenger->send_message(m, m->get_source_inst());

    // FIXME...

    return;
  }

  int used = in->caps_used();

  // update per-mds caps
  const int old_caps = cap.issued;
  const int new_caps = m->get_caps();
  dout(5) << "handle_file_caps on in " << m->get_ino() 
          << " mds" << mds << " seq " << m->get_seq() 
          << " caps now " << cap_string(new_caps) 
          << " was " << cap_string(old_caps) << dendl;
  
  // size/ctime/mtime/atime
  update_inode_file_bits(in, m->get_size(), m->get_ctime(), m->get_mtime(), m->get_atime(), old_caps, m->get_time_warp_seq());

  // max_size
  bool kick_writers = false;
  if (m->get_max_size() != in->inode.max_size) {
    dout(10) << "max_size " << in->inode.max_size << " -> " << m->get_max_size() << dendl;
    in->inode.max_size = m->get_max_size();
    if (in->inode.max_size > in->wanted_max_size) {
      in->wanted_max_size = 0;
      in->requested_max_size = 0;
    }
    kick_writers = true;
  }

  // update caps

  bool ack = false;
  
  if (old_caps & ~new_caps) { 
    dout(10) << "  revocation of " << cap_string(~new_caps & old_caps) << dendl;
    cap.issued = new_caps;

    if ((cap.issued & ~new_caps) & CEPH_CAP_RDCACHE)
      _release(in, false);

    if ((used & ~new_caps) & CEPH_CAP_WRBUFFER)
      _flush(in, false);
    else {
      ack = true;
      cap.implemented = new_caps;

      // share our (possibly newer) file size, mtime, atime
      m->set_size(in->inode.size);
      m->set_max_size(0);  // dont re-request
      m->set_mtime(in->inode.mtime);
      m->set_atime(in->inode.atime);
      m->set_wanted(wanted);
    }
  } else if (old_caps == new_caps) {
    dout(10) << "  caps unchanged at " << cap_string(old_caps) << dendl;
  } else {
    dout(10) << "  grant, new caps are " << cap_string(new_caps & ~old_caps) << dendl;
    cap.issued = cap.implemented = new_caps;
  }

  // wake up waiters
  if (new_caps)
    signal_cond_list(in->waitfor_caps);

  if (ack)
    messenger->send_message(m, m->get_source_inst());
  else
    delete m;
}


// -------------------
// MOUNT

void Client::_try_mount()
{
  dout(10) << "_try_mount" << dendl;
  int mon = monmap->pick_mon();
  dout(2) << "sending client_mount to mon" << mon << dendl;
  messenger->set_dispatcher(this);
  messenger->send_message(new MClientMount, monmap->get_inst(mon));

  // schedule timeout?
  assert(mount_timeout_event == 0);
  mount_timeout_event = new C_MountTimeout(this);
  timer.add_event_after(g_conf.client_mount_timeout, mount_timeout_event);
}

void Client::_mount_timeout()
{
  dout(10) << "_mount_timeout" << dendl;
  mount_timeout_event = 0;
  _try_mount();
}

int Client::mount()
{
  Mutex::Locker lock(client_lock);

  if (mounted) {
    dout(5) << "already mounted" << dendl;;
    return 0;
  }

  // only first mounter does the work
  bool itsme = false;
  if (!mounters) {
    itsme = true;
    objecter->init();
    _try_mount();
  } else {
    dout(5) << "additional mounter" << dendl;
  }
  mounters++;
  
  while (!mdsmap ||
	 !osdmap || 
	 osdmap->get_epoch() == 0 ||
	 (!itsme && !mounted))       // non-doers wait a little longer
    mount_cond.Wait(client_lock);
  
  if (!itsme) {
    dout(5) << "additional mounter returning" << dendl;
    assert(mounted);
    return 0; 
  }

  // finish.
  timer.cancel_event(mount_timeout_event);
  mount_timeout_event = 0;
  
  mounted = true;
  mount_cond.SignalAll(); // wake up non-doers
  
  dout(2) << "mounted: have osdmap " << osdmap->get_epoch() 
	  << " and mdsmap " << mdsmap->get_epoch() 
	  << dendl;
  
  // hack: get+pin root inode.
  //  fuse assumes it's always there.
  Inode *root;
  filepath fpath(1);
  _do_lstat(fpath, CEPH_STAT_MASK_INODE_ALL, &root);
  _ll_get(root);

  // trace?
  if (g_conf.client_trace) {
    traceout.open(g_conf.client_trace);
    if (traceout.is_open()) {
      dout(1) << "opened trace file '" << g_conf.client_trace << "'" << dendl;
    } else {
      dout(1) << "FAILED to open trace file '" << g_conf.client_trace << "'" << dendl;
    }
  }

  /*
  dout(3) << "op: // client trace data structs" << dendl;
  dout(3) << "op: struct stat st;" << dendl;
  dout(3) << "op: struct utimbuf utim;" << dendl;
  dout(3) << "op: int readlinkbuf_len = 1000;" << dendl;
  dout(3) << "op: char readlinkbuf[readlinkbuf_len];" << dendl;
  dout(3) << "op: map<string, inode_t*> dir_contents;" << dendl;
  dout(3) << "op: map<int, int> open_files;" << dendl;
  dout(3) << "op: int fd;" << dendl;
  */
  return 0;
}

void Client::handle_mon_map(MMonMap *m)
{
  dout(10) << "handle_mon_map " << *m << dendl;
  // just ignore it for now.
  delete m;
}


// UNMOUNT


int Client::unmount()
{
  Mutex::Locker lock(client_lock);

  assert(mounted);  // caller is confused?
  assert(mounters > 0);
  if (--mounters > 0) {
    dout(0) << "umount -- still " << mounters << " others, doing nothing" << dendl;
    return -1;  // i'm not the last mounter.
  }


  dout(2) << "unmounting" << dendl;
  unmounting = true;

  if (tick_event)
    timer.cancel_event(tick_event);
  tick_event = 0;

  // NOTE: i'm assuming all caches are already flushing (because all files are closed).
  assert(fd_map.empty());

  dout(10) << "a" << dendl;

  _ll_drop_pins();
  
  dout(10) << "b" << dendl;

  // empty lru cache
  lru.lru_set_max(0);
  trim_cache();

  if (g_conf.client_oc) {
    // release any/all caps
    hash_map<inodeno_t, Inode*>::iterator next;
    for (hash_map<inodeno_t, Inode*>::iterator p = inode_map.begin();
         p != inode_map.end(); 
         p = next) {
      next = p;
      next++;
      Inode *in = p->second;
      if (!in) {
	dout(0) << "null inode_map entry ino " << p->first << dendl;
	assert(in);
      }      
      if (!in->caps.empty()) {
	in->get();
	_release(in);
	_flush(in);
	put_inode(in);
      }
    }
  }

  //if (0) {// hack
  while (lru.lru_get_size() > 0 || 
         !inode_map.empty()) {
    dout(2) << "cache still has " << lru.lru_get_size() 
            << "+" << inode_map.size() << " items" 
	    << ", waiting (for caps to release?)"
            << dendl;
    dump_cache();
    mount_cond.Wait(client_lock);
  }
  assert(lru.lru_get_size() == 0);
  assert(inode_map.empty());
  //}

  // unsafe writes
  if (!g_conf.client_oc) {
    while (unsafe_sync_write > 0) {
      dout(0) << unsafe_sync_write << " unsafe_sync_writes, waiting" 
              << dendl;
      mount_cond.Wait(client_lock);
    }
  }

  // stop tracing
  if (g_conf.client_trace) {
    dout(1) << "closing trace file '" << g_conf.client_trace << "'" << dendl;
    traceout.close();
  }

  
  // send session closes!
  for (map<int,MDSSession>::iterator p = mds_sessions.begin();
       p != mds_sessions.end();
       ++p) {
    dout(2) << "sending client_session close to mds" << p->first
	    << " seq " << p->second.seq << dendl;
    messenger->send_message(new MClientSession(CEPH_SESSION_REQUEST_CLOSE, p->second.seq),
			    mdsmap->get_inst(p->first));
  }

  // send unmount!
  int mon = monmap->pick_mon();
  dout(2) << "sending client_unmount to mon" << mon << dendl;
  messenger->send_message(new MClientUnmount, monmap->get_inst(mon));
  
  while (mounted)
    mount_cond.Wait(client_lock);

  dout(2) << "unmounted." << dendl;

  objecter->shutdown();

  return 0;
}

void Client::handle_unmount(Message* m)
{
  dout(1) << "handle_unmount got ack" << dendl;

  mounted = false;

  delete mdsmap;
  mdsmap = 0;

  mount_cond.Signal();

  delete m;
}


class C_C_Tick : public Context {
  Client *client;
public:
  C_C_Tick(Client *c) : client(c) {}
  void finish(int r) {
    client->tick();
  }
};

void Client::tick()
{
  dout(10) << "tick" << dendl;
  tick_event = new C_C_Tick(this);
  timer.add_event_after(g_conf.client_tick_interval, tick_event);
  
  utime_t now = g_clock.now();
  utime_t el = now - last_cap_renew;
  if (mdsmap && el > mdsmap->get_session_timeout() / 3.0)
    renew_caps();
  
}

void Client::renew_caps()
{
  dout(10) << "renew_caps" << dendl;
  last_cap_renew = g_clock.now();
  
  for (map<int,MDSSession>::iterator p = mds_sessions.begin();
       p != mds_sessions.end();
       p++) {
    dout(15) << "renew_caps requesting from mds" << p->first << dendl;
    p->second.last_cap_renew_request = g_clock.now();
    messenger->send_message(new MClientSession(CEPH_SESSION_REQUEST_RENEWCAPS),
			    mdsmap->get_inst(p->first));
  }
}


// ===============================================================
// high level (POSIXy) interface

// namespace ops

int Client::link(const char *existing, const char *newname) 
{
  Mutex::Locker lock(client_lock);
  tout << "link" << std::endl;
  tout << existing << std::endl;
  tout << newname << std::endl;
  filepath e(existing);
  filepath n(newname);
  return _link(e, n);
}

int Client::_link(const filepath &existing, const filepath &newname, int uid, int gid) 
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_LINK, messenger->get_myinst());
  req->set_filepath(newname);
  req->set_filepath2(existing);
  
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;
  dout(10) << "link result is " << res << dendl;

  trim_cache();
  dout(3) << "link(\"" << existing << "\", \"" << newname << "\") = " << res << dendl;
  return res;
}


int Client::unlink(const char *relpath)
{
  Mutex::Locker lock(client_lock);
  tout << "unlink" << std::endl;
  tout << relpath << std::endl;

  filepath path = mkpath(relpath);
  return _unlink(path);
}

int Client::_unlink(const filepath &path, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_UNLINK, messenger->get_myinst());
  req->set_filepath(path);
 
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  if (res == 0) {
    // remove from local cache
    filepath fp(path);
    Dentry *dn = lookup(fp);
    if (dn) {
      assert(dn->inode);
      unlink(dn);
    }
  }
  delete reply;
  dout(10) << "unlink result is " << res << dendl;

  trim_cache();
  dout(3) << "unlink(\"" << path << "\") = " << res << dendl;
  return res;
}

int Client::rename(const char *relfrom, const char *relto)
{
  Mutex::Locker lock(client_lock);
  tout << "rename" << std::endl;
  tout << relfrom << std::endl;
  tout << relto << std::endl;

  filepath from = mkpath(relfrom);
  filepath to = mkpath(relto);
  return _rename(from, to);
}

int Client::_rename(const filepath &from, const filepath &to, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_RENAME, messenger->get_myinst());
  req->set_filepath(from);
  req->set_filepath2(to);
 
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  if (res == 0) {
    // remove from local cache
    filepath fp(to);
    Dentry *dn = lookup(fp);
    if (dn) {
      assert(dn->inode);
      unlink(dn);
    }
  }
  delete reply;
  dout(10) << "rename result is " << res << dendl;

  // renamed item from our cache

  trim_cache();
  dout(3) << "rename(\"" << from << "\", \"" << to << "\") = " << res << dendl;
  return res;
}

// dirs

int Client::mkdir(const char *relpath, mode_t mode)
{
  Mutex::Locker lock(client_lock);
  tout << "mkdir" << std::endl;
  tout << relpath << std::endl;
  tout << mode << std::endl;

  filepath path = mkpath(relpath);
  return _mkdir(path, mode);
}

int Client::_mkdir(const filepath &path, mode_t mode, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_MKDIR, messenger->get_myinst());
  req->set_filepath(path);
  req->head.args.mkdir.mode = mode;
 
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;
  dout(10) << "mkdir result is " << res << dendl;

  trim_cache();

  dout(3) << "mkdir(\"" << path << "\", 0" << oct << mode << dec << ") = " << res << dendl;
  return res;
}

int Client::rmdir(const char *relpath)
{
  Mutex::Locker lock(client_lock);
  tout << "rmdir" << std::endl;
  tout << relpath << std::endl;
  filepath path = mkpath(relpath);
  return _rmdir(path);
}

int Client::_rmdir(const filepath &path, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_RMDIR, messenger->get_myinst());
  req->set_filepath(path);
 
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  if (res == 0) {
    // remove from local cache
    Dentry *dn = lookup(path);
    if (dn) {
      if (dn->inode->dir && dn->inode->dir->is_empty()) 
        close_dir(dn->inode->dir);  // FIXME: maybe i shoudl proactively hose the whole subtree from cache?
      unlink(dn);
    }
  }
  delete reply;

  trim_cache();
  dout(3) << "rmdir(\"" << path << "\") = " << res << dendl;
  return res;
}

// symlinks
  
int Client::symlink(const char *target, const char *rellink)
{
  Mutex::Locker lock(client_lock);
  tout << "symlink" << std::endl;
  tout << target << std::endl;
  tout << rellink << std::endl;

  filepath path = mkpath(rellink);
  return _symlink(path, target);
}

int Client::_symlink(const filepath &path, const char *target, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_SYMLINK, messenger->get_myinst());
  req->set_filepath(path);
  req->set_path2(target);
 
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;

  trim_cache();
  dout(3) << "_symlink(\"" << path << "\", \"" << target << "\") = " << res << dendl;
  return res;
}

int Client::readlink(const char *relpath, char *buf, loff_t size) 
{
  Mutex::Locker lock(client_lock);
  tout << "readlink" << std::endl;
  tout << relpath << std::endl;
  filepath path = mkpath(relpath);
  return _readlink(path, buf, size);
}

int Client::_readlink(const filepath &path, char *buf, loff_t size, int uid, int gid) 
{ 
  Inode *in;
  int r = _do_lstat(path, CEPH_STAT_MASK_SYMLINK, &in);
  if (r == 0 && !in->inode.is_symlink()) r = -EINVAL;
  if (r == 0) {
    // copy into buf (at most size bytes)
    r = in->symlink->length();
    if (r > size) r = size;
    memcpy(buf, in->symlink->c_str(), r);
  } else {
    buf[0] = 0;
  }
  trim_cache();

  dout(3) << "readlink(\"" << path << "\", \"" << buf << "\", " << size << ") = " << r << dendl;
  return r;
}



// inode stuff

int Client::_do_lstat(const filepath &path, int mask, Inode **in, int uid, int gid)
{  
  MClientRequest *req = 0;
  
  // check whether cache content is fresh enough
  int res = 0;

  Dentry *dn = lookup(path);
  utime_t now = g_clock.real_now();

  int havemask = 0;
  if (dn) {
    havemask = dn->inode->get_effective_lease_mask(now);
    dout(10) << "_lstat has inode " << path << " with mask " << havemask
	     << ", want " << mask << dendl;
  } else {
    dout(10) << "_lstat has no dn for path " << path << dendl;
  }
  
  if (dn && dn->inode && (havemask & mask) == mask) {
    dout(10) << "lstat cache hit w/ sufficient mask, until " << dn->inode->lease_ttl << dendl;
    *in = dn->inode;
  } else {  
    req = new MClientRequest(CEPH_MDS_OP_LSTAT, messenger->get_myinst());
    req->head.args.stat.mask = mask;
    req->set_filepath(path);

    MClientReply *reply = make_request(req, uid, gid, in, 0);
    res = reply->get_result();
    dout(10) << "lstat res is " << res << dendl;
    delete reply;
    if (res != 0) 
      *in = 0;     // not a success.
  }
     
  return res;
}


int Client::fill_stat(Inode *in, struct stat *st, frag_info_t *dirstat) 
{
  dout(10) << "fill_stat on " << in->inode.ino << " mode 0" << oct << in->inode.mode << dec
	   << " mtime " << in->inode.mtime << " ctime " << in->inode.ctime << dendl;
  memset(st, 0, sizeof(struct stat));
  st->st_ino = in->inode.ino;
  st->st_mode = in->inode.mode;
  st->st_rdev = in->inode.rdev;
  st->st_nlink = in->inode.nlink;
  st->st_uid = in->inode.uid;
  st->st_gid = in->inode.gid;
  st->st_ctime = MAX(in->inode.ctime, in->inode.mtime);
  st->st_atime = in->inode.atime;
  st->st_mtime = in->inode.mtime;
  if (in->inode.is_dir()) {
    //st->st_size = in->inode.dirstat.size();
    st->st_size = in->inode.dirstat.rbytes;
    st->st_blocks = 1;
  } else {
    st->st_size = in->inode.size;
    st->st_blocks = (in->inode.size + 511) >> 9;
  }
  st->st_blksize = MAX(ceph_file_layout_su(in->inode.layout), 4096);

  if (dirstat)
    *dirstat = in->inode.dirstat;

  return in->lease_mask;
}


int Client::lstat(const char *relpath, struct stat *stbuf, frag_info_t *dirstat)
{
  Mutex::Locker lock(client_lock);
  tout << "lstat" << std::endl;
  tout << relpath << std::endl;
  filepath path = mkpath(relpath);
  return _lstat(path, stbuf, -1, -1, dirstat);
}

int Client::_lstat(const filepath &path, struct stat *stbuf, int uid, int gid, frag_info_t *dirstat)
{
  Inode *in = 0;
  int res = _do_lstat(path, CEPH_STAT_MASK_INODE_ALL, &in);
  if (res == 0) {
    assert(in);
    fill_stat(in, stbuf, dirstat);
    dout(10) << "stat sez size = " << in->inode.size
	     << " mode = 0" << oct << stbuf->st_mode << dec
	     << " ino = " << stbuf->st_ino << dendl;
  }

  trim_cache();
  dout(3) << "lstat(\"" << path << "\", " << stbuf << ") = " << res << dendl;
  return res;
}


/*
int Client::lstatlite(const char *relpath, struct statlite *stl)
{
  client_lock.Lock();
   
  string abspath;
  mkabspath(relpath, abspath);
  const char *path = abspath.c_str();

  dout(3) << "op: client->lstatlite(\"" << path << "\", &st);" << dendl;
  tout << "lstatlite" << std::endl;
  tout << path << std::endl;

  // make mask
  // FIXME.
  int mask = INODE_MASK_BASE | INODE_MASK_AUTH;
  if (S_ISVALIDSIZE(stl->st_litemask) || 
      S_ISVALIDBLOCKS(stl->st_litemask)) mask |= INODE_MASK_SIZE;
  if (S_ISVALIDMTIME(stl->st_litemask)) mask |= INODE_MASK_FILE;
  if (S_ISVALIDATIME(stl->st_litemask)) mask |= INODE_MASK_FILE;
  
  Inode *in = 0;
  int res = _lstat(path, mask, &in);
  
  if (res == 0) {
    fill_statlite(in->inode,stl);
    dout(10) << "stat sez size = " << in->inode.size << " ino = " << in->inode.ino << dendl;
  }

  trim_cache();
  client_lock.Unlock();
  return res;
}
*/


int Client::chmod(const char *relpath, mode_t mode)
{
  Mutex::Locker lock(client_lock);
  tout << "chmod" << std::endl;
  tout << relpath << std::endl;
  tout << mode << std::endl;
  filepath path = mkpath(relpath);
  return _chmod(path, mode, true);
}

static int symop(int op, bool follow) 
{
  if (follow)
    return op | CEPH_MDS_OP_FOLLOW_LINK;
  else
    return op & ~CEPH_MDS_OP_FOLLOW_LINK;  // just to be safe
}

int Client::_chmod(const filepath &path, mode_t mode, bool followsym, int uid, int gid) 
{
  dout(3) << "_chmod(" << path << ", 0" << oct << mode << dec << ")" << dendl;
  MClientRequest *req = new MClientRequest(symop(CEPH_MDS_OP_CHMOD, followsym),
					   messenger->get_myinst());
  req->set_filepath(path); 
  req->head.args.chmod.mode = mode;

  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;

  trim_cache();
  dout(3) << "_chmod(\"" << path << "\", 0" << oct << mode << dec << ") = " << res << dendl;
  return res;
}

int Client::chown(const char *relpath, uid_t uid, gid_t gid)
{
  Mutex::Locker lock(client_lock);
  tout << "chown" << std::endl;
  tout << relpath << std::endl;
  tout << uid << std::endl;
  tout << gid << std::endl;
  filepath path = mkpath(relpath);
  return _chown(path, uid, gid, true);
}

int Client::_chown(const filepath &path, uid_t uid, gid_t gid, bool followsym, int cuid, int cgid)
{
  dout(3) << "_chown(" << path << ", " << uid << ", " << gid << ")" << dendl;
  MClientRequest *req = new MClientRequest(symop(CEPH_MDS_OP_CHOWN, followsym),
					   messenger->get_myinst());
  req->set_filepath(path); 
  req->head.args.chown.uid = uid;
  req->head.args.chown.gid = gid;

  MClientReply *reply = make_request(req, cuid, cgid);
  int res = reply->get_result();
  delete reply;
  dout(10) << "chown result is " << res << dendl;

  trim_cache();
  dout(3) << "chown(\"" << path << "\", " << uid << ", " << gid << ") = " << res << dendl;
  return res;
}

int Client::utime(const char *relpath, struct utimbuf *buf)
{
  Mutex::Locker lock(client_lock);
  tout << "utime" << std::endl;
  tout << relpath << std::endl;
  tout << buf->modtime << std::endl;
  tout << buf->actime << std::endl;
  filepath path = mkpath(relpath);
  return _utimes(path, utime_t(buf->modtime,0), utime_t(buf->actime,0), true);
}

int Client::_utimes(const filepath &path, utime_t mtime, utime_t atime, bool followsym,
		    int uid, int gid)
{
  dout(3) << "_utimes(" << path << ", " << mtime << ", " << atime << ")" << dendl;

  Dentry *dn = lookup(path);
  int want = CEPH_CAP_WR|CEPH_CAP_WRBUFFER|CEPH_CAP_EXCL;
  if (dn && dn->inode &&
      (dn->inode->caps_issued() & want) == want) {
    dout(5) << " have WR and EXCL caps, just updating our m/atime" << dendl;
    dn->inode->inode.time_warp_seq++;
    dn->inode->inode.mtime = mtime;
    dn->inode->inode.atime = atime;
    return 0;
  }

  MClientRequest *req = new MClientRequest(symop(CEPH_MDS_OP_UTIME, followsym),
					   messenger->get_myinst());
  req->set_filepath(path); 
  mtime.encode_timeval(&req->head.args.utime.mtime);
  atime.encode_timeval(&req->head.args.utime.atime);
  req->head.args.utime.mask = CEPH_UTIME_ATIME | CEPH_UTIME_MTIME;

  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;

  dout(3) << "utimes(\"" << path << "\", " << mtime << ", " << atime << ") = " << res << dendl;
  trim_cache();
  return res;
}



int Client::mknod(const char *relpath, mode_t mode, dev_t rdev) 
{ 
  Mutex::Locker lock(client_lock);
  tout << "mknod" << std::endl;
  tout << relpath << std::endl;
  tout << mode << std::endl;
  tout << rdev << std::endl;
  filepath path = mkpath(relpath);
  return _mknod(path, mode, rdev);
}

int Client::_mknod(const filepath &path, mode_t mode, dev_t rdev, int uid, int gid) 
{ 
  dout(3) << "_mknod(" << path << ", 0" << oct << mode << dec << ", " << rdev << ")" << dendl;

  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_MKNOD, messenger->get_myinst());
  req->set_filepath(path); 
  req->head.args.mknod.mode = mode;
  req->head.args.mknod.rdev = rdev;

  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();

  delete reply;

  trim_cache();

  dout(3) << "mknod(\"" << path << "\", 0" << oct << mode << dec << ") = " << res << dendl;
  return res;
}



  
int Client::getdir(const char *relpath, list<string>& contents)
{
  dout(3) << "getdir(" << relpath << ")" << dendl;
  {
    Mutex::Locker lock(client_lock);
    tout << "getdir" << std::endl;
    tout << relpath << std::endl;
  }

  DIR *d;
  int r = opendir(relpath, &d);
  if (r < 0) return r;

  struct dirent de;
  int n = 0;
  while (readdir_r(d, &de) == 0) {
    contents.push_back(de.d_name);
    n++;
  }
  closedir(d);

  return n;
}

int Client::opendir(const char *relpath, DIR **dirpp) 
{
  Mutex::Locker lock(client_lock);
  tout << "opendir" << std::endl;
  tout << relpath << std::endl;
  filepath path = mkpath(relpath);
  int r = _opendir(path, (DirResult**)dirpp);
  tout << (unsigned long)*dirpp << std::endl;
  return r;
}

int Client::_opendir(const filepath &path, DirResult **dirpp, int uid, int gid) 
{
  *dirpp = new DirResult(path);

  // do we have the inode in our cache?  
  // if so, should be we ask for a different dirfrag?
  Dentry *dn = lookup(path);
  if (dn && dn->inode) {
    (*dirpp)->inode = dn->inode;
    (*dirpp)->inode->get();
    dout(10) << "had inode " << dn->inode << " " << dn->inode->inode.ino << " ref now " << dn->inode->ref << dendl;
    (*dirpp)->set_frag(dn->inode->dirfragtree[0]);
    dout(10) << "_opendir " << path << ", our cache says the first dirfrag is " << (*dirpp)->frag() << dendl;
  }

  // get the first frag
  int r = _readdir_get_frag(*dirpp);
  if (r < 0) {
    _closedir(*dirpp);
    *dirpp = 0;
  } else {
    r = 0;
  }
  dout(3) << "_opendir(" << path << ") = " << r << " (" << *dirpp << ")" << dendl;
  return r;
}

void Client::_readdir_add_dirent(DirResult *dirp, const string& name, Inode *in)
{
  struct stat st;
  int stmask = fill_stat(in, &st);  
  frag_t fg = dirp->frag();
  dirp->buffer[fg].push_back(DirEntry(name, st, stmask));
  dout(10) << "_readdir_add_dirent " << dirp << " added '" << name << "' -> " << in->inode.ino
	   << ", size now " << dirp->buffer[fg].size() << dendl;
}

//struct dirent {
//  ino_t          d_ino;       /* inode number */
//  off_t          d_off;       /* offset to the next dirent */
//  unsigned short d_reclen;    /* length of this record */
//  unsigned char  d_type;      /* type of file */
//  char           d_name[256]; /* filename */
//};
void Client::_readdir_fill_dirent(struct dirent *de, DirEntry *entry, loff_t off)
{
  strncpy(de->d_name, entry->d_name.c_str(), 256);
#ifndef __CYGWIN__
  de->d_ino = entry->st.st_ino;
#ifndef DARWIN
  de->d_off = off + 1;
#endif
  de->d_reclen = 1;
  de->d_type = MODE_TO_DT(entry->st.st_mode);
  dout(10) << "_readdir_fill_dirent '" << de->d_name << "' -> " << de->d_ino
	   << " type " << (int)de->d_type << " at off " << off << dendl;
#endif
}

void Client::_readdir_next_frag(DirResult *dirp)
{
  frag_t fg = dirp->frag();

  // hose old data
  assert(dirp->buffer.count(fg));
  dirp->buffer.erase(fg);

  // advance
  dirp->next_frag();
  if (dirp->at_end()) {
    dout(10) << "_readdir_next_frag advance from " << fg << " to END" << dendl;
  } else {
    dout(10) << "_readdir_next_frag advance from " << fg << " to " << dirp->frag() << dendl;
    _readdir_rechoose_frag(dirp);
  }
}

void Client::_readdir_rechoose_frag(DirResult *dirp)
{
  assert(dirp->inode);
  frag_t cur = dirp->frag();
  frag_t f = dirp->inode->dirfragtree[cur.value()];
  if (f != cur) {
    dout(10) << "_readdir_rechoose_frag frag " << cur << " maps to " << f << dendl;
    dirp->set_frag(f);
  }
}

int Client::_readdir_get_frag(DirResult *dirp)
{
  // get the current frag.
  frag_t fg = dirp->frag();
  assert(dirp->buffer.count(fg) == 0);
  
  dout(10) << "_readdir_get_frag " << dirp << " on " << dirp->path << " fg " << fg << dendl;

  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_READDIR, messenger->get_myinst());
  req->set_filepath(dirp->path); 
  req->head.args.readdir.frag = fg;
  
  Inode *diri;
  utime_t from;
  MClientReply *reply = make_request(req, -1, -1, &diri, &from);
  int res = reply->get_result();
  
  // did i get directory inode?
  if ((res == -EAGAIN || res == 0) && diri) {
    dout(10) << "_readdir_get_frag got diri " << diri << " " << diri->inode.ino << dendl;
    assert(diri->inode.is_dir());
  }
  
  if (!dirp->inode && diri) {
    dout(10) << "_readdir_get_frag attaching inode" << dendl;
    dirp->inode = diri;
    diri->get();
  }

  if (res == -EAGAIN) {
    dout(10) << "_readdir_get_frag got EAGAIN, retrying" << dendl;
    _readdir_rechoose_frag(dirp);
    return _readdir_get_frag(dirp);
  }

  if (res == 0) {
    // stuff dir contents to cache, DirResult
    assert(diri);

    // create empty result vector
    dirp->buffer[fg].clear();

    if (fg.is_leftmost()) {
      // add . and ..?
      string dot(".");
      _readdir_add_dirent(dirp, dot, diri);
      string dotdot("..");
      if (diri->dn)
	_readdir_add_dirent(dirp, dotdot, diri->dn->dir->parent_inode);
      //else
      //_readdir_add_dirent(dirp, dotdot, DT_DIR);
    }
    
    // the rest?
    bufferlist::iterator p = reply->get_dir_bl().begin();
    if (!p.end()) {
      // only open dir if we're actually adding stuff to it!
      Dir *dir = diri->open_dir();
      assert(dir);

      // dirstat
      DirStat dst(p);
      __u32 numdn;
      ::decode(numdn, p);

      string dname;
      LeaseStat dlease, ilease;
      while (numdn) {
	::decode(dname, p);
	::decode(dlease, p);
	InodeStat ist(p);
	::decode(ilease, p);

	// cache
	Inode *in = this->insert_dentry_inode(dir, dname, &dlease, &ist, &ilease, from);

	// caller
	dout(15) << "_readdir_get_frag got " << dname << " to " << in->inode.ino << dendl;
	_readdir_add_dirent(dirp, dname, in);

	numdn--;
      }
      
      if (dir->is_empty())
	close_dir(dir);
    }

    // FIXME: remove items in cache that weren't in my readdir?
    // ***
  } else {
    dout(10) << "_readdir_get_frag got error " << res << ", setting end flag" << dendl;
    dirp->set_end();
  }

  delete reply;

  return res;
}

int Client::readdir_r(DIR *d, struct dirent *de)
{  
  return readdirplus_r(d, de, 0, 0);
}

int Client::readdirplus_r(DIR *d, struct dirent *de, struct stat *st, int *stmask)
{  
  DirResult *dirp = (DirResult*)d;
  
  while (1) {
    if (dirp->at_end()) return -1;

    if (dirp->buffer.count(dirp->frag()) == 0) {
      Mutex::Locker lock(client_lock);
      _readdir_get_frag(dirp);
      if (dirp->at_end()) return -1;
    }

    frag_t fg = dirp->frag();
    uint32_t pos = dirp->fragpos();
    assert(dirp->buffer.count(fg));   
    vector<DirEntry> &ent = dirp->buffer[fg];

    if (ent.empty()) {
      dout(10) << "empty frag " << fg << ", moving on to next" << dendl;
      _readdir_next_frag(dirp);
      continue;
    }

    assert(pos < ent.size());
    _readdir_fill_dirent(de, &ent[pos], dirp->offset);
    if (st) *st = ent[pos].st;
    if (stmask) *stmask = ent[pos].stmask;
    pos++;
    dirp->offset++;

    if (pos == ent.size()) 
      _readdir_next_frag(dirp);

    break;
  }

  return 0;
}


int Client::closedir(DIR *dir) 
{
  Mutex::Locker lock(client_lock);
  tout << "closedir" << std::endl;
  tout << (unsigned long)dir << std::endl;

  dout(3) << "closedir(" << dir << ") = 0" << dendl;
  _closedir((DirResult*)dir);
  return 0;
}

void Client::_closedir(DirResult *dirp)
{
  dout(10) << "_closedir(" << dirp << ")" << dendl;
  if (dirp->inode) {
    dout(10) << "_closedir detaching inode " << dirp->inode << dendl;
    put_inode(dirp->inode);
    dirp->inode = 0;
  }
  delete dirp;
}

void Client::rewinddir(DIR *dirp)
{
  dout(3) << "rewinddir(" << dirp << ")" << dendl;
  DirResult *d = (DirResult*)dirp;
  d->offset = 0;
  d->buffer.clear();
}
 
loff_t Client::telldir(DIR *dirp)
{
  DirResult *d = (DirResult*)dirp;
  dout(3) << "telldir(" << dirp << ") = " << d->offset << dendl;
  return d->offset;
}

void Client::seekdir(DIR *dirp, loff_t offset)
{
  dout(3) << "seekdir(" << dirp << ", " << offset << ")" << dendl;
  DirResult *d = (DirResult*)dirp;
  d->offset = offset;
}







/****** file i/o **********/

int Client::open(const char *relpath, int flags, mode_t mode) 
{
  Mutex::Locker lock(client_lock);
  tout << "open" << std::endl;
  tout << relpath << std::endl;
  tout << flags << std::endl;

  filepath path = mkpath(relpath);

  Fh *fh;
  int r = _open(path, flags, mode, &fh);
  if (r >= 0) {
    // allocate a integer file descriptor
    assert(fh);
    r = get_fd();
    assert(fd_map.count(r) == 0);
    fd_map[r] = fh;
  }
  
  tout << r << std::endl;
  dout(3) << "open(" << path << ", " << flags << ") = " << r << dendl;
  return r;
}

int Client::_open(const filepath &path, int flags, mode_t mode, Fh **fhp, int uid, int gid) 
{
  // go
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_OPEN, messenger->get_myinst());
  req->set_filepath(path); 
  req->head.args.open.flags = flags;
  req->head.args.open.mode = mode;

  int cmode = ceph_flags_to_mode(flags);

  // do i have the inode?
  Dentry *dn = lookup(req->get_filepath());
  Inode *in = 0;
  if (dn) {
    in = dn->inode;
    in->get_open_ref(cmode);  // make note of pending open, since it effects _wanted_ caps.
  }
  
  in = 0;
  MClientReply *reply = make_request(req, uid, gid, &in);
  assert(reply);

  int result = reply->get_result();

  // success?
  if (result >= 0) {
    // yay
    Fh *f = new Fh;
    if (fhp) *fhp = f;
    f->mode = cmode;
    if (flags & O_APPEND)
      f->append = true;

    // inode
    assert(in);
    f->inode = in;
    f->inode->get();
    if (!dn)
      in->get_open_ref(f->mode);  // i may have alrady added it above!

    dout(10) << in->inode.ino << " mode " << cmode << dendl;

    // add the cap
    int mds = reply->get_source().num();
    add_update_inode_cap(in, mds,
			 reply->get_file_caps(),
			 reply->get_file_caps_seq(),
			 reply->get_file_caps_mseq());    
    dout(5) << "open success, fh is " << f << " combined caps " << cap_string(in->caps_issued()) << dendl;
  }

  delete reply;

  trim_cache();

  return result;
}






int Client::close(int fd)
{
  Mutex::Locker lock(client_lock);
  tout << "close" << std::endl;
  tout << fd << std::endl;

  dout(3) << "close(" << fd << ")" << dendl;
  assert(fd_map.count(fd));
  Fh *fh = fd_map[fd];
  _release(fh);
  fd_map.erase(fd);
  return 0;
}

int Client::_release(Fh *f)
{
  //dout(3) << "op: client->close(open_files[ " << fh << " ]);" << dendl;
  //dout(3) << "op: open_files.erase( " << fh << " );" << dendl;
  dout(5) << "_release " << f << dendl;
  Inode *in = f->inode;

  if (in->put_open_ref(f->mode))
    check_caps(in);
  put_inode( in );

  delete f;
  return 0;
}



// ------------
// read, write

loff_t Client::lseek(int fd, loff_t offset, int whence)
{
  Mutex::Locker lock(client_lock);
  tout << "lseek" << std::endl;
  tout << fd << std::endl;
  tout << offset << std::endl;
  tout << whence << std::endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  switch (whence) {
  case SEEK_SET: 
    f->pos = offset;
    break;

  case SEEK_CUR:
    f->pos += offset;
    break;

  case SEEK_END:
    f->pos = in->inode.size + offset;
    break;

  default:
    assert(0);
  }
  
  loff_t pos = f->pos;

  dout(3) << "lseek(" << fd << ", " << offset << ", " << whence << ") = " << pos << dendl;
  return pos;
}


void Client::lock_fh_pos(Fh *f)
{
  dout(10) << "lock_fh_pos " << f << dendl;

  if (f->pos_locked || !f->pos_waiters.empty()) {
    Cond cond;
    f->pos_waiters.push_back(&cond);
    dout(10) << "lock_fh_pos BLOCKING on " << f << dendl;
    while (f->pos_locked || f->pos_waiters.front() != &cond)
      cond.Wait(client_lock);
    dout(10) << "lock_fh_pos UNBLOCKING on " << f << dendl;
    assert(f->pos_waiters.front() == &cond);
    f->pos_waiters.pop_front();
  }
  
  f->pos_locked = true;
}

void Client::unlock_fh_pos(Fh *f)
{
  dout(10) << "unlock_fh_pos " << f << dendl;
  f->pos_locked = false;
}



//char *hackbuf = 0;


// blocking osd interface

int Client::read(int fd, char *buf, loff_t size, loff_t offset) 
{
  Mutex::Locker lock(client_lock);
  tout << "read" << std::endl;
  tout << fd << std::endl;
  tout << size << std::endl;
  tout << offset << std::endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  bufferlist bl;
  int r = _read(f, offset, size, &bl);
  dout(3) << "read(" << fd << ", " << (void*)buf << ", " << size << ", " << offset << ") = " << r << dendl;
  if (r >= 0) {
    bl.copy(0, bl.length(), buf);
    r = bl.length();
  }
  return r;
}

int Client::_read(Fh *f, __s64 offset, __u64 size, bufferlist *bl)
{
  Inode *in = f->inode;

  bool movepos = false;
  if (offset < 0) {
    lock_fh_pos(f);
    offset = f->pos;
    movepos = true;
  }

  bool lazy = f->mode == CEPH_FILE_MODE_LAZY;

  // wait for RD cap and/or a valid file size
  int issued;
  while (1) {
    issued = in->caps_issued();

    if (lazy) {
      // wait for lazy cap
      if ((issued & CEPH_CAP_LAZYIO) == 0) {
	dout(7) << " don't have lazy cap, waiting" << dendl;
	goto wait;
      }
    } else {
      // wait for RD cap?
      while ((issued & CEPH_CAP_RD) == 0) {
	dout(7) << " don't have read cap, waiting" << dendl;
	goto wait;
      }
    }
    
    // async i/o?
    if ((issued & (CEPH_CAP_WRBUFFER|CEPH_CAP_RDCACHE))) {

      // FIXME: this logic needs to move info FileCache!

      // wait for valid file size
      if (!in->have_valid_size()) {
	dout(7) << " don't have (rd+rdcache)|lease|excl for valid file size, waiting" << dendl;
	goto wait;
      }

      dout(10) << "file size: " << in->inode.size << dendl;
      if (offset > 0 && (__u64)offset >= in->inode.size) {
	if (movepos) unlock_fh_pos(f);
	return 0;
      }
      if ((__u64)(offset + size) > in->inode.size) 
	size = in->inode.size - offset;
      
      if (size == 0) {
	dout(10) << "read is size=0, returning 0" << dendl;
	if (movepos) unlock_fh_pos(f);
	return 0;
      }
      break;
    } else {
      // unbuffered, sync i/o.  we will defer to osd.
      break;
    }
    
  wait:
    wait_on_list(in->waitfor_caps);
  }
  
  in->get_cap_ref(CEPH_CAP_RD);

  int rvalue = 0;
  Cond cond;
  bool done = false;
  Context *onfinish = new C_SafeCond(&client_lock, &cond, &done, &rvalue);
  
  int r = 0;
  if (g_conf.client_oc) {

    if (issued & CEPH_CAP_RDCACHE) {
      // we will populate the cache here
      if (in->cap_refs[CEPH_CAP_RDCACHE] == 0)
	in->get_cap_ref(CEPH_CAP_RDCACHE);

      // readahead?
      if (f->nr_consec_read &&
	  (g_conf.client_readahead_max_bytes ||
	   g_conf.client_readahead_max_periods)) {
	loff_t l = f->consec_read_bytes * 2;
	if (g_conf.client_readahead_min)
	  l = MAX(l, g_conf.client_readahead_min);
	if (g_conf.client_readahead_max_bytes)
	  l = MIN(l, g_conf.client_readahead_max_bytes);
	loff_t p = ceph_file_layout_period(in->inode.layout);
	if (g_conf.client_readahead_max_periods)
	  l = MIN(l, g_conf.client_readahead_max_periods * p);
	if (l >= 2*p)
	  // align with period
	  l -= (offset+l) % p;
	// don't read past end of file
	if (offset+l > (loff_t)in->inode.size)
	  l = in->inode.size - offset;

	dout(10) << "readahead " << f->nr_consec_read << " reads " 
		 << f->consec_read_bytes << " bytes ... readahead " << offset << "~" << l
		 << " (caller wants " << offset << "~" << size << ")" << dendl;
	objectcacher->file_read(in->inode.ino, &in->inode.layout, offset, l, NULL, 0, 0);
	dout(10) << "readahead initiated" << dendl;
      }

      // read (and possibly block)
      r = objectcacher->file_read(in->inode.ino, &in->inode.layout, offset, size, bl, 0, onfinish);
      
      if (r == 0) {
	while (!done) 
	  cond.Wait(client_lock);
	r = rvalue;
      } else {
	// it was cached.
	delete onfinish;
      }
    } else {
      r = objectcacher->file_atomic_sync_read(in->inode.ino, &in->inode.layout, offset, size, bl, 0, client_lock);
    }
    
  } else {
    // object cache OFF -- non-atomic sync read from osd
  
    // do sync read
    Objecter::OSDRead *rd = filer->prepare_read(in->inode.ino, &in->inode.layout, offset, size, bl, 0);
    if (in->hack_balance_reads || g_conf.client_hack_balance_reads)
      rd->flags |= CEPH_OSD_OP_BALANCE_READS;
    r = objecter->readx(rd, onfinish);
    assert(r >= 0);

    while (!done)
      cond.Wait(client_lock);
    r = rvalue;
  }
 
  if (movepos) {
    // adjust fd pos
    f->pos = offset+bl->length();
    unlock_fh_pos(f);
  }

  // adjust readahead state
  if (f->last_pos != offset) {
    f->nr_consec_read = f->consec_read_bytes = 0;
  } else {
    f->nr_consec_read++;
  }
  f->consec_read_bytes += bl->length();
  dout(10) << "readahead nr_consec_read " << f->nr_consec_read
	   << " for " << f->consec_read_bytes << " bytes" 
	   << " .. last_pos " << f->last_pos << " .. offset " << offset
	   << dendl;
  f->last_pos = offset+bl->length();

  // done!
  put_cap_ref(in, CEPH_CAP_RD);

  return rvalue;
}



/*
 * we keep count of uncommitted sync writes on the inode, so that
 * fsync can DDRT.
 */
class C_Client_SyncCommit : public Context {
  Client *cl;
  Inode *in;
public:
  C_Client_SyncCommit(Client *c, Inode *i) : cl(c), in(i) {
    in->get();
  }
  void finish(int) {
    cl->sync_write_commit(in);
  }
};

void Client::sync_write_commit(Inode *in)
{
  client_lock.Lock();
  assert(unsafe_sync_write > 0);
  unsafe_sync_write--;

  put_cap_ref(in, CEPH_CAP_WRBUFFER);

  dout(15) << "sync_write_commit unsafe_sync_write = " << unsafe_sync_write << dendl;
  if (unsafe_sync_write == 0 && unmounting) {
    dout(10) << "sync_write_comit -- no more unsafe writes, unmount can proceed" << dendl;
    mount_cond.Signal();
  }

  put_inode(in);

  client_lock.Unlock();
}

int Client::write(int fd, const char *buf, loff_t size, loff_t offset) 
{
  Mutex::Locker lock(client_lock);
  tout << "write" << std::endl;
  tout << fd << std::endl;
  tout << size << std::endl;
  tout << offset << std::endl;

  assert(fd_map.count(fd));
  Fh *fh = fd_map[fd];
  int r = _write(fh, offset, size, buf);
  dout(3) << "write(" << fd << ", \"...\", " << size << ", " << offset << ") = " << r << dendl;
  return r;
}


int Client::_write(Fh *f, __s64 offset, __u64 size, const char *buf)
{
  //dout(7) << "write fh " << fh << " size " << size << " offset " << offset << dendl;
  Inode *in = f->inode;

  // use/adjust fd pos?
  if (offset < 0) {
    lock_fh_pos(f);
    /* 
     * FIXME: this is racy in that we may block _after_ this point waiting for caps, and inode.size may
     * change out from under us.
     */
    if (f->append)
      f->pos = in->inode.size;   // O_APPEND.
    offset = f->pos;
    f->pos = offset+size;    
    unlock_fh_pos(f);
  }

  bool lazy = f->mode == CEPH_FILE_MODE_LAZY;

  dout(10) << "cur file size is " << in->inode.size << dendl;

  // time it.
  utime_t start = g_clock.real_now();
    
  // copy into fresh buffer (since our write may be resub, async)
  bufferptr bp;
  if (size > 0) bp = buffer::copy(buf, size);
  bufferlist bl;
  bl.push_back( bp );

  // request larger max_size?
  __u64 endoff = offset + size;
  if ((endoff >= in->inode.max_size ||
       endoff > (in->inode.size << 1)) &&
      endoff > in->wanted_max_size) {
    dout(10) << "wanted_max_size " << in->wanted_max_size << " -> " << endoff << dendl;
    in->wanted_max_size = endoff;
    check_caps(in);
  }
  
  // wait for caps, max_size
  while ((lazy && (in->caps_issued() & CEPH_CAP_LAZYIO) == 0) ||
	 (!lazy && (in->caps_issued() & CEPH_CAP_WR) == 0) ||
	 endoff > in->inode.max_size) {
    dout(7) << "missing wr|lazy cap OR endoff " << endoff
	    << " > max_size " << in->inode.max_size 
	    << ", waiting" << dendl;
    wait_on_list(in->waitfor_caps);
  }

  in->get_cap_ref(CEPH_CAP_WR);

  // avoid livelock with fsync?
  // FIXME

  if (g_conf.client_oc) {
    if (in->caps_issued() & CEPH_CAP_WRBUFFER) {
      // do buffered write
      if (in->cap_refs[CEPH_CAP_WRBUFFER] == 0)
	in->get_cap_ref(CEPH_CAP_WRBUFFER);
      
      // wait? (this may block!)
      objectcacher->wait_for_write(size, client_lock);
      
      // async, caching, non-blocking.
      objectcacher->file_write(in->inode.ino, &in->inode.layout, offset, size, bl, 0);
    } else {
      // atomic, synchronous, blocking.
      objectcacher->file_atomic_sync_write(in->inode.ino, &in->inode.layout, offset, size, bl, 0, client_lock);
    }   
  } else {
    // simple, non-atomic sync write
    Cond cond;
    bool done = false;
    Context *onfinish = new C_SafeCond(&client_lock, &cond, &done);
    Context *onsafe = new C_Client_SyncCommit(this, in);

    unsafe_sync_write++;
    in->get_cap_ref(CEPH_CAP_WRBUFFER);
    
    filer->write(in->inode.ino, &in->inode.layout, offset, size, bl, 0, onfinish, onsafe);
    
    while (!done)
      cond.Wait(client_lock);
  }

  // time
  utime_t lat = g_clock.real_now();
  lat -= start;
  if (client_logger) {
    client_logger->finc("wrlsum",(double)lat);
    client_logger->inc("wrlnum");
  }
    
  // assume success for now.  FIXME.
  __u64 totalwritten = size;
  
  // extend file?
  if (totalwritten + offset > in->inode.size) {
    in->inode.size = totalwritten + offset;
    
    if ((in->inode.size << 1) >= in->inode.max_size &&
	(in->reported_size << 1) < in->inode.max_size)
      check_caps(in);
      
    dout(7) << "wrote to " << totalwritten+offset << ", extending file size" << dendl;
  } else {
    dout(7) << "wrote to " << totalwritten+offset << ", leaving file size at " << in->inode.size << dendl;
  }

  // mtime
  in->inode.mtime = g_clock.real_now();

  put_cap_ref(in, CEPH_CAP_WR);
  
  // ok!
  return totalwritten;  
}

int Client::_flush(Fh *f)
{
  // no-op, for now.  hrm.
  return 0;
}


int Client::truncate(const char *relpath, loff_t length) 
{
  Mutex::Locker lock(client_lock);
  tout << "truncate" << std::endl;
  tout << relpath << std::endl;
  tout << length << std::endl;
  filepath path = mkpath(relpath);
  return _truncate(path, length, true);
}

int Client::_truncate(const filepath &path, loff_t length, bool followsym, int uid, int gid) 
{
  MClientRequest *req = new MClientRequest(symop(CEPH_MDS_OP_TRUNCATE, followsym),
					   messenger->get_myinst());
  req->set_filepath(path); 
  req->head.args.truncate.length = length;

  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;

  dout(3) << "truncate(\"" << path << "\", " << length << ") = " << res << dendl;
  return res;
}

int Client::ftruncate(int fd, loff_t length) 
{
  Mutex::Locker lock(client_lock);
  tout << "ftruncate" << std::endl;
  tout << fd << std::endl;
  tout << length << std::endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  return _ftruncate(f, length);
}

int Client::_ftruncate(Fh *fh, loff_t length) 
{
  filepath path(fh->inode->inode.ino);
  return _truncate(path, length, false);
}


int Client::fsync(int fd, bool syncdataonly) 
{
  Mutex::Locker lock(client_lock);
  tout << "fsync" << std::endl;
  tout << fd << std::endl;
  tout << syncdataonly << std::endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  int r = _fsync(f, syncdataonly);
  dout(3) << "fsync(" << fd << ", " << syncdataonly << ") = " << r << dendl;
  return r;
}

int Client::_fsync(Fh *f, bool syncdataonly)
{
  int r = 0;

  Inode *in = f->inode;

  dout(3) << "_fsync(" << f << ", " << (syncdataonly ? "dataonly)":"data+metadata)") << dendl;
  
  // metadata?
  if (!syncdataonly)
    dout(0) << "fsync - not syncing metadata yet.. implement me" << dendl;

  if (g_conf.client_oc)
    _flush(in);
  
  while (in->cap_refs[CEPH_CAP_WRBUFFER] > 0) {
    dout(10) << "ino " << in->inode.ino << " has " << in->cap_refs[CEPH_CAP_WRBUFFER] << " uncommitted, waiting" << dendl;
    wait_on_list(in->waitfor_commit);
  }    
  dout(10) << "ino " << in->inode.ino << " has no uncommitted writes" << dendl;

  return r;
}

int Client::fstat(int fd, struct stat *stbuf) 
{
  Mutex::Locker lock(client_lock);
  tout << "fstat" << std::endl;
  tout << fd << std::endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  filepath path(f->inode->ino());
  int r = _lstat(path, stbuf);
  dout(3) << "fstat(" << fd << ", " << stbuf << ") = " << r << dendl;
  return r;
}


// not written yet, but i want to link!

int Client::chdir(const char *relpath)
{
  Mutex::Locker lock(client_lock);
  tout << "chdir" << std::endl;
  tout << relpath << std::endl;
  filepath newcwd = mkpath(relpath);
  dout(3) << "chdir(" << relpath << ")  cwd " << cwd << " -> " << newcwd << dendl;
  cwd = newcwd;
  return 0;
}

int Client::statfs(const char *path, struct statvfs *stbuf)
{
  Mutex::Locker lock(client_lock);
  tout << "statfs" << std::endl;
  return _statfs(stbuf);
}

int Client::ll_statfs(inodeno_t ino, struct statvfs *stbuf)
{
  Mutex::Locker lock(client_lock);
  tout << "ll_statfs" << std::endl;
  return _statfs(stbuf);
}

int Client::_statfs(struct statvfs *stbuf)
{
  dout(3) << "_statfs" << dendl;

  Cond cond;
  tid_t tid = ++last_tid;
  StatfsRequest *req = new StatfsRequest(tid, &cond);
  statfs_requests[tid] = req;

  int mon = monmap->pick_mon();
  messenger->send_message(new MStatfs(req->tid), monmap->get_inst(mon));

  while (req->reply == 0)
    cond.Wait(client_lock);

  // fill it in
  memset(stbuf, 0, sizeof(*stbuf));
  stbuf->f_bsize = 4096;
  stbuf->f_frsize = 4096;
  stbuf->f_blocks = req->reply->stfs.f_total / 4;
  stbuf->f_bfree = req->reply->stfs.f_free / 4;
  stbuf->f_bavail = req->reply->stfs.f_avail / 4;
  stbuf->f_files = req->reply->stfs.f_objects;
  stbuf->f_ffree = -1;
  stbuf->f_favail = -1;
  stbuf->f_fsid = -1;       // ??
  stbuf->f_flag = 0;        // ??
  stbuf->f_namemax = 1024;  // ??

  statfs_requests.erase(req->tid);
  delete req->reply;
  delete req;

  int r = 0;
  dout(3) << "_statfs = " << r << dendl;
  return r;
}

void Client::handle_statfs_reply(MStatfsReply *reply)
{
  if (statfs_requests.count(reply->tid) &&
      statfs_requests[reply->tid]->reply == 0) {
    dout(10) << "handle_statfs_reply " << *reply << ", kicking waiter" << dendl;
    statfs_requests[reply->tid]->reply = reply;
    statfs_requests[reply->tid]->caller_cond->Signal();
  } else {
    dout(10) << "handle_statfs_reply " << *reply << ", dup or old, dropping" << dendl;
    delete reply;
  }
}


int Client::lazyio_propogate(int fd, loff_t offset, size_t count)
{
  client_lock.Lock();
  dout(3) << "op: client->lazyio_propogate(" << fd
          << ", " << offset << ", " << count << ")" << dendl;
  
  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];

  // for now
  _fsync(f, true);

  client_lock.Unlock();
  return 0;
}

int Client::lazyio_synchronize(int fd, loff_t offset, size_t count)
{
  client_lock.Lock();
  dout(3) << "op: client->lazyio_synchronize(" << fd
          << ", " << offset << ", " << count << ")" << dendl;
  
  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;
  
  _fsync(f, true);
  _release(in);
  
  client_lock.Unlock();
  return 0;
}




// =========================================
// low level

// ugly hack for ll
#define FUSE_SET_ATTR_MODE	(1 << 0)
#define FUSE_SET_ATTR_UID	(1 << 1)
#define FUSE_SET_ATTR_GID	(1 << 2)
#define FUSE_SET_ATTR_SIZE	(1 << 3)
#define FUSE_SET_ATTR_ATIME	(1 << 4)
#define FUSE_SET_ATTR_MTIME	(1 << 5)

int Client::ll_lookup(inodeno_t parent, const char *name, struct stat *attr, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_lookup " << parent << " " << name << dendl;
  tout << "ll_lookup" << std::endl;
  tout << parent.val << std::endl;
  tout << name << std::endl;

  string dname = name;
  Inode *diri = 0;
  Inode *in = 0;
  int r = 0;
  utime_t now = g_clock.now();

  if (inode_map.count(parent) == 0) {
    dout(1) << "ll_lookup " << parent << " " << name << " -> ENOENT (parent DNE... WTF)" << dendl;
    r = -ENOENT;
    attr->st_ino = 0;
    goto out;
  }
  diri = inode_map[parent];
  if (!diri->inode.is_dir()) {
    dout(1) << "ll_lookup " << parent << " " << name << " -> ENOTDIR (parent not a dir... WTF)" << dendl;
    r = -ENOTDIR;
    attr->st_ino = 0;
    goto out;
  }

  // get the inode
  if (diri->dir &&
      diri->dir->dentries.count(dname)) {
    Dentry *dn = diri->dir->dentries[dname];
    if (dn->lease_mds >= 0 && dn->lease_ttl > now) {
      touch_dn(dn);
      in = dn->inode;
      dout(1) << "ll_lookup " << parent << " " << name << " -> have valid lease on dentry" << dendl;
    }
  } 
  if (!in) {
    filepath path;
    diri->make_path(path);
    path.push_dentry(name);
    _do_lstat(path, 0, &in, uid, gid);
  }
  if (in) {
    fill_stat(in, attr);
    _ll_get(in);
  } else {
    r = -ENOENT;
    attr->st_ino = 0;
  }

 out:
  dout(3) << "ll_lookup " << parent << " " << name
	  << " -> " << r << " (" << hex << attr->st_ino << dec << ")" << dendl;
  tout << attr->st_ino << std::endl;
  return r;
}

void Client::_ll_get(Inode *in)
{
  if (in->ll_ref == 0) 
    in->get();
  in->ll_get();
  dout(20) << "_ll_get " << in << " " << in->inode.ino << " -> " << in->ll_ref << dendl;
}

int Client::_ll_put(Inode *in, int num)
{
  in->ll_put(num);
  dout(20) << "_ll_put " << in << " " << in->inode.ino << " " << num << " -> " << in->ll_ref << dendl;
  if (in->ll_ref == 0) {
    put_inode(in);
    return 0;
  } else {
    return in->ll_ref;
  }
}

void Client::_ll_drop_pins()
{
  dout(10) << "_ll_drop_pins" << dendl;
  hash_map<inodeno_t, Inode*>::iterator next;
  for (hash_map<inodeno_t, Inode*>::iterator it = inode_map.begin();
       it != inode_map.end();
       it = next) {
    Inode *in = it->second;
    next = it;
    next++;
    if (in->ll_ref)
      _ll_put(in, in->ll_ref);
  }
}

bool Client::ll_forget(inodeno_t ino, int num)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_forget " << ino << " " << num << dendl;
  tout << "ll_forget" << std::endl;
  tout << ino.val << std::endl;
  tout << num << std::endl;

  if (ino == 1) return true;  // ignore forget on root.

  bool last = false;
  if (inode_map.count(ino) == 0) {
    dout(1) << "WARNING: ll_forget on " << ino << " " << num 
	    << ", which I don't have" << dendl;
  } else {
    Inode *in = inode_map[ino];
    assert(in);
    if (in->ll_ref < num) {
      dout(1) << "WARNING: ll_forget on " << ino << " " << num << ", which only has ll_ref=" << in->ll_ref << dendl;
      _ll_put(in, in->ll_ref);
      last = true;
    } else {
      if (_ll_put(in, num) == 0)
	last = true;
    }
  }
  return last;
}

Inode *Client::_ll_get_inode(inodeno_t ino)
{
  if (inode_map.count(ino) == 0) {
    assert(ino == 1);  // must be the root inode.
    Inode *in;
    filepath path(1);
    int r = _do_lstat(path, 0, &in);
    assert(r >= 0);
    return in;
  } else {
    return inode_map[ino];
  }
}


int Client::ll_getattr(inodeno_t ino, struct stat *attr, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_getattr " << ino << dendl;
  tout << "ll_getattr" << std::endl;
  tout << ino.val << std::endl;

  Inode *in = _ll_get_inode(ino);
  filepath path(in->ino());
  int res = _do_lstat(path, CEPH_STAT_MASK_INODE_ALL, &in, uid, gid);
  if (res == 0)
    fill_stat(in, attr);
  dout(3) << "ll_getattr " << ino << " = " << res << dendl;
  return res;
}

int Client::ll_setattr(inodeno_t ino, struct stat *attr, int mask, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_setattr " << ino << " mask " << hex << mask << dec << dendl;
  tout << "ll_setattr" << std::endl;
  tout << ino.val << std::endl;
  tout << attr->st_mode << std::endl;
  tout << attr->st_uid << std::endl;
  tout << attr->st_gid << std::endl;
  tout << attr->st_size << std::endl;
  tout << attr->st_mtime << std::endl;
  tout << attr->st_atime << std::endl;
  tout << mask << std::endl;

  Inode *in = _ll_get_inode(ino);

  filepath path;
  in->make_path(path);

  int r = 0;
  if ((mask & FUSE_SET_ATTR_MODE) &&
      ((r = _chmod(path.c_str(), attr->st_mode, false, uid, gid)) < 0)) return r;

  if ((mask & FUSE_SET_ATTR_UID) && (mask & FUSE_SET_ATTR_GID) &&
      ((r = _chown(path.c_str(), attr->st_uid, attr->st_gid, false, uid, gid)) < 0)) return r;
  //if ((mask & FUSE_SET_ATTR_GID) &&
  //(r = client->_chgrp(path.c_str(), attr->st_gid) < 0)) return r;

  if ((mask & FUSE_SET_ATTR_SIZE) &&
      ((r = _truncate(path.c_str(), attr->st_size, false, uid, gid)) < 0)) return r;
  
  if ((mask & FUSE_SET_ATTR_MTIME) && (mask & FUSE_SET_ATTR_ATIME)) {
    if ((r = _utimes(path.c_str(), utime_t(attr->st_mtime,0), utime_t(attr->st_atime,0), false, uid, gid)) < 0) return r;
  } else if (mask & FUSE_SET_ATTR_MTIME) {
    if ((r = _utimes(path.c_str(), utime_t(attr->st_mtime,0), utime_t(), false, uid, gid)) < 0) return r;
  } else if (mask & FUSE_SET_ATTR_ATIME) {
    if ((r = _utimes(path.c_str(), utime_t(), utime_t(attr->st_atime,0), false, uid, gid)) < 0) return r;
  }
  
  assert(r == 0);
  fill_stat(in, attr);

  dout(3) << "ll_setattr " << ino << " = " << r << dendl;
  return 0;
}

// ----------
// xattrs

int Client::ll_getxattr(inodeno_t ino, const char *name, void *value, size_t size, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_getxattr " << ino << " " << name << " size " << size << dendl;
  tout << "ll_getxattr" << std::endl;
  tout << ino.val << std::endl;
  tout << name << std::endl;

  Inode *in = _ll_get_inode(ino);

  filepath path;
  in->make_path(path);
  return _getxattr(path, name, value, size, false, uid, gid);
}

int Client::_getxattr(const filepath &path, const char *name, void *value, size_t size,
		      bool followsym, int uid, int gid)
{
  Inode *in = 0;
  int r = _do_lstat(path, CEPH_STAT_MASK_XATTR, &in, uid, gid);
  if (r == 0) {
    string n(name);
    r = -ENODATA;
    if (in->xattrs.count(n)) {
      r = in->xattrs[n].length();
      if (size != 0) {
	if (size >= (unsigned)r)
	  memcpy(value, in->xattrs[n].c_str(), r);
	else
	  r = -ERANGE;
      }
    }
  }
  dout(3) << "_getxattr(\"" << path << "\", \"" << name << "\", " << size << ") = " << r << dendl;
  return r;
}

int Client::ll_listxattr(inodeno_t ino, char *names, size_t size, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_listxattr " << ino << " size " << size << dendl;
  tout << "ll_listxattr" << std::endl;
  tout << ino.val << std::endl;
  tout << size << std::endl;

  Inode *in = _ll_get_inode(ino);
  filepath path;
  in->make_path(path);
  return _listxattr(path, names, size, false, uid, gid);
}

int Client::_listxattr(const filepath &path, char *name, size_t size,
		       bool followsym, int uid, int gid)
{
  Inode *in = 0;
  int r = _do_lstat(path, CEPH_STAT_MASK_XATTR, &in, uid, gid);
  if (r == 0) {
    for (map<string,bufferptr>::iterator p = in->xattrs.begin();
	 p != in->xattrs.end();
	 p++)
      r += p->first.length() + 1;
    
    if (size != 0) {
      if (size >= (unsigned)r) {
	for (map<string,bufferptr>::iterator p = in->xattrs.begin();
	     p != in->xattrs.end();
	     p++) {
	  memcpy(name, p->first.c_str(), p->first.length());
	  name += p->first.length();
	  *name = '\0';
	  name++;
	}
      } else
	r = -ERANGE;
    }
  }
  dout(3) << "_listxattr(\"" << path << "\", " << size << ") = " << r << dendl;
  return r;
}

int Client::ll_setxattr(inodeno_t ino, const char *name, const void *value, size_t size, int flags, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_setxattr " << ino << " " << name << " size " << size << dendl;
  tout << "ll_setxattr" << std::endl;
  tout << ino.val << std::endl;
  tout << name << std::endl;

  // only user xattrs, for now
  if (strncmp(name, "user.", 5))
    return -EOPNOTSUPP;

  Inode *in = _ll_get_inode(ino);
  if (in->dn) touch_dn(in->dn);

  filepath path;
  in->make_path(path);
  return _setxattr(path, name, value, size, flags, false, uid, gid);
}

int Client::_setxattr(const filepath &path, const char *name, const void *value, size_t size, int flags,
		      bool followsym, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_LSETXATTR, messenger->get_myinst());
  req->set_filepath(path);
  req->set_path2(name);
  req->head.args.setxattr.flags = flags;

  bufferlist bl;
  bl.append((const char*)value, size);
  req->set_data(bl);
  
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;

  trim_cache();
  dout(3) << "_setxattr(\"" << path << "\", \"" << name << "\") = " << res << dendl;
  return res;
}

int Client::ll_removexattr(inodeno_t ino, const char *name, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_removexattr " << ino << " " << name << dendl;
  tout << "ll_removexattr" << std::endl;
  tout << ino.val << std::endl;
  tout << name << std::endl;

  // only user xattrs, for now
  if (strncmp(name, "user.", 5))
    return -EOPNOTSUPP;

  Inode *in = _ll_get_inode(ino);
  if (in->dn) touch_dn(in->dn);

  filepath path;
  in->make_path(path);
  return _removexattr(path, name, false, uid, gid);
}

int Client::_removexattr(const filepath &path, const char *name, 
			 bool followsym, int uid, int gid)
{
  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_LRMXATTR, messenger->get_myinst());
  req->set_filepath(path);
 
  MClientReply *reply = make_request(req, uid, gid);
  int res = reply->get_result();
  delete reply;

  trim_cache();
  dout(3) << "_removexattr(\"" << path << "\", \"" << name << "\") = " << res << dendl;
  return res;
}



int Client::ll_readlink(inodeno_t ino, const char **value, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_readlink " << ino << dendl;
  tout << "ll_readlink" << std::endl;
  tout << ino.val << std::endl;

  Inode *in = _ll_get_inode(ino);
  if (in->dn) touch_dn(in->dn);

  int r = 0;
  if (in->inode.is_symlink()) {
    *value = in->symlink->c_str();
  } else {
    *value = "";
    r = -EINVAL;
  }
  dout(3) << "ll_readlink " << ino << " = " << r << " (" << *value << ")" << dendl;
  return r;
}

int Client::ll_mknod(inodeno_t parent, const char *name, mode_t mode, dev_t rdev, struct stat *attr, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_mknod " << parent << " " << name << dendl;
  tout << "ll_mknod" << std::endl;
  tout << parent.val << std::endl;
  tout << name << std::endl;
  tout << mode << std::endl;
  tout << rdev << std::endl;

  Inode *diri = _ll_get_inode(parent);

  filepath path;
  diri->make_path(path);
  path.push_dentry(name);
  int r = _mknod(path.c_str(), mode, rdev, uid, gid);
  if (r == 0) {
    string dname(name);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << attr->st_ino << std::endl;
  dout(3) << "ll_mknod " << parent << " " << name
	  << " = " << r << " (" << hex << attr->st_ino << dec << ")" << dendl;
  return r;
}

int Client::ll_mkdir(inodeno_t parent, const char *name, mode_t mode, struct stat *attr, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_mkdir " << parent << " " << name << dendl;
  tout << "ll_mkdir" << std::endl;
  tout << parent.val << std::endl;
  tout << name << std::endl;
  tout << mode << std::endl;

  Inode *diri = _ll_get_inode(parent);

  filepath path;
  diri->make_path(path);
  path.push_dentry(name);
  int r = _mkdir(path.c_str(), mode, uid, gid);
  if (r == 0) {
    string dname(name);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << attr->st_ino << std::endl;
  dout(3) << "ll_mkdir " << parent << " " << name
	  << " = " << r << " (" << hex << attr->st_ino << dec << ")" << dendl;
  return r;
}

int Client::ll_symlink(inodeno_t parent, const char *name, const char *value, struct stat *attr, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_symlink " << parent << " " << name << " -> " << value << dendl;
  tout << "ll_symlink" << std::endl;
  tout << parent.val << std::endl;
  tout << name << std::endl;
  tout << value << std::endl;

  Inode *diri = _ll_get_inode(parent);

  filepath path;
  diri->make_path(path);
  path.push_dentry(name);
  int r = _symlink(path, value, uid, gid);
  if (r == 0) {
    string dname(name);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << attr->st_ino << std::endl;
  dout(3) << "ll_symlink " << parent << " " << name
	  << " = " << r << " (" << hex << attr->st_ino << dec << ")" << dendl;
  return r;
}

int Client::ll_unlink(inodeno_t ino, const char *name, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_unlink " << ino << " " << name << dendl;
  tout << "ll_unlink" << std::endl;
  tout << ino.val << std::endl;
  tout << name << std::endl;

  Inode *diri = _ll_get_inode(ino);

  filepath path;
  diri->make_path(path);
  path.push_dentry(name);
  return _unlink(path.c_str(), uid, gid);
}

int Client::ll_rmdir(inodeno_t ino, const char *name, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_rmdir " << ino << " " << name << dendl;
  tout << "ll_rmdir" << std::endl;
  tout << ino.val << std::endl;
  tout << name << std::endl;

  Inode *diri = _ll_get_inode(ino);

  filepath path;
  diri->make_path(path);
  path.push_dentry(name);
  return _rmdir(path.c_str(), uid, gid);
}

int Client::ll_rename(inodeno_t parent, const char *name, inodeno_t newparent, const char *newname, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_rename " << parent << " " << name << " to "
	  << newparent << " " << newname << dendl;
  tout << "ll_rename" << std::endl;
  tout << parent.val << std::endl;
  tout << name << std::endl;
  tout << newparent.val << std::endl;
  tout << newname << std::endl;

  Inode *diri = _ll_get_inode(parent);
  filepath path;
  diri->make_path(path);
  path.push_dentry(name);

  Inode *newdiri = _ll_get_inode(newparent);
  filepath newpath;
  newdiri->make_path(newpath);
  newpath.push_dentry(newname);

  return _rename(path.c_str(), newpath.c_str(), uid, gid);
}

int Client::ll_link(inodeno_t ino, inodeno_t newparent, const char *newname, struct stat *attr, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_link " << ino << " to " << newparent << " " << newname << dendl;
  tout << "ll_link" << std::endl;
  tout << ino.val << std::endl;
  tout << newparent << std::endl;
  tout << newname << std::endl;

  Inode *old = _ll_get_inode(ino);
  Inode *diri = _ll_get_inode(newparent);

  filepath path;
  old->make_path(path);

  filepath newpath;
  diri->make_path(newpath);
  newpath.push_dentry(newname);

  int r = _link(path.c_str(), newpath.c_str(), uid, gid);
  if (r == 0) {
    Inode *in = _ll_get_inode(ino);
    fill_stat(in, attr);
    _ll_get(in);
  }
  return r;
}

int Client::ll_opendir(inodeno_t ino, void **dirpp, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_opendir " << ino << dendl;
  tout << "ll_opendir" << std::endl;
  tout << ino.val << std::endl;
  
  Inode *diri = inode_map[ino];
  assert(diri);
  filepath path;
  diri->make_path(path);
  dout(10) << " ino path is " << path << dendl;

  int r = _opendir(path.c_str(), (DirResult**)dirpp);

  tout << (unsigned long)*dirpp << std::endl;

  dout(3) << "ll_opendir " << ino << " = " << r << " (" << *dirpp << ")" << dendl;
  return r;
}

void Client::ll_releasedir(void *dirp)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_releasedir " << dirp << dendl;
  tout << "ll_releasedir" << std::endl;
  tout << (unsigned long)dirp << std::endl;
  _closedir((DirResult*)dirp);
}

int Client::ll_open(inodeno_t ino, int flags, Fh **fhp, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_open " << ino << " " << flags << dendl;
  tout << "ll_open" << std::endl;
  tout << ino.val << std::endl;
  tout << flags << std::endl;

  Inode *in = _ll_get_inode(ino);
  filepath path;
  in->make_path(path);

  int r = _open(path.c_str(), flags, 0, fhp, uid, gid);

  tout << (unsigned long)*fhp << std::endl;
  dout(3) << "ll_open " << ino << " " << flags << " = " << r << " (" << *fhp << ")" << dendl;
  return r;
}

int Client::ll_create(inodeno_t parent, const char *name, mode_t mode, int flags, 
		      struct stat *attr, Fh **fhp, int uid, int gid)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_create " << parent << " " << name << " 0" << oct << mode << dec << " " << flags << dendl;
  tout << "ll_create" << std::endl;
  tout << parent.val << std::endl;
  tout << name << std::endl;
  tout << mode << std::endl;
  tout << flags << std::endl;

  Inode *pin = _ll_get_inode(parent);
  filepath path;
  pin->make_path(path);
  path.push_dentry(name);

  int r = _open(path.c_str(), flags|O_CREAT, mode, fhp, uid, gid);
  if (r >= 0) {
    Inode *in = (*fhp)->inode;
    fill_stat(in, attr);
    _ll_get(in);
  } else {
    attr->st_ino = 0;
  }
  tout << (unsigned long)*fhp << std::endl;
  tout << attr->st_ino << std::endl;
  dout(3) << "ll_create " << parent << " " << name << " 0" << oct << mode << dec << " " << flags
	  << " = " << r << " (" << *fhp << " " << hex << attr->st_ino << dec << ")" << dendl;
  return 0;
}

int Client::ll_read(Fh *fh, loff_t off, loff_t len, bufferlist *bl)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_read " << fh << " " << off << "~" << len << dendl;
  tout << "ll_read" << std::endl;
  tout << (unsigned long)fh << std::endl;
  tout << off << std::endl;
  tout << len << std::endl;

  return _read(fh, off, len, bl);
}

int Client::ll_write(Fh *fh, loff_t off, loff_t len, const char *data)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_write " << fh << " " << off << "~" << len << dendl;
  tout << "ll_write" << std::endl;
  tout << (unsigned long)fh << std::endl;
  tout << off << std::endl;
  tout << len << std::endl;

  int r = _write(fh, off, len, data);
  dout(3) << "ll_write " << fh << " " << off << "~" << len << " = " << r << dendl;
  return r;
}

int Client::ll_flush(Fh *fh)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_flush " << fh << dendl;
  tout << "ll_flush" << std::endl;
  tout << (unsigned long)fh << std::endl;

  return _flush(fh);
}

int Client::ll_fsync(Fh *fh, bool syncdataonly) 
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_fsync " << fh << dendl;
  tout << "ll_fsync" << std::endl;
  tout << (unsigned long)fh << std::endl;

  return _fsync(fh, syncdataonly);
}


int Client::ll_release(Fh *fh)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_release " << fh << dendl;
  tout << "ll_release" << std::endl;
  tout << (unsigned long)fh << std::endl;

  _release(fh);
  return 0;
}






// =========================================
// layout


int Client::describe_layout(int fd, ceph_file_layout *lp)
{
  Mutex::Locker lock(client_lock);

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  *lp = in->inode.layout;

  dout(3) << "describe_layout(" << fd << ") = 0" << dendl;
  return 0;
}

int Client::get_stripe_unit(int fd)
{
  ceph_file_layout layout;
  describe_layout(fd, &layout);
  return ceph_file_layout_su(layout);
}

int Client::get_stripe_width(int fd)
{
  ceph_file_layout layout;
  describe_layout(fd, &layout);
  return ceph_file_layout_stripe_width(layout);
}

int Client::get_stripe_period(int fd)
{
  ceph_file_layout layout;
  describe_layout(fd, &layout);
  return ceph_file_layout_period(layout);
}

int Client::enumerate_layout(int fd, list<ObjectExtent>& result,
			     loff_t length, loff_t offset)
{
  Mutex::Locker lock(client_lock);

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  // map to a list of extents
  filer->file_to_extents(in->inode.ino, &in->inode.layout, offset, length, result);

  dout(3) << "enumerate_layout(" << fd << ", " << length << ", " << offset << ") = 0" << dendl;
  return 0;
}


void Client::ms_handle_failure(Message *m, const entity_inst_t& inst)
{
  entity_name_t dest = inst.name;

  if (dest.is_mon()) {
    // resend to a different monitor.
    int mon = monmap->pick_mon(true);
    dout(0) << "ms_handle_failure " << *m << " to " << inst 
            << ", resending to mon" << mon 
            << dendl;
    messenger->send_message(m, monmap->get_inst(mon));
  }
  else if (dest.is_osd()) {
    objecter->ms_handle_failure(m, dest, inst);
  } 
  else {
    dout(0) << "ms_handle_failure " << *m << " to " << inst << ", dropping" << dendl;
    delete m;
  }
}

void Client::ms_handle_reset(const entity_addr_t& addr, entity_name_t last) 
{
  dout(0) << "ms_handle_reset on " << addr << dendl;
}


void Client::ms_handle_remote_reset(const entity_addr_t& addr, entity_name_t last) 
{
  dout(0) << "ms_handle_remote_reset on " << addr << ", last " << last << dendl;
  if (last.is_mds()) {
    int mds = last.num();
    dout(0) << "ms_handle_remote_reset on " << last << ", " << mds_sessions[mds].num_caps
	    << " caps, kicking requests" << dendl;

    mds_sessions.erase(mds); // "kill" session

    // reopen if caps
    if (mds_sessions[mds].num_caps > 0) {
      waiting_for_session[mds].size();  // make sure entry exists
      messenger->send_message(new MClientSession(CEPH_SESSION_REQUEST_OPEN),
			      mdsmap->get_inst(mds));
      
      /*
       * FIXME: actually, we need to do a reconnect or similar to reestablish
       * our caps (where possible)
       */
      dout(0) << "FIXME: client needs to reconnect to restablish existing caps ****" << dendl;
    }

    // or requests
    kick_requests(mds);
  }
}
