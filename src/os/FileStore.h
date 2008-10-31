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


#ifndef __FILESTORE_H
#define __FILESTORE_H

#include "ObjectStore.h"
#include "JournalingObjectStore.h"
#include "common/ThreadPool.h"
#include "common/Mutex.h"

#include "Fake.h"
//#include "FakeStoreBDBCollections.h"

#include <signal.h>

#include <map>
using namespace std;

#include <ext/hash_map>
using namespace __gnu_cxx;


// fake attributes in memory, if we need to.

class FileStore : public JournalingObjectStore {
  string basedir;
  __u64 fsid;
  
  int btrfs;
  bool btrfs_trans_start_end;
  int lock_fd;

  // fake attrs?
  FakeAttrs attrs;
  bool fake_attrs;

  // fake collections?
  FakeCollections collections;
  bool fake_collections;
  
  // helper fns
  void append_oname(const pobject_t &oid, char *s);
  //void get_oname(pobject_t oid, char *s);
  void get_cdir(coll_t cid, char *s);
  void get_coname(coll_t cid, pobject_t oid, char *s);
  bool parse_object(char *s, pobject_t& o);
  bool parse_coll(char *s, coll_t& c);

  // sync thread
  Mutex lock;
  Cond sync_cond;
  bool stop;
  void sync_entry();
  struct SyncThread : public Thread {
    FileStore *fs;
    SyncThread(FileStore *f) : fs(f) {}
    void *entry() {
      fs->sync_entry();
      return 0;
    }
  } sync_thread;

  void sync_fs(); // actuall sync underlying fs

 public:
  FileStore(const char *base) : 
    basedir(base),
    btrfs(false), btrfs_trans_start_end(false),
    lock_fd(-1),
    attrs(this), fake_attrs(false), 
    collections(this), fake_collections(false),
    lock("FileStore::lock"),
    stop(false), sync_thread(this) { }

  int mount();
  int umount();
  int mkfs();

  int statfs(struct statfs *buf);

  unsigned apply_transaction(Transaction& t, Context *onsafe=0);
  int _transaction_start(int len);
  void _transaction_finish(int id);
  unsigned _apply_transaction(Transaction& t);

  // ------------------
  // objects
  int pick_object_revision_lt(pobject_t& oid) {
    return 0;
  }
  bool exists(coll_t cid, pobject_t oid);
  int stat(coll_t cid, pobject_t oid, struct stat *st);
  int read(coll_t cid, pobject_t oid, __u64 offset, size_t len, bufferlist& bl);

  int _remove(coll_t cid, pobject_t oid);
  int _truncate(coll_t cid, pobject_t oid, __u64 size);
  int _write(coll_t cid, pobject_t oid, __u64 offset, size_t len, const bufferlist& bl);
  int _zero(coll_t cid, pobject_t oid, __u64 offset, size_t len);
  int _clone(coll_t cid, pobject_t oldoid, pobject_t newoid);
  int _clone_range(coll_t cid, pobject_t oldoid, pobject_t newoid, __u64 off, __u64 len);
  int _do_clone_range(int from, int to, __u64 off, __u64 len);

  void sync();
  void sync(Context *onsafe);

  // attrs
  int getattr(coll_t cid, pobject_t oid, const char *name, void *value, size_t size);
  int getattr(coll_t cid, pobject_t oid, const char *name, bufferptr &bp);
  int getattrs(coll_t cid, pobject_t oid, map<string,bufferptr>& aset);

  int _getattr(const char *fn, const char *name, bufferptr& bp);
  int _getattrs(const char *fn, map<string,bufferptr>& aset);

  int _setattr(coll_t cid, pobject_t oid, const char *name, const void *value, size_t size);
  int _setattrs(coll_t cid, pobject_t oid, map<string,bufferptr>& aset);
  int _rmattr(coll_t cid, pobject_t oid, const char *name);

  int collection_getattr(coll_t c, const char *name, void *value, size_t size);
  int collection_getattr(coll_t c, const char *name, bufferlist& bl);
  int collection_getattrs(coll_t cid, map<string,bufferptr> &aset);

  int _collection_setattr(coll_t c, const char *name, const void *value, size_t size);
  int _collection_rmattr(coll_t c, const char *name);
  int _collection_setattrs(coll_t cid, map<string,bufferptr> &aset);

  // collections
  int list_collections(vector<coll_t>& ls);
  int collection_stat(coll_t c, struct stat *st);
  bool collection_exists(coll_t c);
  int collection_list(coll_t c, vector<pobject_t>& o);

  int _create_collection(coll_t c);
  int _destroy_collection(coll_t c);
  int _collection_add(coll_t c, coll_t ocid, pobject_t o);
  int _collection_remove(coll_t c, pobject_t o);

  int pick_object_revision_lt(coll_t cid, pobject_t& oid) { return -1; }
  void trim_from_cache(coll_t cid, pobject_t oid, __u64 offset, size_t len) {}
  int is_cached(coll_t cid, pobject_t oid, __u64 offset, size_t len) { return -1; }
};

#endif
