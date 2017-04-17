/*
 * $Id$
 *
 * KazooDB module core functions
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

#include "../../core/mem/mem.h"
#include "../../core/ut.h"
#include "../../core/dprint.h"
#include "../../lib/srdb1/db_pool.h"
#include "../../lib/srdb1/db_ut.h"
#include "../../lib/srdb1/db_res.h"
#include "../../lib/srdb1/db_query.h"
#include "../../core/locking.h"
#include "../../core/globals.h"
#include "../../core/timer.h"

#include "dbase.h"

extern unsigned int sql_buffer_size;

static char *kazoo_sql_buf;

static time_t db_kazoo_to_timet(double rT)
{
	return 86400.0 * (rT - 2440587.5) + 0.5;
}

static double timet_to_sqlite(time_t t)
{
	return ((((double) t) - 0.5) / 86400.0) + 2440587.5;
}


static gen_lock_t *_db_kazoo_cachesem = NULL;

int db_kazoo_lock_init(void)
{
	if(!_db_kazoo_cachesem)
	{
		_db_kazoo_cachesem = lock_alloc();
		if(!_db_kazoo_cachesem)
		{
			LM_CRIT("could not alloc a lock\n");
			return -1;
		}
		if (lock_init(_db_kazoo_cachesem)==0)
		{
			LM_CRIT("could not initialize a lock\n");
			lock_dealloc(_db_kazoo_cachesem);
			return -1;
		}
	}
	return 0;
}

int db_kazoo_lock_destroy(void)
{
	if(!_db_kazoo_cachesem)
		return -1;

	lock_get(_db_kazoo_cachesem);
	lock_destroy(_db_kazoo_cachesem);
	lock_dealloc(_db_kazoo_cachesem);
	_db_kazoo_cachesem = NULL;

	return 0;

}

void db_kazoo_acquire_lock(const db1_con_t* _h)
{
	lock_get(_db_kazoo_cachesem);
	CON_LOCKED(_h) = 1;
}

void db_kazoo_release_lock(const db1_con_t* _h)
{
	if(CON_LOCKED(_h) == 1) {
		lock_release(_db_kazoo_cachesem);
	}
	CON_LOCKED(_h) = 0;
}

/*
 * Initialize database module
 * No function should be called before this
 */

static struct db_kazoo_connection * db_kazoo_new_connection(const struct db_id* id)
{
	struct db_kazoo_connection *con;
	int rc;

	con = pkg_malloc(sizeof(*con));
	if (!con) {
		LM_ERR("failed to allocate driver connection\n");
		return NULL;
	}

	memset(con, 0, sizeof(*con));
	con->hdr.ref = 1;
	con->hdr.id = (struct db_id*) id; /* set here - freed on error */

	int flags =
			  SQLITE_OPEN_CREATE
			// | SQLITE_OPEN_SHAREDCACHE
			// | SQLITE_OPEN_FULLMUTEX
			| SQLITE_OPEN_READWRITE;


	rc = sqlite3_open_v2(id->database, &con->conn, flags, NULL);
	if (rc != SQLITE_OK) {
		pkg_free(con);
		LM_ERR("failed to open sqlite database '%s'\n", id->database);
		return NULL;
	}

	return con;
}

db1_con_t* db_kazoo_init(const str* _url)
{
	db1_con_t* con = db_do_init(_url, (void *) db_kazoo_new_connection);
	if(con != NULL) {
		str read_uncommited_str = str_init("PRAGMA read_uncommitted = true;");
		db_kazoo_sql(con, &read_uncommited_str);
	}
	return con;
}

/*
 * Shut down database module
 * No function should be called after this
 */

static void db_kazoo_free_connection(struct db_kazoo_connection* con)
{
	if (!con)
		return;

	sqlite3_close(con->conn);
	free_db_id(con->hdr.id);
	pkg_free(con);
}

void db_kazoo_close(db1_con_t* _h)
{
	db_do_close(_h, db_kazoo_free_connection);
}

/*
 * Store name of table that will be used by
 * subsequent database functions
 */
int db_kazoo_use_table(db1_con_t* _h, const str* _t)
{
	return db_use_table(_h, _t);
}

/*
 * Reset query context
 */
static void db_kazoo_cleanup_query(const db1_con_t* _c)
{
	struct db_kazoo_connection *conn = CON_DB_KAZOO(_c);
	int rc;

	if (conn->stmt != NULL) {
		rc = sqlite3_finalize(conn->stmt);
		if (rc != SQLITE_OK)
			LM_ERR("finalize failed: %s\n", sqlite3_errmsg(conn->conn));
	}

	conn->stmt = NULL;
	conn->bindpos = 0;
	LM_DBG("releasing lock\n");
	db_kazoo_release_lock(_c);
}

/*
 * Convert value to sql-string as db bind index
 */
static int db_kazoo_val2str(const db1_con_t* _c, const db_val_t* _v, char* _s, int* _len)
{
	struct db_kazoo_connection *conn;
	int ret;

	if (!_c || !_v || !_s || !_len || *_len <= 0) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	conn = CON_DB_KAZOO(_c);
	if (conn->bindpos >= DB_SQLITE_MAX_BINDS) {
		LM_ERR("too many bindings, recompile with larger DB_SQLITE_MAX_BINDS\n");
		return -2;
	}

	conn->bindarg[conn->bindpos] = _v;
	ret = snprintf(_s, *_len, "?%u", ++conn->bindpos);
	if ((unsigned) ret >= (unsigned) *_len)
		return -11;

	*_len = ret;
	return 0;
}

/*
 * Release a result set from memory
 */
int db_kazoo_free_result(const db1_con_t* _h, db1_res_t* _r)
{
	if (!_h || !_r) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	db_kazoo_cleanup_query(_h);

	if (db_free_result(_r) < 0) {
		LM_ERR("failed to free result structure\n");
		return -1;
	}

	LM_DBG("releasing lock\n");
	db_kazoo_release_lock(_h);

	return 0;
}

/*
 * Send an SQL query to the server
 */
static int db_kazoo_submit_query(const db1_con_t* _h, const str* _s)
{
	struct db_kazoo_connection *conn = CON_DB_KAZOO(_h);
	sqlite3_stmt *stmt;
	const db_val_t *val;
	int rc, i;

	LM_DBG("acquiring lock\n");
	db_kazoo_acquire_lock(_h);
	LM_DBG("submit_query: %.*s\n", _s->len, _s->s);

	rc = sqlite3_prepare_v2(conn->conn, _s->s, _s->len, &stmt, NULL);
	if (rc != SQLITE_OK) {
		db_kazoo_release_lock(_h);
		LM_ERR("failed to prepare statement: %.*s : %s\n", _s->len, _s->s, sqlite3_errmsg(conn->conn));
		return -1;
	}
	conn->stmt = stmt;

	for (i = 1; i <= conn->bindpos; i++) {
		val = conn->bindarg[i - 1];
		if (VAL_NULL(val)) {
			rc = sqlite3_bind_null(stmt, i);
		} else
			switch (VAL_TYPE(val)){
			case DB1_INT:
				rc = sqlite3_bind_int(stmt, i, VAL_INT(val));
				break;
			case DB1_BIGINT:
				rc = sqlite3_bind_int64(stmt, i, VAL_BIGINT(val));
				break;
			case DB1_DOUBLE:
				rc = sqlite3_bind_double(stmt, i, VAL_DOUBLE(val));
				break;
			case DB1_STRING:
				rc = sqlite3_bind_text(stmt, i, VAL_STRING(val), -1, NULL);
				break;
			case DB1_STR:
				rc = sqlite3_bind_text(stmt, i, VAL_STR(val).s, VAL_STR(val).len, NULL);
				break;
			case DB1_DATETIME:
				rc = sqlite3_bind_double(stmt, i, timet_to_sqlite(VAL_TIME(val)));
				break;
			case DB1_BLOB:
				rc = sqlite3_bind_blob(stmt, i,
				VAL_BLOB(val).s, VAL_BLOB(val).len, NULL);
				break;
			case DB1_BITMAP:
				rc = sqlite3_bind_int(stmt, i, VAL_BITMAP(val));
				break;
			default:
				LM_ERR("unknown bind value type %d\n", VAL_TYPE(val));
				return -1;
			}
		if (rc != SQLITE_OK) {
			LM_ERR("Parameter bind failed: %s\n", sqlite3_errmsg(conn->conn));
			return -1;
		}
	}

	return 0;
}

#define H3(a,b,c)       ((a<<16) + (b<<8) + c)
#define H4(a,b,c,d)     ((a<<24) + (b<<16) + (c<<8) + d)
//#define H6(a,b,c,d,e,f) ((a<<40) + (b<<32) + (c<<24) + (d<<16) + (e<<8) + f)

static int decltype_to_dbtype(const char *decltype)
{
	/* KazooDB3 has dynamic typing. It does not store the actual
	 * exact type, instead it uses 'affinity' depending on the
	 * value. We have to go through the declaration type to see
	 * what to return.
	 * The loose string matching (4 letter substring match) is what
	 * KazooDB does internally, but our return values differ as we want
	 * the more exact srdb type instead of the affinity. */

	uint32_t h = 0;

	for (; *decltype; decltype++) {
		h <<= 8;
		h += toupper(*decltype);

		switch (h & 0x00ffffff){
		case H3('I', 'N', 'T'):
			return DB1_INT;
		}

		switch (h){
		case H4('S', 'E', 'R', 'I'): /* SERIAL */
			return DB1_INT;
		case H4('B', 'I', 'G', 'I'): /* BIGINT */
			return DB1_BIGINT;
		case H4('C', 'H', 'A', 'R'):
		case H4('C', 'L', 'O', 'B'):
		case H4('V', 'A', 'R', 'C'):
//		case H6('V', 'A', 'R', 'C', 'H', 'A'):
			return DB1_STRING;
		case H4('T', 'E', 'X', 'T'):
			return DB1_STR;
		case H4('R', 'E', 'A', 'L'):
		case H4('F', 'L', 'O', 'A'): /* FLOAT */
		case H4('D', 'O', 'U', 'B'): /* DOUBLE */
			return DB1_DOUBLE;
		case H4('B', 'L', 'O', 'B'):
			return DB1_BLOB;
		case H4('T', 'I', 'M', 'E'):
		case H4('D', 'A', 'T', 'E'):
			return DB1_DATETIME;
		}
	}

	LM_ERR("sqlite decltype '%s' not recognized, defaulting to int", decltype);
	return DB1_INT;
}

static int type_to_dbtype(int type)
{
	switch (type){
	case SQLITE_INTEGER:
		return DB1_INT;
	case SQLITE_FLOAT:
		return DB1_DOUBLE;
	case SQLITE_TEXT:
		return DB1_STR;
	case SQLITE_BLOB:
		return DB1_BLOB;
	default:
		LM_ERR("UNKKNONW %d\n", type);
		/* Unknown, or NULL column value. Assume this is a
		 * string. */
		return DB1_STR;
	}
}

static str* str_dup(const char *_s)
{
	str *s;
	int len = strlen(_s);

	s = (str*) pkg_malloc(sizeof(str) + len + 1);
	if (!s)
		return NULL;

	s->len = len;
	s->s = ((char*) s) + sizeof(str);
	memcpy(s->s, _s, len);
	s->s[len] = '\0';

	return s;
}

static void str_assign(str* s, const char *_s, int len)
{
	s->s = (char *) pkg_malloc(len + 1);
	if (s->s) {
		s->len = len;
		memcpy(s->s, _s, len);
		s->s[len] = 0;
	}
}

/*
 * Read database answer and fill the structure
 */
int db_kazoo_store_result(const db1_con_t* _h, db1_res_t** _r)
{
	struct db_kazoo_connection *conn = CON_DB_KAZOO(_h);
	db1_res_t *res;
	int i, rc, num_rows = 0, num_cols = 0, num_alloc = 0;
	db_row_t *rows = NULL, *row;
	db_val_t *val;

	res = db_new_result();
	if (res == NULL) {
		goto no_mem;
	}

	while (1) {
		rc = sqlite3_step(conn->stmt);
		if (rc == SQLITE_DONE) {
			*_r = res;
			return 0;
		}
		if (rc != SQLITE_ROW) {
			LM_INFO("sqlite3_step failed: %s\n", sqlite3_errmsg(conn->conn));
			goto err;
		}
		if (num_rows == 0) {
			/* get column types */
			num_cols = sqlite3_column_count(conn->stmt);
			if (db_allocate_columns(res, num_cols) != 0) {
				LM_ERR("ERROR ALLOCATING COLUMNS %d\n", num_cols);
				goto err;
			}
			RES_COL_N(res) = num_cols;

			for (i = 0; i < RES_COL_N(res); i++) {
				const char *decltype;
				int dbtype;

				RES_NAMES(res)[i] = str_dup(sqlite3_column_name(conn->stmt, i));
				if (RES_NAMES(res)[i] == NULL) {
					goto no_mem;
				}
				decltype = sqlite3_column_decltype(conn->stmt, i);
				if (decltype != NULL)
					dbtype = decltype_to_dbtype(decltype);
				else
					dbtype = type_to_dbtype(sqlite3_column_type(conn->stmt, i));
				RES_TYPES(res)[i] = dbtype;
			}
		}
		if (num_rows >= num_alloc) {
			if (num_alloc)
				num_alloc *= 2;
			else
				num_alloc = 8;
			rows = pkg_realloc(rows, sizeof(db_row_t) * num_alloc);
			if (rows == NULL) {
				goto no_mem;
			}
			RES_ROWS(res) = rows;
		}

		row = &RES_ROWS(res)[num_rows];
		num_rows++;
		RES_ROW_N(res) = num_rows; /* rows in this result set */
		RES_NUM_ROWS(res) = num_rows; /* rows in total */

		if (db_allocate_row(res, row) != 0) {
			goto no_mem;
		}

		for (i = 0, val = ROW_VALUES(row); i < RES_COL_N(res); i++, val++) {
			VAL_TYPE(val) = RES_TYPES(res)[i];
			VAL_NULL(val) = 0;
			VAL_FREE(val) = 0;
			if (sqlite3_column_type(conn->stmt, i) == SQLITE_NULL) {
				VAL_NULL(val) = 1;
			} else
				switch (VAL_TYPE(val)){
				case DB1_INT:
					VAL_INT(val) = sqlite3_column_int(conn->stmt, i);
					break;
				case DB1_BIGINT:
					VAL_BIGINT(val) = sqlite3_column_int64(conn->stmt, i);
					break;
				case DB1_STRING:
					/* first field of struct str* is the char* so we can just
					 * do whatever DB1_STR case does */
				case DB1_STR:
					str_assign(&VAL_STR(val), (const char*) sqlite3_column_text(conn->stmt, i), sqlite3_column_bytes(conn->stmt, i));
					if (!VAL_STR(val).s)
						goto no_mem;
					VAL_FREE(val) = 1;
					break;
				case DB1_DOUBLE:
					VAL_DOUBLE(val) = sqlite3_column_double(conn->stmt, i);
					break;
				case DB1_DATETIME:
					VAL_TIME(val) = db_kazoo_to_timet(sqlite3_column_double(conn->stmt, i));
					break;
				case DB1_BLOB:
					str_assign(&VAL_BLOB(val), (const char*) sqlite3_column_blob(conn->stmt, i), sqlite3_column_bytes(conn->stmt, i));
					if (!VAL_STR(val).s)
						goto no_mem;
					VAL_FREE(val) = 1;
					break;
				default:
					LM_ERR("unhandled db-type\n");
					goto err;
				}
		}
	}

no_mem:
	LM_ERR("no private memory left\n");
err:
	if (res)
		db_free_result(res);
	return -1;
}

/*
 * Query table for specified rows
 * _h: structure representing database connection
 * _k: key names
 * _op: operators
 * _v: values of the keys that must match
 * _c: column names to return
 * _n: number of key=values pairs to compare
 * _nc: number of columns to return
 * _o: order by the specified column
 */
int db_kazoo_query(const db1_con_t* _h, const db_key_t* _k, const db_op_t* _op, const db_val_t* _v, const db_key_t* _c, int _n, int _nc, const db_key_t _o, db1_res_t** _r)
{
	int rc;

	rc = db_do_query(_h, _k, _op, _v, _c, _n, _nc, _o, _r, db_kazoo_val2str, db_kazoo_submit_query, db_kazoo_store_result);
	db_kazoo_release_lock(_h);
//	db_kazoo_cleanup_query(_h);

	return rc;
}

static int db_kazoo_commit(const db1_con_t* _h)
{
	struct db_kazoo_connection *conn = CON_DB_KAZOO(_h);
	int rc;

	rc = sqlite3_step(conn->stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_OK && rc != SQLITE_ROW) {
		LM_ERR("sqlite commit failed: %d - %s\n", rc, sqlite3_errmsg(conn->conn));
		return -1;
	}

	return 0;
}

/*
 * Insert a row into specified table
 * _h: structure representing database connection
 * _k: key names
 * _v: values of the keys
 * _n: number of key=value pairs
 */
int db_kazoo_insert(const db1_con_t* _h, const db_key_t* _k, const db_val_t* _v, int _n)
{
	int rc = -1;

	db_kazoo_start_transaction(_h, 0);

	rc = db_do_insert(_h, _k, _v, _n, db_kazoo_val2str, db_kazoo_submit_query);
	if (rc == 0) {
		rc = db_kazoo_commit(_h);
		if(rc == 0) {
			db_kazoo_end_transaction(_h);
		} else {
			LM_ERR("INSERT failed: %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
			db_kazoo_abort_transaction(_h);
		}
	} else {
		LM_ERR("INSERT failed:  %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
	}
	db_kazoo_cleanup_query(_h);

	return rc;
}

/*
 * Delete a row from the specified table
 * _h: structure representing database connection
 * _k: key names
 * _o: operators
 * _v: values of the keys that must match
 * _n: number of key=value pairs
 */
int db_kazoo_delete(const db1_con_t* _h, const db_key_t* _k, const db_op_t* _o, const db_val_t* _v, int _n)
{
	int rc;

	db_kazoo_start_transaction(_h, 0);

	rc = db_do_delete(_h, _k, _o, _v, _n, db_kazoo_val2str, db_kazoo_submit_query);
	if (rc == 0) {
		rc = db_kazoo_commit(_h);
		if(rc == 0) {
			db_kazoo_end_transaction(_h);
		} else {
			LM_ERR("DELETE failed: %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
			db_kazoo_abort_transaction(_h);
		}
	} else {
		LM_ERR("DELETE failed:  %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
	}
	db_kazoo_cleanup_query(_h);

	return rc;
}

/*
 * Update some rows in the specified table
 * _h: structure representing database connection
 * _k: key names
 * _o: operators
 * _v: values of the keys that must match
 * _uk: updated columns
 * _uv: updated values of the columns
 * _n: number of key=value pairs
 * _un: number of columns to update
 */
int db_kazoo_update(const db1_con_t* _h, const db_key_t* _k, const db_op_t* _o, const db_val_t* _v, const db_key_t* _uk, const db_val_t* _uv, int _n, int _un)
{
	int rc;

	db_kazoo_start_transaction(_h, 0);

	rc = db_do_update(_h, _k, _o, _v, _uk, _uv, _n, _un, db_kazoo_val2str, db_kazoo_submit_query);
	if (rc == 0) {
		rc = db_kazoo_commit(_h);
		if(rc == 0) {
			db_kazoo_end_transaction(_h);
		} else {
			LM_ERR("UPDATE failed: %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
			db_kazoo_abort_transaction(_h);
		}
	} else {
		LM_ERR("UPDATE failed:  %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
	}
	db_kazoo_cleanup_query(_h);

	return rc;
}

int db_kazoo_raw_query(const db1_con_t* _h, const str* _s, db1_res_t** _r)
{
	int rc;

	db_kazoo_start_transaction(_h, 0);

	rc = db_do_raw_query(_h, _s, _r, db_kazoo_submit_query, db_kazoo_store_result);
	if (rc == 0) {
		if(rc == 0) {
			db_kazoo_end_transaction(_h);
		} else {
			LM_ERR("raw query failed: %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
			db_kazoo_abort_transaction(_h);
		}
	} else {
		LM_ERR("raw query failed: %s\n", sqlite3_errmsg(CON_CONNECTION(_h)));
	}
	db_kazoo_cleanup_query(_h);

	return rc;
}

/**
 * Just like insert, but replace the row if it exists.
 * \param _h database handle
 * \param _k key names
 * \param _v values of the keys that must match
 * \param _n number of key=value pairs
 * \return zero on success, negative value on failure
 */
int db_kazoo_replace(const db1_con_t* _h, const db_key_t* _k,
		const db_val_t* _v, const int _n, const int _un, const int _m)
{
	int rc;

	db_kazoo_start_transaction(_h, 0);

	rc = db_kazoo_do_replace(_h, _k, _v, _n, db_kazoo_val2str, db_kazoo_submit_query);
	if (rc == 0) {
		rc = db_kazoo_commit(_h);
		if(rc == 0) {
			db_kazoo_end_transaction(_h);
		} else {
			LM_ERR("REPLACE failed: %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
			db_kazoo_abort_transaction(_h);
		}
	} else {
		LM_ERR("REPLACE failed:  %d - %s\n", rc, sqlite3_errmsg(CON_CONNECTION(_h)));
	}
	db_kazoo_cleanup_query(_h);

	return rc;

}

int db_kazoo_fetch_result(const db1_con_t* _h, db1_res_t** _r, const int nrows)
{
	struct db_kazoo_connection *conn = CON_DB_KAZOO(_h);
//	db1_res_t *res;
	int i, rc, num_cols=0, num_rows = 0, num_alloc = 0;
	db_row_t *rows = NULL, *row;
	db_val_t *val;

	if (!_h || !_r || nrows < 0) {
		LM_ERR("Invalid parameter value\n");
		return -1;
	}

	/* exit if the fetch count is zero */
	if (nrows == 0) {
		db_kazoo_free_result(_h, *_r);
		*_r = 0;
		return 0;
	}

	LM_DBG("acquiring lock\n");
	db_kazoo_acquire_lock(_h);

	rc = sqlite3_step(conn->stmt);

	if(*_r==0) {
		/* Allocate a new result structure */
		*_r = db_new_result();
		if (*_r == 0) {
			LM_ERR("no memory left\n");
			LM_DBG("releasing lock\n");
			db_kazoo_release_lock(_h);
			return -2;
		}

		if (rc != SQLITE_ROW) {
			LM_DBG("releasing lock\n");
			db_kazoo_release_lock(_h);
			return 0;
		}

		num_cols = sqlite3_column_count(conn->stmt);
		if (num_cols == 0) {
			LM_DBG("0 columns in fetch\n");
			LM_DBG("releasing lock\n");
			db_kazoo_release_lock(_h);
			return 0;
		}

		if (db_allocate_columns(*_r, num_cols) != 0) {
			LM_ERR("no memory left\n");
			LM_DBG("releasing lock\n");
			db_kazoo_release_lock(_h);
			return 0;
		}

		RES_COL_N(*_r) = num_cols;

		for (i = 0; i < RES_COL_N(*_r); i++) {
			const char *decltype;
			int dbtype;

			RES_NAMES(*_r)[i] = str_dup(sqlite3_column_name(conn->stmt, i));
			if (RES_NAMES(*_r)[i] == NULL)
				goto no_mem;
			decltype = sqlite3_column_decltype(conn->stmt, i);
			if (decltype != NULL)
				dbtype = decltype_to_dbtype(decltype);
			else
				dbtype = type_to_dbtype(sqlite3_column_type(conn->stmt, i));
			RES_TYPES(*_r)[i] = dbtype;
		}
	} else {
		/* free old rows */
		if(RES_ROWS(*_r)!=0)
			db_free_rows(*_r);
		RES_ROWS(*_r) = 0;
		RES_ROW_N(*_r) = 0;
		if (rc != SQLITE_ROW) {
			LM_DBG("releasing lock\n");
			db_kazoo_release_lock(_h);
			return 0;
		}
	}

	while(num_rows < nrows && rc == SQLITE_ROW) {

		if (num_rows >= num_alloc) {
			if (num_alloc)
				num_alloc *= 2;
			else
				num_alloc = 64;
			rows = pkg_realloc(rows, sizeof(db_row_t) * num_alloc);
			if (rows == NULL)
				goto no_mem;
			RES_ROWS(*_r) = rows;
		}

		row = &RES_ROWS(*_r)[num_rows];
		num_rows++;

		if (db_allocate_row(*_r, row) != 0)
			goto no_mem;

		for (i = 0, val = ROW_VALUES(row); i < RES_COL_N(*_r); i++, val++) {
			VAL_TYPE(val) = RES_TYPES(*_r)[i];
			VAL_NULL(val) = 0;
			VAL_FREE(val) = 0;
			if (sqlite3_column_type(conn->stmt, i) == SQLITE_NULL) {
				VAL_NULL(val) = 1;
			} else
				switch (VAL_TYPE(val)){
				case DB1_INT:
					VAL_INT(val) = sqlite3_column_int(conn->stmt, i);
					break;
				case DB1_BIGINT:
					VAL_BIGINT(val) = sqlite3_column_int64(conn->stmt, i);
					break;
				case DB1_STRING:
					/* first field of struct str* is the char* so we can just
					 * do whatever DB1_STR case does */
				case DB1_STR:
					str_assign(&VAL_STR(val), (const char*) sqlite3_column_text(conn->stmt, i), sqlite3_column_bytes(conn->stmt, i));
					if (!VAL_STR(val).s)
						goto no_mem;
					VAL_FREE(val) = 1;
					break;
				case DB1_DOUBLE:
					VAL_DOUBLE(val) = sqlite3_column_double(conn->stmt, i);
					break;
				case DB1_DATETIME:
					VAL_TIME(val) = db_kazoo_to_timet(sqlite3_column_double(conn->stmt, i));
					break;
				case DB1_BLOB:
					str_assign(&VAL_BLOB(val), (const char*) sqlite3_column_blob(conn->stmt, i), sqlite3_column_bytes(conn->stmt, i));
					if (!VAL_STR(val).s)
						goto no_mem;
					VAL_FREE(val) = 1;
					break;
				default:
					LM_ERR("unhandled db-type\n");
					goto err;
				}
		}

		if(num_rows < nrows) {
			rc = sqlite3_step(conn->stmt);
		}
	}

	RES_ROW_N(*_r) = num_rows;     /* rows in this result set */
	RES_NUM_ROWS(*_r) += num_rows; /* rows in total */
	RES_LAST_ROW(*_r) = RES_NUM_ROWS(*_r);

//	if(num_rows < nrows) {
//        RES_NUM_ROWS(*_r)++;
//	}

	LM_DBG("releasing lock\n");
	db_kazoo_release_lock(_h);

	return 0;

no_mem:
	LM_ERR("no private memory left\n");
err:
	if (*_r) {
		db_kazoo_free_result(_h, *_r);
		*_r = 0;
	}

	LM_DBG("releasing lock\n");
	db_kazoo_release_lock(_h);

	return -1;

}

/**
 * Returns the last inserted ID.
 * \param _h database handle
 * \return returns the ID as integer or returns 0 if the previous statement
 * does not use an AUTO_INCREMENT value.
 */
int db_kazoo_last_inserted_id(const db1_con_t* _h)
{
	if (!_h) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}
	return sqlite3_last_insert_rowid(CON_CONNECTION(_h));
}


/**
 * Returns the affected rows of the last query.
 * \param _h database handle
 * \return returns the affected rows as integer or -1 on error.
 */
int db_kazoo_affected_rows(const db1_con_t* _h)
{
	if (!_h) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}
	return (int)sqlite3_changes(CON_CONNECTION(_h));
}

int db_kazoo_sql(const db1_con_t* _h, const str* _s)
{
	int rc;
	char *Error = NULL;

	rc = sqlite3_exec(CON_CONNECTION(_h), _s->s, NULL, NULL, &Error);
	if(Error) {
		LM_ERR("ERROR : %s executing '%.*s'", Error, _s->len, _s->s);
		sqlite3_free(Error);
	}

	return rc;
}

/**
 * Starts a single transaction that will consist of one or more queries (SQL BEGIN)
 * \param _h database handle
 * \return 0 on success, negative on failure
 */
int db_kazoo_start_transaction(const db1_con_t* _h, db_locking_t _l)
{
	str begin_str = str_init("BEGIN TRANSACTION;");
//	str lock_start_str = str_init("LOCK TABLES ");
//	str lock_end_str  = str_init(" WRITE;");
//	str lock_str = {0, 0};

	if (!_h) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

//	if (CON_TRANSACTION(_h) == 1) {
//		LM_ERR("transaction already started\n");
//		return -1;
//	}

//	if (db_kazoo_raw_query(_h, &begin_str, NULL) < 0)
	if(db_kazoo_sql(_h, &begin_str) < 0)
	{
		LM_ERR("executing raw_query\n");
		return -1;
	}

	CON_TRANSACTION(_h) ++;

	/*
	switch(_l)
	{
	case DB_LOCKING_NONE:
		break;
	case DB_LOCKING_FULL:
	case DB_LOCKING_WRITE:
		if ((lock_str.s = pkg_malloc((lock_start_str.len + CON_TABLE(_h)->len + lock_end_str.len) * sizeof(char))) == NULL)
		{
			LM_ERR("allocating pkg memory\n");
			goto error;
		}

		memcpy(lock_str.s, lock_start_str.s, lock_start_str.len);
		lock_str.len += lock_start_str.len;
		memcpy(lock_str.s + lock_str.len, CON_TABLE(_h)->s, CON_TABLE(_h)->len);
		lock_str.len += CON_TABLE(_h)->len;
		memcpy(lock_str.s + lock_str.len, lock_end_str.s, lock_end_str.len);
		lock_str.len += lock_end_str.len;

		if (db_kazoo_raw_query(_h, &lock_str, NULL) < 0)
		{
			LM_ERR("executing raw_query : '%.*s'\n", lock_str.len, lock_str.s);
			goto error;
		}

		if (lock_str.s) pkg_free(lock_str.s);
		CON_LOCKEDTABLES(_h) = 1;
		break;

	default:
		LM_WARN("unrecognised lock type\n");
		goto error;
	}
	*/

	return 0;
/*
error:
	if (lock_str.s) pkg_free(lock_str.s);
	db_kazoo_abort_transaction(_h);
	return -1;
*/
}

/**
 * Unlock tables in the session
 * \param _h database handle
 * \return 0 on success, negative on failure
 */
/*
int db_kazoo_unlock_tables(db1_con_t* _h)
{
	str query_str = str_init("UNLOCK TABLES;");

	if (!_h) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	if (CON_LOCKEDTABLES(_h) == 0) {
		LM_DBG("no active locked tables\n");
		return 0;
	}

	if (db_kazoo_raw_query(_h, &query_str, NULL) < 0)
	{
		LM_ERR("executing raw_query\n");
		return -1;
	}

	CON_LOCKEDTABLES(_h) = 0;
	return 0;
}
*/

/**
 * Ends a transaction and commits the changes (SQL COMMIT)
 * \param _h database handle
 * \return 0 on success, negative on failure
 */
int db_kazoo_end_transaction(const db1_con_t* _h)
{
	str commit_query_str = str_init("COMMIT;PRAGMA locking_mode = NORMAL;");
//	str set_query_str = str_init("SET autocommit=1;");

	if (!_h) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	if (CON_TRANSACTION(_h) == 0) {
		LM_ERR("transaction not in progress\n");
		return -1;
	}

//	if (db_kazoo_raw_query(_h, &commit_query_str, NULL) < 0)
	if(db_kazoo_sql(_h, &commit_query_str) < 0)
	{
		LM_ERR("executing raw_query\n");
		return -1;
	}

	/*
	if (db_kazoo_raw_query(_h, &set_query_str, NULL) < 0)
	{
		LM_ERR("executing raw_query\n");
		return -1;
	}
*/

	/* Only _end_ the transaction after the raw_query.  That way, if the
 	   raw_query fails, and the calling module does an abort_transaction()
	   to clean-up, a ROLLBACK will be sent to the DB. */
	CON_TRANSACTION(_h) --;

//	if(db_kazoo_unlock_tables(_h)<0)
//		return -1;

	return 0;
}

/**
 * Ends a transaction and rollsback the changes (SQL ROLLBACK)
 * \param _h database handle
 * \return 1 if there was something to rollback, 0 if not, negative on failure
 */
int db_kazoo_abort_transaction(const db1_con_t* _h)
{
	str rollback_query_str = str_init("ROLLBACK;PRAGMA locking_mode = NORMAL;");
//	str set_query_str = str_init("SET autocommit=1;");
//	int ret;

	if (!_h) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	if (CON_TRANSACTION(_h) == 0) {
		LM_DBG("nothing to rollback\n");
		return 0;
	}

	/* Whether the rollback succeeds or not we need to _end_ the
 	   transaction now or all future starts will fail */
	CON_TRANSACTION(_h) --;

	if(db_kazoo_sql(_h, &rollback_query_str) < 0)
//	if (db_kazoo_raw_query(_h, &rollback_query_str, NULL) < 0)
	{
		LM_ERR("executing ROLLBACK\n");
		return -1;
	}
//	ret = 1;
	return 0;

}


/**
  * Insert a row into a specified table, update on duplicate key.
  * \param _h structure representing database connection
  * \param _k key names
  * \param _v values of the keys
  * \param _n number of key=value pairs
 */
/*
 int db_kazoo_insert_update(const db1_con_t* _h, const db_key_t* _k, const db_val_t* _v,
	const int _n)
 {
	int off, ret;
	static str  sql_str;

	if ((!_h) || (!_k) || (!_v) || (!_n)) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	ret = snprintf(kazoo_sql_buf, sql_buffer_size, "insert into %s%.*s%s (",
			CON_TQUOTESZ(_h), CON_TABLE(_h)->len, CON_TABLE(_h)->s, CON_TQUOTESZ(_h));
	if (ret < 0 || ret >= sql_buffer_size) goto error;
	off = ret;

	ret = db_print_columns(kazoo_sql_buf + off, sql_buffer_size - off, _k, _n, CON_TQUOTESZ(_h));
	if (ret < 0) return -1;
	off += ret;

	ret = snprintf(kazoo_sql_buf + off, sql_buffer_size - off, ") values (");
	if (ret < 0 || ret >= (sql_buffer_size - off)) goto error;
	off += ret;
	ret = db_print_values(_h, kazoo_sql_buf + off, sql_buffer_size - off, _v, _n, db_kazoo_val2str);
	if (ret < 0) return -1;
	off += ret;

	*(kazoo_sql_buf + off++) = ')';

	ret = snprintf(kazoo_sql_buf + off, sql_buffer_size - off, " on duplicate key update ");
	if (ret < 0 || ret >= (sql_buffer_size - off)) goto error;
	off += ret;

	ret = db_print_set(_h, kazoo_sql_buf + off, sql_buffer_size - off, _k, _v, _n, db_kazoo_val2str);
	if (ret < 0) return -1;
	off += ret;

	sql_str.s = kazoo_sql_buf;
	sql_str.len = off;

	if (db_kazoo_submit_query(_h, &sql_str) < 0) {
		LM_ERR("error while submitting query\n");
		return -2;
	}
	return 0;

error:
	LM_ERR("error while preparing insert_update operation\n");
	return -1;
}
*/

 /**
  * Allocate a buffer for database module
  * No function should be called before this
  * \return zero on success, negative value on failure
  */
 int db_kazoo_alloc_buffer(void)
 {
     if (db_api_init())
     {
         LM_ERR("Failed to initialise db api\n");
 		return -1;
     }

     kazoo_sql_buf = pkg_malloc(sql_buffer_size);
     if (kazoo_sql_buf == NULL)
         return -1;
     else
         return 0;
 }

 static inline int db_kazoo_do_submit_query(const db1_con_t* _h, const str *_query,
 		int (*submit_query)(const db1_con_t*, const str*))
 {
 	int ret;
 	unsigned int ms = 0;

 	if(unlikely(cfg_get(core, core_cfg, latency_limit_action)>0))
 		ms = TICKS_TO_MS(get_ticks_raw());

 	ret = submit_query(_h, _query);

 	if(unlikely(cfg_get(core, core_cfg, latency_limit_action)>0)) {
 		ms = TICKS_TO_MS(get_ticks_raw()) - ms;
 		if(ms >= cfg_get(core, core_cfg, latency_limit_action)) {
 				LOG(cfg_get(core, core_cfg, latency_log),
 					"alert - query execution too long [%u ms] for [%.*s]\n",
 				   ms, _query->len<50?_query->len:50, _query->s);
 		}
 	}

 	return ret;
 }

 int db_kazoo_do_replace(const db1_con_t* _h, const db_key_t* _k, const db_val_t* _v,
 	const int _n, int (*val2str) (const db1_con_t*, const db_val_t*, char*,
 	int*), int (*submit_query)(const db1_con_t* _h, const str* _c))
 {
 	int off, ret;
 	str sql_str;

 	if (!_h || !_k || !_v || !val2str|| !submit_query) {
 		LM_ERR("invalid parameter value\n");
 		return -1;
 	}

 	ret = snprintf(kazoo_sql_buf, sql_buffer_size, "replace into %s%.*s%s (",
 			CON_TQUOTESZ(_h), CON_TABLE(_h)->len, CON_TABLE(_h)->s, CON_TQUOTESZ(_h));
 	if (ret < 0 || ret >= sql_buffer_size) goto error;
 	off = ret;

 	ret = db_print_columns(kazoo_sql_buf + off, sql_buffer_size - off, _k, _n, CON_TQUOTESZ(_h));
 	if (ret < 0) return -1;
 	off += ret;

 	ret = snprintf(kazoo_sql_buf + off, sql_buffer_size - off, ") values (");
 	if (ret < 0 || ret >= (sql_buffer_size - off)) goto error;
 	off += ret;

 	ret = db_print_values(_h, kazoo_sql_buf + off, sql_buffer_size - off, _v, _n,
 	val2str);
 	if (ret < 0) return -1;
 	off += ret;

 	if (off + 2 > sql_buffer_size) goto error;
 	kazoo_sql_buf[off++] = ')';
 	kazoo_sql_buf[off] = '\0';
 	sql_str.s = kazoo_sql_buf;
 	sql_str.len = off;

 	if (db_kazoo_do_submit_query(_h, &sql_str, submit_query) < 0) {
 	        LM_ERR("error while submitting query\n");
 		return -2;
 	}
 	return 0;

  error:
 	LM_ERR("error while preparing replace operation\n");
 	return -1;
 }
