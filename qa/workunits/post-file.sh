#!/bin/bash -ex

what="$1"
[ -z "$what" ] && what=/usr/share/aclocal
ceph-post-file -d ceph-test-workunit $what

echo OK
