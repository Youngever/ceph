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

const string PREFIX_COLL = "C"; // collection name -> (nothing)
const string PREFIX_OBJ = "O";  // object name -> onode
const string PREFIX_WAL = "L";  // write ahead log

#include "NewStore.h"

NewStore::NewStore(CephContext *cct, const string& path)
  : ObjectStore(path),
    db(NULL),
    fsid_fd(-1),
    frag_fd(-1),
    mounted(false),
    coll_lock("NewStore::coll_lock"),
    Finisher(cct),
    logger(NULL)
{
  _init_logger();
}

~NewStore::NewStore()
{
  _shutdown_logger();
  assert(!mounted);
  assert(db == NULL);
  assert(fsid_fd < 0);
  assert(frag_fd < 0);
}

void NewStore::_init_logger()
{
  // XXX
}

void NewStore::_shutdown_logger()
{
  // XXX
}

int NewStore::peek_journal_fsid(uuid_d *fsid)
{
  return 0;
}

int NewStore::_open_path()
{
  assert(path_fd < 0);
  path_fd = ::open(path.c_str(), O_DIRECTORY);
  if (path_fd < 0) {
    int r = -errno;
    derr << __func__ << " unable to open " << path << ": " << cpp_strerror(r)
	 << dendl;
    return r;
  }
  return 0;
}

void NewStore::_close_path()
{
  VOID_TEMP_FAILURE_RETRY(::close(path_fd));
  path_fd = -1;
}

int NewStore::_open_frag()
{
  assert(frag_fd < 0);
  frag_fd = ::openat(path_fd, "fragments", O_DIRECTORY);
  if (frag_fd < 0) {
    int r = -errno;
    derr << __func__ << " cannot open " << path << "/fragments: "
	 << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

int NewStore::_create_frag()
{
  assert(frag_fd < 0);
  frag_fd = ::openat(path_fd, "fragments", O_DIRECTORY);
  if (frag_fd < 0 && errno == ENOENT) {
    int r = ::mkdirat(path_fd, "fragments", 0755);
    if (r < 0) {
      r = -errno;
      derr << __func__ << " cannot create " << path << "/fragments: "
	   << cpp_strerror(r) << dendl;
      return r;
    }
    frag_fd = ::open_at(path_fd, "fragments", O_DIRECTORY);
  }
  if (frag_fd < 0) {
    int r = -errno;
    derr << __func__ << " cannot open created " << path << "/fragments: "
	 << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

void NewStore::_close_frag()
{
  VOID_TEMP_FAILURE_RETRY(::close(frag_fd));
  frag_fd = -1;
}

int NewStore::_open_fsid(bool create)
{
  assert(fsid_fd < 0);
  int flags = O_RDWR;
  if (create)
    flags |= O_CREAT;
  fsid_fd = ::openat(path_fd, "fsid", flags, 0644);
  if (fsid_fd < 0) {
    int err = -errno;
    derr << __func__ << " " << cpp_strerror(err) << dendl;
    return err;
  }
  return 0;
}

int NewStore::_read_fsid(uuid_d *uuid)
{
  char fsid_str[40];
  int ret = safe_read(fsid_fd, fsid_str, sizeof(fsid_str));
  if (ret < 0)
    return ret;
  if (ret == 8) {
    // old 64-bit fsid... mirror it.
    *(uint64_t*)&uuid->uuid[0] = *(uint64_t*)fsid_str;
    *(uint64_t*)&uuid->uuid[8] = *(uint64_t*)fsid_str;
    return 0;
  }

  if (ret > 36)
    fsid_str[36] = 0;
  if (!uuid->parse(fsid_str))
    return -EINVAL;
  return 0;
}

int NewStore::_write_fsid()
{
  int r = ::ftruncate(fsid_fd, 0);
  if (r < 0) {
    r = -errno;
    derr << __func__ << " fsid truncate failed: " << cpp_streerror(r) << dendl;
    return r;
  }
  string str = stringify(fsid) + "\n";
  r = safe_write(fsid_fd, str, strlen(str));
  if (r < 0) {
    derr << __func__ << " fsid write failed: " << cpp_strerror(r) << dendl;
    return r;
  }
  r = ::fsync(fsid_fd);
  if (r < 0) {
    derr << __func__ << " fsid fsync failed: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

void NewStore::_close_fsid()
{
  VOID_TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
}

int NewStore::_lock_fsid()
{
  struct flock l;
  memset(&l, 0, sizeof(l));
  l.l_type = F_WRLCK;
  l.l_whence = SEEK_SET;
  l.l_start = 0;
  l.l_len = 0;
  int r = ::fcntl(fsid_fd, F_SETLK, &l);
  if (r < 0) {
    int err = errno;
    derr << __func__ << " failed to lock " << fn
	 << " (is another ceph-osd still running?)"
	 << cpp_strerror(err) << dendl;
    return -err;
  }
  return 0;
}

int FileStore::_read_fsid(uuid_d *uuid)
{
  char fsid_str[40];
  int ret = safe_read(fsid_fd, fsid_str, sizeof(fsid_str));
  if (ret < 0)
    return ret;
  if (ret > 36)
    fsid_str[36] = 0;
  if (!uuid->parse(fsid_str))
    return -EINVAL;
  return 0;
}

bool NewStore::test_mount_in_use()
{
  int r = _open_fsid(false);
  if (r < 0)
    return true;
  r = _lock_fsid();
  _close_fsid();
  return (r < 0);
}

int NewStore::_open_db()
{
  assert(!db);
  snprintf(db_dir, sizeof(db_dir), "%s/db", path.c_str());
  db = KeyValueDB::create(g_ceph_context, "leveldb" /* fixme */, db_dir);
  if (!db) {
    derr << __func__ << " error creating db" << dendl;
    delete db;
    db = NULL;
    return -EIO;
  }
  db->init();
  stringstream err;
  if (db->create_and_open(err)) {
    derr << __func__ << " erroring opening db: " << err << dendl;
    delete db;
    db = NULL;
    return -EIO;
  }
  return 0;
}

int NewStore::_create_db()
{

}

void NewStore::_close_db()
{
  assert(db);
  delete db;
  db = NULL;
}

int NewStore::mkfs()
{
  dout(1) << __func__ << " path " << path << dendl;
  int r;
  uuid_d old_fsid;

  r = _open_path();
  if (r < 0)
    return r;

  r = _open_fsid(true);
  if (r < 0)
    goto out_path_fd;

  r = _lock_fsid();
  if (r < 0)
    goto out_close_fsid;

  r = _read_fsid(&old_fsid);
  if (r < 0 && old_fsid.is_zero()) {
    if (fsid.is_zero()) {
      fsid.generate_random();
      dout(1) << __func__ << " generated fsid " << fsid << dendl;
    } else {
      dout(1) << __func__ << " using provided fsid " << fsid << dendl;
    }
    r = _write_fsid();
    if (r < 0)
      goto out_close_fsid;
  } else {
    if (!fsid.is_zero() && fsid != old_fsid) {
      derr << __func__ << " on-disk fsid " << old_fsid
	   << " != provided " << fsid << dendl;
      ret = -EINVAL;
      goto out_close_fsid;
    }
    fsid = old_fsid;
    dout(1) << __func__ << " fsid is already set to " << fsid << dendl;
  }

  r = _create_frag();
  if (r < 0)
    goto out_close_fsid;

  r = _open_db();
  if (r < 0)
    goto out_close_frag;

  // FIXME: superblock

  dout(10) << __func__ << " success" << dendl;
  r = 0;

 out_close_frag:
  _close_frag();
 out_close_fsid:
  _close_fsid();
 out_path_fd:
  _close_path();
  return r;
}

int NewStore::mount()
{
  char db_dir[PATH_PATH];

  dout(1) << __func__ << " path " << path << dendl;

  int r = _open_path();
  if (r < 0)
    return r;
  r = _open_fsid(false);
  if (r < 0)
    goto out_path;

  r = _read_fsid();
  if (r < 0)
    goto out_fsid;

  r = _lock_fsid();
  if (r < 0)
    goto out_fsid;

  r = _open_frag();
  if (r < 0)
    goto out_fsid;

  // FIXME: superblock, features

  r = _open_db();
  if (r < 0)
    goto out_frag;

  finisher.start();

  mounted = true;
  return 0;

 out_frag:
  _close_frag();
 out_db:
  _close_db();
 out_fsid:
  _close_fsid();
 out_path:
  _close_path();
  return r;
}

int NewStore::umount()
{
  assert(mounted);
  dout(1) << __func__ << dendl;
  if (fset_fd >= 0)
    VOID_TEMP_FAILURE_RETRY(::close(fset_fd));
  finisher.stop();
  _close_db();
  _close_fsid();
  return 0;
}


// ---------------
// cache

CollectionRef NewStore::_get_collection(coll_t cid)
{
  RWLock::RLocker l(coll_lock);
  ceph::unordered_map<coll_t,CollectionRef>::iterator cp = coll_map.find(cid);
  if (cp == coll_map.end())
    return CollectionRef();
  return cp->second;
}

OnodeRef Collection::get_onode(const ghobject_t& oid, bool create)
{
  assert(create ? lock.is_wlocked() : lock.is_locked());

  OnodeRef o = onode_map.lookup(oid);
  if (o)
    return o;

  bufferlist v;
  int r = db->get(ONODE_PREFIX, get_object_key(oid));
  if (r < 0) {
    if (!create)
      return OnodeRef;

    // new
    o = new Onode();
    o->dirty = true;
  } else {
    // loaded
    o = new Onode();
    bufferlist::iterator p = v.begin();
    ::decode(o->onode, p);
  }

  onode_map.add(oid, o, NULL);
  return o;
}


// ---------------
// read operations

bool NewStore::exists(coll_t cid, const ghobject_t& oid)
{
  dout(10) << __func__ << " " << cid << " " << oid << dendl;
  CollectionRef c = get_collection(cid);
  if (!c)
    return false;
  RWLock::RLocker l(c->lock);
  OnodeRef o = c->get(oid, false);
  if (!o || !o->exists)
    return -ENOENT;
  return 0;
}

int MemStore::stat(
    coll_t cid,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio)
{
  dout(10) << __func__ << " " << cid << " " << oid << dendl;
  CollectionRef c = get_collection(cid);
  if (!c)
    return -ENOENT;
  RWLock::RLocker l(c->lock);
  OnodeRef o = c->get_onode(oid, false);
  if (!o || !o->exists)
    return -ENOENT;
  st->st_size = o->onode.size;
  st->st_blksize = 4096;
  st->st_blocks = (st->st_size + st->st_blksize - 1) / st->st_blksize;
  st->st_nlink = 1;
  return 0;
}



// -----------------
// write helpers

int NewStore::_open_next_fid(fid_t *fid)
{
  {
    Mutex::Locker l(fid_lock);
    if (cur_fid.fno < g_conf->newstore>max_dir_size) {
      ++cur_fid.fno;
    } else {
      ++cur_fid.fset;
      cur_fid.fno = 1;
      dout(10) << __func__ << " creating " << cur_fid.fset << dendl;
      char s[32];
      snprintf(s, sizeof(s), "%d", cur_fid.fset);
      int r = ::mkdirat(frag_fd, s, 0755);
      if (r < 0) {
	r = -errno;
	derr << __func__ << " cannot create " << path << "/fragments/"
	     << s << ": " << cpp_strerror(r) << dendl;
	return r;
      }
      if (fset_fd >= 0)
	VOID_TEMP_FAILURE_RETRY(::close(fset_fd));
      fset_fd = ::openat(frag_fd, s, O_DIRECTORY, 0644);
      if (fset_fd < 0) {
	r = -errno;
	derr << __func__ << " cannot open created " << path << "/fragments/"
	     << s << ": " << cpp_strerror(r) << dendl;
      }
    }
    *fid = cur_fid;
  }

  dout(10) << __func__ << " " << cur_fid << dendl;
  char s[32];
  snprintf(s, sizeof(s), "%d", fid->fno);
  int r = ::openat(fset_fd, s, O_RDWR|O_CREAT, 0644);
  if (r < 0) {
    r = -errno;
    derr << __func__ << " cannot create " << path << "/fragments/"
	 << *fid << ": " << cpp_strerror(r) << dendl;
    return r;
  }
  return r;
}

int NewStore::_finalize_onodes(TransContextRef& txc)
{
  dout(20) << __func__ << dendl;
}


// -----------------
// write operations

int NewStore::_touch(TransContextRef& txc,
		     CollectionRef& c,
		     const ghobject_t& oid)
{
  dout(15) << __func__ << " " << c->cid << " " << oid << dendl;
  int r = 0;
  OnodeRef o = c->get_onode(oid, true);
  txc.add_onode(o);
  dout(10) << __func__ << " " << c->cid << " " << oid << " = " << r << dendl;
}

int NewStore::_write(TransContextRef& txc,
		     CollectionRef& c,
		     const ghobject_t& oid,
		     uint64_t offset, size_t len,
		     const bufferlist& bl,
		     uint32_t fadvise_flags)
{
  dout(15) << __func__ << " " << c->cid << " " << oid
	   << " " << offset << "~" << length
	   << dendl;
  int r;

  OnodeRef o = c->get_onode(oid, true);
  int fd;
  if (o.size == 0) {
    fragment_t &f = o->data_map[0];
    f.offset = 0;
    f.length = len;
    fd = _get_next_fid(&f.fid);
    if (fd < 0) {
      r = fd;
      goto out;
    }
  } else {
    assert(0 == "implement me");
  }

  r = bl.write_fd(fd);
  txc.sync_fd(fd);

 out:
  dout(10) << __func__ << " " << c->cid << " " << oid
	   << " " << offset << "~" << length
	   << " = " << r << dendl;
  return r;
}

int NewStore::_zero(TransContextRef& txc,
		    CollectionRef& c,
		    const ghobject_t& oid,
		    uint64_t offset, size_t len)
{
  dout(15) << __func__ << " " << c->cid << " " << oid
	   << " " << offset << "~" << length
	   << dendl;
  int r = 0;

  assert(0 == "write me");

  dout(10) << __func__ << " " << c->cid << " " << oid
	   << " " << offset << "~" << length
	   << " = " << r << dendl;
  return r;
}

int NewStore::_truncate(TransContextRef& txc,
			CollectionRef& c,
			const ghobject_t& oid,
			uint64_t offset)
{
  dout(15) << __func__ << " " << c->cid << " " << oid
	   << " " << offset
	   << dendl;
  int r = 0;

  assert(0 == "write me");

  dout(10) << __func__ << " " << c->cid << " " << oid
	   << " " << offset
	   << " = " << r << dendl;
  return r;
}

int NewStore::_remove(TransContextRef& txc,
		      CollectionRef& c,
		      const ghobject_t& oid)
{
  dout(15) << __func__ << " " << c->cid << " " << oid << dendl;
  int r;

  OnodeRef o = c->get_onode(oid, false);
  if (!o || !o->exists) {
    r = -ENOENT;
    goto out;
  }

  o->exists = false;
  o->size = 0;
  for (map<uint64_t,fragment_t>::iterator p = o->onode.data_map.begin();
       p != o->onode.data_map.end();
       ++p) {
    txc->remove_fid(p->fid);
  }
  o->onode.data_map.clear();
  txc.write_onode(o);
  r = 0;

 out:
  dout(10) << __func__ << " " << c->cid << " " << oid << " = " << r << dendl;
  return r;
}

int NewStore::_setattr(TransContextRef& txc,
		       CollectionRef& c,
		       const ghobject_t& oid,
		       const string& name,
		       bufferptr& val)
{
  dout(15) << __func__ << " " << c->cid << " " << oid
	   << " " << name << " (" << val.length() << " bytes)"
	   << dendl;
  int r = 0;

  OnodeRef& o = c->get_onode(oid, false);
  if (!o || !o->exists) {
    r = -ENOENT;
    goto out;
  }
  o->attrs[name] = val;
  r = 0;

 out:
  dout(10) << __func__ << " " << c->cid << " " << oid
	   << " " << name << " (" << val.length() << " bytes)"
	   << " = " << r << dendl;
  return r;
}

int NewStore::_setattrs(TransContextRef& txc,
			CollectionRef& c,
			const ghobject_t& oid,
			const map<string,bufferptr>& aset)
{
  dout(15) << __func__ << " " << c->cid << " " << oid
	   << " " << aset.size() << " keys"
	   << dendl;
  int r = 0;

  OnodeRef& o = c->get_onode(oid, false);
  if (!o || !o->exists) {
    r = -ENOENT;
    goto out;
  }
  for (map<string,bufferptr>::iterator p = aset.begin(); p != aset.end(); ++p)
    o->attrs[p->first] = p->second;
  r = 0;

 out:
  dout(10) << __func__ << " " << c->cid << " " << oid
	   << " " << aset.size() << " keys"
	   << " = " << r << dendl;
  return r;
}


int NewStore::_rmattr(TransContextRef& txc,
		      CollectionRef& c,
		      const ghobject_t& oid,
		      const string& name)
{
  dout(15) << __func__ << " " << c->cid << " " << oid
	   << " " << name << dendl;
  int r = 0;

  OnodeRef& o = c->get_onode(oid, false);
  if (!o || !o->exists) {
    r = -ENOENT;
    goto out;
  }
  o->attrs.erase(name);
  r = 0;

 out:
  dout(10) << __func__ << " " << c->cid << " " << oid
	   << " " << name << " = " << r << dendl;
  return r;
}

int NewStore::_rmattrs(TransContextRef& txc,
		       CollectionRef& c,
		       const ghobject_t& oid)
{
  dout(15) << __func__ << " " << c->cid << " " << oid << dendl;
  int r = 0;

  OnodeRef& o = c->get_onode(oid, false);
  if (!o || !o->exists) {
    r = -ENOENT;
    goto out;
  }
  o->attrs.clear();
  r = 0;

 out:
  dout(10) << __func__ << " " << c->cid << " " << oid << " = " << r << dendl;
  return r;
}

int NewStore::_clone(TransContextRef& txc,
		     CollectionRef& c,
		     const ghobject_t& old_oid,
		     const ghobject_t& new_oid)
{
  dout(15) << __func__ << " " << c->cid << " " << old_oid << " -> "
	   << new_oid << dendl;
  int r = 0;

  assert(0 == "writ eme");

 out:
  dout(10) << __func__ << " " << c->cid << " " << old_oid << " -> "
	   << new_oid << " = " << r << dendl;
  return r;
}


int NewStore::_create_collection(TransContextRef& txc, coll_t cid)
{
  dout(15) << __func__ << " " << cid << dendl;
  int r;
  CollectionRef c;
  bufferlist empty;

  {
    RWLock::RLocker l(coll_lock);
    ceph::unordered_map<coll_t,CollectionRef>::iterator cp = coll_map.find(cid);
    if (cp != coll_map.end()) {
      r = -EEXIST;
      goto out;
    }
    c = new Collection(cid);
    coll_map[cid] = c;
  }
  txc.t->set(PREFIX_COLL, stringify(cid), empty);
  r = 0;

 out:
  dout(10) << __func__ << " " << cid << " = " << r << dendl;
  return r;
}




// ---------------------------
// transactions

int TransactionContext::wait_sync()
{
  /// XXX fixme: use aio fsync
  for (list<int>::iterator p = fds.begin(); p != fds.end(); ++p) {
    int r = ::fsync(*p);
    if (r < 0) {
      r = -errno;
      derr << __func__ << " fsync: " << cpp_strerror(r) << dendl;
      return r;
    }
  }

  /// XXX sync db txn

  return 0;
}

int NewStore::queue_transactions(
    Sequencer *osr, list<Transaction*>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL)
{
  Context *onreadable;
  Context *ondisk;
  Context *onreadable_sync;
  ObjectStore::Transaction::collect_contexts(
    tls, &onreadable, &ondisk, &onreadable_sync);

  TransContext txc;

  // XXX do it sync for now; this is not crash safe
  for (list<Transaction*>::iterator p = tls.begin(); p != tls.end(); ++p) {
    _do_transaction(*p, &txc, handle);
  }

  if (onreadable_sync)
    onreadable_sync->complete(0);
  if (onreadable)
    finisher.queue(onreadable);

  int r = txc.wait_sync();
  assert(r == !"txc.wait_sync() == 0");

  if (oncommit)
    finisher.queue(oncommit);
}

int NewStore::_do_transaction(Transaction *t,
			      TransContextRef& txc,
			      ThreadPool::TPHandle *handle)
{
  Transaction::iterator i = t.begin();
  int pos = 0;

  vector<CollectionRef> cvec(t->colls.size());
  unsigned i = 0;
  for (vector<coll_t>::iterator p = t->colls.begin(); p != t->colls.end();
       ++p, ++i) {
    cvec[i] = _get_collection(*p);
  }

  while (i.have_op()) {
    Transaction::Op *op = i.decode_op();
    int r = 0;
    CollectionRef &c = cvec[op->cid];

    switch (op->op) {
    case Transaction::OP_NOP:
      break;
    case Transaction::OP_TOUCH:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
	r = _touch(c, oid);
      }
      break;

    case Transaction::OP_WRITE:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        uint64_t len = op->len;
	uint32_t fadvise_flags = i.get_fadvise_flags();
        bufferlist bl;
        i.decode_bl(bl);
	r = _write(txc, c, oid, off, len, bl, fadvise_flags);
      }
      break;

    case Transaction::OP_ZERO:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        uint64_t len = op->len;
	r = _zero(cid, oid, off, len);
      }
      break;

    case Transaction::OP_TRIMCACHE:
      {
        // deprecated, no-op
      }
      break;

    case Transaction::OP_TRUNCATE:
      {
        const ghobject_t& oid = i.get_oid(op->oid);
        uint64_t off = op->off;
	r = _truncate(c, oid, off);
      }
      break;

    case Transaction::OP_REMOVE:
      {
        const ghobject_t& oid = i.get_oid(op->oid);
	r = _remove(c, oid);
      }
      break;

    case Transaction::OP_SETATTR:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
        string name = i.decode_string();
        bufferlist bl;
        i.decode_bl(bl);
	map<string, bufferptr> to_set;
	to_set[name] = bufferptr(bl.c_str(), bl.length());
	r = _setattrs(c, oid, to_set);
      }
      break;

    case Transaction::OP_SETATTRS:
      {
        const ghobject_t& oid = i.get_oid(op->oid);
        map<string, bufferptr> aset;
        i.decode_attrset(aset);
	r = _setattrs(c, oid, aset);
      }
      break;

    case Transaction::OP_RMATTR:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
	string name = i.decode_string();
	r = _rmattr(c, oid, name);
      }
      break;

    case Transaction::OP_RMATTRS:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
	r = _rmattrs(c, oid);
      }
      break;

    case Transaction::OP_CLONE:
      {
        const ghobject_t& oid = i.get_oid(op->oid);
        const ghobject_t& noid = i.get_oid(op->dest_oid);
	r = _clone(c, oid, noid);
      }
      break;

    case Transaction::OP_CLONERANGE:
      assert(0 == "deprecated");
      break;

    case Transaction::OP_CLONERANGE2:
      {
        const ghobject_t &oid = i.get_oid(op->oid);
        const ghobject_t &noid = i.get_oid(op->dest_oid);
        uint64_t srcoff = op->off;
        uint64_t len = op->len;
        uint64_t dstoff = op->dest_off;
	assert(0 == "write me");
	//r = _clone_range(cid, oid, noid, srcoff, len, dstoff);
      }
      break;

    case Transaction::OP_MKCOLL:
      {
	assert(!c);
        coll_t cid = i.get_cid(op->cid);
	r = _create_collection(cid);
      }
      break;

    case Transaction::OP_COLL_HINT:
      {
        coll_t cid = i.get_cid(op->cid);
        uint32_t type = op->hint_type;
        bufferlist hint;
        i.decode_bl(hint);
        bufferlist::iterator hiter = hint.begin();
        if (type == Transaction::COLL_HINT_EXPECTED_NUM_OBJECTS) {
          uint32_t pg_num;
          uint64_t num_objs;
          ::decode(pg_num, hiter);
          ::decode(num_objs, hiter);
          r = _collection_hint_expected_num_objs(cid, pg_num, num_objs);
        } else {
          // Ignore the hint
          dout(10) << "Unrecognized collection hint type: " << type << dendl;
        }
      }
      break;

    case Transaction::OP_RMCOLL:
      {
        coll_t cid = i.get_cid(op->cid);
	r = _destroy_collection(cid);
      }
      break;

    case Transaction::OP_COLL_ADD:
      {
        coll_t ocid = i.get_cid(op->cid);
        coll_t ncid = i.get_cid(op->dest_cid);
        ghobject_t oid = i.get_oid(op->oid);
	r = _collection_add(ncid, ocid, oid);
      }
      break;

    case Transaction::OP_COLL_REMOVE:
       {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
	r = _remove(cid, oid);
       }
      break;

    case Transaction::OP_COLL_MOVE:
      assert(0 == "deprecated");
      break;

    case Transaction::OP_COLL_MOVE_RENAME:
      {
        coll_t oldcid = i.get_cid(op->cid);
        ghobject_t oldoid = i.get_oid(op->oid);
        coll_t newcid = i.get_cid(op->dest_cid);
        ghobject_t newoid = i.get_oid(op->dest_oid);
	r = _collection_move_rename(oldcid, oldoid, newcid, newoid);
      }
      break;

    case Transaction::OP_COLL_SETATTR:
      {
        coll_t cid = i.get_cid(op->cid);
        string name = i.decode_string();
        bufferlist bl;
        i.decode_bl(bl);
	assert(0 == "not implemented");
      }
      break;

    case Transaction::OP_COLL_RMATTR:
      {
        coll_t cid = i.get_cid(op->cid);
        string name = i.decode_string();
	assert(0 == "not implemented");
      }
      break;

    case Transaction::OP_COLL_RENAME:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
	r = -EOPNOTSUPP;
      }
      break;

    case Transaction::OP_OMAP_CLEAR:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
	r = _omap_clear(cid, oid);
      }
      break;
    case Transaction::OP_OMAP_SETKEYS:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        map<string, bufferlist> aset;
        i.decode_attrset(aset);
	r = _omap_setkeys(cid, oid, aset);
      }
      break;
    case Transaction::OP_OMAP_RMKEYS:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        set<string> keys;
        i.decode_keyset(keys);
	r = _omap_rmkeys(cid, oid, keys);
      }
      break;
    case Transaction::OP_OMAP_RMKEYRANGE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        string first, last;
        first = i.decode_string();
        last = i.decode_string();
	r = _omap_rmkeyrange(cid, oid, first, last);
      }
      break;
    case Transaction::OP_OMAP_SETHEADER:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        bufferlist bl;
        i.decode_bl(bl);
	r = _omap_setheader(cid, oid, bl);
      }
      break;
    case Transaction::OP_SPLIT_COLLECTION:
      assert(0 == "deprecated");
      break;
    case Transaction::OP_SPLIT_COLLECTION2:
      {
        coll_t cid = i.get_cid(op->cid);
        uint32_t bits = op->split_bits;
        uint32_t rem = op->split_rem;
        coll_t dest = i.get_cid(op->dest_cid);
	r = _split_collection(cid, bits, rem, dest);
      }
      break;

    case Transaction::OP_SETALLOCHINT:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
      }
      break;

    default:
      derr << "bad op " << op->op << dendl;
      assert(0);
    }

    if (r < 0) {
      bool ok = false;

      if (r == -ENOENT && !(op->op == Transaction::OP_CLONERANGE ||
			    op->op == Transaction::OP_CLONE ||
			    op->op == Transaction::OP_CLONERANGE2 ||
			    op->op == Transaction::OP_COLL_ADD))
	// -ENOENT is usually okay
	ok = true;
      if (r == -ENODATA)
	ok = true;

      if (!ok) {
	const char *msg = "unexpected error code";

	if (r == -ENOENT && (op->op == Transaction::OP_CLONERANGE ||
			     op->op == Transaction::OP_CLONE ||
			     op->op == Transaction::OP_CLONERANGE2))
	  msg = "ENOENT on clone suggests osd bug";

	if (r == -ENOSPC)
	  // For now, if we hit _any_ ENOSPC, crash, before we do any damage
	  // by partially applying transactions.
	  msg = "ENOSPC handling not implemented";

	if (r == -ENOTEMPTY) {
	  msg = "ENOTEMPTY suggests garbage data in osd data dir";
	  dump_all();
	}

	dout(0) << " error " << cpp_strerror(r) << " not handled on operation " << op->op
		<< " (op " << pos << ", counting from 0)" << dendl;
	dout(0) << msg << dendl;
	dout(0) << " transaction dump:\n";
	JSONFormatter f(true);
	f.open_object_section("transaction");
	t.dump(&f);
	f.close_section();
	f.flush(*_dout);
	*_dout << dendl;
	assert(0 == "unexpected error");
      }
    }

    ++pos;
  }
}
