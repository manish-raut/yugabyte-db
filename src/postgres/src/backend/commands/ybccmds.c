/*--------------------------------------------------------------------------------------------------
 *
 * ybccmds.c
 *        YB commands for creating and altering table structures and settings
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *        src/backend/commands/ybccmds.c
 *
 *------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "catalog/pg_attribute.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "catalog/pg_database.h"
#include "commands/ybccmds.h"
#include "catalog/ybctype.h"

#include "catalog/catalog.h"
#include "catalog/index.h"
#include "access/htup_details.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "executor/tuptable.h"
#include "executor/ybcExpr.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"

/* Utility function to calculate column sorting options */
static void
ColumnSortingOptions(SortByDir dir, SortByNulls nulls, bool* is_desc, bool* is_nulls_first)
{
  if (dir == SORTBY_DESC) {
	/*
	 * From postgres doc NULLS FIRST is the default for DESC order.
	 * So SORTBY_NULLS_DEFAULT is equal to SORTBY_NULLS_FIRST here.
	 */
	*is_desc = true;
	*is_nulls_first = (nulls != SORTBY_NULLS_LAST);
  } else {
	/*
	 * From postgres doc ASC is the default sort order and NULLS LAST is the default for it.
	 * So SORTBY_DEFAULT is equal to SORTBY_ASC and SORTBY_NULLS_DEFAULT is equal
	 * to SORTBY_NULLS_LAST here.
	 */
	*is_desc = false;
	*is_nulls_first = (nulls == SORTBY_NULLS_FIRST);
  }
}

/* -------------------------------------------------------------------------- */
/*  Database Functions. */

void
YBCCreateDatabase(Oid dboid, const char *dbname, Oid src_dboid, Oid next_oid)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewCreateDatabase(ybc_pg_session,
										  dbname,
										  dboid,
										  src_dboid,
										  next_oid,
										  &handle));
	HandleYBStmtStatus(YBCPgExecCreateDatabase(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCDropDatabase(Oid dboid, const char *dbname)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropDatabase(ybc_pg_session,
										dbname,
																			dboid,
										&handle));
	HandleYBStmtStatus(YBCPgExecDropDatabase(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCReserveOids(Oid dboid, Oid next_oid, uint32 count, Oid *begin_oid, Oid *end_oid)
{
	HandleYBStatus(YBCPgReserveOids(ybc_pg_session,
									dboid,
									next_oid,
									count,
									begin_oid,
									end_oid));
}

/* ---------------------------------------------------------------------------------------------- */
/*  Table Functions. */

static void CreateTableAddColumn(YBCPgStatement handle,
								  Form_pg_attribute att,
								  bool is_hash,
								  bool is_primary,
								  bool is_desc,
								  bool is_nulls_first)
{
  const AttrNumber attnum = att->attnum;
  const YBCPgTypeEntity *col_type = YBCDataTypeFromOidMod(attnum, att->atttypid);
  HandleYBStmtStatus(YBCPgCreateTableAddColumn(handle,
      NameStr(att->attname),
      attnum,
      col_type,
      is_hash,
      is_primary,
      is_desc,
      is_nulls_first), handle);
}

/* Utility function to add columns to the YB create statement
 * Columns need to be sent in order first hash columns, then rest of primary key columns,
 * then regular columns.
 */
static void CreateTableAddColumns(YBCPgStatement handle,
								  TupleDesc desc,
								  Constraint *primary_key)
{
  /* Add all key columns first with respect to compound key order */
  ListCell *cell;
  if (primary_key != NULL)
  {
    foreach(cell, primary_key->yb_index_params)
    {
      IndexElem *index_elem = (IndexElem *)lfirst(cell);
      bool column_found = false;
      for (int i = 0; i < desc->natts; ++i)
      {
        Form_pg_attribute att = TupleDescAttr(desc, i);
        if (strcmp(NameStr(att->attname), index_elem->name) == 0)
        {
          if (!YBCDataTypeIsValidForKey(att->atttypid))
          {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("PRIMARY KEY containing column of type '%s' not yet supported",
                               YBPgTypeOidToStr(att->atttypid))));
          }
          SortByDir order = index_elem->ordering;
          /* In YB mode first column defaults to HASH if not set */
          const bool is_first_key = (cell == list_head(primary_key->yb_index_params));
          bool is_hash = (order == SORTBY_HASH) || (is_first_key && order == SORTBY_DEFAULT);
          bool is_desc = false;
          bool is_nulls_first = false;
          ColumnSortingOptions(order, index_elem->nulls_ordering, &is_desc, &is_nulls_first);
          CreateTableAddColumn(handle, att, is_hash, true /* is_primary */, is_desc, is_nulls_first);
          column_found = true;
          break;
        }
      }
      if (!column_found)
      {
        ereport(FATAL,
                (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Column '%s' not found in table", index_elem->name)));
      }
    }
  }

  /* Add all non-key columns */
  for (int i = 0; i < desc->natts; ++i)
  {
    Form_pg_attribute att = TupleDescAttr(desc, i);
    bool is_key = false;
    if (primary_key)
    {
      foreach(cell, primary_key->yb_index_params)
      {
        IndexElem *index_elem = (IndexElem *) lfirst(cell);
        if (strcmp(NameStr(att->attname), index_elem->name) == 0)
        {
          is_key = true;
          break;
        }
      }
    }
    if (!is_key)
    {
      CreateTableAddColumn(handle,
          att,
          false /* is_hash */,
          false /* is_primary */,
          false /* is_desc */,
          false /* is_nulls_first */);
    }
  }
}

/* Utility function to handle split points */
static void CreateTableHandleSplitOptions(YBCPgStatement handle,
										  TupleDesc desc,
										  OptSplit *split_options,
										  Constraint *primary_key)
{
	/* Address both types of split options */
	switch (split_options->split_type)
	{
		case NUM_TABLETS: ;
			/* Make sure we have HASH columns */
			ListCell *head = list_head(primary_key->yb_index_params);
			IndexElem *index_elem = (IndexElem*) lfirst(head);
			if (!index_elem ||
				!(index_elem->ordering == SORTBY_HASH ||
				  index_elem->ordering == SORTBY_DEFAULT))
			{
				ereport(ERROR, (errmsg("HASH columns must be present to "
									   "split by number of tablets")));
			}

			/* Tell pggate about it */
			YBCPgCreateTableSetNumTablets(handle, split_options->num_tablets);
			break;
		case SPLIT_POINTS: ;
			/* Number of columns used in the primary key */
			int num_key_cols = list_length(primary_key->keys);

			/* Get the type information on each column of the primary key,
			 * and verify none are HASH columns */
			Oid *col_attrtypes = palloc(sizeof(Oid) * num_key_cols);
			int32 *col_attrtypmods = palloc(sizeof(int32) * num_key_cols);
			ScanKeyData *col_comparators = palloc(sizeof(ScanKeyData) * num_key_cols);

			bool *skips = palloc0(sizeof(bool) * desc->natts);
			int col_num = 0;
			ListCell *cell;
			foreach(cell, primary_key->yb_index_params)
			{
				/* Column constraint for the primary key */
				IndexElem *index_elem = (IndexElem *) lfirst(cell);

				/* Locate the table column that matches */
				for (int i = 0; i < desc->natts; i++)
				{
					if (skips[i]) continue;

					Form_pg_attribute att = TupleDescAttr(desc, i);
					char *attname = NameStr(att->attname);

					/* Found it */
					if (strcmp(attname, index_elem->name) == 0)
					{
						/* Prohibit the use of HASH columns */
						if (index_elem->ordering == SORTBY_HASH ||
							(col_num == 0 && index_elem->ordering == SORTBY_DEFAULT))
						{
							ereport(ERROR, (errmsg("HASH columns cannot be used for "
												   "split points")));
						}

						/* Record information on the attribute */
						col_attrtypes[col_num] = att->atttypid;
						col_attrtypmods[col_num] = att->atttypmod;

						/* Get the comparator */
						Oid opclass = GetDefaultOpClass(att->atttypid, BTREE_AM_OID);
						Oid opfamily = get_opclass_family(opclass);
						Oid type = att->atttypid;
						RegProcedure cmp_proc = get_opfamily_proc(opfamily,
																  type,
																  type,
																  BTORDER_PROC);
						ScanKeyInit(&col_comparators[col_num], 0, BTEqualStrategyNumber,
									cmp_proc, 0);

						/* Know to skip this in any future searches */
						skips[i] = true;
						break;
					}
				}

				/* Next primary key column */
				col_num++;
			}

			/* Array of per-column splits from the previous split point */
			PartitionRangeDatum **prev_splits = palloc0(sizeof(PartitionRangeDatum*)
														* num_key_cols);

			/* Parser state for type conversion and validation */
			ParseState *pstate = make_parsestate(NULL);

			/* Ensure that each split point matches the primary key columns
			 * in number and type, and are in order */
			ListCell *cell1;
			foreach(cell1, split_options->split_points)
			{
				List *split_point = (List *) lfirst(cell1);
				if (list_length(split_point) != num_key_cols)
				{
					ereport(ERROR, (errmsg("Split points must specify a split at "
										   "each primary key column")));
				}

				/* So far, is the current split point less (-1), equal (0), or greater (1)
				 * than the previous split point */
				int curall_vs_prev = -1;

				/* Within a split point, go through the splits for each column */
				int split_num = 0;
				ListCell *cell2;
				foreach(cell2, split_point)
				{
					/* Get the column's split */
					PartitionRangeDatum *split = (PartitionRangeDatum*) lfirst(cell2);

					/* If it contains a value, convert that value */
					if (split->kind == PARTITION_RANGE_DATUM_VALUE)
					{
						A_Const *aconst = (A_Const*) split->value;
						Node *value = (Node *) make_const(pstate, &aconst->val, aconst->location);
						value = coerce_to_target_type(pstate,
													  value, exprType(value),
													  col_attrtypes[split_num],
													  col_attrtypmods[split_num],
													  COERCION_ASSIGNMENT,
													  COERCE_IMPLICIT_CAST,
													  -1);
						if (value == NULL || ((Const*)value)->consttype == 0)
						{
							ereport(ERROR, (errmsg("Type mismatch in split point")));
						}

						split->value = value;
					}
					/* TODO (george): maybe we'll allow MINVALUE/MAXVALUE in the future,
					 * but for now it is illegal */
					else
					{
						ereport(ERROR, (errmsg("Split points must specify finite values")));
					}

					/* Compare current value to previous value
					 * If current split < previous corresponding split, could be a problem */
					PartitionRangeDatum *prev_split = prev_splits[split_num];
					int curcol_vs_prev = 1;
					if (prev_split)
					{
						/* Comparing to MINIMUM */
						if (prev_split->kind == PARTITION_RANGE_DATUM_MINVALUE)
						{
							curcol_vs_prev = (split->kind == PARTITION_RANGE_DATUM_MINVALUE) ?
											 0 : 1;
						}
							/* Comparing to a specified value */
						else if (prev_split->kind == PARTITION_RANGE_DATUM_VALUE)
						{
							if (split->kind == PARTITION_RANGE_DATUM_MINVALUE)
							{
								curcol_vs_prev = -1;
							}
							else if (split->kind == PARTITION_RANGE_DATUM_VALUE)
							{
								/* First check <, then ==, and if neither it is > */
								ScanKey comparator = &col_comparators[split_num];
								Datum cmp_op = ((Const*)(split->value))->constvalue;
								Datum cmp_ref = ((Const*)(prev_split->value))->constvalue;
								curcol_vs_prev = FunctionCall2Coll(&comparator->sk_func,
																   comparator->sk_collation,
																   cmp_op,
																   cmp_ref);
							}
							else if (split->kind == PARTITION_RANGE_DATUM_MAXVALUE)
							{
								curcol_vs_prev = 1;
							}
						}
							/* Comparing to MAXIMUM */
						else if (prev_split->kind == PARTITION_RANGE_DATUM_MAXVALUE)
						{
							curcol_vs_prev = (split->kind == PARTITION_RANGE_DATUM_MAXVALUE) ?
											 0 : -1;
						}
					}

					/* Make sure we maintain sorted order */
					if (curcol_vs_prev >= 0)
					{
						/* Haven't compared any columns yet */
						if (curall_vs_prev == -1)
						{
							curall_vs_prev = curcol_vs_prev;
						}

						/* Equal so far, now greater */
						if (curall_vs_prev == 0 && curcol_vs_prev == 1)
						{
							curall_vs_prev = 1;
						}
					}
					else if (curcol_vs_prev == -1)
					{
						/* If greater so far, in earlier columns which take precedence, fine.
						 * Otherwise we are out of order. */
						if (curall_vs_prev != 1)
						{
							ereport(ERROR, (errmsg("Split points must be in sorted order")));
						}
					}

					/* Finished handling this particular column split */
					prev_splits[split_num++] = split;
				}

				/* TODO (george): Add split point with pggate */
			}

			ereport(WARNING, (errmsg("Range split points are not supported, ignoring")));
			break;
		default:
			ereport(ERROR, (errmsg("Illegal memory state for SPLIT options")));
			break;
	}
}

void
YBCCreateTable(CreateStmt *stmt, char relkind, TupleDesc desc, Oid relationId, Oid namespaceId)
{
	if (relkind != RELKIND_RELATION)
	{
		return;
	}

	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
	{
		return; /* Nothing to do. */
	}

	YBCPgStatement handle = NULL;
	ListCell       *listptr;

	char *db_name = get_database_name(MyDatabaseId);
	char *schema_name = stmt->relation->schemaname;
	if (schema_name == NULL)
	{
		schema_name = get_namespace_name(namespaceId);
	}
	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating Table %s.%s.%s",
					 db_name,
					 schema_name,
					 stmt->relation->relname);

	Constraint *primary_key = NULL;

	foreach(listptr, stmt->constraints)
	{
		Constraint *constraint = lfirst(listptr);

		if (constraint->contype == CONSTR_PRIMARY)
		{
			primary_key = constraint;
		}
	}

	HandleYBStatus(YBCPgNewCreateTable(ybc_pg_session,
									   db_name,
									   schema_name,
									   stmt->relation->relname,
									   MyDatabaseId,
									   relationId,
									   false, /* is_shared_table */
									   false, /* if_not_exists */
									   primary_key == NULL /* add_primary_key */,
									   &handle));

	CreateTableAddColumns(handle, desc, primary_key);

	/* Handle SPLIT statement, if present */
	OptSplit *split_options = stmt->split_options;
	if (split_options)
	{
		/* Illegal without primary key */
		if (primary_key == NULL)
		{
			ereport(ERROR, (errmsg("Cannot have SPLIT options in the absence of a primary key")));
		}

		CreateTableHandleSplitOptions(handle, desc, split_options, primary_key);
	}

	/* Create the table. */
	HandleYBStmtStatus(YBCPgExecCreateTable(handle), handle);

	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCDropTable(Oid relationId)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropTable(ybc_pg_session,
									 MyDatabaseId,
									 relationId,
									 false,    /* if_exists */
									 &handle));
	HandleYBStmtStatus(YBCPgExecDropTable(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCTruncateTable(Relation rel) {
	YBCPgStatement handle;
	Oid relationId = RelationGetRelid(rel);

	/* Truncate the base table */
	HandleYBStatus(YBCPgNewTruncateTable(ybc_pg_session, MyDatabaseId, relationId, &handle));
	HandleYBStmtStatus(YBCPgExecTruncateTable(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));

	if (!rel->rd_rel->relhasindex)
		return;

	/* Truncate the associated secondary indexes */
	List	 *indexlist = RelationGetIndexList(rel);
	ListCell *lc;

	foreach(lc, indexlist)
	{
		Oid indexId = lfirst_oid(lc);

		if (indexId == rel->rd_pkindex)
			continue;

		HandleYBStatus(YBCPgNewTruncateTable(ybc_pg_session, MyDatabaseId, indexId, &handle));
		HandleYBStmtStatus(YBCPgExecTruncateTable(handle), handle);
		HandleYBStatus(YBCPgDeleteStatement(handle));
	}

	list_free(indexlist);
}

void
YBCCreateIndex(const char *indexName,
			   IndexInfo *indexInfo,			   
			   TupleDesc indexTupleDesc,
			   int16 *coloptions,
			   Oid indexId,
			   Relation rel)
{
	char *db_name	  = get_database_name(MyDatabaseId);
	char *schema_name = get_namespace_name(RelationGetNamespace(rel));

	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating index %s.%s.%s",
					 db_name,
					 schema_name,
					 indexName);

	YBCPgStatement handle = NULL;

	HandleYBStatus(YBCPgNewCreateIndex(ybc_pg_session,
									   db_name,
									   schema_name,
									   indexName,
									   MyDatabaseId,
									   indexId,
									   RelationGetRelid(rel),
									   rel->rd_rel->relisshared,
									   indexInfo->ii_Unique,
									   false, /* if_not_exists */
									   &handle));

	for (int i = 0; i < indexTupleDesc->natts; i++)
	{
		Form_pg_attribute     att         = TupleDescAttr(indexTupleDesc, i);
		char                  *attname    = NameStr(att->attname);
		AttrNumber            attnum      = att->attnum;
		const YBCPgTypeEntity *col_type   = YBCDataTypeFromOidMod(attnum, att->atttypid);
		const bool            is_key      = (i < indexInfo->ii_NumIndexKeyAttrs);

		if (is_key)
		{
			if (!YBCDataTypeIsValidForKey(att->atttypid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("INDEX on column of type '%s' not yet supported",
								YBPgTypeOidToStr(att->atttypid))));
		}

	const int16 options        = coloptions[i];
	const bool  is_hash        = options & INDOPTION_HASH;
	const bool  is_desc        = options & INDOPTION_DESC;
	const bool  is_nulls_first = options & INDOPTION_NULLS_FIRST;

		HandleYBStmtStatus(YBCPgCreateIndexAddColumn(handle,
													 attname,
													 attnum,
													 col_type,
													 is_hash,
													 is_key,
													 is_desc,
													 is_nulls_first), handle);
	}

	/* Create the index. */
	HandleYBStmtStatus(YBCPgExecCreateIndex(handle), handle);

	HandleYBStatus(YBCPgDeleteStatement(handle));
}

YBCPgStatement
YBCPrepareAlterTable(AlterTableStmt *stmt, Relation rel, Oid relationId)
{
	YBCPgStatement handle = NULL;
	HandleYBStatus(YBCPgNewAlterTable(ybc_pg_session,
									  MyDatabaseId,
									  relationId,
									  &handle));

	ListCell *lcmd;
	int col = 1;
	bool needsYBAlter = false;

	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		switch (cmd->subtype)
		{
			case AT_AddColumn:
			{
				ColumnDef* colDef = (ColumnDef *) cmd->def;
				Oid			typeOid;
				int32		typmod;
				HeapTuple	typeTuple;
				int order;

				/* Skip yb alter for IF NOT EXISTS with existing column */
				if (cmd->missing_ok)
				{
					HeapTuple tuple = SearchSysCacheAttName(RelationGetRelid(rel), colDef->colname);
					if (HeapTupleIsValid(tuple)) {
						ReleaseSysCache(tuple);
						break;
					}
				}

				typeTuple = typenameType(NULL, colDef->typeName, &typmod);
				typeOid = HeapTupleGetOid(typeTuple);
				order = RelationGetNumberOfAttributes(rel) + col;
				const YBCPgTypeEntity *col_type = YBCDataTypeFromOidMod(order, typeOid);

				HandleYBStmtStatus(YBCPgAlterTableAddColumn(handle, colDef->colname,
															order, col_type,
															colDef->is_not_null), handle);

				++col;
				ReleaseSysCache(typeTuple);
				needsYBAlter = true;

				break;
			}
			case AT_DropColumn:
			{
				/* Skip yb alter for IF EXISTS with non-existent column */
				if (cmd->missing_ok)
				{
					HeapTuple tuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);
					if (!HeapTupleIsValid(tuple))
						break;
					ReleaseSysCache(tuple);
				}

				HandleYBStmtStatus(YBCPgAlterTableDropColumn(handle, cmd->name), handle);
				needsYBAlter = true;

				break;
			}

			case AT_AddIndex:
			case AT_AddIndexConstraint: {
				IndexStmt *index = (IndexStmt *) cmd->def;
				// Only allow adding indexes when it is a unique non-primary-key constraint
				if (!index->unique || index->primary || !index->isconstraint) {
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("This ALTER TABLE command is not yet supported.")));
				}

				break;
			}

			case AT_AddConstraint:
			case AT_DropConstraint:
			case AT_DropOids:
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
			case AT_ChangeOwner:
			case AT_ColumnDefault:
			case AT_DropNotNull:
			case AT_SetNotNull:
			case AT_AddIdentity:
			case AT_SetIdentity:
			case AT_DropIdentity:
			case AT_EnableRowSecurity:
			case AT_DisableRowSecurity:
			case AT_ForceRowSecurity:
			case AT_NoForceRowSecurity:
				/* For these cases a YugaByte alter isn't required, so we do nothing. */
				break;

			default:
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("This ALTER TABLE command is not yet supported.")));
				break;
		}
	}

	if (!needsYBAlter)
	{
		HandleYBStatus(YBCPgDeleteStatement(handle));
		return NULL;
	}

	return handle;
}

void
YBCExecAlterTable(YBCPgStatement handle)
{
	if (handle)
	{
		HandleYBStmtStatus(YBCPgExecAlterTable(handle), handle);
		HandleYBStatus(YBCPgDeleteStatement(handle));
	}
}

void
YBCRename(RenameStmt *stmt, Oid relationId)
{
	YBCPgStatement handle = NULL;
	char *db_name	  = get_database_name(MyDatabaseId);

	switch (stmt->renameType)
	{
		case OBJECT_TABLE:
			HandleYBStatus(YBCPgNewAlterTable(ybc_pg_session,
											  MyDatabaseId,
											  relationId,
											  &handle));
			HandleYBStmtStatus(YBCPgAlterTableRenameTable(handle, db_name, stmt->newname), handle);
			break;

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:

			HandleYBStatus(YBCPgNewAlterTable(ybc_pg_session,
											  MyDatabaseId,
											  relationId,
											  &handle));

			HandleYBStmtStatus(YBCPgAlterTableRenameColumn(handle,
							   stmt->subname, stmt->newname), handle);
			break;

		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("Renaming this object is not yet supported.")));

	}

	if (IsYBRelationById(relationId)) {
		YBCExecAlterTable(handle);
	}
}

void
YBCDropIndex(Oid relationId)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropIndex(ybc_pg_session,
									 MyDatabaseId,
									 relationId,
									 false,	   /* if_exists */
									 &handle));
	HandleYBStmtStatus(YBCPgExecDropIndex(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}
