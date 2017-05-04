#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct stat st = {0};

void chk_path(const char* Path) {
	if (stat(Path, &st) == -1) {
	    mkdir(Path, 0777);
	}
}

void kazoo_db_filename(const char** ptrToFilename)
{
	chk_path("/etc/kazoo/kamailio/db");
	*ptrToFilename = "/etc/kazoo/kamailio/db/kazoo.db";
}
