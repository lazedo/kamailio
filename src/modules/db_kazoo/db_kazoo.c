/*
 * $Id$
 *
 * KazooDB module interface
 *
 * Copyright (C) 2010 Timo Teräs
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <sys/time.h>

#include "../../core/sr_module.h"
#include "../../lib/srdb1/db_query.h"
#include "../../lib/srdb1/db.h"
#include "dbase.h"

MODULE_VERSION


static int db_kazoo_bind_api(db_func_t *dbb)
{
	if (dbb == NULL)
		return -1;

	memset(dbb, 0, sizeof(db_func_t));

	dbb->use_table = db_kazoo_use_table;
	dbb->init = db_kazoo_init;
	dbb->close = db_kazoo_close;
	dbb->query = db_kazoo_query;
	dbb->fetch_result = db_kazoo_fetch_result;
	dbb->raw_query = db_kazoo_raw_query;
	dbb->free_result = db_kazoo_free_result;
	dbb->insert = db_kazoo_insert;
	dbb->delete = db_kazoo_delete;
	dbb->update = db_kazoo_update;
	dbb->replace = db_kazoo_replace;
	dbb->last_inserted_id = db_kazoo_last_inserted_id;
//	dbb->insert_update    = db_kazoo_insert_update;
//	dbb->insert_delayed   = db_kazoo_insert_delayed;
	dbb->affected_rows    = db_kazoo_affected_rows;
//	dbb->start_transaction= db_kazoo_start_transaction;
//	dbb->end_transaction  = db_kazoo_end_transaction;
//	dbb->abort_transaction= db_kazoo_abort_transaction;

	return 0;
}

static cmd_export_t cmds[] = {
		{ "db_bind_api", (cmd_function) db_kazoo_bind_api, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 }
};

int mod_register(char *path, int *dlflags, void *p1, void *p2)
{
	if(db_kazoo_alloc_buffer()<0)
		return -1;
	return 0;
}

static int db_kazoo_mod_init(void)
{
	if(db_kazoo_lock_init())
		return -1;

	sqlite3_initialize();
	LM_INFO("KazooDB library version %s (compiled using %s)\n", sqlite3_libversion(), SQLITE_VERSION);
	return 0;
}

static void db_kazoo_mod_destroy(void)
{
	LM_INFO("KazooDB terminate\n");
	sqlite3_shutdown();
	db_kazoo_lock_destroy();
}

struct module_exports exports = {
		"db_kazoo",
		DEFAULT_DLFLAGS, /* dlopen flags */
		cmds, /* module commands */
		0, /* module parameters */
		0, /* exported statistics */
		0, /* exported MI functions */
		0, /* exported pseudo-variables */
		0, /* extra processes */
		db_kazoo_mod_init, /* module initialization function */
		0, /* response function*/
		db_kazoo_mod_destroy, /* destroy function */
		0 /* per-child init function */
};

