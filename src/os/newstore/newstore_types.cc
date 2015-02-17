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

#include "newstore_types.h"
#include "common/Formatter.h"

void fid_t::dump(Formatter *f) const
{
  f->dump_unsigned("fset", fset);
  f->dump_unsigned("fno", fno);
}

void fid_t::generate_test_instances(list<fid_t*>& o)
{
  o.push_back(new fid_t());
  o.push_back(new fid_t(0, 1));
  o.push_back(new fid_t(123, 3278));
}

void fragment_t::encode(bufferlist& bl) const
{
  ENCODE_START(1, 1, bl);
  ::encode(offset, bl);
  ::encode(length, bl);
  ::encode(fid, bl);
  ENCODE_FINISH(bl);
}

void fragment_t::decode(bufferlist::iterator& p)
{
  DECODE_START(1, p);
  ::decode(offset, p);
  ::decode(length, p);
  ::decode(fid, p);
  DECODE_FINISH(p);
}

void fragment_t::dump(Formatter *f) const
{
  f->dump_unsigned("offset", offset);
  f->dump_unsigned("length", length);
  f->dump_object("fid", fid);
}

void fragment_t::generate_test_instances(list<fragment_t*>& o)
{
  o.push_back(new fragment_t());
  o.push_back(new fragment_t(123, 456));
  o.push_back(new fragment_t(789, 1024, fid_t(3, 400)));
}


void onode_t::encode(bufferlist& bl) const
{
  ENCODE_START(1, 1, bl);
  ::encode(size, bl);
  ::encode(attrs, bl);
  ::encode(data_map, bl);
  ::encode(omap_head, bl);
  ENCODE_FINISH(bl);
}

void onode_t::decode(bufferlist::iterator& p)
{
  DECODE_START(1, p);
  ::decode(size, p);
  ::decode(attrs, p);
  ::decode(data_map, p);
  ::decode(omap_head, p);
  DECODE_FINISH(p);
}

void onode_t::dump(Formatter *f) const
{
  f->dump_unsigned("size", size);
  f->open_object_section("attrs");
  for (map<string,bufferptr>::const_iterator p = attrs.begin();
       p != attrs.end(); ++p) {
    f->open_object_section("attr");
    f->dump_string("name", p->first);
    f->dump_unsigned("len", p->second.length());
    f->close_section();
  }
  f->open_object_section("data_map");
  for (map<uint64_t, fragment_t>::const_iterator p = data_map.begin();
       p != data_map.end(); ++p) {
    f->open_object_section("fragment");
    f->dump_unsigned("fragment_offset", p->first);
    p->second.dump(f);
    f->close_section();
  }
  f->close_section();
  f->dump_unsigned("omap_head", omap_head);
}

void onode_t::generate_test_instances(list<onode_t*>& o)
{
  o.push_back(new onode_t());
  // FIXME
}
