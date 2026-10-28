# dog-ear-host — DogEar, the keys a book's trees changed since the Tally the
# next one is laid against, held on the host with the kernel's own
# settle/dog_ear.c and file_map/ (tools/dog_ear_host.h stands in for the rest
# of the kernel; tools/dog_ear_host.c, spec_v2 §16.5 п.9): spans joined, taken
# by a Tally and given back, lost whole to a heap that refuses them, and a
# change of a file held to its two records Parcel by Parcel and block by
# block — seconds, no machine. Not a part of `make`; a harness asks for it.
DOG_EAR_HOST      := $(BUILDDIR)/dog_ear_host
DOG_EAR_HOST_SRCS := tools/dog_ear_host.c $(SRCDIR)/kernel/tagfs/settle/dog_ear.c \
                     $(addprefix $(SRCDIR)/kernel/tagfs/file_map/,file_record.c file_record_splices.c tract.c)
$(DOG_EAR_HOST): $(DOG_EAR_HOST_SRCS) tools/dog_ear_host.h tools/parcel_host.h \
                 $(SRCDIR)/kernel/tagfs/settle/dog_ear.h $(wildcard $(SRCDIR)/kernel/tagfs/file_map/*.h) \
                 $(SRCDIR)/kernel/tagfs/mount/tally_read.h $(SRCDIR)/include/volume_tally.h | $(BUILDDIR)
	@$(CC_HOST) -O2 -Wall -Wextra -include tools/dog_ear_host.h -Itools -I$(SRCDIR)/include \
	    -I$(SRCDIR)/kernel/tagfs/file_map -o $@ $(DOG_EAR_HOST_SRCS)

.PHONY: dog-ear-host
dog-ear-host: $(DOG_EAR_HOST)
	@$(DOG_EAR_HOST)
