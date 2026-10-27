# parcel-host — the kernel's own file_map/ built for the host, unchanged
# (tools/parcel_host.h stands in for the kernel headers it reaches), and what a
# write into a file of 1 GiB keeps of the heap held to one Parcel and the
# record's head (tools/parcel_host.c, spec_v2 §10.4): seconds, no machine. Not
# a part of `make`; a harness asks for it.
PARCEL_HOST      := $(BUILDDIR)/parcel_host
PARCEL_HOST_SRCS := tools/parcel_host.c $(addprefix $(SRCDIR)/kernel/tagfs/file_map/,file_record.c \
                    file_record_splices.c tract.c)
$(PARCEL_HOST): $(PARCEL_HOST_SRCS) tools/parcel_host.h $(wildcard $(SRCDIR)/kernel/tagfs/file_map/*.h) \
                $(SRCDIR)/include/volume_tally.h $(SRCDIR)/include/volume_book.h | $(BUILDDIR)
	@$(CC_HOST) -O2 -Wall -Wextra -include tools/parcel_host.h -I$(SRCDIR)/include \
	    -I$(SRCDIR)/kernel/tagfs/file_map -o $@ $(PARCEL_HOST_SRCS)

.PHONY: parcel-host
parcel-host: $(PARCEL_HOST)
	@$(PARCEL_HOST)
