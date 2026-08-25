SUBDIRS := 01_closure_materialization 02_callback_state 03_subobject_invalidation 04_receiver_field_state 05_template_semantic_target 06_dependency_import_conflict 07_composite_dispatch 08_anchor_constrained_witness 99_negative_controls

# Extra compiler flags, empty by default; usage: make EXTRA_CXXFLAGS="-O2 -g"
EXTRA_CXXFLAGS ?=

all:
	@for d in $(SUBDIRS); do echo "== $$d =="; $(MAKE) -C $$d all EXTRA_CXXFLAGS="$(EXTRA_CXXFLAGS)" || exit $$?; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || true; done

.PHONY: all clean
