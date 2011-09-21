import random
import struct

from nose import with_setup
from nose.tools import eq_ as eq, assert_raises
from rados import Rados
from rbd import RBD, Image, ImageNotFound, InvalidArgument


rados = None
ioctx = None
IMG_NAME = 'foo'
IMG_SIZE = 8 << 20 # 8 MiB
IMG_ORDER = 22 # 4 MiB objects

def setUp():
    global rados
    rados = Rados()
    rados.conf_read_file()
    rados.connect()
    assert rados.pool_exists('rbd')
    global ioctx
    ioctx = rados.open_ioctx('rbd')

def tearDown():
    global ioctx
    ioctx.__del__()
    global rados
    rados.shutdown()

def create_image():
    RBD().create(ioctx, IMG_NAME, IMG_SIZE, IMG_ORDER)

def remove_image():
    RBD().remove(ioctx, IMG_NAME)
    
def test_version():
    RBD().version()

def test_create():
    create_image()
    remove_image()

def test_remove_dne():
    assert_raises(ImageNotFound, remove_image)

@with_setup(create_image, remove_image)
def test_list():
    eq([IMG_NAME], RBD().list(ioctx))

@with_setup(create_image, remove_image)
def test_rename():
    rbd = RBD()
    rbd.rename(ioctx, IMG_NAME, IMG_NAME + '2')
    eq([IMG_NAME + '2'], rbd.list(ioctx))
    rbd.rename(ioctx, IMG_NAME + '2', IMG_NAME)
    eq([IMG_NAME], rbd.list(ioctx))

def rand_data(size):
    l = [random.Random().getrandbits(64) for _ in xrange(size/8)]
    return struct.pack((size/8)*'Q', *l)

def check_stat(info, size, order):
    assert 'block_name_prefix' in info
    eq(info['size'], size)
    eq(info['order'], order)
    eq(info['num_objs'], size / (1 << order))
    eq(info['obj_size'], 1 << order)
    eq(info['parent_pool'], -1)
    eq(info['parent_name'], '')

class TestImage(object):

    def setUp(self):
        self.rbd = RBD()
        create_image()
        self.image = Image(ioctx, IMG_NAME)

    def tearDown(self):
        self.image.close()
        remove_image()

    def test_stat(self):
        info = self.image.stat()
        check_stat(info, IMG_SIZE, IMG_ORDER)

    def test_write(self):
        data = rand_data(256)
        self.image.write(data, 0)

    def test_read(self):
        data = self.image.read(0, 20)
        eq(data, '\0' * 20)

    def test_write_read(self):
        data = rand_data(256)
        offset = 50
        self.image.write(data, offset)
        read = self.image.read(offset, 256)
        eq(data, read)

    def test_read_bad_offset(self):
        assert_raises(InvalidArgument, self.image.read, IMG_SIZE + 1, IMG_SIZE)

    def test_resize(self):
        new_size = IMG_SIZE * 2
        self.image.resize(new_size)
        info = self.image.stat()
        check_stat(info, new_size, IMG_ORDER)

    def test_copy(self):
        global ioctx
        data = rand_data(256)
        self.image.write(data, 256)
        bytes_copied = self.image.copy(ioctx, IMG_NAME + '2')
        copy = Image(ioctx, IMG_NAME + '2')
        copy_data = copy.read(256, 256)
        copy.close()
        self.rbd.remove(ioctx, IMG_NAME + '2')
        eq(bytes_copied, IMG_SIZE)
        eq(data, copy_data)

    def test_create_snap(self):
        global ioctx
        self.image.create_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)
        data = rand_data(256)
        self.image.write(data, 0)
        read = self.image.read(0, 256)
        eq(read, data)
        at_snapshot = Image(ioctx, IMG_NAME, 'snap1')
        snap_data = at_snapshot.read(0, 256)
        at_snapshot.close()
        eq(snap_data, '\0' * 256)

    def test_list_snaps(self):
        eq([], list(self.image.list_snaps()))
        self.image.create_snap('snap1')
        eq(['snap1'], map(lambda snap: snap['name'], self.image.list_snaps()))
        self.image.create_snap('snap2')
        eq(['snap1', 'snap2'], map(lambda snap: snap['name'], self.image.list_snaps()))

    def test_remove_snap(self):
        eq([], list(self.image.list_snaps()))
        self.image.create_snap('snap1')
        eq(['snap1'], map(lambda snap: snap['name'], self.image.list_snaps()))
        self.image.remove_snap('snap1')
        eq([], list(self.image.list_snaps()))

    def test_rollback_to_snap(self):
        self.image.write('\0' * 256, 0)
        self.image.create_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)
        data = rand_data(256)
        self.image.write(data, 0)
        read = self.image.read(0, 256)
        eq(read, data)
        self.image.rollback_to_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)

    def test_rollback_to_snap_sparse(self):
        self.image.create_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)
        data = rand_data(256)
        self.image.write(data, 0)
        read = self.image.read(0, 256)
        eq(read, data)
        self.image.rollback_to_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)

    def test_set_snap(self):
        self.image.write('\0' * 256, 0)
        self.image.create_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)
        data = rand_data(256)
        self.image.write(data, 0)
        read = self.image.read(0, 256)
        eq(read, data)
        self.image.set_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)

    def test_set_snap_sparse(self):
        self.image.create_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)
        data = rand_data(256)
        self.image.write(data, 0)
        read = self.image.read(0, 256)
        eq(read, data)
        self.image.set_snap('snap1')
        read = self.image.read(0, 256)
        eq(read, '\0' * 256)
