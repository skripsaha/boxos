# tip-in-host — TipIn's choice of what a Tally lays anew and what it names as
# it lies, held on the host with the kernel's own settle/tip_in.c, settle/
# dog_ear.c and file_map/ (tools/dog_ear_host.h stands in for the rest of the
# kernel; tools/tip_in_host.c, spec_v2 §4.1, §16.5 п.16, §10.4 step 2.4): a
# leaf laid anew for each changed key and a neighbour beside each row, the
# inner nodes above them and no other; a file of 1 GiB written over 10 000
# times, a Tally after each write, its nodes counted — seconds, no machine. Not
# a part of `make`; a harness asks for it.
TIP_IN_HOST      := $(BUILDDIR)/tip_in_host
TIP_IN_HOST_SRCS := tools/tip_in_host.c $(SRCDIR)/kernel/tagfs/settle/tip_in.c $(SRCDIR)/kernel/tagfs/settle/dog_ear.c \
                    $(addprefix $(SRCDIR)/kernel/tagfs/file_map/,file_record.c file_record_splices.c tract.c)
$(TIP_IN_HOST): $(TIP_IN_HOST_SRCS) tools/dog_ear_host.h tools/parcel_host.h \
                $(SRCDIR)/kernel/tagfs/settle/tip_in.h $(SRCDIR)/kernel/tagfs/settle/dog_ear.h \
                $(wildcard $(SRCDIR)/kernel/tagfs/file_map/*.h) $(SRCDIR)/kernel/tagfs/mount/tally_read.h \
                $(SRCDIR)/include/volume_tally.h | $(BUILDDIR)
	@$(CC_HOST) -O2 -Wall -Wextra -include tools/dog_ear_host.h -Itools -I$(SRCDIR)/include \
	    -I$(SRCDIR)/kernel/tagfs/file_map -o $@ $(TIP_IN_HOST_SRCS)

.PHONY: tip-in-host
tip-in-host: $(TIP_IN_HOST)
	@$(TIP_IN_HOST)
