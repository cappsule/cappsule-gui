SUBDIRS := agent-linux daemon qubes-drv
CLEAN-SUBDIRS := $(addprefix clean-,$(SUBDIRS))
STRIP-SUBDIRS := $(addprefix strip-,$(SUBDIRS))

.PHONY: all clean strip $(SUBDIRS) $(CLEAN-SUBDIRS) $(STRIP-SUBDIRS)

all: $(SUBDIRS)
strip: $(STRIP-SUBDIRS)
clean: $(CLEAN-SUBDIRS)

# common/*.o are required by several targets, and sometimes lead to an
# error with make -j.
# Building common/ before other targets seems to solve this issue.
$(SUBDIRS):
	$(MAKE) -C common
	$(MAKE) -C $@

$(STRIP-SUBDIRS): strip-%:
	$(MAKE) -C $* strip

$(CLEAN-SUBDIRS): clean-%:
	$(MAKE) -C $* clean
