// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OSD_NEWSTORE_H
#define CEPH_OSD_NEWSTORE_H

#include "include/assert.h"
#include "include/unordered_map.h"
#include "include/memory.h"
#include "common/Finisher.h"
#include "common/RWLock.h"
#include "os/ObjectStore.h"

#include "newstore_types.h"

class NewStore : public ObjectStore {

  // -----------------------------------------------------
  // types

  struct FragmentHandle {
    int fd;
    FragmentHandle() : fd(-1) {}
    FragmentHandle(int f) : fd(f) {}
    ~FragmentHandle() {
      if (fd >= 0)
	::close(fd);
    }
    int fsync() {
      return ::fsync(fd);
    }
    int fdatasync() {
      return ::fdatasync(fd);
    }
  };
  typedef ceph::shared_ptr<FragmentHandle> FragmentHandleRef;

  /// an in-memory object
  struct Onode {
    onode_t onode;
    bool dirty;  // ???

    Onode() : dirty(false) {}
  };

  struct Collection {
    coll_t cid;
    RWLock lock;

    // cache onodes on a per-collection basis to avoid lock
    // contention.
    SharedLRU<ghobject_t,OnodeRef> onode_map;

    OnodeRef get_onode(const ghobject_t& oid, bool create);

    Collection(coll_t c)
      : cid(c),
	lock("NewStore::Collection::lock")
    {}
  };
  typedef ceph::shared_ptr<Collection> CollectionRef;


  class OmapIteratorImpl : public ObjectMap::ObjectMapIteratorImpl {
    CollectionRef c;
    ObjectRef o;
    //map<string,bufferlist>::iterator it;
  public:
    OmapIteratorImpl(CollectionRef c, ObjectRef o)
      : c(c), o(o) /*, it(o->omap.begin())*/ {}

    int seek_to_first() {
      RWLock::RLocker l(c->lock);
      //it = o->omap.begin();
      return 0;
    }
    int upper_bound(const string &after) {
      RWLock::RLocker l(c->lock);
      //it = o->omap.upper_bound(after);
      return 0;
    }
    int lower_bound(const string &to) {
      RWLock::RLocker l(c->lock);
      //it = o->omap.lower_bound(to);
      return 0;
    }
    bool valid() {
      RWLock::RLocker l(c->lock);
      return false; //return it != o->omap.end();
    }
    int next() {
      RWLock::RLocker l(c->lock);
      //++it;
      return 0;
    }
    string key() {
      RWLock::RLocker l(c->lock);
      return string(); //return it->first;
    }
    bufferlist value() {
      RWLock::RLocker l(c->lock);
      return bufferlist(); //return it->second;
    }
    int status() {
      return 0;
    }
  };

  struct TransContext {
    list<int> fds;      ///< these fds need to be synced

    void sync_fd(int f) {
      fds.push_back(f);
    }
    int wait_sync();
  };
  typedef ceph:shared_ptr<TransContextRef> TransContextRef;

  // --------------------------------------------------------
  // members
  string journal_path;
  KeyValueDB *db;
  Journal *journal;
  uuid_d fsid;
  int path_fd;  ///< open handle to $path
  int fsid_fd;  ///< open handle (locked) to $path/fsid
  int frag_fd;  ///< open handle to $path/fragments
  int fset_fd;  ///< open handle to $path/fragments/$cur_fid.fset
  bool mounted;

  RWLock coll_lock;    ///< rwlock to protect coll_map
  ceph::unordered_map<coll_t, CollectionRef> coll_map;

  Mutex fid_lock;
  fid_t cur_fid;

  Finisher finisher;
  Logger *logger;


  // --------------------------------------------------------
  // private methods

  void _init_logger();
  void _shutdown_logger();

  int _open_path();
  void _close_path();
  int _open_fsid(bool create);
  int _lock_fsid();
  int _read_fsid(uuid_d *f);
  void _close_fsid();
  int _open_frag();
  int _create_frag();
  void close_frag();
  int _open_db();
  void _close_db();

  int _open_journal();
  void _close_jouranl();

  CollectionRef _get_collection(coll_t cid);

  int _open_next_fid(fid_t *fid);


public:
  NewStore(CephContext *cct, const string& path, const string& journal_path);
    : ObjectStore(path),
      coll_lock("NewStore::coll_lock"),
      Finisher(cct)
  {}
  ~NewStore() {}

  bool need_journal() { return true; };
  int peek_journal_fsid(uuid_d *fsid);

  bool test_mount_in_use();

  int mount();
  int umount();

  unsigned get_max_object_name_length() {
    return 4096;
  }
  unsigned get_max_attr_name_length() {
    return 256;  // arbitrary; there is no real limit internally
  }

  int mkfs();
  int mkjournal();

private:
  bool sharded;
public:
  void set_allow_sharded_objects() {
    sharded = true;
  }
  bool get_allow_sharded_objects() {
    return sharded;
  }

  int statfs(struct statfs *buf);

  bool exists(coll_t cid, const ghobject_t& oid);
  int stat(
    coll_t cid,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false); // struct stat?
  int read(
    coll_t cid,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0,
    bool allow_eio = false);
  int fiemap(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len, bufferlist& bl);
  int getattr(coll_t cid, const ghobject_t& oid, const char *name, bufferptr& value);
  int getattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset);

  int list_collections(vector<coll_t>& ls);
  bool collection_exists(coll_t c);
  bool collection_empty(coll_t c);
  int collection_list(coll_t cid, vector<ghobject_t>& o);
  int collection_list_partial(coll_t cid, ghobject_t start,
			      int min, int max, snapid_t snap,
			      vector<ghobject_t> *ls, ghobject_t *next);
  int collection_list_range(coll_t cid, ghobject_t start, ghobject_t end,
			    snapid_t seq, vector<ghobject_t> *ls);

  int omap_get(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    );

  /// Get omap header
  int omap_get_header(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    );

  /// Get keys defined on oid
  int omap_get_keys(
    coll_t cid,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    );

  /// Get key values
  int omap_get_values(
    coll_t cid,                    ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    );

  /// Filters keys into out which are defined on oid
  int omap_check_keys(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    );

  ObjectMap::ObjectMapIterator get_omap_iterator(
    coll_t cid,              ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    );

  void set_fsid(uuid_d u);
  uuid_d get_fsid();

  objectstore_perf_stat_t get_cur_stats();

  int queue_transactions(
    Sequencer *osr, list<Transaction*>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL);

private:
  // --------------------------------------------------------
  // write ops

  int _do_transaction(Transaction *t);

  int _write(TransContextRef& txc,
	     CollectionRef& c,
	     const ghobject_t& oid,
	     uint64_t offset, size_t len,
	     const bufferlist& bl,
	     uint32_t fadvise_flags)

};


#endif
