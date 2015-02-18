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

#ifndef CEPH_OSD_NEWSTORE_TYPES_H
#define CEPH_OSD_NEWSTORE_TYPES_H

#include <ostream>
#include "include/types.h"

namespace ceph {
  class Formatter;
}

/// unique id for a local file
struct fid_t {
  uint32_t fset, fno;
  fid_t() : fset(0), fno(0) {}
  fid_t(uint32_t s, uint32_t n) : fset(s), fno(n) {}

  void encode(bufferlist& bl) const {
    ::encode(fset, bl);
    ::encode(fno, bl);
  }
  void decode(bufferlist::iterator& p) {
    ::decode(fset, p);
    ::decode(fno, p);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<fid_t*>& o);
};
WRITE_CLASS_ENCODER(fid_t)

static inline ostream& operator<<(ostream& out, const fid_t& fid) {
  return out << fid.fset << "/" << fid.fno;
}

/// fragment: a byte extent backed by a file
struct fragment_t {
  uint32_t offset;   ///< offset in file to first byte of this fragment
  uint32_t length;   ///< length of fragment/extent
  fid_t fid;         ///< file backing this fragment

  fragment_t() : offset(0), length(0) {}
  fragment_t(uint32_t o, uint32_t l) : offset(o), length(l) {}
  fragment_t(uint32_t o, uint32_t l, fid_t f) : offset(o), length(l), fid(f) {}

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& p);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<fragment_t*>& o);
};
WRITE_CLASS_ENCODER(fragment_t)

/// onode: per-object metadata
struct onode_t {
  uint64_t size;                       ///< object size
  map<string, bufferptr> attrs;        ///< attrs
  map<uint64_t, fragment_t> data_map;  ///< data (offset to fragment mapping)
  uint64_t omap_head;                  ///< id for omap root node

  onode_t() : size(0), omap_head(0) {}

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& p);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<onode_t*>& o);
};
WRITE_CLASS_ENCODER(onode_t)


/// writeahead-logged op
struct wal_op_t {
  typedef enum {
    OP_WRITE = 1,
    OP_TRUNCATE = 3,
    OP_ZERO = 4,
    OP_REMOVE = 5,
  } type_t;
  __u8 op;
  fid_t fid;
  uint64_t offset, length;
  bufferlist bl;

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& p);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<wal_op_t*>& o);
};
WRITE_CLASS_ENCODER(wal_op_t)


/// writeahead-logged transaction
struct wal_transaction_t {
  uint64_t seq;
  list<wal_op_t> ops;

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& p);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<wal_transaction_t*>& o);
};
WRITE_CLASS_ENCODER(wal_transaction_t)

#endif
