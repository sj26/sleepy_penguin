all::
RSYNC_DEST := yhbt.net:/srv/yhbt/sleepy_penguin
rfpackage := sleepy_penguin
include pkg.mk
pkg_extra += ext/sleepy_penguin/git_version.h
.PHONY: .FORCE-GIT-VERSION-FILE doc test $(test_units) manifest
