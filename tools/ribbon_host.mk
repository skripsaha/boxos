# ribbon-host — where a book's writer goes on past the end of its ribbon and
# where each stitch it lays ends, held on the host with the kernel's own rules
# in volume_ribbon.h (tools/ribbon_host.c, spec_v2 §1.5, §16.2 п.11, §16.6
# п.2): never an atom smaller than the medium's, and on a removable medium
# every stitch ending on a flash page — seconds, no machine. Not a part of
# `make`; a harness asks for it.
RIBBON_HOST := $(BUILDDIR)/ribbon_host
$(RIBBON_HOST): tools/ribbon_host.c $(SRCDIR)/include/volume_ribbon.h $(SRCDIR)/include/volume_tally.h \
                $(SRCDIR)/include/volume_book.h | $(BUILDDIR)
	@$(CC_HOST) -O2 -Wall -Wextra -I$(SRCDIR)/include -o $@ tools/ribbon_host.c

.PHONY: ribbon-host
ribbon-host: $(RIBBON_HOST)
	@$(RIBBON_HOST)
