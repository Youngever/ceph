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
  void dump(Formatter *f) {
    f->dump_unsigned("fset", fset);
    f->dump_unsigned("fno", fno);
  }
  static void generate_test_instances(list<onode_t*>& o) {
    o->push_back(new fid_t());
    o->push_back(new fid_t(0, 1));
    o->push_back(new fid_t(123, 3278));
  }
};

static inline operator<<(ostream& out, const fid_t& fid) {
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
  void dump(Formatter *f);
  static void generate_test_instances(list<fragment_t*>& o);
};

/// onode: per-object metadata
struct onode_t {
  uint64_t size;                       ///< object size
  map<string, bufferlist> attrs;       ///< attrs
  map<uint64_t, fragment_t> data_map;  ///< data (offset to fragment mapping)
  uint64_t omap_head;                  ///< id for omap root node

  onode_t() : size(0), omap_head(0) {}

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& p);
  void dump(Formatter *f);
  static void generate_test_instances(list<onode_t*>& o);
};

#endif
