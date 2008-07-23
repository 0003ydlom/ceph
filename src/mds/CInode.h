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



#ifndef __CINODE_H
#define __CINODE_H

#include "config.h"
#include "include/types.h"
#include "include/lru.h"

#include "mdstypes.h"

#include "CDentry.h"
#include "SimpleLock.h"
#include "FileLock.h"
#include "ScatterLock.h"
#include "LocalLock.h"
#include "Capability.h"
#include "snap.h"

#include <cassert>
#include <list>
#include <vector>
#include <set>
#include <map>
#include <iostream>
using namespace std;

class Context;
class CDentry;
class CDir;
class Message;
class CInode;
class CInodeDiscover;
class MDCache;
class LogSegment;
class SnapRealm;

ostream& operator<<(ostream& out, CInode& in);


// cached inode wrapper
class CInode : public MDSCacheObject {
 public:
  // -- pins --
  static const int PIN_DIRFRAG =         -1; 
  static const int PIN_CAPS =             2;  // client caps
  static const int PIN_IMPORTING =       -4;  // importing
  static const int PIN_ANCHORING =        5;
  static const int PIN_UNANCHORING =      6;
  static const int PIN_OPENINGDIR =       7;
  static const int PIN_REMOTEPARENT =     8;
  static const int PIN_BATCHOPENJOURNAL = 9;
  static const int PIN_SCATTERED =        10;
  static const int PIN_STICKYDIRS =       11;
  static const int PIN_PURGING =         -12;	
  static const int PIN_FREEZING =         13;
  static const int PIN_FROZEN =           14;
  static const int PIN_IMPORTINGCAPS =    15;

  const char *pin_name(int p) {
    switch (p) {
    case PIN_DIRFRAG: return "dirfrag";
    case PIN_CAPS: return "caps";
    case PIN_IMPORTING: return "importing";
    case PIN_ANCHORING: return "anchoring";
    case PIN_UNANCHORING: return "unanchoring";
    case PIN_OPENINGDIR: return "openingdir";
    case PIN_REMOTEPARENT: return "remoteparent";
    case PIN_BATCHOPENJOURNAL: return "batchopenjournal";
    case PIN_SCATTERED: return "scattered";
    case PIN_STICKYDIRS: return "stickydirs";
    case PIN_PURGING: return "purging";
    case PIN_FREEZING: return "freezing";
    case PIN_FROZEN: return "frozen";
    case PIN_IMPORTINGCAPS: return "importingcaps";
    default: return generic_pin_name(p);
    }
  }

  // -- state --
  static const int STATE_EXPORTING =   (1<<2);   // on nonauth bystander.
  static const int STATE_ANCHORING =   (1<<3);
  static const int STATE_UNANCHORING = (1<<4);
  static const int STATE_OPENINGDIR =  (1<<5);
  static const int STATE_REJOINUNDEF = (1<<6);   // inode contents undefined.
  static const int STATE_FREEZING =    (1<<7);
  static const int STATE_FROZEN =      (1<<8);
  static const int STATE_AMBIGUOUSAUTH = (1<<9);
  static const int STATE_EXPORTINGCAPS = (1<<10);
  static const int STATE_NEEDSRECOVER = (1<<11);
  static const int STATE_RECOVERING = (1<<11);

  // -- waiters --
  static const int WAIT_DIR         = (1<<0);
  static const int WAIT_ANCHORED    = (1<<1);
  static const int WAIT_UNANCHORED  = (1<<2);
  static const int WAIT_FROZEN      = (1<<3);
  
  static const int WAIT_AUTHLOCK_OFFSET        = 4;
  static const int WAIT_LINKLOCK_OFFSET        = 4 +   SimpleLock::WAIT_BITS;
  static const int WAIT_DIRFRAGTREELOCK_OFFSET = 4 + 2*SimpleLock::WAIT_BITS;
  static const int WAIT_FILELOCK_OFFSET        = 4 + 3*SimpleLock::WAIT_BITS; // same
  static const int WAIT_DIRLOCK_OFFSET         = 4 + 3*SimpleLock::WAIT_BITS; // same
  static const int WAIT_VERSIONLOCK_OFFSET     = 4 + 4*SimpleLock::WAIT_BITS;
  static const int WAIT_XATTRLOCK_OFFSET       = 4 + 5*SimpleLock::WAIT_BITS;
  static const int WAIT_SNAPLOCK_OFFSET        = 4 + 6*SimpleLock::WAIT_BITS;
  static const int WAIT_NESTLOCK_OFFSET        = 4 + 7*SimpleLock::WAIT_BITS;

  static const int WAIT_ANY_MASK	= (0xffffffff);

  // misc
  static const int EXPORT_NONCE = 1; // nonce given to replicas created by export

  ostream& print_db_line_prefix(ostream& out);

 public:
  MDCache *mdcache;

  // inode contents proper
  inode_t          inode;        // the inode itself
  string           symlink;      // symlink dest, if symlink
  map<string, bufferptr> xattrs;
  fragtree_t       dirfragtree;  // dir frag tree, if any.  always consistent with our dirfrag map.
  SnapRealm        *snaprealm;

  SnapRealm        *containing_realm;
  snapid_t          first, last;
  map<snapid_t, old_inode_t> old_inodes;  // key = last, value.first = first
  set<snapid_t> dirty_old_rstats;

  bool is_multiversion() { return snaprealm || inode.is_dir(); }
  snapid_t get_oldest_snap();

  loff_t last_journaled;       // log offset for the last time i was journaled
  loff_t last_open_journaled;  // log offset for the last journaled EOpen

  //bool hack_accessed;
  //utime_t hack_load_stamp;

  // projected values (only defined while dirty)
  list<inode_t*>   projected_inode;

  version_t get_projected_version() {
    if (projected_inode.empty())
      return inode.version;
    else
      return projected_inode.back()->version;
  }
  bool is_projected() {
    return !projected_inode.empty();
  }

  inode_t *get_projected_inode() { 
    if (projected_inode.empty())
      return &inode;
    else
      return projected_inode.back();
  }
  inode_t *project_inode();
  void pop_and_dirty_projected_inode(LogSegment *ls);

  inode_t *get_previous_projected_inode() {
    assert(!projected_inode.empty());
    list<inode_t*>::reverse_iterator p = projected_inode.rbegin();
    p++;
    if (p != projected_inode.rend())
      return *p;
    else
      return &inode;
  }

  map<snapid_t,old_inode_t>::iterator pick_dirty_old_inode(snapid_t last);

  old_inode_t& cow_old_inode(snapid_t follows, inode_t *pi);

  // -- cache infrastructure --
private:
  map<frag_t,CDir*> dirfrags; // cached dir fragments
  int stickydir_ref;

public:
  frag_t pick_dirfrag(const nstring &dn);
  bool has_dirfrags() { return !dirfrags.empty(); }
  CDir* get_dirfrag(frag_t fg) {
    if (dirfrags.count(fg)) {
      assert(g_conf.debug_mds < 2 || dirfragtree.is_leaf(fg)); // performance hack FIXME
      return dirfrags[fg];
    } else
      return 0;
  }
  void get_dirfrags_under(frag_t fg, list<CDir*>& ls);
  CDir* get_approx_dirfrag(frag_t fg);
  void get_dirfrags(list<CDir*>& ls);
  void get_nested_dirfrags(list<CDir*>& ls);
  void get_subtree_dirfrags(list<CDir*>& ls);
  CDir *get_or_open_dirfrag(MDCache *mdcache, frag_t fg);
  CDir *add_dirfrag(CDir *dir);
  void close_dirfrag(frag_t fg);
  void close_dirfrags();
  bool has_subtree_root_dirfrag();

  void get_stickydirs();
  void put_stickydirs();  

 protected:
  // parent dentries in cache
  CDentry         *parent;             // primary link
  set<CDentry*>    remote_parents;     // if hard linked
  CDentry         *projected_parent;   // for in-progress rename

  pair<int,int> inode_auth;

  // -- distributed state --
protected:
  // file capabilities
  map<int, Capability*> client_caps;         // client -> caps
  map<int, int>         mds_caps_wanted;     // [auth] mds -> caps wanted
  int                   replica_caps_wanted; // [replica] what i've requested from auth
  utime_t               replica_caps_wanted_keep_until;


  // LogSegment xlists i (may) belong to
  xlist<CInode*>::item xlist_dirty;
public:
  xlist<CInode*>::item xlist_caps;
  xlist<CInode*>::item xlist_open_file;
  xlist<CInode*>::item xlist_dirty_dirfrag_dir;
  xlist<CInode*>::item xlist_dirty_dirfrag_nest;
  xlist<CInode*>::item xlist_dirty_dirfrag_dirfragtree;
  xlist<CInode*>::item xlist_purging_inode;

private:
  // auth pin
  int auth_pins;
  int nested_auth_pins;
public:
#ifdef MDS_AUTHPIN_SET
  multiset<void*> auth_pin_set;
#endif
  int auth_pin_freeze_allowance;

private:
  int nested_anchors;   // _NOT_ including me!

 public:
  inode_load_vec_t pop;

  // friends
  friend class Server;
  friend class Locker;
  friend class Migrator;
  friend class MDCache;
  friend class CDir;
  friend class CInodeExport;
  friend class CInodeDiscover;

 public:
  // ---------------------------
  CInode(MDCache *c, bool auth=true) : 
    mdcache(c),
    snaprealm(0), containing_realm(0),
    first(1), last(CEPH_NOSNAP),
    last_journaled(0), last_open_journaled(0), 
    //hack_accessed(true),
    stickydir_ref(0),
    parent(0), projected_parent(0),
    inode_auth(CDIR_AUTH_DEFAULT),
    replica_caps_wanted(0),
    xlist_dirty(this), xlist_caps(this), xlist_open_file(this), 
    xlist_dirty_dirfrag_dir(this), 
    xlist_dirty_dirfrag_nest(this), 
    xlist_dirty_dirfrag_dirfragtree(this), 
    xlist_purging_inode(this),
    auth_pins(0), nested_auth_pins(0),
    nested_anchors(0),
    versionlock(this, CEPH_LOCK_IVERSION, WAIT_VERSIONLOCK_OFFSET),
    authlock(this, CEPH_LOCK_IAUTH, WAIT_AUTHLOCK_OFFSET),
    linklock(this, CEPH_LOCK_ILINK, WAIT_LINKLOCK_OFFSET),
    dirfragtreelock(this, CEPH_LOCK_IDFT, WAIT_DIRFRAGTREELOCK_OFFSET),
    filelock(this, CEPH_LOCK_IFILE, WAIT_FILELOCK_OFFSET),
    dirlock(this, CEPH_LOCK_IDIR, WAIT_DIRLOCK_OFFSET),
    xattrlock(this, CEPH_LOCK_IXATTR, WAIT_XATTRLOCK_OFFSET),
    snaplock(this, CEPH_LOCK_ISNAP, WAIT_SNAPLOCK_OFFSET),
    nestlock(this, CEPH_LOCK_INEST, WAIT_NESTLOCK_OFFSET)
  {
    memset(&inode, 0, sizeof(inode));
    state = 0;  
    if (auth) state_set(STATE_AUTH);
  };
  ~CInode() {
    close_dirfrags();
    close_snaprealm();
  }
  

  // -- accessors --
  bool is_file()    { return inode.is_file(); }
  bool is_symlink() { return inode.is_symlink(); }
  bool is_dir()     { return inode.is_dir(); }

  bool is_anchored() { return inode.anchored; }
  bool is_anchoring() { return state_test(STATE_ANCHORING); }
  bool is_unanchoring() { return state_test(STATE_UNANCHORING); }
  
  bool is_root() { return inode.ino == MDS_INO_ROOT; }
  bool is_stray() { return MDS_INO_IS_STRAY(inode.ino); }
  bool is_base() { return inode.ino < MDS_INO_BASE; }

  // note: this overloads MDSCacheObject
  bool is_ambiguous_auth() {
    return state_test(STATE_AMBIGUOUSAUTH) ||
      MDSCacheObject::is_ambiguous_auth();
  }


  inodeno_t ino() const { return inode.ino; }
  vinodeno_t vino() const { return vinodeno_t(inode.ino, last); }
  inode_t& get_inode() { return inode; }
  CDentry* get_parent_dn() { return parent; }
  CDentry* get_projected_parent_dn() { return projected_parent ? projected_parent:parent; }
  CDir *get_parent_dir();
  CInode *get_parent_inode();
  
  bool is_lt(const MDSCacheObject *r) const {
    return ino() < ((CInode*)r)->ino();
  }

  int64_t get_layout_size_increment() {
    return ceph_file_layout_period(inode.layout);
  }

  // -- misc -- 
  bool is_ancestor_of(CInode *other);
  void make_path_string(string& s);
  void make_path(filepath& s);
  void make_anchor_trace(vector<class Anchor>& trace);
  void name_stray_dentry(string& dname);


  
  // -- dirtyness --
  version_t get_version() { return inode.version; }

  version_t pre_dirty();
  void _mark_dirty(LogSegment *ls);
  void mark_dirty(version_t projected_dirv, LogSegment *ls);
  void mark_clean();


  CInodeDiscover* replicate_to(int rep);


  // -- waiting --
  void add_waiter(int tag, Context *c);


  // -- import/export --
  void encode_export(bufferlist& bl);
  void finish_export(utime_t now);
  void abort_export() {
    put(PIN_TEMPEXPORTING);
  }
  void decode_import(bufferlist::iterator& p, LogSegment *ls);
  

  // for giving to clients
  void encode_inodestat(bufferlist& bl, snapid_t snapid=CEPH_NOSNAP);


  // -- locks --
public:
  LocalLock  versionlock;
  SimpleLock authlock;
  SimpleLock linklock;
  ScatterLock dirfragtreelock;
  FileLock   filelock;
  ScatterLock dirlock;
  SimpleLock xattrlock;
  SimpleLock snaplock;
  ScatterLock nestlock;

  SimpleLock* get_lock(int type) {
    switch (type) {
    case CEPH_LOCK_IFILE: return &filelock;
    case CEPH_LOCK_IAUTH: return &authlock;
    case CEPH_LOCK_ILINK: return &linklock;
    case CEPH_LOCK_IDFT: return &dirfragtreelock;
    case CEPH_LOCK_IDIR: return &dirlock;
    case CEPH_LOCK_IXATTR: return &xattrlock;
    case CEPH_LOCK_ISNAP: return &snaplock;
    case CEPH_LOCK_INEST: return &nestlock;
    }
    return 0;
  }

  void set_object_info(MDSCacheObjectInfo &info);
  void encode_lock_state(int type, bufferlist& bl);
  void decode_lock_state(int type, bufferlist& bl);

  void clear_dirty_scattered(int type);
  void finish_scatter_gather_update(int type);


  // -- snap --
  void open_snaprealm();
  void close_snaprealm();
  SnapRealm *find_snaprealm();
  void encode_snap_blob(bufferlist &bl);
  void decode_snap_blob(bufferlist &bl);
  void encode_snap(bufferlist& bl) {
    bufferlist snapbl;
    encode_snap_blob(snapbl);
    ::encode(snapbl, bl);
  }    
  void decode_snap(bufferlist::iterator& p) {
    bufferlist snapbl;
    ::decode(snapbl, p);
    decode_snap_blob(snapbl);
  }

  // -- caps -- (new)
  // client caps
  int count_nonstale_caps() {
    int n = 0;
    for (map<int,Capability*>::iterator it = client_caps.begin();
         it != client_caps.end();
         it++) 
      if (!it->second->is_stale()) {
	if (n) return false;
	n++;
      }
    return n;
  }

  bool is_any_caps() { return !client_caps.empty(); }
  bool is_any_nonstale_caps() { return count_nonstale_caps(); }
  bool is_loner_cap() {
    if (!mds_caps_wanted.empty())
      return false;
    return count_nonstale_caps() == 1;
  }

  map<int,Capability*>& get_client_caps() { return client_caps; }
  Capability *get_client_cap(int client) {
    if (client_caps.count(client))
      return client_caps[client];
    return 0;
  }
  int get_client_cap_pending(int client) {
    Capability *c = get_client_cap(client);
    if (c) return c->pending();
    return 0;
  }
  Capability *add_client_cap(int client, SnapRealm *conrealm=0) {
    if (client_caps.empty()) {
      get(PIN_CAPS);
      if (conrealm)
	containing_realm = conrealm;
      else
	containing_realm = find_snaprealm();
      containing_realm->inodes_with_caps.push_back(&xlist_caps);
    }

    assert(client_caps.count(client) == 0);
    Capability *cap = client_caps[client] = new Capability;
    cap->set_inode(this);
   
    containing_realm->add_cap(client, cap);
    
    return cap;
  }
  void remove_client_cap(int client) {
    assert(client_caps.count(client) == 1);

    containing_realm->remove_cap(client, client_caps[client]);

    delete client_caps[client];
    client_caps.erase(client);
    if (client_caps.empty()) {
      put(PIN_CAPS);
      xlist_caps.remove_myself();
      containing_realm = NULL;
      xlist_open_file.remove_myself();  // unpin logsegment
    }
  }
  void move_to_containing_realm(SnapRealm *realm) {
    for (map<int,Capability*>::iterator q = client_caps.begin();
	 q != client_caps.end();
	 q++) {
      containing_realm->remove_cap(q->first, q->second);
      realm->add_cap(q->first, q->second);
    }
    xlist_caps.remove_myself();
    realm->inodes_with_caps.push_back(&xlist_caps);
    containing_realm = realm;
  }

  Capability *reconnect_cap(int client, inode_caps_reconnect_t& icr) {
    Capability *cap = get_client_cap(client);
    if (cap) {
      cap->merge(icr.wanted, icr.issued);
    } else {
      cap = add_client_cap(client);
      cap->set_wanted(icr.wanted);
      cap->issue(icr.issued);
    }
    inode.size = MAX(inode.size, icr.size);
    inode.mtime = MAX(inode.mtime, icr.mtime);
    inode.atime = MAX(inode.atime, icr.atime);
    return cap;
  }
  void clear_client_caps() {
    while (!client_caps.empty())
      remove_client_cap(client_caps.begin()->first);
  }
  void export_client_caps(map<int,Capability::Export>& cl) {
    for (map<int,Capability*>::iterator it = client_caps.begin();
         it != client_caps.end();
         it++) {
      cl[it->first] = it->second->make_export();
    }
  }

  // caps issued, wanted
  int get_caps_issued() {
    int c = 0;
    for (map<int,Capability*>::iterator it = client_caps.begin();
         it != client_caps.end();
         it++) 
      c |= it->second->issued();
    return c;
  }
  int get_caps_wanted() {
    int w = 0;
    for (map<int,Capability*>::iterator it = client_caps.begin();
         it != client_caps.end();
         it++) {
      if (!it->second->is_stale())
	w |= it->second->wanted();
      //cout << " get_caps_wanted client " << it->first << " " << cap_string(it->second.wanted()) << endl;
    }
    if (is_auth())
      for (map<int,int>::iterator it = mds_caps_wanted.begin();
           it != mds_caps_wanted.end();
           it++) {
        w |= it->second;
        //cout << " get_caps_wanted mds " << it->first << " " << cap_string(it->second) << endl;
      }
    return w;
  }

  void replicate_relax_locks() {
    //dout(10) << " relaxing locks on " << *this << dendl;
    assert(is_auth());
    assert(!is_replicated());

    authlock.replicate_relax();
    linklock.replicate_relax();
    dirfragtreelock.replicate_relax();

    if ((get_caps_issued() & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) == 0) 
      filelock.replicate_relax();

    dirlock.replicate_relax();
    xattrlock.replicate_relax();
    snaplock.replicate_relax();
    nestlock.replicate_relax();
  }


  // -- authority --
  pair<int,int> authority();


  // -- auth pins --
  int is_auth_pinned() { return auth_pins; }
  int get_num_auth_pins() { return auth_pins; }
  void adjust_nested_auth_pins(int a);
  bool can_auth_pin();
  void auth_pin(void *by);
  void auth_unpin(void *by);

  void adjust_nested_anchors(int by);
  int get_nested_anchors() { return nested_anchors; }

  // -- freeze --
  bool is_freezing_inode() { return state_test(STATE_FREEZING); }
  bool is_frozen_inode() { return state_test(STATE_FROZEN); }
  bool is_frozen();
  bool is_frozen_dir();
  bool is_freezing();

  bool freeze_inode(int auth_pin_allowance=0);
  void unfreeze_inode(list<Context*>& finished);


  // -- reference counting --
  void bad_put(int by) {
    generic_dout(0) << " bad put " << *this << " by " << by << " " << pin_name(by) << " was " << ref
#ifdef MDS_REF_SET
		    << " (" << ref_set << ")"
#endif
		    << dendl;
#ifdef MDS_REF_SET
    assert(ref_set.count(by) == 1);
#endif
    assert(ref > 0);
  }
  void bad_get(int by) {
    generic_dout(0) << " bad get " << *this << " by " << by << " " << pin_name(by) << " was " << ref
#ifdef MDS_REF_SET
		    << " (" << ref_set << ")"
#endif
		    << dendl;
#ifdef MDS_REF_SET
    assert(ref_set.count(by) == 0);
#endif
  }
  void first_get();
  void last_put();


  // -- hierarchy stuff --
public:
  void set_primary_parent(CDentry *p) {
    assert(parent == 0);
    parent = p;
    if (projected_parent) {
      assert(projected_parent == p);
      projected_parent = 0;
    }
  }
  void remove_primary_parent(CDentry *dn) {
    assert(dn == parent);
    parent = 0;
  }
  void add_remote_parent(CDentry *p);
  void remove_remote_parent(CDentry *p);
  int num_remote_parents() {
    return remote_parents.size(); 
  }


  /*
  // for giving to clients
  void get_dist_spec(set<int>& ls, int auth, timepair_t& now) {
    if (( is_dir() && popularity[MDS_POP_CURDOM].get(now) > g_conf.mds_bal_replicate_threshold) ||
        (!is_dir() && popularity[MDS_POP_JUSTME].get(now) > g_conf.mds_bal_replicate_threshold)) {
      //if (!cached_by.empty() && inode.ino > 1) dout(1) << "distributed spec for " << *this << dendl;
      ls = cached_by;
    }
  }
  */

  void print(ostream& out);

};




// -- encoded state

// discover

class CInodeDiscover {
  
  inode_t    inode;
  string     symlink;
  fragtree_t dirfragtree;
  map<string, bufferptr> xattrs;
  __s32        replica_nonce;
  
  __u32      authlock_state;
  __u32      linklock_state;
  __u32      dirfragtreelock_state;
  __u32      filelock_state;
  __u32      dirlock_state;
  __u32      xattrlock_state;
  __u32      snaplock_state;
  __u32      nestlock_state;

 public:
  CInodeDiscover() {}
  CInodeDiscover(CInode *in, int nonce) {
    inode = in->inode;
    symlink = in->symlink;
    dirfragtree = in->dirfragtree;
    xattrs = in->xattrs;

    replica_nonce = nonce;

    authlock_state = in->authlock.get_replica_state();
    linklock_state = in->linklock.get_replica_state();
    dirfragtreelock_state = in->dirfragtreelock.get_replica_state();
    filelock_state = in->filelock.get_replica_state();
    dirlock_state = in->dirlock.get_replica_state();
    xattrlock_state = in->xattrlock.get_replica_state();
    snaplock_state = in->snaplock.get_replica_state();
    nestlock_state = in->nestlock.get_replica_state();
  }
  CInodeDiscover(bufferlist::iterator &p) {
    decode(p);
  }

  inodeno_t get_ino() { return inode.ino; }
  int get_replica_nonce() { return replica_nonce; }

  void update_inode(CInode *in) {
    if (in->parent && inode.anchored != in->inode.anchored)
      in->parent->adjust_nested_anchors((int)inode.anchored - (int)in->inode.anchored);
    in->inode = inode;
    in->symlink = symlink;
    in->dirfragtree = dirfragtree;
    in->xattrs = xattrs;
    in->replica_nonce = replica_nonce;
  }
  void init_inode_locks(CInode *in) {
    in->authlock.set_state(authlock_state);
    in->linklock.set_state(linklock_state);
    in->dirfragtreelock.set_state(dirfragtreelock_state);
    in->filelock.set_state(filelock_state);
    in->dirlock.set_state(dirlock_state);
    in->xattrlock.set_state(xattrlock_state);
    in->snaplock.set_state(snaplock_state);
    in->nestlock.set_state(nestlock_state);
  }
  
  void encode(bufferlist &bl) const {
    ::encode(inode, bl);
    ::encode(symlink, bl);
    ::encode(dirfragtree, bl);
    ::encode(xattrs, bl);
    ::encode(replica_nonce, bl);
    ::encode(authlock_state, bl);
    ::encode(linklock_state, bl);
    ::encode(dirfragtreelock_state, bl);
    ::encode(filelock_state, bl);
    ::encode(dirlock_state, bl);
    ::encode(xattrlock_state, bl);
    ::encode(snaplock_state, bl);
    ::encode(nestlock_state, bl);
  }

  void decode(bufferlist::iterator &p) {
    ::decode(inode, p);
    ::decode(symlink, p);
    ::decode(dirfragtree, p);
    ::decode(xattrs, p);
    ::decode(replica_nonce, p);
    ::decode(authlock_state, p);
    ::decode(linklock_state, p);
    ::decode(dirfragtreelock_state, p);
    ::decode(filelock_state, p);
    ::decode(dirlock_state, p);
    ::decode(xattrlock_state, p);
    ::decode(snaplock_state, p);
    ::decode(nestlock_state, p);
  }  

};
WRITE_CLASS_ENCODER(CInodeDiscover)


#endif
