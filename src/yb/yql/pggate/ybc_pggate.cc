// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include <cds/init.h>

#include "yb/common/ybc-internal.h"
#include "yb/util/atomic.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "yb/yql/pggate/pggate.h"
#include "yb/yql/pggate/pggate_flags.h"

DECLARE_bool(client_suppress_created_logs);

DEFINE_int32(pggate_num_connections_to_server, 1,
             "Number of underlying connections to each server from a PostgreSQL backend process. "
             "This overrides the value of --num_connections_to_server.");

DECLARE_int32(num_connections_to_server);

DECLARE_int32(delay_alter_sequence_sec);

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// C++ Implementation.
// All C++ objects and structures in this module are listed in the following namespace.
//--------------------------------------------------------------------------------------------------
namespace {

// Using a raw pointer here to fully control object initialization and destruction.
pggate::PgApiImpl* pgapi;
std::atomic<bool> pgapi_shutdown_done;

} // anonymous namespace

//--------------------------------------------------------------------------------------------------
// C API.
//--------------------------------------------------------------------------------------------------
extern "C" {

void YBCInitPgGate(const YBCPgTypeEntity *YBCDataTypeTable, int count) {
  InitThreading();

  CHECK(pgapi == nullptr) << ": " << __PRETTY_FUNCTION__ << " can only be called once";

  YBCInitFlags();

  pgapi_shutdown_done.exchange(false);
  pgapi = new pggate::PgApiImpl(YBCDataTypeTable, count);
  VLOG(1) << "PgGate open";
}

void YBCDestroyPgGate() {
  if (pgapi_shutdown_done.exchange(true)) {
    LOG(FATAL) << __PRETTY_FUNCTION__ << " can only be called once";
  }
  delete pgapi;
  pgapi = nullptr;
  VLOG(1) << __PRETTY_FUNCTION__ << " finished";
}

YBCStatus YBCPgCreateEnv(YBCPgEnv *pg_env) {
  return ToYBCStatus(pgapi->CreateEnv(pg_env));
}

YBCStatus YBCPgDestroyEnv(YBCPgEnv pg_env) {
  return ToYBCStatus(pgapi->DestroyEnv(pg_env));
}

YBCStatus YBCPgCreateSession(const YBCPgEnv pg_env,
                             const char *database_name,
                             YBCPgSession *pg_session) {
  string db_name = database_name == NULL ? "" : database_name;
  return ToYBCStatus(pgapi->CreateSession(pg_env, database_name, pg_session));
}

YBCStatus YBCPgDestroySession(YBCPgSession pg_session) {
  return ToYBCStatus(pgapi->DestroySession(pg_session));
}

YBCStatus YBCPgInvalidateCache(YBCPgSession pg_session) {
  return ToYBCStatus(pgapi->InvalidateCache(pg_session));
}

const YBCPgTypeEntity *YBCPgFindTypeEntity(int type_oid) {
  return pgapi->FindTypeEntity(type_oid);
}

YBCPgDataType YBCPgGetType(const YBCPgTypeEntity *type_entity) {
  if (type_entity) {
    return type_entity->yb_type;
  }
  return YB_YQL_DATA_TYPE_UNKNOWN_DATA;
}

bool YBCPgAllowForPrimaryKey(const YBCPgTypeEntity *type_entity) {
  if (type_entity) {
    return type_entity->allow_for_primary_key;
  }
  return false;
}

//--------------------------------------------------------------------------------------------------
// DDL Statements.
//--------------------------------------------------------------------------------------------------
// Database Operations -----------------------------------------------------------------------------

YBCStatus YBCPgConnectDatabase(YBCPgSession pg_session, const char *database_name) {
  return ToYBCStatus(pgapi->ConnectDatabase(pg_session, database_name));
}

YBCStatus YBCPgNewCreateDatabase(YBCPgSession pg_session,
                                 const char *database_name,
                                 const YBCPgOid database_oid,
                                 const YBCPgOid source_database_oid,
                                 const YBCPgOid next_oid,
                                 YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewCreateDatabase(pg_session, database_name, database_oid,
                                              source_database_oid, next_oid, handle));
}

YBCStatus YBCPgExecCreateDatabase(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateDatabase(handle));
}

YBCStatus YBCPgNewDropDatabase(YBCPgSession pg_session,
                               const char *database_name,
                               const YBCPgOid database_oid,
                               YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewDropDatabase(pg_session, database_name, database_oid, handle));
}

YBCStatus YBCPgExecDropDatabase(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDropDatabase(handle));
}

YBCStatus YBCPgNewAlterDatabase(YBCPgSession pg_session,
                               const char *database_name,
                               const YBCPgOid database_oid,
                               YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewAlterDatabase(pg_session, database_name, database_oid, handle));
}

YBCStatus YBCPgAlterDatabaseRenameDatabase(YBCPgStatement handle, const char *newname) {
  return ToYBCStatus(pgapi->AlterDatabaseRenameDatabase(handle, newname));
}

YBCStatus YBCPgExecAlterDatabase(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecAlterDatabase(handle));
}

YBCStatus YBCPgReserveOids(YBCPgSession pg_session,
                           const YBCPgOid database_oid,
                           const YBCPgOid next_oid,
                           const uint32_t count,
                           YBCPgOid *begin_oid,
                           YBCPgOid *end_oid) {
  return ToYBCStatus(pgapi->ReserveOids(pg_session, database_oid, next_oid, count,
                                        begin_oid, end_oid));
}

YBCStatus YBCPgGetCatalogMasterVersion(YBCPgSession pg_session, uint64_t *version) {
  return ToYBCStatus(pgapi->GetCatalogMasterVersion(pg_session, version));
}

// Statement Operations ----------------------------------------------------------------------------

YBCStatus YBCPgDeleteStatement(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->DeleteStatement(handle));
}

YBCStatus YBCPgClearBinds(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ClearBinds(handle));
}

// Sequence Operations -----------------------------------------------------------------------------

YBCStatus YBCInsertSequenceTuple(YBCPgSession pg_session,
                                 int64_t db_oid,
                                 int64_t seq_oid,
                                 uint64_t ysql_catalog_version,
                                 int64_t last_val,
                                 bool is_called) {
  return ToYBCStatus(pgapi->InsertSequenceTuple(pg_session, db_oid, seq_oid, ysql_catalog_version,
      last_val, is_called));
}

YBCStatus YBCUpdateSequenceTupleConditionally(YBCPgSession pg_session,
                                              int64_t db_oid,
                                              int64_t seq_oid,
                                              uint64_t ysql_catalog_version,
                                              int64_t last_val,
                                              bool is_called,
                                              int64_t expected_last_val,
                                              bool expected_is_called,
                                              bool *skipped) {
  return ToYBCStatus(
      pgapi->UpdateSequenceTupleConditionally(pg_session, db_oid, seq_oid, ysql_catalog_version,
          last_val, is_called, expected_last_val, expected_is_called, skipped));
}

YBCStatus YBCUpdateSequenceTuple(YBCPgSession pg_session,
                                 int64_t db_oid,
                                 int64_t seq_oid,
                                 uint64_t ysql_catalog_version,
                                 int64_t last_val,
                                 bool is_called,
                                 bool* skipped) {
  return ToYBCStatus(pgapi->UpdateSequenceTuple(pg_session, db_oid, seq_oid, ysql_catalog_version,
      last_val, is_called, skipped));
}

YBCStatus YBCReadSequenceTuple(YBCPgSession pg_session,
                               int64_t db_oid,
                               int64_t seq_oid,
                               uint64_t ysql_catalog_version,
                               int64_t *last_val,
                               bool *is_called) {
  return ToYBCStatus(pgapi->ReadSequenceTuple(pg_session, db_oid, seq_oid, ysql_catalog_version,
      last_val, is_called));
}

YBCStatus YBCDeleteSequenceTuple(YBCPgSession pg_session, int64_t db_oid, int64_t seq_oid) {
  return ToYBCStatus(pgapi->DeleteSequenceTuple(pg_session, db_oid, seq_oid));
}

// Table Operations -------------------------------------------------------------------------------

YBCStatus YBCPgNewCreateTable(YBCPgSession pg_session,
                              const char *database_name,
                              const char *schema_name,
                              const char *table_name,
                              const YBCPgOid database_oid,
                              const YBCPgOid table_oid,
                              bool is_shared_table,
                              bool if_not_exist,
                              bool add_primary_key,
                              YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewCreateTable(pg_session, database_name, schema_name, table_name,
                                           table_id, is_shared_table, if_not_exist, add_primary_key,
                                           handle));
}

YBCStatus YBCPgCreateTableAddColumn(YBCPgStatement handle, const char *attr_name, int attr_num,
                                    const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                                    bool is_desc, bool is_nulls_first) {
  return ToYBCStatus(pgapi->CreateTableAddColumn(handle, attr_name, attr_num, attr_type,
                                                 is_hash, is_range, is_desc, is_nulls_first));
}

YBCStatus YBCPgCreateTableSetNumTablets(YBCPgStatement handle, int32_t num_tablets) {
  return ToYBCStatus(pgapi->CreateTableSetNumTablets(handle, num_tablets));
}

YBCStatus YBCPgExecCreateTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateTable(handle));
}

YBCStatus YBCPgNewAlterTable(YBCPgSession pg_session,
                             const YBCPgOid database_oid,
                             const YBCPgOid table_oid,
                             YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewAlterTable(pg_session, table_id, handle));
}

YBCStatus YBCPgAlterTableAddColumn(YBCPgStatement handle, const char *name, int order,
                                   const YBCPgTypeEntity *attr_type, bool is_not_null) {
  return ToYBCStatus(pgapi->AlterTableAddColumn(handle, name, order, attr_type, is_not_null));
}

YBCStatus YBCPgAlterTableRenameColumn(YBCPgStatement handle, const char *oldname,
                                      const char *newname) {
  return ToYBCStatus(pgapi->AlterTableRenameColumn(handle, oldname, newname));
}

YBCStatus YBCPgAlterTableDropColumn(YBCPgStatement handle, const char *name) {
  return ToYBCStatus(pgapi->AlterTableDropColumn(handle, name));
}

YBCStatus YBCPgAlterTableRenameTable(YBCPgStatement handle, const char *db_name,
                                     const char *newname) {
  return ToYBCStatus(pgapi->AlterTableRenameTable(handle, db_name, newname));
}

YBCStatus YBCPgExecAlterTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecAlterTable(handle));
}

YBCStatus YBCPgNewDropTable(YBCPgSession pg_session,
                            const YBCPgOid database_oid,
                            const YBCPgOid table_oid,
                            bool if_exist,
                            YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewDropTable(pg_session, table_id, if_exist, handle));
}

YBCStatus YBCPgExecDropTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDropTable(handle));
}

YBCStatus YBCPgGetTableDesc(YBCPgSession pg_session,
                            const YBCPgOid database_oid,
                            const YBCPgOid table_oid,
                            YBCPgTableDesc *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->GetTableDesc(pg_session, table_id, handle));
}

YBCStatus YBCPgDeleteTableDesc(YBCPgTableDesc handle) {
  return ToYBCStatus(pgapi->DeleteTableDesc(handle));
}

YBCStatus YBCPgGetColumnInfo(YBCPgTableDesc table_desc,
                             int16_t attr_number,
                             bool *is_primary,
                             bool *is_hash) {
  return ToYBCStatus(pgapi->GetColumnInfo(table_desc, attr_number, is_primary, is_hash));
}

YBCStatus YBCPgSetCatalogCacheVersion(YBCPgStatement handle,
                                      uint64_t catalog_cache_version) {
  return ToYBCStatus(pgapi->SetCatalogCacheVersion(handle, catalog_cache_version));
}

YBCStatus YBCPgDmlModifiesRow(YBCPgStatement handle, bool *modifies_row) {
  return ToYBCStatus(pgapi->DmlModifiesRow(handle, modifies_row));
}

YBCStatus YBCPgSetIsSysCatalogVersionChange(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->SetIsSysCatalogVersionChange(handle));
}

YBCStatus YBCPgNewTruncateTable(YBCPgSession pg_session,
                                const YBCPgOid database_oid,
                                const YBCPgOid table_oid,
                                YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewTruncateTable(pg_session, table_id, handle));
}

YBCStatus YBCPgExecTruncateTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecTruncateTable(handle));
}

// Index Operations -------------------------------------------------------------------------------

YBCStatus YBCPgNewCreateIndex(YBCPgSession pg_session,
                              const char *database_name,
                              const char *schema_name,
                              const char *index_name,
                              const YBCPgOid database_oid,
                              const YBCPgOid index_oid,
                              const YBCPgOid table_oid,
                              bool is_shared_index,
                              bool is_unique_index,
                              bool if_not_exist,
                              YBCPgStatement *handle) {
  const PgObjectId index_id(database_oid, index_oid);
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewCreateIndex(pg_session, database_name, schema_name, index_name,
                                           index_id, table_id, is_shared_index, is_unique_index,
                                           if_not_exist, handle));
}

YBCStatus YBCPgCreateIndexAddColumn(YBCPgStatement handle, const char *attr_name, int attr_num,
                                    const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                                    bool is_desc, bool is_nulls_first) {
  return ToYBCStatus(pgapi->CreateIndexAddColumn(handle, attr_name, attr_num, attr_type,
                                                 is_hash, is_range, is_desc, is_nulls_first));
}

YBCStatus YBCPgExecCreateIndex(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateIndex(handle));
}

YBCStatus YBCPgNewDropIndex(YBCPgSession pg_session,
                            const YBCPgOid database_oid,
                            const YBCPgOid index_oid,
                            bool if_exist,
                            YBCPgStatement *handle) {
  const PgObjectId index_id(database_oid, index_oid);
  return ToYBCStatus(pgapi->NewDropIndex(pg_session, index_id, if_exist, handle));
}

YBCStatus YBCPgExecDropIndex(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDropIndex(handle));
}

//--------------------------------------------------------------------------------------------------
// DML Statements.
//--------------------------------------------------------------------------------------------------

YBCStatus YBCPgDmlAppendTarget(YBCPgStatement handle, YBCPgExpr target) {
  return ToYBCStatus(pgapi->DmlAppendTarget(handle, target));
}

YBCStatus YBCPgDmlBindColumn(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlBindColumn(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlBindColumnCondEq(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlBindColumnCondEq(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlBindColumnCondBetween(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value,
    YBCPgExpr attr_value_end) {
  return ToYBCStatus(pgapi->DmlBindColumnCondBetween(handle, attr_num, attr_value, attr_value_end));
}

YBCStatus YBCPgDmlBindColumnCondIn(YBCPgStatement handle, int attr_num, int n_attr_values,
    YBCPgExpr *attr_values) {
  return ToYBCStatus(pgapi->DmlBindColumnCondIn(handle, attr_num, n_attr_values, attr_values));
}

YBCStatus YBCPgDmlBindIndexColumn(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlBindIndexColumn(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlAssignColumn(YBCPgStatement handle,
                               int attr_num,
                               YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlAssignColumn(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlFetch(YBCPgStatement handle, int32_t natts, uint64_t *values, bool *isnulls,
                        YBCPgSysColumns *syscols, bool *has_data) {
  return ToYBCStatus(pgapi->DmlFetch(handle, natts, values, isnulls, syscols, has_data));
}

YBCStatus YBCPgStartBufferingWriteOperations(YBCPgSession pg_session) {
  return ToYBCStatus(pgapi->StartBufferingWriteOperations(pg_session));
}

YBCStatus YBCPgFlushBufferedWriteOperations(YBCPgSession pg_session) {
  return ToYBCStatus(pgapi->FlushBufferedWriteOperations(pg_session));
}

YBCStatus YBCPgDmlExecWriteOp(YBCPgStatement handle, int32_t *rows_affected_count) {
  return ToYBCStatus(pgapi->DmlExecWriteOp(handle, rows_affected_count));
}

YBCStatus YBCPgDmlBuildYBTupleId(YBCPgStatement handle, const YBCPgAttrValueDescriptor *attrs,
                                 int32_t nattrs, uint64_t *ybctid) {
  return ToYBCStatus(pgapi->DmlBuildYBTupleId(handle, attrs, nattrs, ybctid));
}

// INSERT Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewInsert(YBCPgSession pg_session,
                         const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         const bool is_single_row_txn,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewInsert(pg_session, table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecInsert(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecInsert(handle));
}

// UPDATE Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewUpdate(YBCPgSession pg_session,
                         const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewUpdate(pg_session, table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecUpdate(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecUpdate(handle));
}

// DELETE Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewDelete(YBCPgSession pg_session,
                         const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewDelete(pg_session, table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecDelete(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDelete(handle));
}

// SELECT Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewSelect(YBCPgSession pg_session,
                         const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         const YBCPgOid index_oid,
                         bool prevent_restart,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  const PgObjectId index_id(database_oid, index_oid);
  return ToYBCStatus(pgapi->NewSelect(
      pg_session, table_id, index_id, PreventRestart(prevent_restart), handle));
}

YBCStatus YBCPgSetForwardScan(YBCPgStatement handle, bool is_forward_scan) {
  return ToYBCStatus(pgapi->SetForwardScan(handle, is_forward_scan));
}

YBCStatus YBCPgExecSelect(YBCPgStatement handle, const YBCPgExecParameters *exec_params) {
  return ToYBCStatus(pgapi->ExecSelect(handle, exec_params));
}

//--------------------------------------------------------------------------------------------------
// Expression Operations
//--------------------------------------------------------------------------------------------------

YBCStatus YBCPgNewColumnRef(YBCPgStatement stmt, int attr_num, const YBCPgTypeEntity *type_entity,
                            const YBCPgTypeAttrs *type_attrs, YBCPgExpr *expr_handle) {
  return ToYBCStatus(pgapi->NewColumnRef(stmt, attr_num, type_entity, type_attrs, expr_handle));
}

YBCStatus YBCPgNewConstant(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                           uint64_t datum, bool is_null, YBCPgExpr *expr_handle) {
  return ToYBCStatus(pgapi->NewConstant(stmt, type_entity, datum, is_null, expr_handle));
}

YBCStatus YBCPgNewConstantOp(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                           uint64_t datum, bool is_null, YBCPgExpr *expr_handle, bool is_gt) {
  return ToYBCStatus(pgapi->NewConstantOp(stmt, type_entity, datum, is_null, expr_handle, is_gt));
}

// Overwriting the expression's result with any desired values.
YBCStatus YBCPgUpdateConstInt2(YBCPgExpr expr, int16_t value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstInt4(YBCPgExpr expr, int32_t value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstInt8(YBCPgExpr expr, int64_t value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstFloat4(YBCPgExpr expr, float value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstFloat8(YBCPgExpr expr, double value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstText(YBCPgExpr expr, const char *value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstChar(YBCPgExpr expr, const char *value,  int64_t bytes, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, bytes, is_null));
}

YBCStatus YBCPgNewOperator(YBCPgStatement stmt, const char *opname,
                           const YBCPgTypeEntity *type_entity,
                           YBCPgExpr *op_handle) {
  return ToYBCStatus(pgapi->NewOperator(stmt, opname, type_entity, op_handle));
}

YBCStatus YBCPgOperatorAppendArg(YBCPgExpr op_handle, YBCPgExpr arg) {
  return ToYBCStatus(pgapi->OperatorAppendArg(op_handle, arg));
}

//------------------------------------------------------------------------------------------------
// Transaction operation.
//------------------------------------------------------------------------------------------------

YBCPgTxnManager YBCGetPgTxnManager() {
  return pgapi->GetPgTxnManager();
}

bool YBCIsInitDbModeEnvVarSet() {
  static bool cached_value = false;
  static bool cached = false;

  if (!cached) {
    const char* initdb_mode_env_var_value = getenv("YB_PG_INITDB_MODE");
    cached_value = initdb_mode_env_var_value && strcmp(initdb_mode_env_var_value, "1") == 0;
    cached = true;
  }

  return cached_value;
}

void YBCInitFlags() {
  if (YBCIsInitDbModeEnvVarSet()) {
    // Suppress log spew during initdb.
    FLAGS_client_suppress_created_logs = true;
  }

  SetAtomicFlag(GetAtomicFlag(&FLAGS_pggate_num_connections_to_server),
                &FLAGS_num_connections_to_server);

  // TODO(neil) Init a gflag for "YB_PG_TRANSACTIONS_ENABLED" here also.
  // Mikhail agreed that this flag should just be initialized once at the beginning here.
  // Currently, it is initialized for every CREATE statement.
}

YBCStatus YBCPgIsInitDbDone(YBCPgSession pg_session, bool* initdb_done) {
  return ToYBCStatus(pg_session->IsInitDbDone(initdb_done));
}

YBCStatus YBCGetSharedCatalogVersion(YBCPgSession pg_session, uint64_t* catalog_version) {
  auto result = pg_session->GetSharedCatalogVersion();
  if (result.ok()) {
    *catalog_version = *result;
    return YBCStatusOK();
  }
  return ToYBCStatus(result.status());
}

int32_t YBCGetMaxReadRestartAttempts() {
  return FLAGS_ysql_max_read_restart_attempts;
}

int32_t YBCGetOutputBufferSize() {
  return FLAGS_ysql_output_buffer_size;
}

} // extern "C"

} // namespace pggate
} // namespace yb
