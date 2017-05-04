#
# WARNING: do not run this directly, it should be run by the master Makefile

include ../../../Makefile.dirs
include ../../../Makefile.defs
include ../../../Makefile.targets

COREPATH=../../..
NAME=KazooDB
UTIL_SRC_NAME=shell
auto_gen=


DEFS+=-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DHAVE_LINENOISE=1 -DSQLITE_SHELL_DBNAME_PROC=kazoo_db_filename
## -DHAVE_READLINE=1 -DHAVE_EDITLINE=1
##-DHAVE_LINENOISE=1
##LIBS= -lreadline -editline

include ../../../Makefile.utils

modules:
