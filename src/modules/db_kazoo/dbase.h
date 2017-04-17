/*
 * $Id$
 *
 * SQlite module core functions
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

#ifndef DBASE_H
#define DBASE_H

#include "../../lib/srdb1/db_pool.h"
#include "../../lib/srdb1/db_con.h"
#include "../../lib/srdb1/db_res.h"
#include "../../lib/srdb1/db_key.h"
#include "../../lib/srdb1/db_op.h"
#include "../../lib/srdb1/db_val.h"

#include "sqlite3.h"

#define DB_SQLITE_MAX_BINDS 64

struct db_kazoo_connection {
	struct pool_con hdr;

	sqlite3 *conn;
	int bindpos;

	sqlite3_stmt *stmt;
	const db_val_t *bindarg[DB_SQLITE_MAX_BINDS];
	time_t timestamp;        /*!< Timestamp of last query */
	int transaction;         /*!< Multi-query transaction is currently open */
	int lockedtables;        /*!< Table locks were aquired */
	int locked;
};

#define CON_DB_KAZOO(db_con)     ((struct db_kazoo_connection *) db_con->tail)
#define CON_CONNECTION(db_con)   (((struct db_kazoo_connection*)((db_con)->tail))->conn)
#define CON_TIMESTAMP(db_con)    (((struct db_kazoo_connection*)((db_con)->tail))->timestamp)
#define CON_TRANSACTION(db_con)  (((struct db_kazoo_connection*)((db_con)->tail))->transaction)
#define CON_LOCKEDTABLES(db_con) (((struct db_kazoo_connection*)((db_con)->tail))->lockedtables)
#define CON_LOCKED(db_con)       (((struct db_kazoo_connection*)((db_con)->tail))->locked)


/*
 * Some convenience wrappers
 */


int db_kazoo_lock_init(void);
int db_kazoo_lock_destroy(void);

db1_con_t* db_kazoo_init(const str* _sqlurl);
void db_kazoo_close(db1_con_t* _h);

int db_kazoo_free_result(const db1_con_t* _h, db1_res_t* _r);

int db_kazoo_query(const db1_con_t* _h, const db_key_t* _k, const db_op_t* _op, const db_val_t* _v, const db_key_t* _c, int _n, int _nc, const db_key_t _o,
                   db1_res_t** _r);
int db_kazoo_insert(const db1_con_t* _h, const db_key_t* _k, const db_val_t* _v, int _n);
int db_kazoo_delete(const db1_con_t* _h, const db_key_t* _k, const db_op_t* _o, const db_val_t* _v, int _n);
int db_kazoo_update(const db1_con_t* _h, const db_key_t* _k, const db_op_t* _o, const db_val_t* _v, const db_key_t* _uk, const db_val_t* _uv, int _n, int _un);
int db_kazoo_replace(const db1_con_t* _h, const db_key_t* _k, const db_val_t* _v, const int _n, const int _un, const int _m);
int db_kazoo_raw_query(const db1_con_t* _h, const str* _s, db1_res_t** _r);

int db_kazoo_use_table(db1_con_t* _h, const str* _t);

int db_kazoo_alloc_buffer(void);
int db_kazoo_abort_transaction(const db1_con_t* _h);
int db_kazoo_end_transaction(const db1_con_t* _h);
//int db_kazoo_unlock_tables(db1_con_t* _h);
int db_kazoo_start_transaction(const db1_con_t* _h, db_locking_t _l);
int db_kazoo_affected_rows(const db1_con_t* _h);
int db_kazoo_last_inserted_id(const db1_con_t* _h);
int db_kazoo_fetch_result(const db1_con_t* _h, db1_res_t** _r, const int nrows);
int db_kazoo_sql(const db1_con_t* _h, const str* _s);

int db_kazoo_do_replace(const db1_con_t* _h, const db_key_t* _k, const db_val_t* _v,
 	const int _n, int (*val2str) (const db1_con_t*, const db_val_t*, char*,
 	int*), int (*submit_query)(const db1_con_t* _h, const str* _c));

#endif /* DBASE_H */
