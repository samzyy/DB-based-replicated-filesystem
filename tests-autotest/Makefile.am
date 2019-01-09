EXTRA_DIST = testsuite.at.in testsuite $(TESTSUITE)
CONFIG_CLEAN_FILES = atconfig atlocal package.m4 testsuite testsuite.log
TESTSUITE = $(top_builddir)/$(subdir)/testsuite
check-local: atconfig atlocal $(TESTSUITE) timeout
	$(SHELL) $(TESTSUITE)
	rm -fr $(subdir)/testsuite.dir

check_PROGRAMS = timeout
timeout_SOURCES = timeout.c

AUTOTEST = $(AUTOM4TE) --language=autotest
testsuite $(TESTSUITE): testsuite.at $(srcdir)/package.m4
	@echo Making $@...
	$(AUTOTEST) -I $(srcdir)/$(srcdir) $< | sed -e 's/@at_testdir@/$(subdir)/g' > $@.tmp && mv $@.tmp $@

atconfig: $(top_builddir)/config.status
	cd $(top_builddir) && \
	  $(SHELL) ./config.status $(subdir)/$@

$(srcdir)/package.m4: $(top_srcdir)/configure.in
	@echo Making $@...
	@{	\
	  echo 'm4_define([AT_PACKAGE_NAME], 		[@PACKAGE_NAME@])'; \
	  echo 'm4_define([AT_PACKAGE_TARNAME], 	[@PACKAGE_TARNAME@])'; \
	  echo 'm4_define([AT_PACKAGE_VERSION], 	[@PACKAGE_VERSION@])'; \
	  echo 'm4_define([AT_PACKAGE_STRING], 		[@PACKAGE_STRING@])'; \
	  echo 'm4_define([AT_PACKAGE_BUGREPORT], 	[@PACKAGE_BUGREPORT@])'; \
	} >$@


