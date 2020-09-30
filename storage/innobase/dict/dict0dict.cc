/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file dict/dict0dict.cc
Data dictionary system

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include <my_config.h>
#include <string>

#include "ha_prototypes.h"
#include <mysqld.h>
#include <strfunc.h>

#include "dict0dict.h"
#include "fts0fts.h"
#include "fil0fil.h"
#include <algorithm>
#include "sql_class.h"
#include "sql_table.h"
#include <mysql/service_thd_mdl.h>

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/** Flag to control insert buffer debugging. */
extern uint	ibuf_debug;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "buf0buf.h"
#include "data0type.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "dict0mem.h"
#include "dict0priv.h"
#include "dict0stats.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "lock0lock.h"
#include "mach0data.h"
#include "mem0mem.h"
#include "page0page.h"
#include "page0zip.h"
#include "pars0pars.h"
#include "pars0sym.h"
#include "que0que.h"
#include "rem0cmp.h"
#include "row0log.h"
#include "row0merge.h"
#include "row0mysql.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0undo.h"

#include <vector>
#include <algorithm>

/** the dictionary system */
dict_sys_t	dict_sys;

/** Diagnostic message for exceeding the mutex_lock_wait() timeout */
const char dict_sys_t::fatal_msg[]=
  "innodb_fatal_semaphore_wait_threshold was exceeded for dict_sys.mutex. "
  "Please refer to "
  "https://mariadb.com/kb/en/how-to-produce-a-full-stack-trace-for-mysqld/";

/** Percentage of compression failures that are allowed in a single
round */
ulong	zip_failure_threshold_pct = 5;

/** Maximum percentage of a page that can be allowed as a pad to avoid
compression failures */
ulong	zip_pad_max = 50;

#define	DICT_HEAP_SIZE		100	/*!< initial memory heap size when
					creating a table or index object */
#define DICT_POOL_PER_TABLE_HASH 512	/*!< buffer pool max size per table
					hash table fixed size in bytes */
#define DICT_POOL_PER_VARYING	4	/*!< buffer pool max size per data
					dictionary varying size in bytes */

/** Identifies generated InnoDB foreign key names */
static char	dict_ibfk[] = "_ibfk_";

bool		innodb_table_stats_not_found = false;
bool		innodb_index_stats_not_found = false;
static bool	innodb_table_stats_not_found_reported = false;
static bool	innodb_index_stats_not_found_reported = false;

/*******************************************************************//**
Tries to find column names for the index and sets the col field of the
index.
@param[in]	index	index
@param[in]	add_v	new virtual columns added along with an add index call
@return whether the column names were found */
static
bool
dict_index_find_cols(
	dict_index_t*		index,
	const dict_add_v_col_t*	add_v);
/*******************************************************************//**
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the clustered index */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
	dict_index_t*		index);	/*!< in: user representation of
					a clustered index */
/*******************************************************************//**
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the non-clustered index */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
	dict_index_t*		index);	/*!< in: user representation of
					a non-clustered index */
/**********************************************************************//**
Builds the internal dictionary cache representation for an FTS index.
@return own: the internal representation of the FTS index */
static
dict_index_t*
dict_index_build_internal_fts(
/*==========================*/
	dict_index_t*	index);	/*!< in: user representation of an FTS index */

/**********************************************************************//**
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in/out: table */
	dict_index_t*	index,		/*!< in, own: index */
	ibool		lru_evict);	/*!< in: TRUE if page being evicted
					to make room in the table LRU list */
#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate the dictionary table LRU list.
@return TRUE if validate OK */
static
ibool
dict_lru_validate(void);
/*===================*/
#endif /* UNIV_DEBUG */

/* Stream for storing detailed information about the latest foreign key
and unique key errors. Only created if !srv_read_only_mode */
FILE*	dict_foreign_err_file		= NULL;
/* mutex protecting the foreign and unique error buffers */
mysql_mutex_t dict_foreign_err_mutex;

/********************************************************************//**
Checks if the database name in two table names is the same.
@return TRUE if same db name */
ibool
dict_tables_have_same_db(
/*=====================*/
	const char*	name1,	/*!< in: table name in the form
				dbname '/' tablename */
	const char*	name2)	/*!< in: table name in the form
				dbname '/' tablename */
{
	for (; *name1 == *name2; name1++, name2++) {
		if (*name1 == '/') {
			return(TRUE);
		}
		ut_a(*name1); /* the names must contain '/' */
	}
	return(FALSE);
}

/********************************************************************//**
Return the end of table name where we have removed dbname and '/'.
@return table name */
const char*
dict_remove_db_name(
/*================*/
	const char*	name)	/*!< in: table name in the form
				dbname '/' tablename */
{
	const char*	s = strchr(name, '/');
	ut_a(s);

	return(s + 1);
}

/** Open a persistent table.
@param[in]	table_id	persistent table identifier
@param[in]	ignore_err	errors to ignore
@param[in]	cached_only	whether to skip loading
@return persistent table
@retval	NULL if not found */
static dict_table_t* dict_table_open_on_id_low(
	table_id_t		table_id,
	dict_err_ignore_t	ignore_err,
	bool			cached_only)
{
	dict_table_t* table = dict_sys.get_table(table_id);

	if (!table && !cached_only) {
		table = dict_load_table_on_id(table_id, ignore_err);
	}

	return table;
}

/**********************************************************************//**
Try to drop any indexes after an aborted index creation.
This can also be after a server kill during DROP INDEX. */
static
void
dict_table_try_drop_aborted(
/*========================*/
	dict_table_t*	table,		/*!< in: table, or NULL if it
					needs to be looked up again */
	table_id_t	table_id,	/*!< in: table identifier */
	uint32_t	ref_count)	/*!< in: expected table->n_ref_count */
{
	trx_t*		trx;

	trx = trx_create();
	trx->op_info = "try to drop any indexes after an aborted index creation";
	row_mysql_lock_data_dictionary(trx);
	trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);

	if (table == NULL) {
		table = dict_table_open_on_id_low(
			table_id, DICT_ERR_IGNORE_FK_NOKEY, FALSE);
	} else {
		ut_ad(table->id == table_id);
	}

	if (table && table->get_ref_count() == ref_count && table->drop_aborted
	    && !UT_LIST_GET_FIRST(table->locks)) {
		/* Silence a debug assertion in row_merge_drop_indexes(). */
		ut_d(table->acquire());
		row_merge_drop_indexes(trx, table, TRUE);
		ut_d(table->release());
		ut_ad(table->get_ref_count() == ref_count);
		trx_commit_for_mysql(trx);
	}

	row_mysql_unlock_data_dictionary(trx);
	trx->free();
}

/**********************************************************************//**
When opening a table,
try to drop any indexes after an aborted index creation.
Release the dict_sys.mutex. */
static
void
dict_table_try_drop_aborted_and_mutex_exit(
/*=======================================*/
	dict_table_t*	table,		/*!< in: table (may be NULL) */
	ibool		try_drop)	/*!< in: FALSE if should try to
					drop indexes whose online creation
					was aborted */
{
	if (try_drop
	    && table != NULL
	    && table->drop_aborted
	    && table->get_ref_count() == 1
	    && dict_table_get_first_index(table)) {

		/* Attempt to drop the indexes whose online creation
		was aborted. */
		table_id_t	table_id = table->id;

		dict_sys.mutex_unlock();

		dict_table_try_drop_aborted(table, table_id, 1);
	} else {
		dict_sys.mutex_unlock();
	}
}

/** Decrements the count of open handles of a table.
@param[in,out]	table		table
@param[in]	dict_locked	data dictionary locked
@param[in]	try_drop	try to drop any orphan indexes after
				an aborted online index creation
@param[in]	thd		thread to release MDL
@param[in]	mdl		metadata lock or NULL if the thread
				is a foreground one. */
void
dict_table_close(
	dict_table_t*	table,
	bool		dict_locked,
	bool		try_drop,
	THD*		thd,
	MDL_ticket*	mdl)
{
	if (!dict_locked) {
		dict_sys.mutex_lock();
	}

	dict_sys.assert_locked();
	ut_a(table->get_ref_count() > 0);

	const bool last_handle = table->release();

	/* Force persistent stats re-read upon next open of the table
	so that FLUSH TABLE can be used to forcibly fetch stats from disk
	if they have been manually modified. We reset table->stat_initialized
	only if table reference count is 0 because we do not want too frequent
	stats re-reads (e.g. in other cases than FLUSH TABLE). */
	if (last_handle && strchr(table->name.m_name, '/') != NULL
	    && dict_stats_is_persistent_enabled(table)) {

		dict_stats_deinit(table);
	}

	MONITOR_DEC(MONITOR_TABLE_REFERENCE);

	ut_ad(dict_lru_validate());
	ut_ad(dict_sys.find(table));

	if (!dict_locked) {
		table_id_t	table_id	= table->id;
		const bool	drop_aborted	= last_handle && try_drop
			&& table->drop_aborted
			&& dict_table_get_first_index(table);

		dict_sys.mutex_unlock();

		/* dict_table_try_drop_aborted() can generate undo logs.
		So it should be avoided after shutdown of background
		threads */
		if (drop_aborted && !srv_undo_sources) {
			dict_table_try_drop_aborted(NULL, table_id, 0);
		}
	}

	if (!thd || !mdl) {
	} else if (MDL_context *mdl_context= static_cast<MDL_context*>(
			   thd_mdl_context(thd))) {
		mdl_context->release_lock(mdl);
	}
}

/********************************************************************//**
Closes the only open handle to a table and drops a table while assuring
that dict_sys.mutex is held the whole time.  This assures that the table
is not evicted after the close when the count of open handles goes to zero.
Because dict_sys.mutex is held, we do not need to call
dict_table_prevent_eviction().  */
void
dict_table_close_and_drop(
/*======================*/
	trx_t*		trx,		/*!< in: data dictionary transaction */
	dict_table_t*	table)		/*!< in/out: table */
{
	dberr_t err = DB_SUCCESS;

	ut_d(dict_sys.assert_locked());
	ut_ad(trx->dict_operation != TRX_DICT_OP_NONE);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));

	dict_table_close(table, true, false);

#if defined UNIV_DEBUG || defined UNIV_DDL_DEBUG
	/* Nobody should have initialized the stats of the newly created
	table when this is called. So we know that it has not been added
	for background stats gathering. */
	ut_a(!table->stat_initialized);
#endif /* UNIV_DEBUG || UNIV_DDL_DEBUG */

	err = row_merge_drop_table(trx, table);

	if (err != DB_SUCCESS) {
		ib::error() << "At " << __FILE__ << ":" << __LINE__
			    << " row_merge_drop_table returned error: " << err
			    << " table: " << table->name;
	}
}

/** Check if the table has a given (non_virtual) column.
@param[in]	table		table object
@param[in]	col_name	column name
@param[in]	col_nr		column number guessed, 0 as default
@return column number if the table has the specified column,
otherwise table->n_def */
ulint
dict_table_has_column(
	const dict_table_t*	table,
	const char*		col_name,
	ulint			col_nr)
{
	ulint		col_max = table->n_def;

	ut_ad(table);
	ut_ad(col_name);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	if (col_nr < col_max
	    && innobase_strcasecmp(
		col_name, dict_table_get_col_name(table, col_nr)) == 0) {
		return(col_nr);
	}

	/** The order of column may changed, check it with other columns */
	for (ulint i = 0; i < col_max; i++) {
		if (i != col_nr
		    && innobase_strcasecmp(
			col_name, dict_table_get_col_name(table, i)) == 0) {

			return(i);
		}
	}

	return(col_max);
}

/** Retrieve the column name.
@param[in]	table	the table of this column */
const char* dict_col_t::name(const dict_table_t& table) const
{
	ut_ad(table.magic_n == DICT_TABLE_MAGIC_N);

	size_t col_nr;
	const char *s;

	if (is_virtual()) {
		col_nr = size_t(reinterpret_cast<const dict_v_col_t*>(this)
				- table.v_cols);
		ut_ad(col_nr < table.n_v_def);
		s = table.v_col_names;
	} else {
		col_nr = size_t(this - table.cols);
		ut_ad(col_nr < table.n_def);
		s = table.col_names;
	}

	if (s) {
		for (size_t i = 0; i < col_nr; i++) {
			s += strlen(s) + 1;
		}
	}

	return(s);
}

/** Returns a virtual column's name.
@param[in]	table	target table
@param[in]	col_nr	virtual column number (nth virtual column)
@return column name or NULL if column number out of range. */
const char*
dict_table_get_v_col_name(
	const dict_table_t*	table,
	ulint			col_nr)
{
	const char*	s;

	ut_ad(table);
	ut_ad(col_nr < table->n_v_def);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	if (col_nr >= table->n_v_def) {
		return(NULL);
	}

	s = table->v_col_names;

	if (s != NULL) {
		for (ulint i = 0; i < col_nr; i++) {
			s += strlen(s) + 1;
		}
	}

	return(s);
}

/** Search virtual column's position in InnoDB according to its position
in original table's position
@param[in]	table	target table
@param[in]	col_nr	column number (nth column in the MySQL table)
@return virtual column's position in InnoDB, ULINT_UNDEFINED if not find */
static
ulint
dict_table_get_v_col_pos_for_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i;

	ut_ad(table);
	ut_ad(col_nr < static_cast<ulint>(table->n_t_def));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	for (i = 0; i < table->n_v_def; i++) {
		if (col_nr == dict_get_v_col_mysql_pos(
				table->v_cols[i].m_col.ind)) {
			break;
		}
	}

	if (i == table->n_v_def) {
		return(ULINT_UNDEFINED);
	}

	return(i);
}

/** Returns a virtual column's name according to its original
MySQL table position.
@param[in]	table	target table
@param[in]	col_nr	column number (nth column in the table)
@return column name. */
static
const char*
dict_table_get_v_col_name_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i = dict_table_get_v_col_pos_for_mysql(table, col_nr);

	if (i == ULINT_UNDEFINED) {
		return(NULL);
	}

	return(dict_table_get_v_col_name(table, i));
}

/** Get nth virtual column according to its original MySQL table position
@param[in]	table	target table
@param[in]	col_nr	column number in MySQL Table definition
@return dict_v_col_t ptr */
dict_v_col_t*
dict_table_get_nth_v_col_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i = dict_table_get_v_col_pos_for_mysql(table, col_nr);

	if (i == ULINT_UNDEFINED) {
		return(NULL);
	}

	return(dict_table_get_nth_v_col(table, i));
}


/** Get all the FTS indexes on a table.
@param[in]	table	table
@param[out]	indexes	all FTS indexes on this table
@return number of FTS indexes */
ulint
dict_table_get_all_fts_indexes(
	const dict_table_t*	table,
	ib_vector_t*		indexes)
{
	dict_index_t* index;

	ut_a(ib_vector_size(indexes) == 0);

	for (index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {

		if (index->type == DICT_FTS) {
			ib_vector_push(indexes, &index);
		}
	}

	return(ib_vector_size(indexes));
}

/** Looks for column n in an index.
@param[in]	index		index
@param[in]	n		column number
@param[in]	inc_prefix	true=consider column prefixes too
@param[in]	is_virtual	true==virtual column
@param[out]	prefix_col_pos	col num if prefix
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_col_or_prefix_pos(
	const dict_index_t*	index,
	ulint			n,
	bool			inc_prefix,
	bool			is_virtual,
	ulint*			prefix_col_pos)
{
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	if (prefix_col_pos) {
		*prefix_col_pos = ULINT_UNDEFINED;
	}

	if (is_virtual) {
		col = &(dict_table_get_nth_v_col(index->table, n)->m_col);
	} else {
		col = dict_table_get_nth_col(index->table, n);
	}

	if (dict_index_is_clust(index)) {

		return(dict_col_get_clust_pos(col, index));
	}

	n_fields = dict_index_get_n_fields(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {
			if (prefix_col_pos) {
				*prefix_col_pos = pos;
			}
			if (inc_prefix || field->prefix_len == 0) {
				return(pos);
			}
		}
	}

	return(ULINT_UNDEFINED);
}

/** Check if the index contains a column or a prefix of that column.
@param[in]	n		column number
@param[in]	is_virtual	whether it is a virtual col
@return whether the index contains the column or its prefix */
bool dict_index_t::contains_col_or_prefix(ulint n, bool is_virtual) const
{
	ut_ad(magic_n == DICT_INDEX_MAGIC_N);

	if (is_primary()) {
		return(!is_virtual);
	}

	const dict_col_t* col = is_virtual
		? &dict_table_get_nth_v_col(table, n)->m_col
		: dict_table_get_nth_col(table, n);

	for (ulint pos = 0; pos < n_fields; pos++) {
		if (col == fields[pos].col) {
			return true;
		}
	}

	return false;
}

/********************************************************************//**
Looks for a matching field in an index. The column has to be the same. The
column in index must be complete, or must contain a prefix longer than the
column in index2. That is, we must be able to construct the prefix in index2
from the prefix in index.
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_field_pos(
/*=========================*/
	const dict_index_t*	index,	/*!< in: index from which to search */
	const dict_index_t*	index2,	/*!< in: index */
	ulint			n)	/*!< in: field number in index2 */
{
	const dict_field_t*	field;
	const dict_field_t*	field2;
	ulint			n_fields;
	ulint			pos;

	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	field2 = dict_index_get_nth_field(index2, n);

	n_fields = dict_index_get_n_fields(index);

	/* Are we looking for a MBR (Minimum Bound Box) field of
	a spatial index */
	bool	is_mbr_fld = (n == 0 && dict_index_is_spatial(index2));

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		/* The first field of a spatial index is a transformed
		MBR (Minimum Bound Box) field made out of original column,
		so its field->col still points to original cluster index
		col, but the actual content is different. So we cannot
		consider them equal if neither of them is MBR field */
		if (pos == 0 && dict_index_is_spatial(index) && !is_mbr_fld) {
			continue;
		}

		if (field->col == field2->col
		    && (field->prefix_len == 0
			|| (field->prefix_len >= field2->prefix_len
			    && field2->prefix_len != 0))) {

			return(pos);
		}
	}

	return(ULINT_UNDEFINED);
}

/** Parse the table file name into table name and database name.
@tparam		dict_locked	whether dict_sys.mutex is being held
@param[in,out]	db_name		database name buffer
@param[in,out]	tbl_name	table name buffer
@param[out]	db_name_len	database name length
@param[out]	tbl_name_len	table name length
@return whether the table name is visible to SQL */
template<bool dict_locked>
bool dict_table_t::parse_name(char (&db_name)[NAME_LEN + 1],
                              char (&tbl_name)[NAME_LEN + 1],
                              size_t *db_name_len, size_t *tbl_name_len) const
{
  char db_buf[MAX_DATABASE_NAME_LEN + 1];
  char tbl_buf[MAX_TABLE_NAME_LEN + 1];

  if (!dict_locked)
    dict_sys.mutex_lock(); /* protect against renaming */
  dict_sys.assert_locked();
  const size_t db_len= name.dblen();
  ut_ad(db_len <= MAX_DATABASE_NAME_LEN);

  memcpy(db_buf, name.m_name, db_len);
  db_buf[db_len]= 0;

  size_t tbl_len= strlen(name.m_name + db_len + 1);

  const bool is_temp= tbl_len > TEMP_FILE_PREFIX_LENGTH &&
    !strncmp(name.m_name, TEMP_FILE_PREFIX, TEMP_FILE_PREFIX_LENGTH);

  if (is_temp);
  else if (const char *is_part= static_cast<const char*>
           (memchr(name.m_name + db_len + 1, '#', tbl_len)))
    tbl_len= static_cast<size_t>(is_part - &name.m_name[db_len + 1]);

  memcpy(tbl_buf, name.m_name + db_len + 1, tbl_len);
  tbl_buf[tbl_len]= 0;

  if (!dict_locked)
    dict_sys.mutex_unlock();

  *db_name_len= filename_to_tablename(db_buf, db_name,
                                      MAX_DATABASE_NAME_LEN + 1, true);

  if (is_temp)
    return false;

  *tbl_name_len= filename_to_tablename(tbl_buf, tbl_name,
                                       MAX_TABLE_NAME_LEN + 1, true);
  return true;
}

template bool
dict_table_t::parse_name<>(char(&)[NAME_LEN + 1], char(&)[NAME_LEN + 1],
                           size_t*, size_t*) const;

/** Acquire MDL shared for the table name.
@tparam trylock whether to use non-blocking operation
@param[in,out]  table           table object
@param[in,out]  thd             background thread
@param[out]     mdl             mdl ticket
@param[in]      table_op        operation to perform when opening
@return table object after locking MDL shared
@retval nullptr if the table is not readable, or if trylock && MDL blocked */
template<bool trylock>
dict_table_t*
dict_acquire_mdl_shared(dict_table_t *table,
                        THD *thd,
                        MDL_ticket **mdl,
                        dict_table_op_t table_op)
{
  if (!table || !mdl)
    return table;

  MDL_context *mdl_context= static_cast<MDL_context*>(thd_mdl_context(thd));
  size_t db_len;

  if (trylock)
  {
    dict_sys.mutex_lock();
    db_len= dict_get_db_name_len(table->name.m_name);
    dict_sys.mutex_unlock();
  }
  else
  {
    dict_sys.assert_locked();
    db_len= dict_get_db_name_len(table->name.m_name);
  }

  if (db_len == 0)
    return table; /* InnoDB system tables are not covered by MDL */

  if (!mdl_context)
    return nullptr;

  table_id_t table_id= table->id;
  char db_buf[NAME_LEN + 1], db_buf1[NAME_LEN + 1];
  char tbl_buf[NAME_LEN + 1], tbl_buf1[NAME_LEN + 1];
  size_t tbl_len;
  bool unaccessible= false;

  if (!table->parse_name<!trylock>(db_buf, tbl_buf, &db_len, &tbl_len))
    /* The name of an intermediate table starts with #sql */
    return table;

retry:
  if (!unaccessible && (!table->is_readable() || table->corrupted))
  {
is_unaccessible:
    if (*mdl)
    {
      mdl_context->release_lock(*mdl);
      *mdl= nullptr;
    }
    unaccessible= true;
  }

  if (!trylock)
    table->release();

  if (unaccessible)
    return nullptr;

  if (!trylock)
    dict_sys.mutex_unlock();
  {
    MDL_request request;
    MDL_REQUEST_INIT(&request,MDL_key::TABLE, db_buf, tbl_buf, MDL_SHARED, MDL_EXPLICIT);
    if (trylock
        ? mdl_context->try_acquire_lock(&request)
        : mdl_context->acquire_lock(&request,
                                    /* FIXME: use compatible type, and maybe
                                    remove this parameter altogether! */
                                    static_cast<double>(global_system_variables
                                                        .lock_wait_timeout)))
    {
      *mdl= nullptr;
      if (trylock)
        return nullptr;
    }
    else
      *mdl= request.ticket;
  }

  if (!trylock)
    dict_sys.mutex_lock();
  else if (!*mdl)
    return nullptr;

  table= dict_table_open_on_id(table_id, !trylock, table_op);

  if (!table)
  {
    /* The table was dropped. */
    if (*mdl)
    {
      mdl_context->release_lock(*mdl);
      *mdl= nullptr;
    }
    return nullptr;
  }

  if (!table->is_accessible())
    goto is_unaccessible;

  size_t db1_len, tbl1_len;

  if (!table->parse_name<!trylock>(db_buf1, tbl_buf1, &db1_len, &tbl1_len))
  {
    /* The table was renamed to #sql prefix.
    Release MDL (if any) for the old name and return. */
    if (*mdl)
    {
      mdl_context->release_lock(*mdl);
      *mdl= nullptr;
    }
    return table;
  }

  if (*mdl)
  {
    if (db_len == db1_len && tbl_len == tbl1_len &&
        !memcmp(db_buf, db_buf1, db_len) &&
        !memcmp(tbl_buf, tbl_buf1, tbl_len))
      return table;

    /* The table was renamed. Release MDL for the old name and
    try to acquire MDL for the new name. */
    mdl_context->release_lock(*mdl);
    *mdl= nullptr;
  }

  db_len= db1_len;
  tbl_len= tbl1_len;

  memcpy(tbl_buf, tbl_buf1, tbl_len + 1);
  memcpy(db_buf, db_buf1, db_len + 1);
  goto retry;
}

template dict_table_t*
dict_acquire_mdl_shared<true>(dict_table_t*,THD*,MDL_ticket**,dict_table_op_t);

/** Look up a table by numeric identifier.
@param[in]      table_id        table identifier
@param[in]      dict_locked     data dictionary locked
@param[in]      table_op        operation to perform when opening
@param[in,out]  thd             background thread, or NULL to not acquire MDL
@param[out]     mdl             mdl ticket, or NULL
@return table, NULL if does not exist */
dict_table_t*
dict_table_open_on_id(table_id_t table_id, bool dict_locked,
                      dict_table_op_t table_op, THD *thd,
                      MDL_ticket **mdl)
{
	ut_ad(!dict_locked || !thd);

	if (!dict_locked) {
		dict_sys.mutex_lock();
	}

	dict_sys.assert_locked();

	dict_table_t* table = dict_table_open_on_id_low(
		table_id,
		table_op == DICT_TABLE_OP_LOAD_TABLESPACE
		? DICT_ERR_IGNORE_RECOVER_LOCK
		: DICT_ERR_IGNORE_FK_NOKEY,
		table_op == DICT_TABLE_OP_OPEN_ONLY_IF_CACHED);

	if (table != NULL) {
		dict_sys.acquire(table);
		MONITOR_INC(MONITOR_TABLE_REFERENCE);
	}

	if (!dict_locked) {
		if (thd) {
			table = dict_acquire_mdl_shared<false>(
				table, thd, mdl, table_op);
		}

		dict_table_try_drop_aborted_and_mutex_exit(
			table, table_op == DICT_TABLE_OP_DROP_ORPHAN);
	}

	return table;
}

/********************************************************************//**
Looks for column n position in the clustered index.
@return position in internal representation of the clustered index */
unsigned
dict_table_get_nth_col_pos(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n,	/*!< in: column number */
	ulint*			prefix_col_pos)
{
  ulint pos= dict_index_get_nth_col_pos(dict_table_get_first_index(table),
					n, prefix_col_pos);
  DBUG_ASSERT(pos <= dict_index_t::MAX_N_FIELDS);
  return static_cast<unsigned>(pos);
}

/********************************************************************//**
Checks if a column is in the ordering columns of the clustered index of a
table. Column prefixes are treated like whole columns.
@return TRUE if the column, or its prefix, is in the clustered key */
ibool
dict_table_col_in_clustered_key(
/*============================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n)	/*!< in: column number */
{
	const dict_index_t*	index;
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	col = dict_table_get_nth_col(table, n);

	index = dict_table_get_first_index(table);

	n_fields = dict_index_get_n_unique(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/** Initialise the data dictionary cache. */
void dict_sys_t::create()
{
  ut_ad(this == &dict_sys);
  ut_ad(!is_initialised());
  m_initialised= true;
  UT_LIST_INIT(table_LRU, &dict_table_t::table_LRU);
  UT_LIST_INIT(table_non_LRU, &dict_table_t::table_LRU);

  mysql_mutex_init(dict_sys_mutex_key, &mutex, nullptr);

  const ulint hash_size = buf_pool_get_curr_size()
    / (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE);

  table_hash.create(hash_size);
  table_id_hash.create(hash_size);
  temp_id_hash.create(hash_size);

  latch.SRW_LOCK_INIT(dict_operation_lock_key);

  if (!srv_read_only_mode)
  {
    dict_foreign_err_file= os_file_create_tmpfile();
    ut_a(dict_foreign_err_file);
  }

  mysql_mutex_init(dict_foreign_err_mutex_key, &dict_foreign_err_mutex,
                   nullptr);
}


/** Acquire a reference to a cached table. */
inline void dict_sys_t::acquire(dict_table_t* table)
{
  ut_ad(dict_sys.find(table));
  if (table->can_be_evicted)
  {
    UT_LIST_REMOVE(dict_sys.table_LRU, table);
    UT_LIST_ADD_FIRST(dict_sys.table_LRU, table);
  }

  table->acquire();
}

void dict_sys_t::mutex_lock_wait()
{
  ulonglong now= my_hrtime_coarse().val, old= 0;
  if (mutex_wait_start.compare_exchange_strong
      (old, now, std::memory_order_relaxed, std::memory_order_relaxed))
  {
    mysql_mutex_lock(&mutex);
    mutex_wait_start.store(0, std::memory_order_relaxed);
    return;
  }

  ut_ad(old);
  /* We could have old > now due to our use of my_hrtime_coarse(). */
  ulong waited= old <= now ? static_cast<ulong>((now - old) / 1000000) : 0;
  const ulong threshold= srv_fatal_semaphore_wait_threshold;

  if (waited >= threshold)
    ib::fatal() << fatal_msg;

  if (waited > threshold / 4)
    ib::warn() << "A long wait (" << waited
               << " seconds) was observed for dict_sys.mutex";
  mysql_mutex_lock(&mutex);
}

#ifdef HAVE_PSI_MUTEX_INTERFACE
/** Acquire the mutex */
void dict_sys_t::mutex_lock()
{
  if (mysql_mutex_trylock(&mutex))
    mutex_lock_wait();
}

/** Release the mutex */
void dict_sys_t::mutex_unlock() { mysql_mutex_unlock(&mutex); }
#endif

/** Lock the data dictionary cache. */
void dict_sys_t::lock(SRW_LOCK_ARGS(const char *file, unsigned line))
{
  ut_ad(this == &dict_sys);
  ut_ad(is_initialised());
  latch.wr_lock(SRW_LOCK_ARGS(file, line));
  ut_ad(!latch_ex);
  ut_d(latch_ex= true);
  mutex_lock();
}

/**********************************************************************//**
Returns a table object and increment its open handle count.
NOTE! This is a high-level function to be used mainly from outside the
'dict' module. Inside this directory dict_table_get_low
is usually the appropriate function.
@return table, NULL if does not exist */
dict_table_t*
dict_table_open_on_name(
/*====================*/
	const char*	table_name,	/*!< in: table name */
	ibool		dict_locked,	/*!< in: TRUE=data dictionary locked */
	ibool		try_drop,	/*!< in: TRUE=try to drop any orphan
					indexes after an aborted online
					index creation */
	dict_err_ignore_t
			ignore_err)	/*!< in: error to be ignored when
					loading a table definition */
{
	dict_table_t*	table;
	DBUG_ENTER("dict_table_open_on_name");
	DBUG_PRINT("dict_table_open_on_name", ("table: '%s'", table_name));

	if (!dict_locked) {
		dict_sys.mutex_lock();
	}

	ut_ad(table_name);
	dict_sys.assert_locked();

	table = dict_load_table(table_name, ignore_err);

	ut_ad(!table || table->cached);

	if (table != NULL) {

		/* If table is encrypted or corrupted */
		if (!(ignore_err & ~DICT_ERR_IGNORE_FK_NOKEY)
		    && !table->is_readable()) {
			/* Make life easy for drop table. */
			dict_sys.prevent_eviction(table);

			if (table->corrupted) {

				ib::error() << "Table " << table->name
					<< " is corrupted. Please "
					"drop the table and recreate.";
				if (!dict_locked) {
					dict_sys.mutex_unlock();
				}

				DBUG_RETURN(NULL);
			}

			dict_sys.acquire(table);

			if (!dict_locked) {
				dict_sys.mutex_unlock();
			}

			DBUG_RETURN(table);
		}

		dict_sys.acquire(table);
		MONITOR_INC(MONITOR_TABLE_REFERENCE);
	}

	ut_ad(dict_lru_validate());

	if (!dict_locked) {
		dict_table_try_drop_aborted_and_mutex_exit(table, try_drop);
	}

	DBUG_RETURN(table);
}

/**********************************************************************//**
Adds system columns to a table object. */
void
dict_table_add_system_columns(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	mem_heap_t*	heap)	/*!< in: temporary heap */
{
	ut_ad(table->n_def == table->n_cols - DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!table->cached);

	/* NOTE: the system columns MUST be added in the following order
	(so that they can be indexed by the numerical value of DATA_ROW_ID,
	etc.) and as the last columns of the table memory object.
	The clustered index will not always physically contain all system
	columns. */

	dict_mem_table_add_col(table, heap, "DB_ROW_ID", DATA_SYS,
			       DATA_ROW_ID | DATA_NOT_NULL,
			       DATA_ROW_ID_LEN);

	compile_time_assert(DATA_ROW_ID == 0);
	dict_mem_table_add_col(table, heap, "DB_TRX_ID", DATA_SYS,
			       DATA_TRX_ID | DATA_NOT_NULL,
			       DATA_TRX_ID_LEN);
	compile_time_assert(DATA_TRX_ID == 1);
	dict_mem_table_add_col(table, heap, "DB_ROLL_PTR", DATA_SYS,
			       DATA_ROLL_PTR | DATA_NOT_NULL,
			       DATA_ROLL_PTR_LEN);
	compile_time_assert(DATA_ROLL_PTR == 2);

	/* This check reminds that if a new system column is added to
	the program, it should be dealt with here */
	compile_time_assert(DATA_N_SYS_COLS == 3);
}

/** Add the table definition to the data dictionary cache */
void dict_table_t::add_to_cache()
{
	cached = TRUE;

	dict_sys.add(this);
}

/** Add a table definition to the data dictionary cache */
inline void dict_sys_t::add(dict_table_t* table)
{
	ut_ad(!find(table));

	ulint fold = ut_fold_string(table->name.m_name);

	new (&table->autoinc_mutex) std::mutex();

	/* Look for a table with the same name: error if such exists */
	{
		dict_table_t*	table2;
		HASH_SEARCH(name_hash, &table_hash, fold,
			    dict_table_t*, table2, ut_ad(table2->cached),
			    !strcmp(table2->name.m_name, table->name.m_name));
		ut_a(table2 == NULL);

#ifdef UNIV_DEBUG
		/* Look for the same table pointer with a different name */
		HASH_SEARCH_ALL(name_hash, &table_hash,
				dict_table_t*, table2, ut_ad(table2->cached),
				table2 == table);
		ut_ad(table2 == NULL);
#endif /* UNIV_DEBUG */
	}
	HASH_INSERT(dict_table_t, name_hash, &table_hash, fold, table);

	/* Look for a table with the same id: error if such exists */
	hash_table_t* id_hash = table->is_temporary()
		? &temp_id_hash : &table_id_hash;
	const ulint id_fold = ut_fold_ull(table->id);
	{
		dict_table_t*	table2;
		HASH_SEARCH(id_hash, id_hash, id_fold,
			    dict_table_t*, table2, ut_ad(table2->cached),
			    table2->id == table->id);
		ut_a(table2 == NULL);

#ifdef UNIV_DEBUG
		/* Look for the same table pointer with a different id */
		HASH_SEARCH_ALL(id_hash, id_hash,
				dict_table_t*, table2, ut_ad(table2->cached),
				table2 == table);
		ut_ad(table2 == NULL);
#endif /* UNIV_DEBUG */

		HASH_INSERT(dict_table_t, id_hash, id_hash, id_fold, table);
	}

	UT_LIST_ADD_FIRST(table->can_be_evicted ? table_LRU : table_non_LRU,
			  table);
	ut_ad(dict_lru_validate());
}

/**********************************************************************//**
Test whether a table can be evicted from the LRU cache.
@return TRUE if table can be evicted. */
static
ibool
dict_table_can_be_evicted(
/*======================*/
	dict_table_t*	table)		/*!< in: table to test */
{
	ut_d(dict_sys.assert_locked());
	ut_a(table->can_be_evicted);
	ut_a(table->foreign_set.empty());
	ut_a(table->referenced_set.empty());

	if (table->get_ref_count() == 0) {
		/* The transaction commit and rollback are called from
		outside the handler interface. This means that there is
		a window where the table->n_ref_count can be zero but
		the table instance is in "use". */

		if (lock_table_has_locks(table)) {
			return(FALSE);
		}

#ifdef BTR_CUR_HASH_ADAPT
		/* We cannot really evict the table if adaptive hash
		index entries are pointing to any of its indexes. */
		for (dict_index_t* index = dict_table_get_first_index(table);
		     index != NULL;
		     index = dict_table_get_next_index(index)) {
			if (index->n_ahi_pages()) {
				return(FALSE);
			}
		}
#endif /* BTR_CUR_HASH_ADAPT */

		return(TRUE);
	}

	return(FALSE);
}

#ifdef BTR_CUR_HASH_ADAPT
/** @return a clone of this */
dict_index_t *dict_index_t::clone() const
{
  ut_ad(n_fields);
  ut_ad(!(type & (DICT_IBUF | DICT_SPATIAL | DICT_FTS)));
  ut_ad(online_status == ONLINE_INDEX_COMPLETE);
  ut_ad(is_committed());
  ut_ad(!is_dummy);
  ut_ad(!parser);
  ut_ad(!online_log);
  ut_ad(!rtr_track);

  const size_t size= sizeof *this + n_fields * sizeof(*fields) +
#ifdef BTR_CUR_ADAPT
    sizeof *search_info +
#endif
    1 + strlen(name) +
    n_uniq * (sizeof *stat_n_diff_key_vals +
              sizeof *stat_n_sample_sizes +
              sizeof *stat_n_non_null_key_vals);

  mem_heap_t* heap= mem_heap_create(size);
  dict_index_t *index= static_cast<dict_index_t*>
    (mem_heap_alloc(heap, sizeof *this));
  *index= *this;
  index->lock.SRW_LOCK_INIT(index_tree_rw_lock_key);
  index->heap= heap;
  index->name= mem_heap_strdup(heap, name);
  index->fields= static_cast<dict_field_t*>
    (mem_heap_dup(heap, fields, n_fields * sizeof *fields));
#ifdef BTR_CUR_ADAPT
  index->search_info= btr_search_info_create(index->heap);
#endif /* BTR_CUR_ADAPT */
  index->stat_n_diff_key_vals= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_diff_key_vals));
  index->stat_n_sample_sizes= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_sample_sizes));
  index->stat_n_non_null_key_vals= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_non_null_key_vals));
  new (&index->zip_pad.mutex) std::mutex();
  return index;
}

/** Clone this index for lazy dropping of the adaptive hash.
@return this or a clone */
dict_index_t *dict_index_t::clone_if_needed()
{
  if (!search_info->ref_count)
    return this;
  dict_index_t *prev= UT_LIST_GET_PREV(indexes, this);

  UT_LIST_REMOVE(table->indexes, this);
  UT_LIST_ADD_LAST(table->freed_indexes, this);
  dict_index_t *index= clone();
  set_freed();
  if (prev)
    UT_LIST_INSERT_AFTER(table->indexes, prev, index);
  else
    UT_LIST_ADD_FIRST(table->indexes, index);
  return index;
}
#endif /* BTR_CUR_HASH_ADAPT */

/**********************************************************************//**
Make room in the table cache by evicting an unused table. The unused table
should not be part of FK relationship and currently not used in any user
transaction. There is no guarantee that it will remove a table.
@return number of tables evicted. If the number of tables in the dict_LRU
is less than max_tables it will not do anything. */
ulint
dict_make_room_in_cache(
/*====================*/
	ulint		max_tables,	/*!< in: max tables allowed in cache */
	ulint		pct_check)	/*!< in: max percent to check */
{
	ulint		i;
	ulint		len;
	dict_table_t*	table;
	ulint		check_up_to;
	ulint		n_evicted = 0;

	ut_a(pct_check > 0);
	ut_a(pct_check <= 100);
	ut_d(dict_sys.assert_locked());
	ut_ad(dict_lru_validate());

	i = len = UT_LIST_GET_LEN(dict_sys.table_LRU);

	if (len < max_tables) {
		return(0);
	}

	check_up_to = len - ((len * pct_check) / 100);

	/* Check for overflow */
	ut_a(i == 0 || check_up_to <= i);

	/* Find a suitable candidate to evict from the cache. Don't scan the
	entire LRU list. Only scan pct_check list entries. */

	for (table = UT_LIST_GET_LAST(dict_sys.table_LRU);
	     table != NULL
	     && i > check_up_to
	     && (len - n_evicted) > max_tables;
	     --i) {

		dict_table_t*	prev_table;

	        prev_table = UT_LIST_GET_PREV(table_LRU, table);

		if (dict_table_can_be_evicted(table)) {
			ut_ad(!table->fts);
			dict_sys.remove(table, true);

			++n_evicted;
		}

		table = prev_table;
	}

	return(n_evicted);
}

/** Looks for an index with the given id given a table instance.
@param[in]	table	table instance
@param[in]	id	index id
@return index or NULL */
dict_index_t*
dict_table_find_index_on_id(
	const dict_table_t*	table,
	index_id_t		id)
{
	dict_index_t*	index;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (id == index->id) {
			/* Found */

			return(index);
		}
	}

	return(NULL);
}

/**********************************************************************//**
Looks for an index with the given id. NOTE that we do not reserve
the dictionary mutex: this function is for emergency purposes like
printing info of a corrupt database page!
@return index or NULL if not found in cache */
dict_index_t*
dict_index_find_on_id_low(
/*======================*/
	index_id_t	id)	/*!< in: index id */
{
	if (!dict_sys.is_initialised()) return NULL;

	dict_table_t*	table;

	for (table = UT_LIST_GET_FIRST(dict_sys.table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		dict_index_t*	index = dict_table_find_index_on_id(table, id);

		if (index != NULL) {
			return(index);
		}
	}

	for (table = UT_LIST_GET_FIRST(dict_sys.table_non_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		dict_index_t*	index = dict_table_find_index_on_id(table, id);

		if (index != NULL) {
			return(index);
		}
	}

	return(NULL);
}

/** Function object to remove a foreign key constraint from the
referenced_set of the referenced table.  The foreign key object is
also removed from the dictionary cache.  The foreign key constraint
is not removed from the foreign_set of the table containing the
constraint. */
struct dict_foreign_remove_partial
{
	void operator()(dict_foreign_t* foreign) {
		dict_table_t*	table = foreign->referenced_table;
		if (table != NULL) {
			table->referenced_set.erase(foreign);
		}
		dict_foreign_free(foreign);
	}
};

/**********************************************************************//**
Renames a table object.
@return TRUE if success */
dberr_t
dict_table_rename_in_cache(
/*=======================*/
	dict_table_t*	table,		/*!< in/out: table */
	const char*	new_name,	/*!< in: new name */
	bool		rename_also_foreigns,
					/*!< in: in ALTER TABLE we want
					to preserve the original table name
					in constraints which reference it */
	bool		replace_new_file)
					/*!< in: whether to replace the
					file with the new name
					(as part of rolling back TRUNCATE) */
{
	dberr_t		err;
	dict_foreign_t*	foreign;
	ulint		fold;
	char		old_name[MAX_FULL_NAME_LEN + 1];
	os_file_type_t	ftype;

	dict_sys.assert_locked();

	/* store the old/current name to an automatic variable */
	ut_a(strlen(table->name.m_name) < sizeof old_name);
	strcpy(old_name, table->name.m_name);

	fold = ut_fold_string(new_name);

	/* Look for a table with the same name: error if such exists */
	dict_table_t*	table2;
	HASH_SEARCH(name_hash, &dict_sys.table_hash, fold,
			dict_table_t*, table2, ut_ad(table2->cached),
			(strcmp(table2->name.m_name, new_name) == 0));
	DBUG_EXECUTE_IF("dict_table_rename_in_cache_failure",
		if (table2 == NULL) {
			table2 = (dict_table_t*) -1;
		} );
	if (table2) {
		ib::error() << "Cannot rename table '" << old_name
			<< "' to '" << new_name << "' since the"
			" dictionary cache already contains '" << new_name << "'.";
		return(DB_ERROR);
	}

	/* If the table is stored in a single-table tablespace, rename the
	.ibd file and rebuild the .isl file if needed. */

	if (!table->space) {
		bool		exists;
		char*		filepath;

		ut_ad(dict_table_is_file_per_table(table));
		ut_ad(!table->is_temporary());

		/* Make sure the data_dir_path is set. */
		dict_get_and_save_data_dir_path(table, true);

		if (DICT_TF_HAS_DATA_DIR(table->flags)) {
			ut_a(table->data_dir_path);

			filepath = fil_make_filepath(
				table->data_dir_path, table->name.m_name,
				IBD, true);
		} else {
			filepath = fil_make_filepath(
				NULL, table->name.m_name, IBD, false);
		}

		if (filepath == NULL) {
			return(DB_OUT_OF_MEMORY);
		}

		fil_delete_tablespace(table->space_id, !table->space);

		/* Delete any temp file hanging around. */
		if (os_file_status(filepath, &exists, &ftype)
		    && exists
		    && !os_file_delete_if_exists(innodb_temp_file_key,
						 filepath, NULL)) {

			ib::info() << "Delete of " << filepath << " failed.";
		}
		ut_free(filepath);

	} else if (dict_table_is_file_per_table(table)) {
		char*	new_path;
		const char* old_path = UT_LIST_GET_FIRST(table->space->chain)
			->name;

		ut_ad(!table->is_temporary());

		if (DICT_TF_HAS_DATA_DIR(table->flags)) {
			new_path = os_file_make_new_pathname(
				old_path, new_name);
			err = RemoteDatafile::create_link_file(
				new_name, new_path);

			if (err != DB_SUCCESS) {
				ut_free(new_path);
				return(DB_TABLESPACE_EXISTS);
			}
		} else {
			new_path = fil_make_filepath(
				NULL, new_name, IBD, false);
		}

		/* New filepath must not exist. */
		err = table->space->rename(new_name, new_path, true,
					   replace_new_file);
		ut_free(new_path);

		/* If the tablespace is remote, a new .isl file was created
		If success, delete the old one. If not, delete the new one. */
		if (DICT_TF_HAS_DATA_DIR(table->flags)) {
			RemoteDatafile::delete_link_file(
				err == DB_SUCCESS ? old_name : new_name);
		}

		if (err != DB_SUCCESS) {
			return err;
		}
	}

	/* Remove table from the hash tables of tables */
	HASH_DELETE(dict_table_t, name_hash, &dict_sys.table_hash,
		    ut_fold_string(old_name), table);

	if (strlen(new_name) > strlen(table->name.m_name)) {
		/* We allocate MAX_FULL_NAME_LEN + 1 bytes here to avoid
		memory fragmentation, we assume a repeated calls of
		ut_realloc() with the same size do not cause fragmentation */
		ut_a(strlen(new_name) <= MAX_FULL_NAME_LEN);

		table->name.m_name = static_cast<char*>(
			ut_realloc(table->name.m_name, MAX_FULL_NAME_LEN + 1));
	}
	strcpy(table->name.m_name, new_name);

	/* Add table to hash table of tables */
	HASH_INSERT(dict_table_t, name_hash, &dict_sys.table_hash, fold,
		    table);

	if (!rename_also_foreigns) {
		/* In ALTER TABLE we think of the rename table operation
		in the direction table -> temporary table (#sql...)
		as dropping the table with the old name and creating
		a new with the new name. Thus we kind of drop the
		constraints from the dictionary cache here. The foreign key
		constraints will be inherited to the new table from the
		system tables through a call of dict_load_foreigns. */

		/* Remove the foreign constraints from the cache */
		std::for_each(table->foreign_set.begin(),
			      table->foreign_set.end(),
			      dict_foreign_remove_partial());
		table->foreign_set.clear();

		/* Reset table field in referencing constraints */
		for (dict_foreign_set::iterator it
			= table->referenced_set.begin();
		     it != table->referenced_set.end();
		     ++it) {

			foreign = *it;
			foreign->referenced_table = NULL;
			foreign->referenced_index = NULL;

		}

		/* Make the set of referencing constraints empty */
		table->referenced_set.clear();

		return(DB_SUCCESS);
	}

	/* Update the table name fields in foreign constraints, and update also
	the constraint id of new format >= 4.0.18 constraints. Note that at
	this point we have already changed table->name to the new name. */

	dict_foreign_set	fk_set;

	for (;;) {

		dict_foreign_set::iterator	it
			= table->foreign_set.begin();

		if (it == table->foreign_set.end()) {
			break;
		}

		foreign = *it;

		if (foreign->referenced_table) {
			foreign->referenced_table->referenced_set.erase(foreign);
		}

		if (strlen(foreign->foreign_table_name)
		    < strlen(table->name.m_name)) {
			/* Allocate a longer name buffer;
			TODO: store buf len to save memory */

			foreign->foreign_table_name = mem_heap_strdup(
				foreign->heap, table->name.m_name);
			dict_mem_foreign_table_name_lookup_set(foreign, TRUE);
		} else {
			strcpy(foreign->foreign_table_name,
			       table->name.m_name);
			dict_mem_foreign_table_name_lookup_set(foreign, FALSE);
		}
		if (strchr(foreign->id, '/')) {
			/* This is a >= 4.0.18 format id */

			ulint	db_len;
			char*	old_id;
			char    old_name_cs_filename[MAX_FULL_NAME_LEN+1];
			uint    errors = 0;

			/* All table names are internally stored in charset
			my_charset_filename (except the temp tables and the
			partition identifier suffix in partition tables). The
			foreign key constraint names are internally stored
			in UTF-8 charset.  The variable fkid here is used
			to store foreign key constraint name in charset
			my_charset_filename for comparison further below. */
			char    fkid[MAX_TABLE_NAME_LEN+20];
			ibool	on_tmp = FALSE;

			/* The old table name in my_charset_filename is stored
			in old_name_cs_filename */

			strcpy(old_name_cs_filename, old_name);
			old_name_cs_filename[MAX_FULL_NAME_LEN] = '\0';
			if (strstr(old_name, TEMP_TABLE_PATH_PREFIX) == NULL) {

				innobase_convert_to_system_charset(
					strchr(old_name_cs_filename, '/') + 1,
					strchr(old_name, '/') + 1,
					MAX_TABLE_NAME_LEN, &errors);

				if (errors) {
					/* There has been an error to convert
					old table into UTF-8.  This probably
					means that the old table name is
					actually in UTF-8. */
					innobase_convert_to_filename_charset(
						strchr(old_name_cs_filename,
						       '/') + 1,
						strchr(old_name, '/') + 1,
						MAX_TABLE_NAME_LEN);
				} else {
					/* Old name already in
					my_charset_filename */
					strcpy(old_name_cs_filename, old_name);
					old_name_cs_filename[MAX_FULL_NAME_LEN]
						= '\0';
				}
			}

			strncpy(fkid, foreign->id, MAX_TABLE_NAME_LEN);

			if (strstr(fkid, TEMP_TABLE_PATH_PREFIX) == NULL) {
				innobase_convert_to_filename_charset(
					strchr(fkid, '/') + 1,
					strchr(foreign->id, '/') + 1,
					MAX_TABLE_NAME_LEN+20);
			} else {
				on_tmp = TRUE;
			}

			old_id = mem_strdup(foreign->id);

			if (strlen(fkid) > strlen(old_name_cs_filename)
			    + ((sizeof dict_ibfk) - 1)
			    && !memcmp(fkid, old_name_cs_filename,
				       strlen(old_name_cs_filename))
			    && !memcmp(fkid + strlen(old_name_cs_filename),
				       dict_ibfk, (sizeof dict_ibfk) - 1)) {

				/* This is a generated >= 4.0.18 format id */

				char	table_name[MAX_TABLE_NAME_LEN + 1];
				uint	errors = 0;

				if (strlen(table->name.m_name)
				    > strlen(old_name)) {
					foreign->id = static_cast<char*>(
						mem_heap_alloc(
						foreign->heap,
						strlen(table->name.m_name)
						+ strlen(old_id) + 1));
				}

				/* Convert the table name to UTF-8 */
				strncpy(table_name, table->name.m_name,
					MAX_TABLE_NAME_LEN);
				table_name[MAX_TABLE_NAME_LEN] = '\0';
				innobase_convert_to_system_charset(
					strchr(table_name, '/') + 1,
					strchr(table->name.m_name, '/') + 1,
					MAX_TABLE_NAME_LEN, &errors);

				if (errors) {
					/* Table name could not be converted
					from charset my_charset_filename to
					UTF-8. This means that the table name
					is already in UTF-8 (#mysql50#). */
					strncpy(table_name, table->name.m_name,
						MAX_TABLE_NAME_LEN);
					table_name[MAX_TABLE_NAME_LEN] = '\0';
				}

				/* Replace the prefix 'databasename/tablename'
				with the new names */
				strcpy(foreign->id, table_name);
				if (on_tmp) {
					strcat(foreign->id,
					       old_id + strlen(old_name));
				} else {
					sprintf(strchr(foreign->id, '/') + 1,
						"%s%s",
						strchr(table_name, '/') +1,
						strstr(old_id, "_ibfk_") );
				}

			} else {
				/* This is a >= 4.0.18 format id where the user
				gave the id name */
				db_len = dict_get_db_name_len(
					table->name.m_name) + 1;

				if (db_len - 1
				    > dict_get_db_name_len(foreign->id)) {

					foreign->id = static_cast<char*>(
						mem_heap_alloc(
						foreign->heap,
						db_len + strlen(old_id) + 1));
				}

				/* Replace the database prefix in id with the
				one from table->name */

				memcpy(foreign->id,
				       table->name.m_name, db_len);

				strcpy(foreign->id + db_len,
				       dict_remove_db_name(old_id));
			}

			ut_free(old_id);
		}

		table->foreign_set.erase(it);
		fk_set.insert(foreign);

		if (foreign->referenced_table) {
			foreign->referenced_table->referenced_set.insert(foreign);
		}
	}

	ut_a(table->foreign_set.empty());
	table->foreign_set.swap(fk_set);

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;

		if (strlen(foreign->referenced_table_name)
		    < strlen(table->name.m_name)) {
			/* Allocate a longer name buffer;
			TODO: store buf len to save memory */

			foreign->referenced_table_name = mem_heap_strdup(
				foreign->heap, table->name.m_name);

			dict_mem_referenced_table_name_lookup_set(
				foreign, TRUE);
		} else {
			/* Use the same buffer */
			strcpy(foreign->referenced_table_name,
			       table->name.m_name);

			dict_mem_referenced_table_name_lookup_set(
				foreign, FALSE);
		}
	}

	return(DB_SUCCESS);
}

/**********************************************************************//**
Change the id of a table object in the dictionary cache. This is used in
DISCARD TABLESPACE. */
void
dict_table_change_id_in_cache(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table object already in cache */
	table_id_t	new_id)	/*!< in: new id to set */
{
	dict_sys.assert_locked();
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!table->is_temporary());

	/* Remove the table from the hash table of id's */

	HASH_DELETE(dict_table_t, id_hash, &dict_sys.table_id_hash,
		    ut_fold_ull(table->id), table);
	table->id = new_id;

	/* Add the table back to the hash table */
	HASH_INSERT(dict_table_t, id_hash, &dict_sys.table_id_hash,
		    ut_fold_ull(table->id), table);
}

/** Evict a table definition from the InnoDB data dictionary cache.
@param[in,out]	table	cached table definition to be evicted
@param[in]	lru	whether this is part of least-recently-used eviction
@param[in]	keep	whether to keep (not free) the object */
void dict_sys_t::remove(dict_table_t* table, bool lru, bool keep)
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;

	ut_ad(dict_lru_validate());
	ut_a(table->get_ref_count() == 0);
	ut_a(table->n_rec_locks == 0);
	ut_ad(find(table));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Remove the foreign constraints from the cache */
	std::for_each(table->foreign_set.begin(), table->foreign_set.end(),
		      dict_foreign_remove_partial());
	table->foreign_set.clear();

	/* Reset table field in referencing constraints */
	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;
		foreign->referenced_table = NULL;
		foreign->referenced_index = NULL;
	}

	/* Remove the indexes from the cache */

	for (index = UT_LIST_GET_LAST(table->indexes);
	     index != NULL;
	     index = UT_LIST_GET_LAST(table->indexes)) {

		dict_index_remove_from_cache_low(table, index, lru);
	}

	/* Remove table from the hash tables of tables */

	HASH_DELETE(dict_table_t, name_hash, &table_hash,
		    ut_fold_string(table->name.m_name), table);

	hash_table_t* id_hash = table->is_temporary()
		? &temp_id_hash : &table_id_hash;
	const ulint id_fold = ut_fold_ull(table->id);
	HASH_DELETE(dict_table_t, id_hash, id_hash, id_fold, table);

	/* Remove table from LRU or non-LRU list. */
	if (table->can_be_evicted) {
		UT_LIST_REMOVE(table_LRU, table);
	} else {
		UT_LIST_REMOVE(table_non_LRU, table);
	}

	if (lru && table->drop_aborted) {
		/* When evicting the table definition,
		drop the orphan indexes from the data dictionary
		and free the index pages. */
		trx_t* trx = trx_create();

		ut_d(dict_sys.assert_locked());
		/* Mimic row_mysql_lock_data_dictionary(). */
		trx->dict_operation_lock_mode = RW_X_LATCH;

		trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);
		row_merge_drop_indexes_dict(trx, table->id);
		trx_commit_for_mysql(trx);
		trx->dict_operation_lock_mode = 0;
		trx->free();
	}

	/* Free virtual column template if any */
	if (table->vc_templ != NULL) {
		dict_free_vc_templ(table->vc_templ);
		UT_DELETE(table->vc_templ);
	}

	table->autoinc_mutex.~mutex();

	if (keep) {
		return;
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (UNIV_UNLIKELY(UT_LIST_GET_LEN(table->freed_indexes) != 0)) {
		if (table->fts) {
			fts_optimize_remove_table(table);
			fts_free(table);
			table->fts = NULL;
		}

		table->vc_templ = NULL;
		table->id = 0;
		return;
	}
#endif /* BTR_CUR_HASH_ADAPT */

	dict_mem_table_free(table);
}

/****************************************************************//**
If the given column name is reserved for InnoDB system columns, return
TRUE.
@return TRUE if name is reserved */
ibool
dict_col_name_is_reserved(
/*======================*/
	const char*	name)	/*!< in: column name */
{
	static const char*	reserved_names[] = {
		"DB_ROW_ID", "DB_TRX_ID", "DB_ROLL_PTR"
	};

	compile_time_assert(UT_ARR_SIZE(reserved_names) == DATA_N_SYS_COLS);

	for (ulint i = 0; i < UT_ARR_SIZE(reserved_names); i++) {
		if (innobase_strcasecmp(name, reserved_names[i]) == 0) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/** Adds an index to the dictionary cache, with possible indexing newly
added column.
@param[in,out]	index	index; NOTE! The index memory
			object is freed in this function!
@param[in]	page_no	root page number of the index
@param[in]	add_v	virtual columns being added along with ADD INDEX
@return DB_SUCCESS, or DB_CORRUPTION */
dberr_t
dict_index_add_to_cache(
	dict_index_t*&		index,
	ulint			page_no,
	const dict_add_v_col_t* add_v)
{
	dict_index_t*	new_index;
	ulint		n_ord;
	ulint		i;

	dict_sys.assert_locked();
	ut_ad(index->n_def == index->n_fields);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(!dict_index_is_ibuf(index));

	ut_d(mem_heap_validate(index->heap));
	ut_a(!dict_index_is_clust(index)
	     || UT_LIST_GET_LEN(index->table->indexes) == 0);
	ut_ad(dict_index_is_clust(index) || !index->table->no_rollback());

	if (!dict_index_find_cols(index, add_v)) {

		dict_mem_index_free(index);
		index = NULL;
		return DB_CORRUPTION;
	}

	/* Build the cache internal representation of the index,
	containing also the added system fields */

	if (dict_index_is_clust(index)) {
		new_index = dict_index_build_internal_clust(index);
	} else {
		new_index = (index->type & DICT_FTS)
			? dict_index_build_internal_fts(index)
			: dict_index_build_internal_non_clust(index);
		new_index->n_core_null_bytes = static_cast<uint8_t>(
			UT_BITS_IN_BYTES(unsigned(new_index->n_nullable)));
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields in the cache internal representation */

	new_index->n_fields = new_index->n_def;
	new_index->trx_id = index->trx_id;
	new_index->set_committed(index->is_committed());
	new_index->nulls_equal = index->nulls_equal;
#ifdef MYSQL_INDEX_DISABLE_AHI
	new_index->disable_ahi = index->disable_ahi;
#endif

	n_ord = new_index->n_uniq;
	/* Flag the ordering columns and also set column max_prefix */

	for (i = 0; i < n_ord; i++) {
		const dict_field_t*	field
			= dict_index_get_nth_field(new_index, i);

		/* Check the column being added in the index for
		the first time and flag the ordering column. */
		if (field->col->ord_part == 0 ) {
			field->col->max_prefix = field->prefix_len;
			field->col->ord_part = 1;
		} else if (field->prefix_len == 0) {
			/* Set the max_prefix for a column to 0 if
			its prefix length is 0 (for this index)
			even if it was a part of any other index
			with some prefix length. */
			field->col->max_prefix = 0;
		} else if (field->col->max_prefix != 0
			   && field->prefix_len
			   > field->col->max_prefix) {
			/* Set the max_prefix value based on the
			prefix_len. */
			ut_ad(field->col->is_binary()
			      || field->prefix_len % field->col->mbmaxlen == 0);
			field->col->max_prefix = field->prefix_len;
		}
		ut_ad(field->col->ord_part == 1);
	}

	new_index->stat_n_diff_key_vals =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_diff_key_vals)));

	new_index->stat_n_sample_sizes =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_sample_sizes)));

	new_index->stat_n_non_null_key_vals =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_non_null_key_vals)));

	new_index->stat_index_size = 1;
	new_index->stat_n_leaf_pages = 1;

	new_index->stat_defrag_n_pages_freed = 0;
	new_index->stat_defrag_n_page_split = 0;

	new_index->stat_defrag_sample_next_slot = 0;
	memset(&new_index->stat_defrag_data_size_sample,
	       0x0, sizeof(ulint) * STAT_DEFRAG_DATA_SIZE_N_SAMPLE);

	/* Add the new index as the last index for the table */

	UT_LIST_ADD_LAST(new_index->table->indexes, new_index);
#ifdef BTR_CUR_ADAPT
	new_index->search_info = btr_search_info_create(new_index->heap);
#endif /* BTR_CUR_ADAPT */

	new_index->page = unsigned(page_no);
	new_index->lock.SRW_LOCK_INIT(index_tree_rw_lock_key);

	new_index->n_core_fields = new_index->n_fields;

	dict_mem_index_free(index);
	index = new_index;
	return DB_SUCCESS;
}

/**********************************************************************//**
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in/out: table */
	dict_index_t*	index,		/*!< in, own: index */
	ibool		lru_evict)	/*!< in: TRUE if index being evicted
					to make room in the table LRU list */
{
	ut_ad(table && index);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	dict_sys.assert_locked();
	ut_ad(table->id);
#ifdef BTR_CUR_HASH_ADAPT
	ut_ad(!index->freed());
#endif /* BTR_CUR_HASH_ADAPT */

	/* No need to acquire the dict_index_t::lock here because
	there can't be any active operations on this index (or table). */

	if (index->online_log) {
		ut_ad(index->online_status == ONLINE_INDEX_CREATION);
		row_log_free(index->online_log);
		index->online_log = NULL;
	}

	/* Remove the index from the list of indexes of the table */
	UT_LIST_REMOVE(table->indexes, index);

	/* The index is being dropped, remove any compression stats for it. */
	if (!lru_evict && DICT_TF_GET_ZIP_SSIZE(index->table->flags)) {
		mysql_mutex_lock(&page_zip_stat_per_index_mutex);
		page_zip_stat_per_index.erase(index->id);
		mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
	}

	/* Remove the index from affected virtual column index list */
	index->detach_columns();

#ifdef BTR_CUR_HASH_ADAPT
	/* We always create search info whether or not adaptive
	hash index is enabled or not. */
	/* We are not allowed to free the in-memory index struct
	dict_index_t until all entries in the adaptive hash index
	that point to any of the page belonging to his b-tree index
	are dropped. This is so because dropping of these entries
	require access to dict_index_t struct. To avoid such scenario
	We keep a count of number of such pages in the search_info and
	only free the dict_index_t struct when this count drops to
	zero. See also: dict_table_can_be_evicted() */

	if (index->n_ahi_pages()) {
		index->set_freed();
		UT_LIST_ADD_LAST(table->freed_indexes, index);
		return;
	}
#endif /* BTR_CUR_HASH_ADAPT */

	index->lock.free();

	dict_mem_index_free(index);
}

/**********************************************************************//**
Removes an index from the dictionary cache. */
void
dict_index_remove_from_cache(
/*=========================*/
	dict_table_t*	table,	/*!< in/out: table */
	dict_index_t*	index)	/*!< in, own: index */
{
	dict_index_remove_from_cache_low(table, index, FALSE);
}

/** Tries to find column names for the index and sets the col field of the
index.
@param[in]	table	table
@param[in,out]	index	index
@param[in]	add_v	new virtual columns added along with an add index call
@return whether the column names were found */
static
bool
dict_index_find_cols(
	dict_index_t*		index,
	const dict_add_v_col_t*	add_v)
{
	std::vector<ulint, ut_allocator<ulint> >	col_added;
	std::vector<ulint, ut_allocator<ulint> >	v_col_added;

	const dict_table_t* table = index->table;
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	dict_sys.assert_locked();

	for (ulint i = 0; i < index->n_fields; i++) {
		ulint		j;
		dict_field_t*	field = dict_index_get_nth_field(index, i);

		for (j = 0; j < table->n_cols; j++) {
			if (!innobase_strcasecmp(dict_table_get_col_name(table, j),
				    field->name)) {

				/* Check if same column is being assigned again
				which suggest that column has duplicate name. */
				bool exists =
					std::find(col_added.begin(),
						  col_added.end(), j)
					!= col_added.end();

				if (exists) {
					/* Duplicate column found. */
					goto dup_err;
				}

				field->col = dict_table_get_nth_col(table, j);

				col_added.push_back(j);

				goto found;
			}
		}

		/* Let's check if it is a virtual column */
		for (j = 0; j < table->n_v_cols; j++) {
			if (!strcmp(dict_table_get_v_col_name(table, j),
				    field->name)) {

				/* Check if same column is being assigned again
				which suggest that column has duplicate name. */
				bool exists =
					std::find(v_col_added.begin(),
						  v_col_added.end(), j)
					!= v_col_added.end();

				if (exists) {
					/* Duplicate column found. */
					break;
				}

				field->col = reinterpret_cast<dict_col_t*>(
					dict_table_get_nth_v_col(table, j));

				v_col_added.push_back(j);

				goto found;
			}
		}

		if (add_v) {
			for (j = 0; j < add_v->n_v_col; j++) {
				if (!strcmp(add_v->v_col_name[j],
					    field->name)) {
					field->col = const_cast<dict_col_t*>(
						&add_v->v_col[j].m_col);
					goto found;
				}
			}
		}

dup_err:
#ifdef UNIV_DEBUG
		/* It is an error not to find a matching column. */
		ib::error() << "No matching column for " << field->name
			<< " in index " << index->name
			<< " of table " << table->name;
#endif /* UNIV_DEBUG */
		return(FALSE);

found:
		;
	}

	return(TRUE);
}

/*******************************************************************//**
Adds a column to index. */
void
dict_index_add_col(
/*===============*/
	dict_index_t*		index,		/*!< in/out: index */
	const dict_table_t*	table,		/*!< in: table */
	dict_col_t*		col,		/*!< in: column */
	ulint			prefix_len)	/*!< in: column prefix length */
{
	dict_field_t*	field;
	const char*	col_name;

	if (col->is_virtual()) {
		dict_v_col_t*	v_col = reinterpret_cast<dict_v_col_t*>(col);
		/* Register the index with the virtual column index list */
		v_col->v_indexes.push_front(dict_v_idx_t(index, index->n_def));
		col_name = dict_table_get_v_col_name_mysql(
			table, dict_col_get_no(col));
	} else {
		col_name = dict_table_get_col_name(table, dict_col_get_no(col));
	}

	dict_mem_index_add_field(index, col_name, prefix_len);

	field = dict_index_get_nth_field(index, unsigned(index->n_def) - 1);

	field->col = col;
	field->fixed_len = static_cast<uint16_t>(
		dict_col_get_fixed_size(
			col, dict_table_is_comp(table)))
		& ((1U << 10) - 1);

	if (prefix_len && field->fixed_len > prefix_len) {
		field->fixed_len = static_cast<uint16_t>(prefix_len)
			& ((1U << 10) - 1);
	}

	/* Long fixed-length fields that need external storage are treated as
	variable-length fields, so that the extern flag can be embedded in
	the length word. */

	if (field->fixed_len > DICT_MAX_FIXED_COL_LEN) {
		field->fixed_len = 0;
	}

	/* The comparison limit above must be constant.  If it were
	changed, the disk format of some fixed-length columns would
	change, which would be a disaster. */
	compile_time_assert(DICT_MAX_FIXED_COL_LEN == 768);

	if (!(col->prtype & DATA_NOT_NULL)) {
		index->n_nullable++;
	}
}

/*******************************************************************//**
Copies fields contained in index2 to index1. */
static
void
dict_index_copy(
/*============*/
	dict_index_t*		index1,	/*!< in: index to copy to */
	const dict_index_t*	index2,	/*!< in: index to copy from */
	ulint			start,	/*!< in: first position to copy */
	ulint			end)	/*!< in: last position to copy */
{
	dict_field_t*	field;
	ulint		i;

	/* Copy fields contained in index2 */

	for (i = start; i < end; i++) {

		field = dict_index_get_nth_field(index2, i);

		dict_index_add_col(index1, index2->table, field->col,
				   field->prefix_len);
	}
}

/*******************************************************************//**
Copies types of fields contained in index to tuple. */
void
dict_index_copy_types(
/*==================*/
	dtuple_t*		tuple,		/*!< in/out: data tuple */
	const dict_index_t*	index,		/*!< in: index */
	ulint			n_fields)	/*!< in: number of
						field types to copy */
{
	ulint		i;

	if (dict_index_is_ibuf(index)) {
		dtuple_set_types_binary(tuple, n_fields);

		return;
	}

	for (i = 0; i < n_fields; i++) {
		const dict_field_t*	ifield;
		dtype_t*		dfield_type;

		ifield = dict_index_get_nth_field(index, i);
		dfield_type = dfield_get_type(dtuple_get_nth_field(tuple, i));
		dict_col_copy_type(dict_field_get_col(ifield), dfield_type);
		if (dict_index_is_spatial(index)
		    && DATA_GEOMETRY_MTYPE(dfield_type->mtype)) {
			dfield_type->prtype |= DATA_GIS_MBR;
		}
	}
}

/** Copies types of virtual columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create().
@param[in,out]	tuple	data tuple
@param[in]	table	table
*/
void
dict_table_copy_v_types(
	dtuple_t*		tuple,
	const dict_table_t*	table)
{
	/* tuple could have more virtual columns than existing table,
	if we are calling this for creating index along with adding
	virtual columns */
	ulint	n_fields = ut_min(dtuple_get_n_v_fields(tuple),
				  static_cast<ulint>(table->n_v_def));

	for (ulint i = 0; i < n_fields; i++) {

		dfield_t*	dfield	= dtuple_get_nth_v_field(tuple, i);
		dtype_t*	dtype	= dfield_get_type(dfield);

		dfield_set_null(dfield);
		dict_col_copy_type(
			&(dict_table_get_nth_v_col(table, i)->m_col),
			dtype);
	}
}
/*******************************************************************//**
Copies types of columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create(). */
void
dict_table_copy_types(
/*==================*/
	dtuple_t*		tuple,	/*!< in/out: data tuple */
	const dict_table_t*	table)	/*!< in: table */
{
	ulint		i;

	for (i = 0; i < dtuple_get_n_fields(tuple); i++) {

		dfield_t*	dfield	= dtuple_get_nth_field(tuple, i);
		dtype_t*	dtype	= dfield_get_type(dfield);

		dfield_set_null(dfield);
		dict_col_copy_type(dict_table_get_nth_col(table, i), dtype);
	}

	dict_table_copy_v_types(tuple, table);
}

/*******************************************************************//**
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the clustered index */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
	dict_index_t*		index)	/*!< in: user representation of
					a clustered index */
{
	dict_table_t*	table = index->table;
	dict_index_t*	new_index;
	dict_field_t*	field;
	ulint		trx_id_pos;
	ulint		i;
	ibool*		indexed;

	ut_ad(dict_index_is_clust(index));
	ut_ad(!dict_index_is_ibuf(index));

	dict_sys.assert_locked();

	/* Create a new index object with certainly enough fields */
	new_index = dict_mem_index_create(index->table, index->name,
					  index->type,
					  unsigned(index->n_fields
						   + table->n_cols));

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy the fields of index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	if (dict_index_is_unique(index)) {
		/* Only the fields defined so far are needed to identify
		the index entry uniquely */

		new_index->n_uniq = new_index->n_def;
	} else {
		/* Also the row id is needed to identify the entry */
		new_index->n_uniq = unsigned(new_index->n_def + 1)
			& dict_index_t::MAX_N_FIELDS;
	}

	new_index->trx_id_offset = 0;

	/* Add system columns, trx id first */

	trx_id_pos = new_index->n_def;

	compile_time_assert(DATA_ROW_ID == 0);
	compile_time_assert(DATA_TRX_ID == 1);
	compile_time_assert(DATA_ROLL_PTR == 2);

	if (!dict_index_is_unique(index)) {
		dict_index_add_col(new_index, table,
				   dict_table_get_sys_col(
					   table, DATA_ROW_ID),
				   0);
		trx_id_pos++;
	}

	dict_index_add_col(
		new_index, table,
		dict_table_get_sys_col(table, DATA_TRX_ID), 0);

	for (i = 0; i < trx_id_pos; i++) {

		ulint	fixed_size = dict_col_get_fixed_size(
			dict_index_get_nth_col(new_index, i),
			dict_table_is_comp(table));

		if (fixed_size == 0) {
			new_index->trx_id_offset = 0;

			break;
		}

		dict_field_t* field = dict_index_get_nth_field(
			new_index, i);
		if (field->prefix_len > 0) {
			new_index->trx_id_offset = 0;

			break;
		}

		/* Add fixed_size to new_index->trx_id_offset.
		Because the latter is a bit-field, an overflow
		can theoretically occur. Check for it. */
		fixed_size += new_index->trx_id_offset;

		new_index->trx_id_offset = static_cast<unsigned>(fixed_size)
			& ((1U << 12) - 1);

		if (new_index->trx_id_offset != fixed_size) {
			/* Overflow. Pretend that this is a
			variable-length PRIMARY KEY. */
			ut_ad(0);
			new_index->trx_id_offset = 0;
			break;
		}
	}

	dict_index_add_col(
		new_index, table,
		dict_table_get_sys_col(table, DATA_ROLL_PTR), 0);

	/* Remember the table columns already contained in new_index */
	indexed = static_cast<ibool*>(
		ut_zalloc_nokey(table->n_cols * sizeof *indexed));

	/* Mark the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

			indexed[field->col->ind] = TRUE;
		}
	}

	/* Add to new_index non-system columns of table not yet included
	there */
	for (i = 0; i + DATA_N_SYS_COLS < ulint(table->n_cols); i++) {
		dict_col_t*	col = dict_table_get_nth_col(table, i);
		ut_ad(col->mtype != DATA_SYS);

		if (!indexed[col->ind]) {
			dict_index_add_col(new_index, table, col, 0);
		}
	}

	ut_free(indexed);

	ut_ad(UT_LIST_GET_LEN(table->indexes) == 0);

	new_index->n_core_null_bytes = table->supports_instant()
		? dict_index_t::NO_CORE_NULL_BYTES
		: static_cast<uint8_t>(
			UT_BITS_IN_BYTES(unsigned(new_index->n_nullable)));
	new_index->cached = TRUE;

	return(new_index);
}

/*******************************************************************//**
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the non-clustered index */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
	dict_index_t*		index)	/*!< in: user representation of
					a non-clustered index */
{
	dict_field_t*	field;
	dict_index_t*	new_index;
	dict_index_t*	clust_index;
	dict_table_t*	table = index->table;
	ulint		i;
	ibool*		indexed;

	ut_ad(table && index);
	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_ibuf(index));
	dict_sys.assert_locked();

	/* The clustered index should be the first in the list of indexes */
	clust_index = UT_LIST_GET_FIRST(table->indexes);

	ut_ad(clust_index);
	ut_ad(dict_index_is_clust(clust_index));
	ut_ad(!dict_index_is_ibuf(clust_index));

	/* Create a new index */
	new_index = dict_mem_index_create(
		index->table, index->name, index->type,
		ulint(index->n_fields + 1 + clust_index->n_uniq));

	/* Copy other relevant data from the old index
	struct to the new struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	/* Remember the table columns already contained in new_index */
	indexed = static_cast<ibool*>(
		ut_zalloc_nokey(table->n_cols * sizeof *indexed));

	/* Mark the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		if (field->col->is_virtual()) {
			continue;
		}

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

			indexed[field->col->ind] = TRUE;
		}
	}

	/* Add to new_index the columns necessary to determine the clustered
	index entry uniquely */

	for (i = 0; i < clust_index->n_uniq; i++) {

		field = dict_index_get_nth_field(clust_index, i);

		if (!indexed[field->col->ind]) {
			dict_index_add_col(new_index, table, field->col,
					   field->prefix_len);
		} else if (dict_index_is_spatial(index)) {
			/*For spatial index, we still need to add the
			field to index. */
			dict_index_add_col(new_index, table, field->col,
					   field->prefix_len);
		}
	}

	ut_free(indexed);

	if (dict_index_is_unique(index)) {
		new_index->n_uniq = index->n_fields;
	} else {
		new_index->n_uniq = new_index->n_def;
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields */

	new_index->n_fields = new_index->n_def;

	new_index->cached = TRUE;

	return(new_index);
}

/***********************************************************************
Builds the internal dictionary cache representation for an FTS index.
@return own: the internal representation of the FTS index */
static
dict_index_t*
dict_index_build_internal_fts(
/*==========================*/
	dict_index_t*	index)	/*!< in: user representation of an FTS index */
{
	dict_index_t*	new_index;

	ut_ad(index->type == DICT_FTS);
	dict_sys.assert_locked();

	/* Create a new index */
	new_index = dict_mem_index_create(index->table, index->name,
					  index->type, index->n_fields);

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	new_index->n_uniq = 0;
	new_index->cached = TRUE;

	dict_table_t* table = index->table;

	if (table->fts->cache == NULL) {
		table->fts->cache = fts_cache_create(table);
	}

	mysql_mutex_lock(&table->fts->cache->init_lock);
	/* Notify the FTS cache about this index. */
	fts_cache_index_cache_create(table, new_index);
	mysql_mutex_unlock(&table->fts->cache->init_lock);

	return(new_index);
}
/*====================== FOREIGN KEY PROCESSING ========================*/

/*********************************************************************//**
Checks if a table is referenced by foreign keys.
@return TRUE if table is referenced by a foreign key */
ibool
dict_table_is_referenced_by_foreign_key(
/*====================================*/
	const dict_table_t*	table)	/*!< in: InnoDB table */
{
	return(!table->referenced_set.empty());
}

/**********************************************************************//**
Removes a foreign constraint struct from the dictionary cache. */
void
dict_foreign_remove_from_cache(
/*===========================*/
	dict_foreign_t*	foreign)	/*!< in, own: foreign constraint */
{
	dict_sys.assert_locked();
	ut_a(foreign);

	if (foreign->referenced_table != NULL) {
		foreign->referenced_table->referenced_set.erase(foreign);
	}

	if (foreign->foreign_table != NULL) {
		foreign->foreign_table->foreign_set.erase(foreign);
	}

	dict_foreign_free(foreign);
}

/**********************************************************************//**
Looks for the foreign constraint from the foreign and referenced lists
of a table.
@return foreign constraint */
static
dict_foreign_t*
dict_foreign_find(
/*==============*/
	dict_table_t*	table,		/*!< in: table object */
	dict_foreign_t*	foreign)	/*!< in: foreign constraint */
{
	dict_sys.assert_locked();

	ut_ad(dict_foreign_set_validate(table->foreign_set));
	ut_ad(dict_foreign_set_validate(table->referenced_set));

	dict_foreign_set::iterator it = table->foreign_set.find(foreign);

	if (it != table->foreign_set.end()) {
		return(*it);
	}

	it = table->referenced_set.find(foreign);

	if (it != table->referenced_set.end()) {
		return(*it);
	}

	return(NULL);
}

/*********************************************************************//**
Tries to find an index whose first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return matching index, NULL if not found */
dict_index_t*
dict_foreign_find_index(
/*====================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
{
	dict_sys.assert_locked();

	if (error) {
		*error = FK_INDEX_NOT_FOUND;
	}

	for (dict_index_t* index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {
		if (types_idx != index
		    && !index->to_be_dropped
		    && !dict_index_is_online_ddl(index)
		    && dict_foreign_qualify_index(
			    table, col_names, columns, n_cols,
			    index, types_idx,
			    check_charsets, check_null,
			    error, err_col_no, err_index)) {
			if (error) {
				*error = FK_SUCCESS;
			}

			return(index);
		}
	}

	return(NULL);
}
/**********************************************************************//**
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report_low(
/*==========================*/
	FILE*		file,	/*!< in: output stream */
	const char*	name)	/*!< in: table name */
{
	rewind(file);
	ut_print_timestamp(file);
	fprintf(file, " Error in foreign key constraint of table %s:\n",
		name);
}

/**********************************************************************//**
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report(
/*======================*/
	FILE*		file,	/*!< in: output stream */
	dict_foreign_t*	fk,	/*!< in: foreign key constraint */
	const char*	msg)	/*!< in: the error message */
{
	std::string fk_str;
	mysql_mutex_lock(&dict_foreign_err_mutex);
	dict_foreign_error_report_low(file, fk->foreign_table_name);
	fputs(msg, file);
	fputs(" Constraint:\n", file);
	fk_str = dict_print_info_on_foreign_key_in_create_format(NULL, fk, TRUE);
	fputs(fk_str.c_str(), file);
	putc('\n', file);
	if (fk->foreign_index) {
		fprintf(file, "The index in the foreign key in table is"
			" %s\n%s\n", fk->foreign_index->name(),
			FOREIGN_KEY_CONSTRAINTS_MSG);
	}
	mysql_mutex_unlock(&dict_foreign_err_mutex);
}

/**********************************************************************//**
Adds a foreign key constraint object to the dictionary cache. May free
the object if there already is an object with the same identifier in.
At least one of the foreign table and the referenced table must already
be in the dictionary cache!
@return DB_SUCCESS or error code */
dberr_t
dict_foreign_add_to_cache(
/*======================*/
	dict_foreign_t*		foreign,
				/*!< in, own: foreign key constraint */
	const char**		col_names,
				/*!< in: column names, or NULL to use
				foreign->foreign_table->col_names */
	bool			check_charsets,
				/*!< in: whether to check charset
				compatibility */
	dict_err_ignore_t	ignore_err)
				/*!< in: error to be ignored */
{
	dict_table_t*	for_table;
	dict_table_t*	ref_table;
	dict_foreign_t*	for_in_cache		= NULL;
	dict_index_t*	index;
	ibool		added_to_referenced_list= FALSE;
	ibool		added_to_foreign_list	= FALSE;
	FILE*		ef			= dict_foreign_err_file;

	DBUG_ENTER("dict_foreign_add_to_cache");
	DBUG_PRINT("dict_foreign_add_to_cache", ("id: %s", foreign->id));

	dict_sys.assert_locked();

	for_table = dict_table_check_if_in_cache_low(
		foreign->foreign_table_name_lookup);

	ref_table = dict_table_check_if_in_cache_low(
		foreign->referenced_table_name_lookup);
	ut_a(for_table || ref_table);

	if (for_table) {
		for_in_cache = dict_foreign_find(for_table, foreign);
	}

	if (!for_in_cache && ref_table) {
		for_in_cache = dict_foreign_find(ref_table, foreign);
	}

	if (for_in_cache) {
		if (foreign != for_in_cache) {
			if (for_table != for_in_cache->foreign_table) {
				dict_foreign_remove_from_cache(for_in_cache);
				for_in_cache = foreign;
			} else {
				dict_foreign_free(foreign);
			}
		}
	} else {
		for_in_cache = foreign;
	}

	if (ref_table && !for_in_cache->referenced_table) {
		index = dict_foreign_find_index(
			ref_table, NULL,
			for_in_cache->referenced_col_names,
			for_in_cache->n_fields, for_in_cache->foreign_index,
			check_charsets, false);

		if (index == NULL
		    && !(ignore_err & DICT_ERR_IGNORE_FK_NOKEY)) {
			dict_foreign_error_report(
				ef, for_in_cache,
				"there is no index in referenced table"
				" which would contain\n"
				"the columns as the first columns,"
				" or the data types in the\n"
				"referenced table do not match"
				" the ones in table.");

			if (for_in_cache == foreign) {
				dict_foreign_free(foreign);
			}

			DBUG_RETURN(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->referenced_table = ref_table;
		for_in_cache->referenced_index = index;

		std::pair<dict_foreign_set::iterator, bool>	ret
			= ref_table->referenced_set.insert(for_in_cache);

		ut_a(ret.second);	/* second is true if the insertion
					took place */
		added_to_referenced_list = TRUE;
	}

	if (for_table && !for_in_cache->foreign_table) {
		index = dict_foreign_find_index(
			for_table, col_names,
			for_in_cache->foreign_col_names,
			for_in_cache->n_fields,
			for_in_cache->referenced_index, check_charsets,
			for_in_cache->type
			& (DICT_FOREIGN_ON_DELETE_SET_NULL
			   | DICT_FOREIGN_ON_UPDATE_SET_NULL));

		if (index == NULL
		    && !(ignore_err & DICT_ERR_IGNORE_FK_NOKEY)) {
			dict_foreign_error_report(
				ef, for_in_cache,
				"there is no index in the table"
				" which would contain\n"
				"the columns as the first columns,"
				" or the data types in the\n"
				"table do not match"
				" the ones in the referenced table\n"
				"or one of the ON ... SET NULL columns"
				" is declared NOT NULL.");

			if (for_in_cache == foreign) {
				if (added_to_referenced_list) {
					const dict_foreign_set::size_type
						n = ref_table->referenced_set
						  .erase(for_in_cache);

					ut_a(n == 1);	/* the number of
							elements removed must
							be one */
				}

				dict_foreign_free(foreign);
			}

			DBUG_RETURN(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->foreign_table = for_table;
		for_in_cache->foreign_index = index;

		std::pair<dict_foreign_set::iterator, bool>	ret
			= for_table->foreign_set.insert(for_in_cache);

		ut_a(ret.second);	/* second is true if the insertion
					took place */
		added_to_foreign_list = true;
	}

	/* We need to move the table to the non-LRU end of the table LRU
	list. Otherwise it will be evicted from the cache. */

	if (ref_table != NULL && added_to_referenced_list) {
		dict_sys.prevent_eviction(ref_table);
	}

	if (for_table != NULL && added_to_foreign_list) {
		dict_sys.prevent_eviction(for_table);
	}

	if (!for_in_cache->v_cols
	    && (added_to_foreign_list || added_to_referenced_list)) {
		dict_mem_foreign_fill_vcol_set(for_in_cache);
	}

	ut_ad(dict_lru_validate());
	DBUG_RETURN(DB_SUCCESS);
}

/*********************************************************************//**
Scans from pointer onwards. Stops if is at the start of a copy of
'string' where characters are compared without case sensitivity, and
only outside `` or "" quotes. Stops also at NUL.
@return scanned up to this */
static
const char*
dict_scan_to(
/*=========*/
	const char*	ptr,	/*!< in: scan from */
	const char*	string)	/*!< in: look for this */
{
	char	quote	= '\0';
	bool	escape	= false;

	for (; *ptr; ptr++) {
		if (*ptr == quote) {
			/* Closing quote character: do not look for
			starting quote or the keyword. */

			/* If the quote character is escaped by a
			backslash, ignore it. */
			if (escape) {
				escape = false;
			} else {
				quote = '\0';
			}
		} else if (quote) {
			/* Within quotes: do nothing. */
			if (escape) {
				escape = false;
			} else if (*ptr == '\\') {
				escape = true;
			}
		} else if (*ptr == '`' || *ptr == '"' || *ptr == '\'') {
			/* Starting quote: remember the quote character. */
			quote = *ptr;
		} else {
			/* Outside quotes: look for the keyword. */
			ulint	i;
			for (i = 0; string[i]; i++) {
				if (toupper((int)(unsigned char)(ptr[i]))
				    != toupper((int)(unsigned char)
					       (string[i]))) {
					goto nomatch;
				}
			}
			break;
nomatch:
			;
		}
	}

	return(ptr);
}

/*********************************************************************//**
Accepts a specified string. Comparisons are case-insensitive.
@return if string was accepted, the pointer is moved after that, else
ptr is returned */
static
const char*
dict_accept(
/*========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scan from this */
	const char*	string,	/*!< in: accept only this string as the next
				non-whitespace string */
	ibool*		success)/*!< out: TRUE if accepted */
{
	const char*	old_ptr = ptr;
	const char*	old_ptr2;

	*success = FALSE;

	while (my_isspace(cs, *ptr)) {
		ptr++;
	}

	old_ptr2 = ptr;

	ptr = dict_scan_to(ptr, string);

	if (*ptr == '\0' || old_ptr2 != ptr) {
		return(old_ptr);
	}

	*success = TRUE;

	return ptr + strlen(string);
}

/*********************************************************************//**
Scans an id. For the lexical definition of an 'id', see the code below.
Strips backquotes or double quotes from around the id.
@return scanned to */
static
const char*
dict_scan_id(
/*=========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scanned to */
	mem_heap_t*	heap,	/*!< in: heap where to allocate the id
				(NULL=id will not be allocated, but it
				will point to string near ptr) */
	const char**	id,	/*!< out,own: the id; NULL if no id was
				scannable */
	ibool		table_id,/*!< in: TRUE=convert the allocated id
				as a table name; FALSE=convert to UTF-8 */
	ibool		accept_also_dot)
				/*!< in: TRUE if also a dot can appear in a
				non-quoted id; in a quoted id it can appear
				always */
{
	char		quote	= '\0';
	ulint		len	= 0;
	const char*	s;
	char*		str;
	char*		dst;

	*id = NULL;

	while (my_isspace(cs, *ptr)) {
		ptr++;
	}

	if (*ptr == '\0') {

		return(ptr);
	}

	if (*ptr == '`' || *ptr == '"') {
		quote = *ptr++;
	}

	s = ptr;

	if (quote) {
		for (;;) {
			if (!*ptr) {
				/* Syntax error */
				return(ptr);
			}
			if (*ptr == quote) {
				ptr++;
				if (*ptr != quote) {
					break;
				}
			}
			ptr++;
			len++;
		}
	} else {
		while (!my_isspace(cs, *ptr) && *ptr != '(' && *ptr != ')'
		       && (accept_also_dot || *ptr != '.')
		       && *ptr != ',' && *ptr != '\0') {

			ptr++;
		}

		len = ulint(ptr - s);
	}

	if (heap == NULL) {
		/* no heap given: id will point to source string */
		*id = s;
		return(ptr);
	}

	if (quote) {
		char*	d;

		str = d = static_cast<char*>(
			mem_heap_alloc(heap, len + 1));

		while (len--) {
			if ((*d++ = *s++) == quote) {
				s++;
			}
		}
		*d++ = 0;
		len = ulint(d - str);
		ut_ad(*s == quote);
		ut_ad(s + 1 == ptr);
	} else {
		str = mem_heap_strdupl(heap, s, len);
	}

	if (!table_id) {
convert_id:
		/* Convert the identifier from connection character set
		to UTF-8. */
		len = 3 * len + 1;
		*id = dst = static_cast<char*>(mem_heap_alloc(heap, len));

		innobase_convert_from_id(cs, dst, str, len);
	} else if (!strncmp(str, srv_mysql50_table_name_prefix,
			    sizeof(srv_mysql50_table_name_prefix) - 1)) {
		/* This is a pre-5.1 table name
		containing chars other than [A-Za-z0-9].
		Discard the prefix and use raw UTF-8 encoding. */
		str += sizeof(srv_mysql50_table_name_prefix) - 1;
		len -= sizeof(srv_mysql50_table_name_prefix) - 1;
		goto convert_id;
	} else {
		/* Encode using filename-safe characters. */
		len = 5 * len + 1;
		*id = dst = static_cast<char*>(mem_heap_alloc(heap, len));

		innobase_convert_from_table_id(cs, dst, str, len);
	}

	return(ptr);
}

bool
dict_table_t::build_name(
	const char*    database_name,	  /*!< in: table db name */
	ulint	       database_name_len, /*!< in: db name length */
	const char*    table_name,	  /*!< in: table name */
	ulint	       table_name_len,	  /*!< in: table name length */
	char *&dict_name,
	ulint &dict_name_len,
	mem_heap_t*    alloc,
	CHARSET_INFO*  cs_db,
	CHARSET_INFO*  cs_table) /*!< in: table name charset */
{
	char		db_name[MAX_DATABASE_NAME_LEN];
	char		tbl_name[MAX_TABLE_NAME_LEN];
	CHARSET_INFO*	to_cs = &my_charset_filename;
	uint		errors;
	ut_ad(database_name);
	ut_ad(database_name_len);
	ut_ad(table_name);
	ut_ad(table_name_len);

	if (!strncmp(table_name, srv_mysql50_table_name_prefix,
		     sizeof(srv_mysql50_table_name_prefix) - 1)) {
		/* This is a pre-5.1 table name
		containing chars other than [A-Za-z0-9].
		Discard the prefix and use raw UTF-8 encoding. */
		table_name += sizeof(srv_mysql50_table_name_prefix) - 1;
		table_name_len -= sizeof(srv_mysql50_table_name_prefix) - 1;

		to_cs = system_charset_info;
	}

	if (cs_table != to_cs) {
		table_name_len = strconvert(cs_table, table_name,
					    table_name_len, to_cs, tbl_name,
					    MAX_TABLE_NAME_LEN, &errors);
		if (errors > 0) {
			return true;
		}
		table_name = tbl_name;
	}

	if (!strncmp(database_name, srv_mysql50_table_name_prefix,
		     sizeof(srv_mysql50_table_name_prefix) - 1)) {
		database_name += sizeof(srv_mysql50_table_name_prefix) - 1;
		database_name_len -= sizeof(srv_mysql50_table_name_prefix) - 1;
		to_cs = system_charset_info;
	} else {
		to_cs = &my_charset_filename;
	}

	if (cs_db != to_cs) {
		database_name_len = strconvert(
			cs_table, database_name, database_name_len, to_cs,
			db_name, MAX_DATABASE_NAME_LEN, &errors);
		if (errors > 0) {
			return true;
		}
		database_name = db_name;
	}

	dict_name_len = database_name_len + table_name_len + 1;

	/* Copy database_name, '/', table_name, '\0' */
	if (alloc) {
		dict_name = static_cast<char*>(
			mem_heap_alloc(alloc, dict_name_len + 1));
		if (!dict_name) {
			return true;
		}
	}
	memcpy(dict_name, database_name, database_name_len);
	dict_name[database_name_len] = '/';
	memcpy(dict_name + database_name_len + 1, table_name,
	       table_name_len + 1);

	/* Values;  0 = Store and compare as given; case sensitive
		    1 = Store and compare in lower; case insensitive
		    2 = Store as given, compare in lower; case semi-sensitive */
	if (innobase_get_lower_case_table_names() == 1) {
		innobase_casedn_str(dict_name);
	}

	return false;
}

/*********************************************************************//**
Open a table from its database and table name, this is currently used by
foreign constraint parser to get the referenced table.
@return complete table name with database and table name, allocated from
heap memory passed in */
char*
dict_get_referenced_table(
	const char*    name,		  /*!< in: foreign key table name */
	const char*    database_name,	  /*!< in: table db name */
	ulint	       database_name_len, /*!< in: db name length */
	const char*    table_name,	  /*!< in: table name */
	ulint	       table_name_len,	  /*!< in: table name length */
	dict_table_t** table,		  /*!< out: table object or NULL */
	mem_heap_t*    heap,		  /*!< in/out: heap memory */
	CHARSET_INFO*  from_cs)		  /*!< in: table name charset */
{
	char*	      dict_name;
	ulint	      dict_name_len;
	CHARSET_INFO* db_cs = from_cs;
	ut_ad(database_name || name);
	ut_ad(table_name);

	if (!database_name) {
		/* Use the database name of the foreign key table */

		database_name = name;
		database_name_len = dict_get_db_name_len(name);
		db_cs		  = &my_charset_filename;
	}

	if (dict_table_t::build_name(database_name, database_name_len,
				     table_name, table_name_len, dict_name,
				     dict_name_len, heap, from_cs, db_cs)) {
		return NULL;
	}

	/* Values;  0 = Store and compare as given; case sensitive
	            1 = Store and compare in lower; case insensitive
	            2 = Store as given, compare in lower; case semi-sensitive */
	if (innobase_get_lower_case_table_names() == 2) {
		char buf[MAX_FULL_NAME_LEN];
		memcpy(buf, dict_name, dict_name_len + 1);
		innobase_casedn_str(buf);
		*table = dict_table_get_low(buf);

	} else {
		*table = dict_table_get_low(dict_name);
	}

	return (dict_name);
}

/*********************************************************************//**
Removes MySQL comments from an SQL string. A comment is either
(a) '#' to the end of the line,
(b) '--[space]' to the end of the line, or
(c) '[slash][asterisk]' till the next '[asterisk][slash]' (like the familiar
C comment syntax).
@return own: SQL string stripped from comments; the caller must free
this with ut_free()! */
static
char*
dict_strip_comments(
/*================*/
	const char*	sql_string,	/*!< in: SQL string */
	size_t		sql_length)	/*!< in: length of sql_string */
{
	char*		str;
	const char*	sptr;
	const char*	eptr	= sql_string + sql_length;
	char*		ptr;
	/* unclosed quote character (0 if none) */
	char		quote	= 0;
	bool		escape = false;

	DBUG_ENTER("dict_strip_comments");

	DBUG_PRINT("dict_strip_comments", ("%s", sql_string));

	str = static_cast<char*>(ut_malloc_nokey(sql_length + 1));

	sptr = sql_string;
	ptr = str;

	for (;;) {
scan_more:
		if (sptr >= eptr || *sptr == '\0') {
end_of_string:
			*ptr = '\0';

			ut_a(ptr <= str + sql_length);

			DBUG_PRINT("dict_strip_comments", ("%s", str));
			DBUG_RETURN(str);
		}

		if (*sptr == quote) {
			/* Closing quote character: do not look for
			starting quote or comments. */

			/* If the quote character is escaped by a
			backslash, ignore it. */
			if (escape) {
				escape = false;
			} else {
				quote = 0;
			}
		} else if (quote) {
			/* Within quotes: do not look for
			starting quotes or comments. */
			if (escape) {
				escape = false;
			} else if (*sptr == '\\') {
				escape = true;
			}
		} else if (*sptr == '"' || *sptr == '`' || *sptr == '\'') {
			/* Starting quote: remember the quote character. */
			quote = *sptr;
		} else if (*sptr == '#'
			   || (sptr[0] == '-' && sptr[1] == '-'
			       && sptr[2] == ' ')) {
			for (;;) {
				if (++sptr >= eptr) {
					goto end_of_string;
				}

				/* In Unix a newline is 0x0A while in Windows
				it is 0x0D followed by 0x0A */

				switch (*sptr) {
				case (char) 0X0A:
				case (char) 0x0D:
				case '\0':
					goto scan_more;
				}
			}
		} else if (!quote && *sptr == '/' && *(sptr + 1) == '*') {
			sptr += 2;
			for (;;) {
				if (sptr >= eptr) {
					goto end_of_string;
				}

				switch (*sptr) {
				case '\0':
					goto scan_more;
				case '*':
					if (sptr[1] == '/') {
						sptr += 2;
						goto scan_more;
					}
				}

				sptr++;
			}
		}

		*ptr = *sptr;

		ptr++;
		sptr++;
	}
}

/*********************************************************************//**
Finds the highest [number] for foreign key constraints of the table. Looks
only at the >= 4.0.18-format id's, which are of the form
databasename/tablename_ibfk_[number].
@return highest number, 0 if table has no new format foreign key constraints */
ulint
dict_table_get_highest_foreign_id(
/*==============================*/
	dict_table_t*	table)	/*!< in: table in the dictionary memory cache */
{
	dict_foreign_t*	foreign;
	char*		endp;
	ulint		biggest_id	= 0;
	ulint		id;
	ulint		len;

	DBUG_ENTER("dict_table_get_highest_foreign_id");

	ut_a(table);

	len = strlen(table->name.m_name);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {
		char    fkid[MAX_TABLE_NAME_LEN+20];
		foreign = *it;

		strcpy(fkid, foreign->id);
		/* Convert foreign key identifier on dictionary memory
		cache to filename charset. */
		innobase_convert_to_filename_charset(
				strchr(fkid, '/') + 1,
				strchr(foreign->id, '/') + 1,
				MAX_TABLE_NAME_LEN);

		if (strlen(fkid) > ((sizeof dict_ibfk) - 1) + len
		    && 0 == memcmp(fkid, table->name.m_name, len)
		    && 0 == memcmp(fkid + len,
				   dict_ibfk, (sizeof dict_ibfk) - 1)
		    && fkid[len + ((sizeof dict_ibfk) - 1)] != '0') {
			/* It is of the >= 4.0.18 format */

			id = strtoul(fkid + len
				     + ((sizeof dict_ibfk) - 1),
				     &endp, 10);
			if (*endp == '\0') {
				ut_a(id != biggest_id);

				if (id > biggest_id) {
					biggest_id = id;
				}
			}
		}
	}

	DBUG_PRINT("dict_table_get_highest_foreign_id",
		   ("id: " ULINTPF, biggest_id));

	DBUG_RETURN(biggest_id);
}

/**********************************************************************//**
Parses the CONSTRAINT id's to be dropped in an ALTER TABLE statement.
@return DB_SUCCESS or DB_CANNOT_DROP_CONSTRAINT if syntax error or the
constraint id does not match */
dberr_t
dict_foreign_parse_drop_constraints(
/*================================*/
	mem_heap_t*	heap,			/*!< in: heap from which we can
						allocate memory */
	trx_t*		trx,			/*!< in: transaction */
	dict_table_t*	table,			/*!< in: table */
	ulint*		n,			/*!< out: number of constraints
						to drop */
	const char***	constraints_to_drop)	/*!< out: id's of the
						constraints to drop */
{
	ibool			success;
	char*			str;
	size_t			len;
	const char*		ptr;
	const char*		ptr1;
	const char*		id;
	CHARSET_INFO*		cs;

	ut_a(trx->mysql_thd);

	cs = thd_charset(trx->mysql_thd);

	*n = 0;

	*constraints_to_drop = static_cast<const char**>(
		mem_heap_alloc(heap, 1000 * sizeof(char*)));

	ptr = innobase_get_stmt_unsafe(trx->mysql_thd, &len);

	str = dict_strip_comments(ptr, len);

	ptr = str;

	dict_sys.assert_locked();
loop:
	ptr = dict_scan_to(ptr, "DROP");

	if (*ptr == '\0') {
		ut_free(str);

		return(DB_SUCCESS);
	}

	ptr = dict_accept(cs, ptr, "DROP", &success);

	if (!my_isspace(cs, *ptr)) {

		goto loop;
	}

	ptr = dict_accept(cs, ptr, "FOREIGN", &success);

	if (!success || !my_isspace(cs, *ptr)) {

		goto loop;
	}

	ptr = dict_accept(cs, ptr, "KEY", &success);

	if (!success) {

		goto syntax_error;
	}

	ptr1 = dict_accept(cs, ptr, "IF", &success);

	if (success && my_isspace(cs, *ptr1)) {
		ptr1 = dict_accept(cs, ptr1, "EXISTS", &success);
		if (success) {
			ptr = ptr1;
		}
	}

	ptr = dict_scan_id(cs, ptr, heap, &id, FALSE, TRUE);

	if (id == NULL) {

		goto syntax_error;
	}

	ut_a(*n < 1000);
	(*constraints_to_drop)[*n] = id;
	(*n)++;

	if (std::find_if(table->foreign_set.begin(),
			 table->foreign_set.end(),
			 dict_foreign_matches_id(id))
	    == table->foreign_set.end()) {

		if (!srv_read_only_mode) {
			FILE*	ef = dict_foreign_err_file;

			mysql_mutex_lock(&dict_foreign_err_mutex);
			rewind(ef);
			ut_print_timestamp(ef);
			fputs(" Error in dropping of a foreign key"
			      " constraint of table ", ef);
			ut_print_name(ef, NULL, table->name.m_name);
			fprintf(ef, ",\nin SQL command\n%s"
				"\nCannot find a constraint with the"
				" given id %s.\n", str, id);
			mysql_mutex_unlock(&dict_foreign_err_mutex);
		}

		ut_free(str);

		return(DB_CANNOT_DROP_CONSTRAINT);
	}

	goto loop;

syntax_error:
	if (!srv_read_only_mode) {
		FILE*	ef = dict_foreign_err_file;

		mysql_mutex_lock(&dict_foreign_err_mutex);
		rewind(ef);
		ut_print_timestamp(ef);
		fputs(" Syntax error in dropping of a"
		      " foreign key constraint of table ", ef);
		ut_print_name(ef, NULL, table->name.m_name);
		fprintf(ef, ",\n"
			"close to:\n%s\n in SQL command\n%s\n", ptr, str);
		mysql_mutex_unlock(&dict_foreign_err_mutex);
	}

	ut_free(str);

	return(DB_CANNOT_DROP_CONSTRAINT);
}

/*==================== END OF FOREIGN KEY PROCESSING ====================*/

/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
Assumes that dict_sys.mutex is already being held.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache_low(
/*===========================*/
	index_id_t	index_id)	/*!< in: index id */
{
	dict_sys.assert_locked();

	return(dict_index_find_on_id_low(index_id));
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache(
/*=======================*/
	index_id_t	index_id)	/*!< in: index id */
{
	dict_index_t*	index;

	if (!dict_sys.is_initialised()) {
		return(NULL);
	}

	dict_sys.mutex_lock();

	index = dict_index_get_if_in_cache_low(index_id);

	dict_sys.mutex_unlock();

	return(index);
}

/**********************************************************************//**
Checks that a tuple has n_fields_cmp value in a sensible range, so that
no comparison can occur with the page number field in a node pointer.
@return TRUE if ok */
ibool
dict_index_check_search_tuple(
/*==========================*/
	const dict_index_t*	index,	/*!< in: index tree */
	const dtuple_t*		tuple)	/*!< in: tuple used in a search */
{
	ut_ad(dtuple_get_n_fields_cmp(tuple)
	      <= dict_index_get_n_unique_in_tree(index));
	return(TRUE);
}
#endif /* UNIV_DEBUG */

/**********************************************************************//**
Builds a node pointer out of a physical record and a page number.
@return own: node pointer */
dtuple_t*
dict_index_build_node_ptr(
/*======================*/
	const dict_index_t*	index,	/*!< in: index */
	const rec_t*		rec,	/*!< in: record for which to build node
					pointer */
	ulint			page_no,/*!< in: page number to put in node
					pointer */
	mem_heap_t*		heap,	/*!< in: memory heap where pointer
					created */
	ulint			level)	/*!< in: level of rec in tree:
					0 means leaf level */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;
	ulint		n_unique;

	if (dict_index_is_ibuf(index)) {
		/* In a universal index tree, we take the whole record as
		the node pointer if the record is on the leaf level,
		on non-leaf levels we remove the last field, which
		contains the page number of the child page */

		ut_a(!dict_table_is_comp(index->table));
		n_unique = rec_get_n_fields_old(rec);

		if (level > 0) {
			ut_a(n_unique > 1);
			n_unique--;
		}
	} else {
		n_unique = dict_index_get_n_unique_in_tree_nonleaf(index);
	}

	tuple = dtuple_create(heap, n_unique + 1);

	/* When searching in the tree for the node pointer, we must not do
	comparison on the last field, the page number field, as on upper
	levels in the tree there may be identical node pointers with a
	different page number; therefore, we set the n_fields_cmp to one
	less: */

	dtuple_set_n_fields_cmp(tuple, n_unique);

	dict_index_copy_types(tuple, index, n_unique);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	field = dtuple_get_nth_field(tuple, n_unique);
	dfield_set_data(field, buf, 4);

	dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);

	rec_copy_prefix_to_dtuple(tuple, rec, index, !level, n_unique, heap);
	dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple)
			     | REC_STATUS_NODE_PTR);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/** Convert a physical record into a search tuple.
@param[in]	rec		index record (not necessarily in an index page)
@param[in]	index		index
@param[in]	leaf		whether rec is in a leaf page
@param[in]	n_fields	number of data fields
@param[in,out]	heap		memory heap for allocation
@return own: data tuple */
dtuple_t*
dict_index_build_data_tuple(
	const rec_t*		rec,
	const dict_index_t*	index,
	bool			leaf,
	ulint			n_fields,
	mem_heap_t*		heap)
{
	dtuple_t* tuple = dtuple_create(heap, n_fields);

	dict_index_copy_types(tuple, index, n_fields);

	rec_copy_prefix_to_dtuple(tuple, rec, index, leaf, n_fields, heap);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/*********************************************************************//**
Calculates the minimum record length in an index. */
ulint
dict_index_calc_min_rec_len(
/*========================*/
	const dict_index_t*	index)	/*!< in: index */
{
	ulint	sum	= 0;
	ulint	i;
	ulint	comp	= dict_table_is_comp(index->table);

	if (comp) {
		ulint nullable = 0;
		sum = REC_N_NEW_EXTRA_BYTES;
		for (i = 0; i < dict_index_get_n_fields(index); i++) {
			const dict_col_t*	col
				= dict_index_get_nth_col(index, i);
			ulint	size = dict_col_get_fixed_size(col, comp);
			sum += size;
			if (!size) {
				size = col->len;
				sum += size < 128 ? 1 : 2;
			}
			if (!(col->prtype & DATA_NOT_NULL)) {
				nullable++;
			}
		}

		/* round the NULL flags up to full bytes */
		sum += UT_BITS_IN_BYTES(nullable);

		return(sum);
	}

	for (i = 0; i < dict_index_get_n_fields(index); i++) {
		sum += dict_col_get_fixed_size(
			dict_index_get_nth_col(index, i), comp);
	}

	if (sum > 127) {
		sum += 2 * dict_index_get_n_fields(index);
	} else {
		sum += dict_index_get_n_fields(index);
	}

	sum += REC_N_OLD_EXTRA_BYTES;

	return(sum);
}

/**********************************************************************//**
Outputs info on a foreign key of a table in a format suitable for
CREATE TABLE. */
std::string
dict_print_info_on_foreign_key_in_create_format(
/*============================================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	ibool		add_newline)	/*!< in: whether to add a newline */
{
	const char*	stripped_id;
	ulint	i;
	std::string	str;

	if (strchr(foreign->id, '/')) {
		/* Strip the preceding database name from the constraint id */
		stripped_id = foreign->id + 1
			+ dict_get_db_name_len(foreign->id);
	} else {
		stripped_id = foreign->id;
	}

	str.append(",");

	if (add_newline) {
		/* SHOW CREATE TABLE wants constraints each printed nicely
		on its own line, while error messages want no newlines
		inserted. */
		str.append("\n ");
	}

	str.append(" CONSTRAINT ");

	str.append(innobase_quote_identifier(trx, stripped_id));
	str.append(" FOREIGN KEY (");

	for (i = 0;;) {
		str.append(innobase_quote_identifier(trx, foreign->foreign_col_names[i]));

		if (++i < foreign->n_fields) {
			str.append(", ");
		} else {
			break;
		}
	}

	str.append(") REFERENCES ");

	if (dict_tables_have_same_db(foreign->foreign_table_name_lookup,
				     foreign->referenced_table_name_lookup)) {
		/* Do not print the database name of the referenced table */
		str.append(ut_get_name(trx,
			      dict_remove_db_name(
				      foreign->referenced_table_name)));
	} else {
		str.append(ut_get_name(trx,
				foreign->referenced_table_name));
	}

	str.append(" (");

	for (i = 0;;) {
		str.append(innobase_quote_identifier(trx,
				foreign->referenced_col_names[i]));

		if (++i < foreign->n_fields) {
			str.append(", ");
		} else {
			break;
		}
	}

	str.append(")");

	if (foreign->type & DICT_FOREIGN_ON_DELETE_CASCADE) {
		str.append(" ON DELETE CASCADE");
	}

	if (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL) {
		str.append(" ON DELETE SET NULL");
	}

	if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
		str.append(" ON DELETE NO ACTION");
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
		str.append(" ON UPDATE CASCADE");
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
		str.append(" ON UPDATE SET NULL");
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
		str.append(" ON UPDATE NO ACTION");
	}

	return str;
}

/**********************************************************************//**
Outputs info on foreign keys of a table. */
std::string
dict_print_info_on_foreign_keys(
/*============================*/
	ibool		create_table_format, /*!< in: if TRUE then print in
				a format suitable to be inserted into
				a CREATE TABLE, otherwise in the format
				of SHOW TABLE STATUS */
	trx_t*		trx,	/*!< in: transaction */
	dict_table_t*	table)	/*!< in: table */
{
	dict_foreign_t*	foreign;
	std::string 	str;

	dict_sys.mutex_lock();

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (create_table_format) {
			str.append(
				dict_print_info_on_foreign_key_in_create_format(
					trx, foreign, TRUE));
		} else {
			ulint	i;
			str.append("; (");

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					str.append(" ");
				}

				str.append(innobase_quote_identifier(trx,
						foreign->foreign_col_names[i]));
			}

			str.append(") REFER ");
			str.append(ut_get_name(trx,
					foreign->referenced_table_name));
			str.append(")");

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					str.append(" ");
				}
				str.append(innobase_quote_identifier(
						trx,
						foreign->referenced_col_names[i]));
			}

			str.append(")");

			if (foreign->type == DICT_FOREIGN_ON_DELETE_CASCADE) {
				str.append(" ON DELETE CASCADE");
			}

			if (foreign->type == DICT_FOREIGN_ON_DELETE_SET_NULL) {
				str.append(" ON DELETE SET NULL");
			}

			if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
				str.append(" ON DELETE NO ACTION");
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
				str.append(" ON UPDATE CASCADE");
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
				str.append(" ON UPDATE SET NULL");
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
				str.append(" ON UPDATE NO ACTION");
			}
		}
	}

	dict_sys.mutex_unlock();
	return str;
}

/** Given a space_id of a file-per-table tablespace, search the
dict_sys.table_LRU list and return the dict_table_t* pointer for it.
@param	space	tablespace
@return table if found, NULL if not */
static
dict_table_t*
dict_find_single_table_by_space(const fil_space_t* space)
{
	dict_table_t*	table;
	ulint		num_item;
	ulint		count = 0;

	ut_ad(space->id > 0);

	if (!dict_sys.is_initialised()) {
		/* This could happen when it's in redo processing. */
		return(NULL);
	}

	table = UT_LIST_GET_FIRST(dict_sys.table_LRU);
	num_item =  UT_LIST_GET_LEN(dict_sys.table_LRU);

	/* This function intentionally does not acquire mutex as it is used
	by error handling code in deep call stack as last means to avoid
	killing the server, so it worth to risk some consequences for
	the action. */
	while (table && count < num_item) {
		if (table->space == space) {
			if (dict_table_is_file_per_table(table)) {
				return(table);
			}
			return(NULL);
		}

		table = UT_LIST_GET_NEXT(table_LRU, table);
		count++;
	}

	return(NULL);
}

/**********************************************************************//**
Flags a table with specified space_id corrupted in the data dictionary
cache
@return true if successful */
bool dict_set_corrupted_by_space(const fil_space_t* space)
{
	dict_table_t*   table;

	table = dict_find_single_table_by_space(space);

	if (!table) {
		return false;
	}

	/* mark the table->corrupted bit only, since the caller
	could be too deep in the stack for SYS_INDEXES update */
	table->corrupted = true;
	table->file_unreadable = true;
	return true;
}

/** Flag a table encrypted in the data dictionary cache. */
void dict_set_encrypted_by_space(const fil_space_t* space)
{
	if (dict_table_t* table = dict_find_single_table_by_space(space)) {
		table->file_unreadable = true;
	}
}

/**********************************************************************//**
Flags an index corrupted both in the data dictionary cache
and in the SYS_INDEXES */
void
dict_set_corrupted(
/*===============*/
	dict_index_t*	index,	/*!< in/out: index */
	trx_t*		trx,	/*!< in/out: transaction */
	const char*	ctx)	/*!< in: context */
{
	mem_heap_t*	heap;
	mtr_t		mtr;
	dict_index_t*	sys_index;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	const char*	status;
	btr_cur_t	cursor;
	bool		locked	= RW_X_LATCH == trx->dict_operation_lock_mode;

	if (!locked) {
		row_mysql_lock_data_dictionary(trx);
	}

	dict_sys.assert_locked();
	ut_ad(!dict_table_is_comp(dict_sys.sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys.sys_indexes));

	/* Mark the table as corrupted only if the clustered index
	is corrupted */
	if (dict_index_is_clust(index)) {
		index->table->corrupted = TRUE;
	}

	if (index->type & DICT_CORRUPT) {
		/* The index was already flagged corrupted. */
		ut_ad(!dict_index_is_clust(index) || index->table->corrupted);
		goto func_exit;
	}

	/* If this is read only mode, do not update SYS_INDEXES, just
	mark it as corrupted in memory */
	if (srv_read_only_mode) {
		index->type |= DICT_CORRUPT;
		goto func_exit;
	}

	heap = mem_heap_create(sizeof(dtuple_t) + 2 * (sizeof(dfield_t)
			       + sizeof(que_fork_t) + sizeof(upd_node_t)
			       + sizeof(upd_t) + 12));
	mtr_start(&mtr);
	index->type |= DICT_CORRUPT;

	sys_index = UT_LIST_GET_FIRST(dict_sys.sys_indexes->indexes);

	/* Find the index row in SYS_INDEXES */
	tuple = dtuple_create(heap, 2);

	dfield = dtuple_get_nth_field(tuple, 0);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->table->id);
	dfield_set_data(dfield, buf, 8);

	dfield = dtuple_get_nth_field(tuple, 1);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);
	dfield_set_data(dfield, buf, 8);

	dict_index_copy_types(tuple, sys_index, 2);

	btr_cur_search_to_nth_level(sys_index, 0, tuple, PAGE_CUR_LE,
				    BTR_MODIFY_LEAF, &cursor, 0, &mtr);

	if (cursor.low_match == dtuple_get_n_fields(tuple)) {
		/* UPDATE SYS_INDEXES SET TYPE=index->type
		WHERE TABLE_ID=index->table->id AND INDEX_ID=index->id */
		ulint	len;
		byte*	field	= rec_get_nth_field_old(
			btr_cur_get_rec(&cursor),
			DICT_FLD__SYS_INDEXES__TYPE, &len);
		if (len != 4) {
			goto fail;
		}
		mtr.write<4>(*btr_cur_get_block(&cursor), field, index->type);
		status = "Flagged";
	} else {
fail:
		status = "Unable to flag";
	}

	mtr_commit(&mtr);
	mem_heap_empty(heap);
	ib::error() << status << " corruption of " << index->name
		<< " in table " << index->table->name << " in " << ctx;
	mem_heap_free(heap);

func_exit:
	if (!locked) {
		row_mysql_unlock_data_dictionary(trx);
	}
}

/** Flags an index corrupted in the data dictionary cache only. This
is used mostly to mark a corrupted index when index's own dictionary
is corrupted, and we force to load such index for repair purpose
@param[in,out]	index	index which is corrupted */
void
dict_set_corrupted_index_cache_only(
	dict_index_t*	index)
{
	ut_ad(index != NULL);
	ut_ad(index->table != NULL);
	dict_sys.assert_locked();
	ut_ad(!dict_table_is_comp(dict_sys.sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys.sys_indexes));

	/* Mark the table as corrupted only if the clustered index
	is corrupted */
	if (dict_index_is_clust(index)) {
		index->table->corrupted = TRUE;
		index->table->file_unreadable = true;
	}

	index->type |= DICT_CORRUPT;
}

/** Sets merge_threshold in the SYS_INDEXES
@param[in,out]	index		index
@param[in]	merge_threshold	value to set */
void
dict_index_set_merge_threshold(
	dict_index_t*	index,
	ulint		merge_threshold)
{
	mem_heap_t*	heap;
	mtr_t		mtr;
	dict_index_t*	sys_index;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	btr_cur_t	cursor;

	ut_ad(index != NULL);
	ut_ad(!dict_table_is_comp(dict_sys.sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys.sys_indexes));

	dict_sys_lock();

	heap = mem_heap_create(sizeof(dtuple_t) + 2 * (sizeof(dfield_t)
			       + sizeof(que_fork_t) + sizeof(upd_node_t)
			       + sizeof(upd_t) + 12));

	mtr_start(&mtr);

	sys_index = UT_LIST_GET_FIRST(dict_sys.sys_indexes->indexes);

	/* Find the index row in SYS_INDEXES */
	tuple = dtuple_create(heap, 2);

	dfield = dtuple_get_nth_field(tuple, 0);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->table->id);
	dfield_set_data(dfield, buf, 8);

	dfield = dtuple_get_nth_field(tuple, 1);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);
	dfield_set_data(dfield, buf, 8);

	dict_index_copy_types(tuple, sys_index, 2);

	btr_cur_search_to_nth_level(sys_index, 0, tuple, PAGE_CUR_GE,
				    BTR_MODIFY_LEAF, &cursor, 0, &mtr);

	if (cursor.up_match == dtuple_get_n_fields(tuple)
	    && rec_get_n_fields_old(btr_cur_get_rec(&cursor))
	       == DICT_NUM_FIELDS__SYS_INDEXES) {
		ulint	len;
		byte*	field	= rec_get_nth_field_old(
			btr_cur_get_rec(&cursor),
			DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD, &len);

		ut_ad(len == 4);
		mtr.write<4,mtr_t::MAYBE_NOP>(*btr_cur_get_block(&cursor),
					      field, merge_threshold);
	}

	mtr_commit(&mtr);
	mem_heap_free(heap);

	dict_sys_unlock();
}

#ifdef UNIV_DEBUG
/** Sets merge_threshold for all indexes in the list of tables
@param[in]	list	pointer to the list of tables */
inline
void
dict_set_merge_threshold_list_debug(
	UT_LIST_BASE_NODE_T(dict_table_t)*	list,
	uint					merge_threshold_all)
{
	for (dict_table_t* table = UT_LIST_GET_FIRST(*list);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {
		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {
			index->lock.x_lock(SRW_LOCK_CALL);
			index->merge_threshold = merge_threshold_all
				& ((1U << 6) - 1);
			index->lock.x_unlock();
		}
	}
}

/** Sets merge_threshold for all indexes in dictionary cache for debug.
@param[in]	merge_threshold_all	value to set for all indexes */
void
dict_set_merge_threshold_all_debug(
	uint	merge_threshold_all)
{
	dict_sys.mutex_lock();

	dict_set_merge_threshold_list_debug(
		&dict_sys.table_LRU, merge_threshold_all);
	dict_set_merge_threshold_list_debug(
		&dict_sys.table_non_LRU, merge_threshold_all);

	dict_sys.mutex_unlock();
}

#endif /* UNIV_DEBUG */

/** Get an index by name.
@param[in]	table		the table where to look for the index
@param[in]	name		the index name to look for
@return index, NULL if does not exist */
dict_index_t*
dict_table_get_index_on_name(dict_table_t* table, const char* name)
{
	dict_index_t*	index;

	index = dict_table_get_first_index(table);

	while (index != NULL) {
		if (index->is_committed() && !strcmp(index->name, name)) {
			return(index);
		}

		index = dict_table_get_next_index(index);
	}

	return(NULL);
}

/**********************************************************************//**
Replace the index passed in with another equivalent index in the
foreign key lists of the table.
@return whether all replacements were found */
bool
dict_foreign_replace_index(
/*=======================*/
	dict_table_t*		table,  /*!< in/out: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const dict_index_t*	index)	/*!< in: index to be replaced */
{
	bool		found	= true;
	dict_foreign_t*	foreign;

	ut_ad(index->to_be_dropped);
	ut_ad(index->table == table);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;
		if (foreign->foreign_index == index) {
			ut_ad(foreign->foreign_table == index->table);

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->foreign_table, col_names,
				foreign->foreign_col_names,
				foreign->n_fields, index,
				/*check_charsets=*/TRUE, /*check_null=*/FALSE,
				NULL, NULL, NULL);
			if (new_index) {
				ut_ad(new_index->table == index->table);
				ut_ad(!new_index->to_be_dropped);
			} else {
				found = false;
			}

			foreign->foreign_index = new_index;
		}
	}

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;
		if (foreign->referenced_index == index) {
			ut_ad(foreign->referenced_table == index->table);

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->referenced_table, NULL,
				foreign->referenced_col_names,
				foreign->n_fields, index,
				/*check_charsets=*/TRUE, /*check_null=*/FALSE,
				NULL, NULL, NULL);
			/* There must exist an alternative index,
			since this must have been checked earlier. */
			if (new_index) {
				ut_ad(new_index->table == index->table);
				ut_ad(!new_index->to_be_dropped);
			} else {
				found = false;
			}

			foreign->referenced_index = new_index;
		}
	}

	return(found);
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Check for duplicate index entries in a table [using the index name] */
void
dict_table_check_for_dup_indexes(
/*=============================*/
	const dict_table_t*	table,	/*!< in: Check for dup indexes
					in this table */
	enum check_name		check)	/*!< in: whether and when to allow
					temporary index names */
{
	/* Check for duplicates, ignoring indexes that are marked
	as to be dropped */

	const dict_index_t*	index1;
	const dict_index_t*	index2;

	dict_sys.assert_locked();

	/* The primary index _must_ exist */
	ut_a(UT_LIST_GET_LEN(table->indexes) > 0);

	index1 = UT_LIST_GET_FIRST(table->indexes);

	do {
		if (!index1->is_committed()) {
			ut_a(!dict_index_is_clust(index1));

			switch (check) {
			case CHECK_ALL_COMPLETE:
				ut_error;
			case CHECK_ABORTED_OK:
				switch (dict_index_get_online_status(index1)) {
				case ONLINE_INDEX_COMPLETE:
				case ONLINE_INDEX_CREATION:
					ut_error;
					break;
				case ONLINE_INDEX_ABORTED:
				case ONLINE_INDEX_ABORTED_DROPPED:
					break;
				}
				/* fall through */
			case CHECK_PARTIAL_OK:
				break;
			}
		}

		for (index2 = UT_LIST_GET_NEXT(indexes, index1);
		     index2 != NULL;
		     index2 = UT_LIST_GET_NEXT(indexes, index2)) {
			ut_ad(index1->is_committed()
			      != index2->is_committed()
			      || strcmp(index1->name, index2->name) != 0);
		}

		index1 = UT_LIST_GET_NEXT(indexes, index1);
	} while (index1);
}
#endif /* UNIV_DEBUG */

/** Auxiliary macro used inside dict_table_schema_check(). */
#define CREATE_TYPES_NAMES() \
	dtype_sql_name((unsigned) req_schema->columns[i].mtype, \
		       (unsigned) req_schema->columns[i].prtype_mask, \
		       (unsigned) req_schema->columns[i].len, \
		       req_type, sizeof(req_type)); \
	dtype_sql_name(table->cols[j].mtype, \
		       table->cols[j].prtype, \
		       table->cols[j].len, \
		       actual_type, sizeof(actual_type))

/*********************************************************************//**
Checks whether a table exists and whether it has the given structure.
The table must have the same number of columns with the same names and
types. The order of the columns does not matter.
The caller must own the dictionary mutex.
dict_table_schema_check() @{
@return DB_SUCCESS if the table exists and contains the necessary columns */
dberr_t
dict_table_schema_check(
/*====================*/
	dict_table_schema_t*	req_schema,	/*!< in/out: required table
						schema */
	char*			errstr,		/*!< out: human readable error
						message if != DB_SUCCESS is
						returned */
	size_t			errstr_sz)	/*!< in: errstr size */
{
	char		buf[MAX_FULL_NAME_LEN];
	char		req_type[64];
	char		actual_type[64];
	dict_table_t*	table;
	ulint		i;

	dict_sys.assert_locked();

	table = dict_table_get_low(req_schema->table_name);

	if (table == NULL) {
		bool should_print=true;
		/* no such table */

		if (innobase_strcasecmp(req_schema->table_name, "mysql/innodb_table_stats") == 0) {
			if (innodb_table_stats_not_found_reported == false) {
				innodb_table_stats_not_found = true;
				innodb_table_stats_not_found_reported = true;
			} else {
				should_print = false;
			}
		} else if (innobase_strcasecmp(req_schema->table_name, "mysql/innodb_index_stats") == 0 ) {
			if (innodb_index_stats_not_found_reported == false) {
				innodb_index_stats_not_found = true;
				innodb_index_stats_not_found_reported = true;
			} else {
				should_print = false;
			}
		}

		if (should_print) {
			snprintf(errstr, errstr_sz,
				"Table %s not found.",
				ut_format_name(req_schema->table_name,
					   buf, sizeof(buf)));
			return(DB_TABLE_NOT_FOUND);
		} else {
			return(DB_STATS_DO_NOT_EXIST);
		}
	}

	if (!table->is_readable() && !table->space) {
		/* missing tablespace */

		snprintf(errstr, errstr_sz,
			    "Tablespace for table %s is missing.",
			    ut_format_name(req_schema->table_name,
					   buf, sizeof(buf)));

		return(DB_TABLE_NOT_FOUND);
	}

	if (ulint(table->n_def - DATA_N_SYS_COLS) != req_schema->n_cols) {
		/* the table has a different number of columns than required */
		snprintf(errstr, errstr_sz,
			 "%s has %d columns but should have " ULINTPF ".",
			 ut_format_name(req_schema->table_name, buf,
					sizeof buf),
			 table->n_def - DATA_N_SYS_COLS,
			 req_schema->n_cols);

		return(DB_ERROR);
	}

	/* For each column from req_schema->columns[] search
	whether it is present in table->cols[].
	The following algorithm is O(n_cols^2), but is optimized to
	be O(n_cols) if the columns are in the same order in both arrays. */

	for (i = 0; i < req_schema->n_cols; i++) {
		ulint	j = dict_table_has_column(
			table, req_schema->columns[i].name, i);

		if (j == table->n_def) {

			snprintf(errstr, errstr_sz,
				    "required column %s"
				    " not found in table %s.",
				    req_schema->columns[i].name,
				    ut_format_name(
					    req_schema->table_name,
					    buf, sizeof(buf)));

			return(DB_ERROR);
		}

		/* we found a column with the same name on j'th position,
		compare column types and flags */

		/* check length for exact match */
		if (req_schema->columns[i].len == table->cols[j].len) {
		} else if (!strcmp(req_schema->table_name, TABLE_STATS_NAME)
			   || !strcmp(req_schema->table_name,
				      INDEX_STATS_NAME)) {
			ut_ad(table->cols[j].len < req_schema->columns[i].len);
			ib::warn() << "Table " << req_schema->table_name
				   << " has length mismatch in the"
				   << " column name "
				   << req_schema->columns[i].name
				   << ".  Please run mysql_upgrade";
		} else {
			CREATE_TYPES_NAMES();

			snprintf(errstr, errstr_sz,
				    "Column %s in table %s is %s"
				    " but should be %s (length mismatch).",
				    req_schema->columns[i].name,
				    ut_format_name(req_schema->table_name,
						   buf, sizeof(buf)),
				    actual_type, req_type);

			return(DB_ERROR);
		}

		/*
                  check mtype for exact match.
                  This check is relaxed to allow use to use TIMESTAMP
                  (ie INT) for last_update instead of DATA_BINARY.
                  We have to test for both values as the innodb_table_stats
                  table may come from MySQL and have the old type.
                */
		if (req_schema->columns[i].mtype != table->cols[j].mtype &&
                    !(req_schema->columns[i].mtype == DATA_INT &&
                      table->cols[j].mtype == DATA_FIXBINARY))
                {
			CREATE_TYPES_NAMES();

			snprintf(errstr, errstr_sz,
				    "Column %s in table %s is %s"
				    " but should be %s (type mismatch).",
				    req_schema->columns[i].name,
				    ut_format_name(req_schema->table_name,
						   buf, sizeof(buf)),
				    actual_type, req_type);

			return(DB_ERROR);
		}

		/* check whether required prtype mask is set */
		if (req_schema->columns[i].prtype_mask != 0
		    && (table->cols[j].prtype
			& req_schema->columns[i].prtype_mask)
		       != req_schema->columns[i].prtype_mask) {

			CREATE_TYPES_NAMES();

			snprintf(errstr, errstr_sz,
				    "Column %s in table %s is %s"
				    " but should be %s (flags mismatch).",
				    req_schema->columns[i].name,
				    ut_format_name(req_schema->table_name,
						   buf, sizeof(buf)),
				    actual_type, req_type);

			return(DB_ERROR);
		}
	}

	if (req_schema->n_foreign != table->foreign_set.size()) {
		snprintf(
			errstr, errstr_sz,
			"Table %s has " ULINTPF " foreign key(s) pointing"
			" to other tables, but it must have " ULINTPF ".",
			ut_format_name(req_schema->table_name,
				       buf, sizeof(buf)),
			static_cast<ulint>(table->foreign_set.size()),
			req_schema->n_foreign);
		return(DB_ERROR);
	}

	if (req_schema->n_referenced != table->referenced_set.size()) {
		snprintf(
			errstr, errstr_sz,
			"There are " ULINTPF " foreign key(s) pointing to %s, "
			"but there must be " ULINTPF ".",
			static_cast<ulint>(table->referenced_set.size()),
			ut_format_name(req_schema->table_name,
				       buf, sizeof(buf)),
			req_schema->n_referenced);
		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}
/* @} */

/*********************************************************************//**
Converts a database and table name from filesystem encoding
(e.g. d@i1b/a@q1b@1Kc, same format as used in dict_table_t::name) in two
strings in UTF8 encoding (e.g. dцb and aюbØc). The output buffers must be
at least MAX_DB_UTF8_LEN and MAX_TABLE_UTF8_LEN bytes. */
void
dict_fs2utf8(
/*=========*/
	const char*	db_and_table,	/*!< in: database and table names,
					e.g. d@i1b/a@q1b@1Kc */
	char*		db_utf8,	/*!< out: database name, e.g. dцb */
	size_t		db_utf8_size,	/*!< in: dbname_utf8 size */
	char*		table_utf8,	/*!< out: table name, e.g. aюbØc */
	size_t		table_utf8_size)/*!< in: table_utf8 size */
{
	char	db[MAX_DATABASE_NAME_LEN + 1];
	ulint	db_len;
	uint	errors;

	db_len = dict_get_db_name_len(db_and_table);

	ut_a(db_len <= sizeof(db));

	memcpy(db, db_and_table, db_len);
	db[db_len] = '\0';

	strconvert(
		&my_charset_filename, db, uint(db_len), system_charset_info,
		db_utf8, uint(db_utf8_size), &errors);

	/* convert each # to @0023 in table name and store the result in buf */
	const char*	table = dict_remove_db_name(db_and_table);
	const char*	table_p;
	char		buf[MAX_TABLE_NAME_LEN * 5 + 1];
	char*		buf_p;
	for (table_p = table, buf_p = buf; table_p[0] != '\0'; table_p++) {
		if (table_p[0] != '#') {
			buf_p[0] = table_p[0];
			buf_p++;
		} else {
			buf_p[0] = '@';
			buf_p[1] = '0';
			buf_p[2] = '0';
			buf_p[3] = '2';
			buf_p[4] = '3';
			buf_p += 5;
		}
		ut_a((size_t) (buf_p - buf) < sizeof(buf));
	}
	buf_p[0] = '\0';

	errors = 0;
	strconvert(
		&my_charset_filename, buf, (uint) (buf_p - buf),
		system_charset_info,
		table_utf8, uint(table_utf8_size),
		&errors);

	if (errors != 0) {
		snprintf(table_utf8, table_utf8_size, "%s%s",
			    srv_mysql50_table_name_prefix, table);
	}
}

/** Resize the hash tables based on the current buffer pool size. */
void dict_sys_t::resize()
{
  ut_ad(this == &dict_sys);
  ut_ad(is_initialised());
  mutex_lock();

  /* all table entries are in table_LRU and table_non_LRU lists */
  table_hash.free();
  table_id_hash.free();
  temp_id_hash.free();

  const ulint hash_size = buf_pool_get_curr_size()
    / (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE);
  table_hash.create(hash_size);
  table_id_hash.create(hash_size);
  temp_id_hash.create(hash_size);

  for (dict_table_t *table= UT_LIST_GET_FIRST(table_LRU); table;
       table= UT_LIST_GET_NEXT(table_LRU, table))
  {
    ut_ad(!table->is_temporary());
    ulint fold= ut_fold_string(table->name.m_name);
    ulint id_fold= ut_fold_ull(table->id);

    HASH_INSERT(dict_table_t, name_hash, &table_hash, fold, table);
    HASH_INSERT(dict_table_t, id_hash, &table_id_hash, id_fold, table);
  }

  for (dict_table_t *table = UT_LIST_GET_FIRST(table_non_LRU); table;
       table= UT_LIST_GET_NEXT(table_LRU, table))
  {
    ulint fold= ut_fold_string(table->name.m_name);
    ulint id_fold= ut_fold_ull(table->id);

    HASH_INSERT(dict_table_t, name_hash, &table_hash, fold, table);

    hash_table_t *id_hash= table->is_temporary()
      ? &temp_id_hash : &table_id_hash;

    HASH_INSERT(dict_table_t, id_hash, id_hash, id_fold, table);
  }

  mutex_unlock();
}

/** Close the data dictionary cache on shutdown. */
void dict_sys_t::close()
{
  ut_ad(this == &dict_sys);
  if (!is_initialised()) return;

  mutex_lock();

  /* Free the hash elements. We don't remove them from the table
  because we are going to destroy the table anyway. */
  for (ulint i= table_hash.n_cells; i--; )
    while (dict_table_t *table= static_cast<dict_table_t*>
           (HASH_GET_FIRST(&table_hash, i)))
      dict_sys.remove(table);

  table_hash.free();

  /* table_id_hash contains the same elements as in table_hash,
  therefore we don't delete the individual elements. */
  table_id_hash.free();

  /* No temporary tables should exist at this point. */
  temp_id_hash.free();

  mutex_unlock();
  mysql_mutex_destroy(&mutex);
  latch.destroy();

  mysql_mutex_destroy(&dict_foreign_err_mutex);

  if (dict_foreign_err_file)
  {
    my_fclose(dict_foreign_err_file, MYF(MY_WME));
    dict_foreign_err_file = NULL;
  }

  m_initialised= false;
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate the dictionary table LRU list.
@return TRUE if valid */
static
ibool
dict_lru_validate(void)
/*===================*/
{
	dict_table_t*	table;

	dict_sys.assert_locked();

	for (table = UT_LIST_GET_FIRST(dict_sys.table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(table->can_be_evicted);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys.table_non_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(!table->can_be_evicted);
	}

	return(TRUE);
}
#endif /* UNIV_DEBUG */
/*********************************************************************//**
Check an index to see whether its first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return true if the index qualifies, otherwise false */
bool
dict_foreign_qualify_index(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	index,	/*!< in: index to check */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
{
	if (dict_index_get_n_fields(index) < n_cols) {
		return(false);
	}

	if (index->type & (DICT_SPATIAL | DICT_FTS | DICT_CORRUPT)) {
		return false;
	}

	if (index->online_status >= ONLINE_INDEX_ABORTED) {
		return false;
	}

	for (ulint i = 0; i < n_cols; i++) {
		dict_field_t*	field;
		const char*	col_name;
		ulint		col_no;

		field = dict_index_get_nth_field(index, i);
		col_no = dict_col_get_no(field->col);

		if (field->prefix_len != 0) {
			/* We do not accept column prefix
			indexes here */
			if (error && err_col_no && err_index) {
				*error = FK_IS_PREFIX_INDEX;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}
			return(false);
		}

		if (check_null
		    && (field->col->prtype & DATA_NOT_NULL)) {
			if (error && err_col_no && err_index) {
				*error = FK_COL_NOT_NULL;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}
			return(false);
		}

		if (field->col->is_virtual()) {
			col_name = "";
			for (ulint j = 0; j < table->n_v_def; j++) {
				col_name = dict_table_get_v_col_name(table, j);
				if (innobase_strcasecmp(field->name,col_name) == 0) {
					break;
				}
			}
		} else {
			col_name = col_names
				? col_names[col_no]
				: dict_table_get_col_name(table, col_no);
		}

		if (0 != innobase_strcasecmp(columns[i], col_name)) {
			return(false);
		}

		if (types_idx && !cmp_cols_are_equal(
			    dict_index_get_nth_col(index, i),
			    dict_index_get_nth_col(types_idx, i),
			    check_charsets)) {
			if (error && err_col_no && err_index) {
				*error = FK_COLS_NOT_EQUAL;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}

			return(false);
		}
	}

	return(true);
}

/*********************************************************************//**
Update the state of compression failure padding heuristics. This is
called whenever a compression operation succeeds or fails.
The caller must be holding info->mutex */
static
void
dict_index_zip_pad_update(
/*======================*/
	zip_pad_info_t*	info,	/*<! in/out: info to be updated */
	ulint	zip_threshold)	/*<! in: zip threshold value */
{
	ulint	total;
	ulint	fail_pct;

	ut_ad(info);
	ut_ad(info->pad % ZIP_PAD_INCR == 0);

	total = info->success + info->failure;

	ut_ad(total > 0);

	if (zip_threshold == 0) {
		/* User has just disabled the padding. */
		return;
	}

	if (total < ZIP_PAD_ROUND_LEN) {
		/* We are in middle of a round. Do nothing. */
		return;
	}

	/* We are at a 'round' boundary. Reset the values but first
	calculate fail rate for our heuristic. */
	fail_pct = (info->failure * 100) / total;
	info->failure = 0;
	info->success = 0;

	if (fail_pct > zip_threshold) {
		/* Compression failures are more then user defined
		threshold. Increase the pad size to reduce chances of
		compression failures.

		Only do increment if it won't increase padding
		beyond max pad size. */
		if (info->pad + ZIP_PAD_INCR
		    < (srv_page_size * zip_pad_max) / 100) {
			info->pad.fetch_add(ZIP_PAD_INCR);

			MONITOR_INC(MONITOR_PAD_INCREMENTS);
		}

		info->n_rounds = 0;

	} else {
		/* Failure rate was OK. Another successful round
		completed. */
		++info->n_rounds;

		/* If enough successful rounds are completed with
		compression failure rate in control, decrease the
		padding. */
		if (info->n_rounds >= ZIP_PAD_SUCCESSFUL_ROUND_LIMIT
		    && info->pad > 0) {
			info->pad.fetch_sub(ZIP_PAD_INCR);

			info->n_rounds = 0;

			MONITOR_INC(MONITOR_PAD_DECREMENTS);
		}
	}
}

/*********************************************************************//**
This function should be called whenever a page is successfully
compressed. Updates the compression padding information. */
void
dict_index_zip_success(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
{
	ulint zip_threshold = zip_failure_threshold_pct;
	if (!zip_threshold) {
		/* Disabled by user. */
		return;
	}

	index->zip_pad.mutex.lock();
	++index->zip_pad.success;
	dict_index_zip_pad_update(&index->zip_pad, zip_threshold);
	index->zip_pad.mutex.unlock();
}

/*********************************************************************//**
This function should be called whenever a page compression attempt
fails. Updates the compression padding information. */
void
dict_index_zip_failure(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
{
	ulint zip_threshold = zip_failure_threshold_pct;
	if (!zip_threshold) {
		/* Disabled by user. */
		return;
	}

	index->zip_pad.mutex.lock();
	++index->zip_pad.failure;
	dict_index_zip_pad_update(&index->zip_pad, zip_threshold);
	index->zip_pad.mutex.unlock();
}

/*********************************************************************//**
Return the optimal page size, for which page will likely compress.
@return page size beyond which page might not compress */
ulint
dict_index_zip_pad_optimal_page_size(
/*=================================*/
	dict_index_t*	index)	/*!< in: index for which page size
				is requested */
{
	ulint	pad;
	ulint	min_sz;
	ulint	sz;

	if (!zip_failure_threshold_pct) {
		/* Disabled by user. */
		return(srv_page_size);
	}

	pad = index->zip_pad.pad;

	ut_ad(pad < srv_page_size);
	sz = srv_page_size - pad;

	/* Min size allowed by user. */
	ut_ad(zip_pad_max < 100);
	min_sz = (srv_page_size * (100 - zip_pad_max)) / 100;

	return(ut_max(sz, min_sz));
}

/*************************************************************//**
Convert table flag to row format string.
@return row format name. */
const char*
dict_tf_to_row_format_string(
/*=========================*/
	ulint	table_flag)		/*!< in: row format setting */
{
	switch (dict_tf_get_rec_format(table_flag)) {
	case REC_FORMAT_REDUNDANT:
		return("ROW_TYPE_REDUNDANT");
	case REC_FORMAT_COMPACT:
		return("ROW_TYPE_COMPACT");
	case REC_FORMAT_COMPRESSED:
		return("ROW_TYPE_COMPRESSED");
	case REC_FORMAT_DYNAMIC:
		return("ROW_TYPE_DYNAMIC");
	}

	ut_error;
	return(0);
}
