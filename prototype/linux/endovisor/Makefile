# Kbuild file for the endovisor.
#
# Built from the parent directory: make module DIR=linux/endovisor
#
# The module carries no kernelet image: an image is a file of its own, which
# the holder registers as a kind (KLET_KIND_REGISTER) and image.c loads and
# relocates. One endovisor.ko serves every kernel proper.
obj-m += endovisor.o

endovisor-y := main.o image.o sandbox.o carrier.o space.o evict.o watch.o sched.o vsock.o switch.o

# objtool does not understand a function that returns on a different stack
# than it was called on.
OBJECT_FILES_NON_STANDARD_switch.o := y
