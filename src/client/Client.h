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


#ifndef __CLIENT_H
#define __CLIENT_H


#include "mds/MDSMap.h"
#include "osd/OSDMap.h"
#include "mon/MonMap.h"

#include "msg/Message.h"
#include "msg/Dispatcher.h"
#include "msg/Messenger.h"

#include "messages/MClientReply.h"

#include "include/types.h"
#include "include/lru.h"
#include "include/filepath.h"
#include "include/interval_set.h"

#include "common/Mutex.h"
#include "common/Timer.h"

//#include "FileCache.h"


// stl
#include <set>
#include <map>
#include <fstream>
using std::set;
using std::map;
using std::fstream;

#include <ext/hash_map>
using namespace __gnu_cxx;



class MStatfsReply;
class MClientSession;
class MClientRequest;
class MClientRequestForward;
class MClientLease;
class MMonMap;

class Filer;
class Objecter;
class ObjectCacher;

extern class LogType client_logtype;
extern class Logger  *client_logger;



// ============================================
// types for my local metadata cache
/* basic structure:
   
 - Dentries live in an LRU loop.  they get expired based on last access.
      see include/lru.h.  items can be bumped to "mid" or "top" of list, etc.
 - Inode has ref count for each Fh, Dir, or Dentry that points to it.
 - when Inode ref goes to 0, it's expired.
 - when Dir is empty, it's removed (and it's Inode ref--)
 
*/

class Dir;
class Inode;

class Dentry : public LRUObject {
 public:
  string  name;                      // sort of lame
  //const char *name;
  Dir     *dir;
  Inode   *inode;
  int     ref;                       // 1 if there's a dir beneath me.
  int lease_mds;
  utime_t lease_ttl;
  
  void get() { 
    assert(ref == 0); ref++; lru_pin(); 
    //cout << "dentry.get on " << this << " " << name << " now " << ref << std::endl;
  }
  void put() { 
    assert(ref == 1); ref--; lru_unpin(); 
    //cout << "dentry.put on " << this << " " << name << " now " << ref << std::endl;
  }
  
  Dentry() : dir(0), inode(0), ref(0), lease_mds(-1) { }

  /*Dentry() : name(0), dir(0), inode(0), ref(0) { }
  Dentry(string& n) : name(0), dir(0), inode(0), ref(0) { 
    name = new char[n.length()+1];
    strcpy((char*)name, n.c_str());
  }
  ~Dentry() {
    delete[] name;
    }*/
};

class Dir {
 public:
  Inode    *parent_inode;  // my inode
  //hash_map<const char*, Dentry*, hash<const char*>, eqstr> dentries;
  hash_map<string, Dentry*> dentries;

  Dir(Inode* in) { parent_inode = in; }

  bool is_empty() {  return dentries.empty(); }
};


class InodeCap {
 public:
  unsigned issued;
  unsigned implemented;
  unsigned seq;

  int issued_exporting;        // if export comes first
  set<int> importing_from;    // if import comes first

  InodeCap() : issued(0), implemented(0), seq(0),
	       issued_exporting(0) {}
  
  bool can_drop() {
    return issued == 0 &&
      issued_exporting == 0 &&
      importing_from.empty();
  }
};


class Inode {
 public:
  inode_t   inode;    // the actual inode
  int       lease_mask, lease_mds;
  utime_t   lease_ttl;

  // about the dir (if this is one!)
  int       dir_auth;
  set<int>  dir_contacts;
  bool      dir_hashed, dir_replicated;

  // per-mds caps
  map<int,InodeCap> caps;            // mds -> InodeCap

  //int open_by_mode[CEPH_FILE_MODE_NUM];
  map<int,int> open_by_mode;
  map<int,int> cap_refs;

  __u64     reported_size, wanted_max_size, requested_max_size;

  int       ref;      // ref count. 1 for each dentry, fh that links to me.
  int       ll_ref;   // separate ref count for ll client
  Dir       *dir;     // if i'm a dir.
  Dentry    *dn;      // if i'm linked to a dentry.
  string    *symlink; // symlink content, if it's a symlink
  fragtree_t dirfragtree;
  map<string,bufferptr> xattrs;
  map<frag_t,int> fragmap;  // known frag -> mds mappings

  list<Cond*>       waitfor_caps;
  list<Cond*>       waitfor_commit;

  // <hack>
  bool hack_balance_reads;
  // </hack>

  void make_path(filepath& p) {
    if (dn) {
      assert(dn->dir && dn->dir->parent_inode);
      dn->dir->parent_inode->make_path(p);
      p.push_dentry(dn->name);
    } else
      p = filepath(inode.ino);
  }

  void get() { 
    ref++; 
    //cout << "inode.get on " << this << " " << hex << inode.ino << dec << " now " << ref << std::endl;
  }
  void put(int n=1) { 
    ref -= n; assert(ref >= 0); 
    //cout << "inode.put on " << this << " " << hex << inode.ino << dec << " now " << ref << std::endl;
  }

  void ll_get() {
    ll_ref++;
  }
  void ll_put(int n=1) {
    assert(ll_ref >= n);
    ll_ref -= n;
  }

  Inode(inodeno_t ino, ceph_file_layout *layout) : 
    //inode(_inode),
    lease_mask(0), lease_mds(-1),
    dir_auth(-1), dir_hashed(false), dir_replicated(false), 
    reported_size(0), wanted_max_size(0), requested_max_size(0),
    ref(0), ll_ref(0), 
    dir(0), dn(0), symlink(0),
    hack_balance_reads(false)
  {
    memset(&inode, 0, sizeof(inode));
    //memset(open_by_mode, 0, sizeof(int)*CEPH_FILE_MODE_NUM);
    inode.ino = ino;
  }
  ~Inode() {
    if (symlink) { delete symlink; symlink = 0; }
  }

  inodeno_t ino() { return inode.ino; }

  bool is_dir() { return inode.is_dir(); }


  // CAPS --------
  void get_open_ref(int mode) {
    open_by_mode[mode]++;
  }
  bool put_open_ref(int mode) {
    //cout << "open_by_mode[" << mode << "] " << open_by_mode[mode] << " -> " << (open_by_mode[mode]-1) << std::endl;
    if (--open_by_mode[mode] == 0)
      return true;
    return false;
  }

  void get_cap_ref(int cap);
  bool put_cap_ref(int cap);

  int caps_issued() {
    int c = 0;
    for (map<int,InodeCap>::iterator it = caps.begin();
         it != caps.end();
         it++)
      c |= it->second.issued | it->second.issued_exporting;
    return c;
  }

  int caps_used() {
    int w = 0;
    for (map<int,int>::iterator p = cap_refs.begin();
	 p != cap_refs.end();
	 p++)
      if (p->second)
	w |= p->first;
    return w;
  }
  int caps_file_wanted() {
    int want = 0;
    for (map<int,int>::iterator p = open_by_mode.begin();
	 p != open_by_mode.end();
	 p++)
      if (p->second)
	want |= ceph_caps_for_mode(p->first);
    return want;
  }
  int caps_wanted() {
    int want = caps_file_wanted() | caps_used();
    if (want & CEPH_CAP_WRBUFFER)
      want |= CEPH_CAP_EXCL;
    return want;
  }

  int get_effective_lease_mask(utime_t now) {
    int havemask = 0;
    if (now < lease_ttl && lease_mds >= 0)
      havemask |= lease_mask;
    if (caps_issued() & CEPH_CAP_EXCL) 
      havemask |= CEPH_LOCK_ICONTENT;
    if (havemask & CEPH_LOCK_ICONTENT)
      havemask |= CEPH_LOCK_ICONTENT;   // hack: if we have one, we have both, for the purposes of below
    return havemask;
  }

  bool have_valid_size() {
    // RD+RDCACHE or WR+WRBUFFER => valid size
    if ((caps_issued() & (CEPH_CAP_RD|CEPH_CAP_RDCACHE)) == (CEPH_CAP_RD|CEPH_CAP_RDCACHE))
      return true;
    if ((caps_issued() & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) == (CEPH_CAP_WR|CEPH_CAP_WRBUFFER))
      return true;
    // otherwise, look for lease or EXCL...
    if (get_effective_lease_mask(g_clock.now()) & CEPH_LOCK_ICONTENT)
      return true;
    return false;
  }

 
  int authority(const string& dname) {
    if (!dirfragtree.empty()) {
      __gnu_cxx::hash<string> H;
      frag_t fg = dirfragtree[H(dname)];
      while (fg != frag_t()) {
	if (fragmap.count(fg) &&
	    fragmap[fg] >= 0) {
	  //cout << "picked frag ino " << inode.ino << " dname " << dname << " fg " << fg << " mds" << fragmap[fg] << std::endl;
	  return fragmap[fg];
	}
	fg = frag_t(fg.value(), fg.bits()-1); // try more general...
      }
    }
    return authority();
  }

  int authority() {
    if (dir_auth >= 0)
      return dir_auth;

    assert(dn);
    return dn->dir->parent_inode->authority(dn->name);
  }


  int pick_replica(MDSMap *mdsmap) {
    // replicas?
    /* fixme
    if (//ino() > 1ULL && 
	dir_contacts.size()) {
      set<int>::iterator it = dir_contacts.begin();
      if (dir_contacts.size() == 1)
        return *it;
      else {
	//cout << "dir_contacts on " << inode.ino << " is " << dir_contacts << std::endl;
	int r = 1 + (rand() % dir_contacts.size());
	int a = authority();
	while (r--) {
	  it++;
	  if (mdsmap->is_down(*it)) it++;
	  if (it == dir_contacts.end()) it = dir_contacts.begin();
	  if (*it == a) it++;  // skip the authority
	  if (it == dir_contacts.end()) it = dir_contacts.begin();
	}
	return *it;
      }
    }
    */

    if (dir_replicated) {// || ino() == 1) {
      // pick a random mds that isn't the auth
      set<int> s;
      mdsmap->get_in_mds_set(s);
      set<int>::iterator it = s.begin();
      if (s.empty())
	return 0;
      if (s.size() == 1)
        return *it;
      else {
	//cout << "dir_contacts on " << inode.ino << " is " << dir_contacts << std::endl;
	int r = 1 + (rand() % s.size());
	int a = authority();
	while (r--) {
	  it++;
	  if (mdsmap->is_down(*it)) it++;
	  if (it == s.end()) it = s.begin();
	  if (*it == a) it++;  // skip the authority
	  if (it == s.end()) it = s.begin();
	}
	//if (inode.ino == 1) cout << "chose " << *it << " from " << s << std::endl;
	return *it;
      }
      //cout << "num_mds is " << mdcluster->get_num_mds() << endl;
      //return mdsmap->get_random_in_mds();
      //return rand() % mdsmap->get_num_mds();  // huh.. pick a random mds!
    }
    else
      return authority();
  }


  // open Dir for an inode.  if it's not open, allocated it (and pin dentry in memory).
  Dir *open_dir() {
    if (!dir) {
      if (dn) dn->get();      // pin dentry
      get();                  // pin inode
      dir = new Dir(this);
    }
    return dir;
  }

};




// file handle for any open file state

struct Fh {
  Inode    *inode;
  off_t     pos;
  int       mds;        // have to talk to mds we opened with (for now)
  int       mode;       // the mode i opened the file with

  bool is_lazy() { return mode & O_LAZY; }

  bool append;
  bool pos_locked;           // pos is currently in use
  list<Cond*> pos_waiters;   // waiters for pos

  Fh() : inode(0), pos(0), mds(0), mode(0), append(false), pos_locked(false) {}
};





// ========================================================
// client interface

class Client : public Dispatcher {
 public:
  
  /* getdir result */
  struct DirEntry {
    string d_name;
    struct stat st;
    int stmask;
    DirEntry(const string &s) : d_name(s), stmask(0) {}
    DirEntry(const string &n, struct stat& s, int stm) : d_name(n), st(s), stmask(stm) {}
  };

  struct DirResult {
    static const int SHIFT = 28;
    static const int64_t MASK = (1 << SHIFT) - 1;
    static const off_t END = 1ULL << (SHIFT + 32);

    filepath path;
    Inode *inode;
    int64_t offset;   // high bits: frag_t, low bits: an offset
    map<frag_t, vector<DirEntry> > buffer;

    DirResult(const filepath &fp, Inode *in=0) : path(fp), inode(in), offset(0) { 
      if (inode) inode->get();
    }

    frag_t frag() { return frag_t(offset >> SHIFT); }
    unsigned fragpos() { return offset & MASK; }

    void next_frag() {
      frag_t fg = offset >> SHIFT;
      if (fg.is_rightmost())
	set_end();
      else 
	set_frag(fg.next());
    }
    void set_frag(frag_t f) {
      offset = (uint64_t)f << SHIFT;
      assert(sizeof(offset) == 8);
    }
    void set_end() { offset = END; }
    bool at_end() { return (offset == END); }
  };


  // cluster descriptors
  MDSMap *mdsmap; 
  OSDMap *osdmap;

  SafeTimer timer;

  Context *tick_event;
  utime_t last_cap_renew;
  void renew_caps();
public:
  void tick();

 protected:
  Messenger *messenger;  
  int whoami;
  MonMap *monmap;
  
  // mds sessions
  struct MDSSession {
    version_t seq;
    __u64 cap_gen;
    utime_t cap_ttl, last_cap_renew_request;
    int num_caps;
    MDSSession() : seq(0), cap_gen(0), num_caps(0) {}
  };
  map<int, MDSSession> mds_sessions;  // mds -> push seq
  map<int, list<Cond*> > waiting_for_session;
  list<Cond*> waiting_for_mdsmap;

  void handle_client_session(MClientSession *m);
  void send_reconnect(int mds);

  // mds requests
  struct MetaRequest {
    tid_t tid;
    MClientRequest *request;    
    bufferlist request_payload;  // in case i have to retry
    
    int uid, gid;

    utime_t  sent_stamp;
    bool     idempotent;         // is request idempotent?
    set<int> mds;                // who i am asking
    int      resend_mds;         // someone wants you to (re)send the request here
    int      num_fwd;            // # of times i've been forwarded
    int      retry_attempt;

    MClientReply *reply;         // the reply

    Cond  *caller_cond;          // who to take up
    Cond  *dispatch_cond;        // who to kick back

    MetaRequest(MClientRequest *req, tid_t t) : 
      tid(t), request(req), 
      idempotent(false), resend_mds(-1), num_fwd(0), retry_attempt(0),
      reply(0), 
      caller_cond(0), dispatch_cond(0) { }
  };
  tid_t last_tid;
  map<tid_t, MetaRequest*> mds_requests;
  set<int>                 failed_mds;

  struct StatfsRequest {
    tid_t tid;
    MStatfsReply *reply;
    Cond *caller_cond;
    StatfsRequest(tid_t t, Cond *cc) : tid(t), reply(0), caller_cond(cc) {}
  };
  map<tid_t,StatfsRequest*> statfs_requests;
  
  MClientReply *make_request(MClientRequest *req, int uid, int gid,
			     Inode **ppin=0, utime_t *pfrom=0, 
			     int use_mds=-1);
  int choose_target_mds(MClientRequest *req);
  void send_request(MetaRequest *request, int mds);
  void kick_requests(int mds);
  void handle_client_request_forward(MClientRequestForward *reply);
  void handle_client_reply(MClientReply *reply);
  void handle_statfs_reply(MStatfsReply *reply);

  bool   mounted;
  int mounters;
  bool   unmounting;
  Cond   mount_cond;  

  int    unsafe_sync_write;
public:
  entity_name_t get_myname() { return messenger->get_myname(); } 
  void sync_write_commit(Inode *in);

protected:
  Filer                 *filer;     
  ObjectCacher          *objectcacher;
  Objecter              *objecter;     // (non-blocking) osd interface
  
  // cache
  hash_map<inodeno_t, Inode*> inode_map;
  Inode*                 root;
  LRU                    lru;    // lru list of Dentry's in our local metadata cache.

  // file handles, etc.
  filepath cwd;
  interval_set<int> free_fd_set;  // unused fds
  hash_map<int, Fh*> fd_map;
  
  int get_fd() {
    int fd = free_fd_set.start();
    free_fd_set.erase(fd, 1);
    return fd;
  }
  void put_fd(int fd) {
    free_fd_set.insert(fd, 1);
  }

  filepath mkpath(const char *rel) {
    if (rel[0] == '/') 
      return filepath(rel, 1);
    
    filepath t = cwd;
    filepath r(rel);
    t.append(r);
    return t;
  }


  // global client lock
  //  - protects Client and buffer cache both!
  Mutex                  client_lock;

  // helpers
  void wait_on_list(list<Cond*>& ls);
  void signal_cond_list(list<Cond*>& ls);

  // -- metadata cache stuff

  // decrease inode ref.  delete if dangling.
  void put_inode(Inode *in, int n=1) {
    //cout << "put_inode on " << in << " " << in->inode.ino << endl;
    in->put(n);
    if (in->ref == 0) {
      //cout << "put_inode deleting " << in << " " << in->inode.ino << std::endl;
      inode_map.erase(in->inode.ino);
      if (in == root) root = 0;
      delete in;
    }
  }

  void close_dir(Dir *dir) {
    assert(dir->is_empty());
    
    Inode *in = dir->parent_inode;
    if (in->dn) in->dn->put();   // unpin dentry
    
    delete in->dir;
    in->dir = 0;
    put_inode(in);               // unpin inode
  }

  //int get_cache_size() { return lru.lru_get_size(); }
  //void set_cache_size(int m) { lru.lru_set_max(m); }

  Dentry* link(Dir *dir, const string& name, Inode *in) {
    Dentry *dn = new Dentry;
    dn->name = name;
    
    // link to dir
    dn->dir = dir;
    //cout << "link dir " << dir->parent_inode->inode.ino << " '" << name << "' -> inode " << in->inode.ino << endl;
    dir->dentries[dn->name] = dn;

    // link to inode
    dn->inode = in;
    assert(in->dn == 0);
    in->dn = dn;
    in->get();

    if (in->dir) dn->get();  // dir -> dn pin

    lru.lru_insert_mid(dn);    // mid or top?
    return dn;
  }

  void unlink(Dentry *dn, bool keepdir = false) {
    Inode *in = dn->inode;
    assert(in->dn == dn);

    // unlink from inode
    if (dn->inode->dir) dn->put();        // dir -> dn pin
    dn->inode = 0;
    in->dn = 0;
    put_inode(in);
        
    // unlink from dir
    dn->dir->dentries.erase(dn->name);
    if (dn->dir->is_empty() && !keepdir) 
      close_dir(dn->dir);
    dn->dir = 0;

    // delete den
    lru.lru_remove(dn);
    delete dn;
  }

  Dentry *relink_inode(Dir *dir, const string& name, Inode *in) {
    Dentry *olddn = in->dn;
    Dir *olddir = olddn->dir;  // note: might == dir!

    // newdn, attach to inode.  don't touch inode ref.
    Dentry *newdn = new Dentry;
    newdn->dir = dir;
    newdn->name = name;
    newdn->inode = in;
    in->dn = newdn;

    if (in->dir) { // dir -> dn pin
      newdn->get();
      olddn->put();
    }

    // unlink old dn from dir
    olddir->dentries.erase(olddn->name);
    olddn->inode = 0;
    olddn->dir = 0;
    lru.lru_remove(olddn);
    delete olddn;
    
    // link new dn to dir
    dir->dentries[name] = newdn;
    lru.lru_insert_mid(newdn);
    
    // olddir now empty?  (remember, olddir might == dir)
    if (olddir->is_empty()) 
      close_dir(olddir);

    return newdn;
  }

  // move dentry to top of lru
  void touch_dn(Dentry *dn) { lru.lru_touch(dn); }  

  // trim cache.
  void trim_cache();
  void dump_inode(Inode *in, set<Inode*>& did);
  void dump_cache();  // debug
  
  // find dentry based on filepath
  Dentry *lookup(const filepath& path);

  int fill_stat(Inode *in, struct stat *st, frag_info_t *dirstat=0);

  
  // trace generation
  ofstream traceout;


  // friends
  friend class SyntheticClient;

 public:
  Client(Messenger *m, MonMap *mm);
  ~Client();
  void tear_down_cache();   

  int get_nodeid() { return whoami; }

  void init();
  void shutdown();

  // messaging
  void dispatch(Message *m);

  void handle_mon_map(MMonMap *m);
  void handle_unmount(Message*);
  void handle_mds_map(class MMDSMap *m);

  void handle_lease(MClientLease *m);

  void release_lease(Inode *in, Dentry *dn, int mask);

  // file caps
  void handle_file_caps(class MClientFileCaps *m);
  void check_caps(Inode *in);
  void put_cap_ref(Inode *in, int cap);

  void _release(Inode *in, bool checkafter=true);
  void _flush(Inode *in, bool checkafter=true);
  void _flushed(Inode *in, bool checkafter);

  void close_release(Inode *in);
  void close_safe(Inode *in);

  void lock_fh_pos(Fh *f);
  void unlock_fh_pos(Fh *f);
  
  // metadata cache
  void update_dir_dist(Inode *in, DirStat *st);

  Inode* insert_trace(MClientReply *reply, utime_t ttl);
  void update_inode_file_bits(Inode *in, __u64 size, utime_t ctime, utime_t mtime, utime_t atime,
			      int issued, __u64 time_warp_seq);
  void update_inode(Inode *in, InodeStat *st, LeaseStat *l, utime_t ttl);
  Inode* insert_dentry_inode(Dir *dir, const string& dname, LeaseStat *dlease, 
			     InodeStat *ist, LeaseStat *ilease, 
			     utime_t from);


  // ----------------------
  // fs ops.
private:
  void _try_mount();
  void _mount_timeout();
  Context *mount_timeout_event;

  class C_MountTimeout : public Context {
    Client *client;
  public:
    C_MountTimeout(Client *c) : client(c) { }
    void finish(int r) {
      if (r >= 0) client->_mount_timeout();
    }
  };

  // some helpers
  int _do_lstat(const filepath &path, int mask, Inode **in, int uid=-1, int gid=-1);
  int _opendir(const filepath &path, DirResult **dirpp, int uid=-1, int gid=-1);
  void _readdir_add_dirent(DirResult *dirp, const string& name, Inode *in);
  void _readdir_fill_dirent(struct dirent *de, DirEntry *entry, off_t);
  bool _readdir_have_frag(DirResult *dirp);
  void _readdir_next_frag(DirResult *dirp);
  void _readdir_rechoose_frag(DirResult *dirp);
  int _readdir_get_frag(DirResult *dirp);
  void _closedir(DirResult *dirp);
  void _ll_get(Inode *in);
  int _ll_put(Inode *in, int num);
  void _ll_drop_pins();

  // internal interface
  //   call these with client_lock held!
  int _link(const filepath &existing, const filepath &newname, int uid=-1, int gid=-1);
  int _unlink(const filepath &path, int uid=-1, int gid=-1);
  int _rename(const filepath &from, const filepath &to, int uid=-1, int gid=-1);
  int _mkdir(const filepath &path, mode_t mode, int uid=-1, int gid=-1);
  int _rmdir(const filepath &path, int uid=-1, int gid=-1);
  int _readlink(const filepath &path, char *buf, off_t size, int uid=-1, int gid=-1);
  int _symlink(const filepath &path, const char *target, int uid=-1, int gid=-1);
  int _lstat(const filepath &path, struct stat *stbuf, int uid=-1, int gid=-1, frag_info_t *dirstat=0);
  int _chmod(const filepath &path, mode_t mode, bool followsym, int uid=-1, int gid=-1);
  int _chown(const filepath &path, uid_t uid, gid_t gid, bool followsym, int cuid=-1, int cgid=-1);
  int _getxattr(const filepath &path, const char *name, void *value, size_t len, bool followsym, int uid=-1, int gid=-1);
  int _listxattr(const filepath &path, char *names, size_t len, bool followsym, int uid=-1, int gid=-1);
  int _setxattr(const filepath &path, const char *name, const void *value, size_t len, int flags, bool followsym, int uid=-1, int gid=-1);
  int _removexattr(const filepath &path, const char *nm, bool followsym, int uid=-1, int gid=-1);
  int _utimes(const filepath &path, utime_t mtime, utime_t atime, bool followsym, int uid=-1, int gid=-1);
  int _mknod(const filepath &path, mode_t mode, dev_t rdev, int uid=-1, int gid=-1);
  int _open(const filepath &path, int flags, mode_t mode, Fh **fhp, int uid=-1, int gid=-1);
  int _release(Fh *fh);
  int _read(Fh *fh, __s64 offset, __u64 size, bufferlist *bl);
  int _write(Fh *fh, __s64 offset, __u64 size, const char *buf);
  int _flush(Fh *fh);
  int _truncate(const filepath &path, loff_t length, bool followsym, int uid=-1, int gid=-1);
  int _ftruncate(Fh *fh, loff_t length);
  int _fsync(Fh *fh, bool syncdataonly);
  int _statfs(struct statvfs *stbuf);


public:
  int mount();
  int unmount();

  // these shoud (more or less) mirror the actual system calls.
  int statfs(const char *path, struct statvfs *stbuf);

  // crap
  int chdir(const char *s);
  const char *getcwd() { return cwd.c_str(); }

  // namespace ops
  int getdir(const char *relpath, list<string>& names);  // get the whole dir at once.

  int opendir(const char *name, DIR **dirpp);
  int closedir(DIR *dirp);
  int readdir_r(DIR *dirp, struct dirent *de);
  int readdirplus_r(DIR *dirp, struct dirent *de, struct stat *st, int *stmask);
  void rewinddir(DIR *dirp); 
  off_t telldir(DIR *dirp);
  void seekdir(DIR *dirp, off_t offset);

  struct dirent_plus *readdirplus(DIR *dirp);
  int readdirplus_r(DIR *dirp, struct dirent_plus *entry, struct dirent_plus **result);
  struct dirent_lite *readdirlite(DIR *dirp);
  int readdirlite_r(DIR *dirp, struct dirent_lite *entry, struct dirent_lite **result);
 
  int link(const char *existing, const char *newname);
  int unlink(const char *path);
  int rename(const char *from, const char *to);

  // dirs
  int mkdir(const char *path, mode_t mode);
  int rmdir(const char *path);

  // symlinks
  int readlink(const char *path, char *buf, off_t size);
  int symlink(const char *existing, const char *newname);

  // inode stuff
  int lstat(const char *path, struct stat *stbuf, frag_info_t *dirstat=0);
  int lstatlite(const char *path, struct statlite *buf);

  int chmod(const char *path, mode_t mode);
  int chown(const char *path, uid_t uid, gid_t gid);
  int utime(const char *path, struct utimbuf *buf);

  // file ops
  int mknod(const char *path, mode_t mode, dev_t rdev=0);
  int open(const char *path, int flags, mode_t mode=0);
  int close(int fd);
  off_t lseek(int fd, off_t offset, int whence);
  int read(int fd, char *buf, off_t size, off_t offset=-1);
  int write(int fd, const char *buf, off_t size, off_t offset=-1);
  int fake_write_size(int fd, off_t size);
  int truncate(const char *file, off_t size);
  int ftruncate(int fd, off_t size);
  int fsync(int fd, bool syncdataonly);
  int fstat(int fd, struct stat *stbuf);

  // hpc lazyio
  int lazyio_propogate(int fd, off_t offset, size_t count);
  int lazyio_synchronize(int fd, off_t offset, size_t count);

  // expose file layout
  int describe_layout(int fd, ceph_file_layout* layout);
  int get_stripe_unit(int fd);
  int get_stripe_width(int fd);
  int get_stripe_period(int fd);
  int enumerate_layout(int fd, list<ObjectExtent>& result,
		       off_t length, off_t offset);

  // low-level interface
  int ll_lookup(inodeno_t parent, const char *name, struct stat *attr, int uid = -1, int gid = -1);
  bool ll_forget(inodeno_t ino, int count);
  Inode *_ll_get_inode(inodeno_t ino);
  int ll_getattr(inodeno_t ino, struct stat *st, int uid = -1, int gid = -1);
  int ll_setattr(inodeno_t ino, struct stat *st, int mask, int uid = -1, int gid = -1);
  int ll_getxattr(inodeno_t ino, const char *name, void *value, size_t size, int uid=-1, int gid=-1);
  int ll_setxattr(inodeno_t ino, const char *name, const void *value, size_t size, int flags, int uid=-1, int gid=-1);
  int ll_removexattr(inodeno_t ino, const char *name, int uid=-1, int gid=-1);
  int ll_listxattr(inodeno_t ino, char *list, size_t size, int uid=-1, int gid=-1);
  int ll_opendir(inodeno_t ino, void **dirpp, int uid = -1, int gid = -1);
  void ll_releasedir(void *dirp);
  int ll_readlink(inodeno_t ino, const char **value, int uid = -1, int gid = -1);
  int ll_mknod(inodeno_t ino, const char *name, mode_t mode, dev_t rdev, struct stat *attr, int uid = -1, int gid = -1);
  int ll_mkdir(inodeno_t ino, const char *name, mode_t mode, struct stat *attr, int uid = -1, int gid = -1);
  int ll_symlink(inodeno_t ino, const char *name, const char *value, struct stat *attr, int uid = -1, int gid = -1);
  int ll_unlink(inodeno_t ino, const char *name, int uid = -1, int gid = -1);
  int ll_rmdir(inodeno_t ino, const char *name, int uid = -1, int gid = -1);
  int ll_rename(inodeno_t parent, const char *name, inodeno_t newparent, const char *newname, int uid = -1, int gid = -1);
  int ll_link(inodeno_t ino, inodeno_t newparent, const char *newname, struct stat *attr, int uid = -1, int gid = -1);
  int ll_open(inodeno_t ino, int flags, Fh **fh, int uid = -1, int gid = -1);
  int ll_create(inodeno_t parent, const char *name, mode_t mode, int flags, struct stat *attr, Fh **fh, int uid = -1, int gid = -1);
  int ll_read(Fh *fh, off_t off, off_t len, bufferlist *bl);
  int ll_write(Fh *fh, off_t off, off_t len, const char *data);
  int ll_flush(Fh *fh);
  int ll_fsync(Fh *fh, bool syncdataonly);
  int ll_release(Fh *fh);
  int ll_statfs(inodeno_t, struct statvfs *stbuf);

  // failure
  void ms_handle_failure(Message*, const entity_inst_t& inst);
  void ms_handle_reset(const entity_addr_t& addr, entity_name_t last);
  void ms_handle_remote_reset(const entity_addr_t& addr, entity_name_t last);
};

#endif
