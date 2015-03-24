/*
 * db.c
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <stdio.h>
#include <mysql/errmsg.h>

#include <map>

#include "db.h"

//***************************************************************************
// DB Statement
//***************************************************************************

int cDbStatement::explain = no;

cDbStatement::cDbStatement(cDbTable* aTable)
{
   table = aTable;
   connection = table->getConnection();
   stmtTxt = "";
   stmt = 0;
   inCount = 0;
   outCount = 0;
   inBind = 0;
   outBind = 0;
   affected = 0;
   metaResult = 0;
   bindPrefix = 0;
   firstExec = yes;
   buildErrors = 0;

   callsPeriod = 0;
   callsTotal = 0;
   duration = 0;

   if (connection)
      connection->statements.append(this);
}

cDbStatement::cDbStatement(cDbConnection* aConnection, const char* sText)
{
   table = 0;
   connection = aConnection;
   stmtTxt = sText;
   stmt = 0;
   inCount = 0;
   outCount = 0;
   inBind = 0;
   outBind = 0;
   affected = 0;
   metaResult = 0;
   bindPrefix = 0;
   firstExec = yes;

   callsPeriod = 0;
   callsTotal = 0;
   duration = 0;
   buildErrors = 0;

   if (connection)
      connection->statements.append(this);
}

cDbStatement::~cDbStatement()  
{ 
   if (connection)
      connection->statements.remove(this);

   clear();
}

//***************************************************************************
// Execute
//***************************************************************************

int cDbStatement::execute(int noResult)
{
   affected = 0;

   if (!connection || !connection->getMySql())
      return fail;

   if (!stmt)
      return connection->errorSql(connection, "execute(missing statement)");

//    if (explain && firstExec)
//    {
//       firstExec = no;

//       if (strstr(stmtTxt.c_str(), "select "))
//       {
//          MYSQL_RES* result;
//          MYSQL_ROW row;
//          string q = "explain " + stmtTxt;

//          if (connection->query(q.c_str()) != success)
//             connection->errorSql(connection, "explain ", 0);
//          else if ((result = mysql_store_result(connection->getMySql())))
//          {
//             while ((row = mysql_fetch_row(result)))
//             {
//                tell(0, "EXPLAIN: %s) %s %s %s %s %s %s %s %s %s", 
//                     row[0], row[1], row[2], row[3],
//                     row[4], row[5], row[6], row[7], row[8], row[9]);
//             }
            
//             mysql_free_result(result);
//          }
//       }
//    }

   // tell(0, "execute %d [%s]", stmt, stmtTxt.c_str());

   double start = usNow();

   if (mysql_stmt_execute(stmt))
      return connection->errorSql(connection, "execute(stmt_execute)", stmt, stmtTxt.c_str());

   duration += usNow() - start;
   callsPeriod++;
   callsTotal++;

   // out binding - if needed

   if (outCount && !noResult)
   {
      if (mysql_stmt_store_result(stmt))
         return connection->errorSql(connection, "execute(store_result)", stmt, stmtTxt.c_str());

      // fetch the first result - if any

      if (mysql_stmt_affected_rows(stmt) > 0)
         mysql_stmt_fetch(stmt);
   }
   else if (outCount)
   {
      mysql_stmt_store_result(stmt);
   }

   // result was stored (above) only if output (outCound) is expected, 
   // therefore we don't need to call freeResult() after insert() or update()
   
   affected = mysql_stmt_affected_rows(stmt);

   return success;
}

//***************************************************************************
// 
//***************************************************************************

int cDbStatement::getLastInsertId()
{
   MYSQL_RES* result = 0;
   int insertId = na;

   if ((result = mysql_store_result(connection->getMySql())) == 0 &&
       mysql_field_count(connection->getMySql()) == 0 &&
       mysql_insert_id(connection->getMySql()) != 0)
   {
      insertId = mysql_insert_id(connection->getMySql());
   }

   mysql_free_result(result);

   return insertId;
}

int cDbStatement::getResultCount()
{
   mysql_stmt_store_result(stmt);

   return mysql_stmt_affected_rows(stmt);
}

int cDbStatement::find()
{
   if (execute() != success)
      return fail;

   return getAffected() > 0 ? yes : no;
}

int cDbStatement::fetch()
{
   if (!mysql_stmt_fetch(stmt))
      return yes;

   return no;
}

int cDbStatement::freeResult()
{
   if (metaResult)
      mysql_free_result(metaResult);
   
   if (stmt)
      mysql_stmt_free_result(stmt);
   
   return success;
}

//***************************************************************************
// Build Statements - new Interface
//***************************************************************************

int cDbStatement::build(const char* format, ...)
{
   if (format)
   {
      char* tmp;

      va_list more;
      va_start(more, format);
      vasprintf(&tmp, format, more);

      stmtTxt += tmp;
      free(tmp);
   }

   return success;
}

int cDbStatement::bind(const char* fname, int mode, const char* delim)
{
   return bind(table->getValue(fname), mode, delim);
}

int cDbStatement::bind(cDbFieldDef* field, int mode, const char* delim)
{
   return bind(table->getValue(field), mode, delim);
}

int cDbStatement::bind(cDbTable* aTable, cDbFieldDef* field, int mode, const char* delim)
{
   return bind(aTable->getValue(field), mode, delim);
}

int cDbStatement::bind(cDbTable* aTable, const char* fname, int mode, const char* delim)
{
   return bind(aTable->getValue(fname), mode, delim);
}

int cDbStatement::bind(cDbValue* value, int mode, const char* delim)
{
   if (!value || !value->getField())
   {
      tell(0, "Error: Missing %s value", !value ? "bind" : "field of bind");
      buildErrors++;
      return fail;
   }

   if (delim)
      stmtTxt += delim;

   if (bindPrefix)
      stmtTxt += bindPrefix;

   if (mode & bndIn)
   {
      if (mode & bndSet)
         stmtTxt += value->getDbName() + string(" =");
      
      stmtTxt += " ?";
      appendBinding(value, bndIn);
   }
   else if (mode & bndOut)
   {
      stmtTxt += value->getDbName();
      appendBinding(value, bndOut);
   }

   return success;
}

int cDbStatement::bindAllOut(const char* delim)
{
   int n = 0;
   std::map<std::string, cDbFieldDef*>::iterator f;
   cDbTableDef* tableDef = table->getTableDef();

   if (delim)
      stmtTxt += delim;

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      if (f->second->getType() & ftMeta)
         continue;
      
      bind(f->second, bndOut, n++ ? ", " : "");
   }

   return success;
}

int cDbStatement::bindCmp(const char* ctable, cDbValue* value, 
                          const char* comp, const char* delim)
{
   if (delim)  build("%s", delim);
   if (ctable) build("%s.", ctable);

   build("%s%s %s ?", bindPrefix ? bindPrefix : "", value->getDbName(), comp);

   appendBinding(value, bndIn);

   return success;
}

int cDbStatement::bindCmp(const char* ctable, cDbFieldDef* field, cDbValue* value, 
                          const char* comp, const char* delim)
{
   cDbValue* vf = table->getRow()->getValue(field);
   cDbValue* vv = value ? value : vf;

   if (!vf || !vv)
   {
      buildErrors++;
      return fail;
   }

   if (delim)  build("%s", delim);
   if (ctable) build("%s.", ctable);

   build("%s%s %s ?", bindPrefix ? bindPrefix : "", vf->getDbName(), comp);

   appendBinding(vv, bndIn);

   return success;
}

int cDbStatement::bindCmp(const char* ctable, const char* fname, cDbValue* value, 
                          const char* comp, const char* delim)
{
   cDbValue* vf = table->getRow()->getValue(fname);
   cDbValue* vv = value ? value : vf;

   if (!vf || !vv)
   {
      buildErrors++;
      return fail;
   }

   if (delim)  build("%s", delim);
   if (ctable) build("%s.", ctable);

   build("%s%s %s ?", bindPrefix ? bindPrefix : "", vf->getDbName(), comp);

   appendBinding(vv, bndIn);

   return success;
}

//***************************************************************************
// Bind In Char   - like <field> in ('A','B','C')
//***************************************************************************

int cDbStatement::bindInChar(const char* ctable, const char* fname, 
                             cDbValue* value, const char* delim)
{
   cDbValue* vf = table->getRow()->getValue(fname);
   cDbValue* vv = value ? value : vf;

   if (!vf || !vv)
   {
      buildErrors++;
      return fail;
   }

   build("%s find_in_set(cast(%s%s%s%s as char),?)", 
         delim ? delim : "", 
         bindPrefix ? bindPrefix : "", 
         ctable ? ctable : "", 
         ctable ? "." : "", 
         vf->getDbName());

   appendBinding(vv, bndIn);

   return success;
}

//***************************************************************************
// Clear
//***************************************************************************

void cDbStatement::clear()
{
   stmtTxt = "";
   affected = 0;

   if (inCount)
   {
      free(inBind);
      inCount = 0;
      inBind = 0;
   }
   
   if (outCount)
   {
      free(outBind);
      outCount = 0;
      outBind = 0;
   }
   
   if (stmt) 
   { 
      mysql_stmt_free_result(stmt);
      mysql_stmt_close(stmt); 
      stmt = 0; 
   }
}

//***************************************************************************
// Append Binding 
//***************************************************************************

int cDbStatement::appendBinding(cDbValue* value, BindType bt)
{
   int count = 0;
   MYSQL_BIND** bindings = 0;
   MYSQL_BIND* newBinding;

   if (bt & bndIn)
   {
      count = ++inCount;
      bindings = &inBind;
   }
   else if (bt & bndOut)
   {
      count = ++outCount;
      bindings = &outBind;
   }
   else
      return 0;

   if (!bindings)
      *bindings = (MYSQL_BIND*)malloc(count * sizeof(MYSQL_BIND));
   else
      *bindings = (MYSQL_BIND*)srealloc(*bindings, count * sizeof(MYSQL_BIND));

   newBinding = &((*bindings)[count-1]);

   memset(newBinding, 0, sizeof(MYSQL_BIND));

   if (value->getField()->getFormat() == ffAscii || value->getField()->getFormat() == ffText || value->getField()->getFormat() == ffMText)
   {
      newBinding->buffer_type = MYSQL_TYPE_STRING;
      newBinding->buffer = value->getStrValueRef();
      newBinding->buffer_length = value->getField()->getSize();
      newBinding->length = value->getStrValueSizeRef();
      
      newBinding->is_null = value->getNullRef();
      newBinding->error = 0;            // #TODO
   }
   else if (value->getField()->getFormat() == ffMlob)
   {
      newBinding->buffer_type = MYSQL_TYPE_BLOB;
      newBinding->buffer = value->getStrValueRef();
      newBinding->buffer_length = value->getField()->getSize();
      newBinding->length = value->getStrValueSizeRef();
      
      newBinding->is_null = value->getNullRef();
      newBinding->error = 0;            // #TODO
   }
   else if (value->getField()->getFormat() == ffFloat)
   {
      newBinding->buffer_type = MYSQL_TYPE_FLOAT;
      newBinding->buffer = value->getFloatValueRef();
      
      newBinding->length = 0;            // #TODO
      newBinding->is_null =  value->getNullRef();
      newBinding->error = 0;             // #TODO
   }
   else if (value->getField()->getFormat() == ffDateTime)
   {
      newBinding->buffer_type = MYSQL_TYPE_DATETIME;
      newBinding->buffer = value->getTimeValueRef();
      
      newBinding->length = 0;            // #TODO
      newBinding->is_null =  value->getNullRef();
      newBinding->error = 0;             // #TODO
   }
   else if (value->getField()->getFormat() == ffBigInt || value->getField()->getFormat() == ffUBigInt)
   {
      newBinding->buffer_type = MYSQL_TYPE_LONGLONG;
      newBinding->buffer = value->getBigIntValueRef();
      newBinding->is_unsigned = (value->getField()->getFormat() == ffUBigInt);

      newBinding->length = 0;
      newBinding->is_null =  value->getNullRef();
      newBinding->error = 0;             // #TODO
   }
   else
   {
      newBinding->buffer_type = MYSQL_TYPE_LONG;
      newBinding->buffer = value->getIntValueRef();
      newBinding->is_unsigned = (value->getField()->getFormat() == ffUInt);

      newBinding->length = 0;
      newBinding->is_null =  value->getNullRef();
      newBinding->error = 0;             // #TODO
   }

   return success;
}

//***************************************************************************
// Prepare Statement
//***************************************************************************

int cDbStatement::prepare()
{
   if (!stmtTxt.length() || !connection->getMySql())
      return fail;
   
   if (buildErrors)
      return fail;

   stmt = mysql_stmt_init(connection->getMySql());
   
   // prepare statement
   
   if (mysql_stmt_prepare(stmt, stmtTxt.c_str(), stmtTxt.length()))
      return connection->errorSql(connection, "prepare(stmt_prepare)", stmt, stmtTxt.c_str());

   if (outBind)
   {
      if (mysql_stmt_bind_result(stmt, outBind))
         return connection->errorSql(connection, "execute(bind_result)", stmt);
   }

   if (inBind)
   {
      if (mysql_stmt_bind_param(stmt, inBind))
         return connection->errorSql(connection, "buildPrimarySelect(bind_param)", stmt);
   }

   tell(2, "Statement '%s' with (%ld) in parameters and (%d) out bindings prepared", 
        stmtTxt.c_str(), mysql_stmt_param_count(stmt), outCount);
   
   return success;
}

//***************************************************************************
// Show Statistic
//***************************************************************************

void cDbStatement::showStat()
{
   if (callsPeriod)
   {
      tell(0, "calls %4ld in %6.2fms; total %4ld [%s]", 
           callsPeriod, duration/1000, callsTotal, stmtTxt.c_str());

      callsPeriod = 0;
      duration = 0;
   }
}

//***************************************************************************
// Class cDbTable
//***************************************************************************

char* cDbConnection::confPath = 0;
char* cDbConnection::encoding = 0;
char* cDbConnection::dbHost = strdup("localhost");
int   cDbConnection::dbPort = 3306;
char* cDbConnection::dbUser = 0;
char* cDbConnection::dbPass = 0;
char* cDbConnection::dbName = 0;
Sem*  cDbConnection::sem = 0;

//***************************************************************************
// Object
//***************************************************************************

cDbTable::cDbTable(cDbConnection* aConnection, const char* name)
{
   connection = aConnection;
   holdInMemory = no;
   attached = no;

   row = 0;
   stmtSelect = 0;
   stmtInsert = 0;
   stmtUpdate = 0;
   lastInsertId = na;

   tableDef = dbDict.getTable(name);
   
   if (tableDef)
      row = new cDbRow(tableDef);
   else
      tell(0, "Fatal: Table '%s' missing in dictionary '%s'!", name, dbDict.getPath());
}

cDbTable::~cDbTable()
{
   close();

   delete row;
}

//***************************************************************************
// Open / Close
//***************************************************************************

int cDbTable::open(int allowAlter)
{
   if (!tableDef || !row)
      return abrt;

   if (attach() != success)
   {
      tell(0, "Could not access database '%s:%d' (tried to open %s)", 
           connection->getHost(), connection->getPort(), TableName());

      return fail;
   }
   
   return init(allowAlter);
}

int cDbTable::close()
{
   if (stmtSelect) { delete stmtSelect; stmtSelect = 0; }
   if (stmtInsert) { delete stmtInsert; stmtInsert = 0; }
   if (stmtUpdate) { delete stmtUpdate; stmtUpdate = 0; }

   detach();

   return success;
}

//***************************************************************************
// Attach / Detach
//***************************************************************************

int cDbTable::attach()
{
   if (isAttached())
      return success;

   if (connection->attachConnection() != success)
   {
      tell(0, "Could not access database '%s:%d'",
           connection->getHost(), connection->getPort());

      return fail;
   }

   attached = yes;

   return success;
}

int cDbTable::detach()
{
   if (isAttached())
   {
      connection->detachConnection();
      attached = no;
   }

   return success;
}

//***************************************************************************
// Init
//***************************************************************************

int cDbTable::init(int allowAlter)
{
   string str;
   std::map<std::string, cDbFieldDef*>::iterator f;
   int n = 0;

   if (!isConnected()) 
      return fail;

   // check/create table ...

   if (exist() && allowAlter)
      validateStructure();

   if (createTable() != success)
      return fail;

   // check/create indices

   createIndices();

   // ------------------------------
   // prepare BASIC statements
   // ------------------------------

   // select by primary key ...

   stmtSelect = new cDbStatement(this);
   
   stmtSelect->build("select ");

   n = 0;

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
      stmtSelect->bind(f->second, bndOut, n++ ? ", " : "");

   stmtSelect->build(" from %s where ", TableName());

   n = 0;

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      if (!(f->second->getType() & ftPrimary))
         continue;
      
      stmtSelect->bind(f->second, bndIn | bndSet, n++ ? " and " : "");
   }
   
   stmtSelect->build(";");
 
   if (stmtSelect->prepare() != success)
      return fail;

   // -----------------------------------------
   // insert 

   stmtInsert = new cDbStatement(this);

   stmtInsert->build("insert into %s set ", TableName());

   n = 0;

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      // don't insert autoinc and calculated fields

      if (f->second->getType() & ftAutoinc)
         continue;

      stmtInsert->bind(f->second, bndIn | bndSet, n++ ? ", " : "");
   }

   stmtInsert->build(";");

   if (stmtInsert->prepare() != success)
      return fail;

   // -----------------------------------------
   // update via primary key ...

   stmtUpdate = new cDbStatement(this);

   stmtUpdate->build("update %s set ", TableName());
         
   n = 0;

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      // don't update PKey, autoinc and not used fields

      if (f->second->getType() & ftPrimary || 
          f->second->getType() & ftAutoinc)
         continue;
      
      if (strcasecmp(f->second->getName(), "inssp") == 0)  // don't update the insert stamp
         continue;
      
      stmtUpdate->bind(f->second, bndIn | bndSet, n++ ? ", " : "");
   }

   stmtUpdate->build(" where ");

   n = 0;

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      if (!(f->second->getType() & ftPrimary))
         continue;

      stmtUpdate->bind(f->second, bndIn | bndSet, n++ ? " and " : "");
   }

   stmtUpdate->build(";");

   if (stmtUpdate->prepare() != success)
      return fail;

   return success;
}

//***************************************************************************
// Check Table 
//***************************************************************************

int cDbTable::exist(const char* name)
{
   if (isEmpty(name))
      name = TableName();

   MYSQL_RES* result = mysql_list_tables(connection->getMySql(), name);
   MYSQL_ROW tabRow = mysql_fetch_row(result);
   mysql_free_result(result);

   return tabRow ? yes : no;
}

//***************************************************************************
// Validate Structure
//***************************************************************************

struct FieldInfo
{
   string columnFormat;
   string description;
};

int cDbTable::validateStructure()
{
   map<string, FieldInfo, _casecmp_> fields;
   MYSQL_RES* result;
   MYSQL_ROW row;
   std::map<std::string, cDbFieldDef*>::iterator f;

   const char* select = "select column_name, column_type, column_comment, data_type, is_nullable, "
      " character_maximum_length, column_default, numeric_precision "
      " from information_schema.columns "
      " where table_name = '%s' and table_schema= '%s'";

   if (attach() != success)
      return fail;
   
   // ------------------------
   // execute query
   
   if (connection->query(select, TableName(), connection->getName()) != success)
   {
      connection->errorSql(getConnection(), "validateStructure()", 0);
      return fail;
   }

   // ------------------------
   // process the result
   
   if (!(result = mysql_store_result(connection->getMySql())))
   {
      connection->errorSql(getConnection(), "validateStructure()");
      return fail;
   }

   while ((row = mysql_fetch_row(result)))
   {
      fields[row[0]].columnFormat = row[1];
      fields[row[0]].description = row[2];
   }
   
   mysql_free_result(result);

   // ------------------------
   // validate 

   for (int i = 0; i < fieldCount(); i++)
   {
      char colType[100];

      tell(4, "Check field '%s'", getField(i)->getName());

      if (fields.find(getField(i)->getDbName()) == fields.end())
         alterAddField(getField(i));
      
      else
      {
         FieldInfo* fieldInfo = &fields[getField(i)->getDbName()];

         getField(i)->toColumnFormat(colType);

         if (strcasecmp(fieldInfo->columnFormat.c_str(), colType) != 0 ||
             strcasecmp(fieldInfo->description.c_str(), getField(i)->getDescription()) != 0)
         {
            alterModifyField(getField(i));
         }
      }
   }

   return success;
}

//***************************************************************************
// Alter 'Modify Field'
//***************************************************************************

int cDbTable::alterModifyField(cDbFieldDef* def)
{
   char* statement;
   char colType[100];

   tell(0, "  Info: Definition of field '%s.%s' modified, try to alter table", 
        TableName(), def->getName());

   // alter table events modify column guest varchar(50)

   asprintf(&statement, "alter table %s modify column %s %s comment '%s'", 
            TableName(), 
            def->getDbName(), 
            def->toColumnFormat(colType), 
            def->getDbDescription());

   tell(1, "%s", statement);

   if (connection->query(statement))
      return connection->errorSql(getConnection(), "alterAddField()", 
                                  0, statement);

   free(statement);

   return done;
}

//***************************************************************************
// Alter 'Add Field'
//***************************************************************************

int cDbTable::alterAddField(cDbFieldDef* def)
{
   string statement;
   char colType[100];

   tell(0, "Info: Missing field '%s.%s', try to alter table", 
        TableName(), def->getName());

   // alter table channelmap add column ord int(11) [after source]

   statement = string("alter table ") + TableName() + string(" add column ")
      + def->getDbName() + string(" ") + def->toColumnFormat(colType);

   if (def->getFormat() != ffMlob)
   {
      if (def->getType() & ftAutoinc)
         statement += " not null auto_increment";
      else if (def->getType() & ftDef0)
         statement += " default '0'";
   }
   
   if (!isEmpty(def->getDbDescription()))
      statement += string(" comment '") + def->getDbDescription() + string("'");
   
   if (def->getIndex() > 0)
      statement += string(" after ") + getField(def->getIndex()-1)->getDbName();

   tell(1, "%s", statement.c_str());

   if (connection->query(statement.c_str()))
      return connection->errorSql(getConnection(), "alterAddField()", 
                                  0, statement.c_str());

   return done;
}

//***************************************************************************
// Create Table
//***************************************************************************

int cDbTable::createTable()
{
   string statement;
   string aKey;

   if (!tableDef || !row)
      return abrt;

   if (attach() != success)
      return fail;

   // table exists -> nothing to do

   if (exist())
      return done;

   tell(0, "Initialy creating table '%s'", TableName());

   // build 'create' statement ...

   statement = string("create table ") + TableName() + string("(");

   for (int i = 0; i < fieldCount(); i++)
   {
      char colType[100];

      if (i) statement += string(", ");

      statement += string(getField(i)->getDbName()) + " " + string(getField(i)->toColumnFormat(colType));

      if (getField(i)->getFormat() != ffMlob)
      {
         if (getField(i)->getType() & ftAutoinc)
            statement += " not null auto_increment";
         else if (getField(i)->getType() & ftDef0)
            statement += " default '0'";
      }

      if (!isEmpty(getField(i)->getDbDescription()))
         statement += string(" comment '") + getField(i)->getDbDescription() + string("'");
   }

   aKey = "";

   for (int i = 0, n = 0; i < fieldCount(); i++)
   {
      if (getField(i)->getType() & ftPrimary)
      {
         if (n++) aKey += string(", ");
         aKey += string(getField(i)->getDbName()) + " DESC";
      }
   }

   if (aKey.length())
   {
      statement += string(", PRIMARY KEY(");
      statement += aKey;
      statement += ")";
   }

   aKey = "";

   for (int i = 0, n = 0; i < fieldCount(); i++)
   {
      if (getField(i)->getType() & ftAutoinc && !(getField(i)->getType() & ftPrimary))
      {
         if (n++) aKey += string(", ");
         aKey += string(getField(i)->getDbName()) + " DESC";
      }
   }

   if (aKey.length())
   {
      statement += string(", KEY(");
      statement += aKey;
      statement += ")";
   }

   // statement += string(") ENGINE MYISAM;");
   statement += string(") ENGINE InnoDB;");

   tell(1, "%s", statement.c_str());

   if (connection->query(statement.c_str()))
      return connection->errorSql(getConnection(), "createTable()", 
                                  0, statement.c_str());

   return success;
}

//***************************************************************************
// Create Indices
//***************************************************************************

int cDbTable::createIndices()
{
   string statement;

   tell(5, "Initialy checking indices for '%s'", TableName());

   // check/create indexes

   for (int i = 0; i < tableDef->indexCount(); i++)
   {
      cDbIndexDef* index = tableDef->getIndex(i);
      int fCount;
      string idxName;

      if (!index->fieldCount())
         continue;
         
      // check

      idxName = "idx" + string(index->getName());

      checkIndex(idxName.c_str(), fCount);

      if (fCount != index->fieldCount())
      {
         // create index
            
         statement = "create index " + idxName;
         statement += " on " + string(TableName()) + "(";
            
         int n = 0;
            
         for (int f = 0; f < index->fieldCount(); f++)
         {              
            cDbFieldDef* fld = index->getField(f);
               
            if (fld)
            {
               if (n++) statement += string(", ");
               statement += fld->getDbName();
            }
         }
            
         if (!n) continue;
            
         statement += ");";
         tell(1, "%s", statement.c_str());
            
         if (connection->query(statement.c_str()))
            return connection->errorSql(getConnection(), "createIndices()", 
                                        0, statement.c_str());
      }
   }

   return success;
}

//***************************************************************************
// Check Index
//***************************************************************************

int cDbTable::checkIndex(const char* idxName, int& fieldCount)
{
   enum IndexQueryFields
   {
      idTable,
      idNonUnique,
      idKeyName,
      idSeqInIndex,
      idColumnName,
      idCollation,
      idCardinality,
      idSubPart,
      idPacked,
      idNull,
      idIndexType,
      idComment,
      idIndexComment,
      
      idCount
   };

   MYSQL_RES* result;
   MYSQL_ROW row;

   fieldCount = 0;

   if (connection->query("show index from %s", TableName()) != success)
   {
      connection->errorSql(getConnection(), "checkIndex()", 0);

      return fail;
   }

   if ((result = mysql_store_result(connection->getMySql())))
   {
      while ((row = mysql_fetch_row(result)))
      {
         tell(5, "%s:  %-20s %s %s", 
              row[idTable], row[idKeyName],
              row[idSeqInIndex], row[idColumnName]);

         if (strcasecmp(row[idKeyName], idxName) == 0)
            fieldCount++;
      }
      
      mysql_free_result(result);

      return success;
   }

   connection->errorSql(getConnection(), "checkIndex()");

   return fail;
}

//***************************************************************************
// SQL Error 
//***************************************************************************

int cDbConnection::errorSql(cDbConnection* connection, const char* prefix, MYSQL_STMT* stmt, const char* stmtTxt)
{
   if (!connection || !connection->mysql)
   {
      tell(0, "SQL-Error in '%s'", prefix);
      return fail;
   }

   int error = mysql_errno(connection->mysql);
   char* conErr = 0;
   char* stmtErr = 0;

   if (error == CR_SERVER_LOST ||
       error == CR_SERVER_GONE_ERROR ||
       error == CR_INVALID_CONN_HANDLE ||
       error == CR_COMMANDS_OUT_OF_SYNC ||
       error == CR_SERVER_LOST_EXTENDED ||
       error == CR_STMT_CLOSED ||
       error == CR_CONN_UNKNOW_PROTOCOL ||
       error == CR_UNSUPPORTED_PARAM_TYPE ||
       error == CR_NO_PREPARE_STMT ||
       error == CR_SERVER_HANDSHAKE_ERR ||
       error == CR_WRONG_HOST_INFO ||
       error == CR_OUT_OF_MEMORY ||
       error == CR_IPSOCK_ERROR ||
       error == CR_SOCKET_CREATE_ERROR ||
       error == CR_CONNECTION_ERROR ||
       error == CR_TCP_CONNECTION ||
       error == CR_PARAMS_NOT_BOUND ||
       error == CR_CONN_HOST_ERROR ||
       error == CR_SSL_CONNECTION_ERROR
       
       // to be continued - not all errors should result in a reconnect ...

      )
   {
      connectDropped = yes;
   }

   if (error)
      asprintf(&conErr, "%s (%d) ", mysql_error(connection->mysql), error);

   if (stmt || stmtTxt)
      asprintf(&stmtErr, "'%s' [%s]",
               stmt ? mysql_stmt_error(stmt) : "",
               stmtTxt ? stmtTxt : "");

   tell(0, "SQL-Error in '%s' - %s%s", prefix, 
        conErr ? conErr : "", stmtErr ? stmtErr : "");

   free(conErr);
   free(stmtErr);

   if (connectDropped)
      tell(0, "Fatal, lost connection to mysql server, aborting pending actions");

   return fail;
}

//***************************************************************************
// Delete Where
//***************************************************************************

int cDbTable::deleteWhere(const char* where, ...)
{
   string stmt;
   char* tmp;
   va_list more;

   va_start(more, where);
   vasprintf(&tmp, where, more);

   if (!connection || !connection->getMySql())
      return fail;

   stmt = "delete from " + string(TableName()) + " where " + string(tmp);
   
   free(tmp);

   if (connection->query(stmt.c_str()))
      return connection->errorSql(connection, "deleteWhere()", 0, stmt.c_str());

   return success;
}

//***************************************************************************
// Coiunt Where
//***************************************************************************

int cDbTable::countWhere(const char* where, int& count, const char* what)
{
   string tmp;
   MYSQL_RES* res;
   MYSQL_ROW data;

   count = 0;
   
   if (isEmpty(what))
      what = "count(1)";

   if (!isEmpty(where))
      tmp = "select " + string(what) + " from " + string(TableName()) + " where " + string(where);
   else
      tmp = "select " + string(what) + " from " + string(TableName());
   
   if (connection->query(tmp.c_str()))
      return connection->errorSql(connection, "countWhere()", 0, tmp.c_str());

   if (res = mysql_store_result(connection->getMySql()))
   {
      data = mysql_fetch_row(res);

      if (data)
         count = atoi(data[0]);

      mysql_free_result(res);
   }

   return success;
}

//***************************************************************************
// Truncate
//***************************************************************************

int cDbTable::truncate()
{
   string tmp;

   tmp = "delete from " + string(TableName());

   if (connection->query(tmp.c_str()))
      return connection->errorSql(connection, "truncate()", 0, tmp.c_str());

   return success;
}


//***************************************************************************
// Store
//***************************************************************************

int cDbTable::store()
{
   int found;

   // insert or just update ...

   if (stmtSelect->execute(/*noResult =*/ yes) != success)
   {
      connection->errorSql(connection, "store()");
      return no;
   }

   found = stmtSelect->getAffected() == 1;
   stmtSelect->freeResult();
   
   if (found)
      return update();
   else
      return insert();
}

//***************************************************************************
// Insert
//***************************************************************************

int cDbTable::insert()
{
   std::map<std::string, cDbFieldDef*>::iterator f;
   lastInsertId = na;

   if (!stmtInsert)
   {
      tell(0, "Fatal missing insert statement\n");
      return fail;
   }

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      cDbFieldDef* fld = f->second;

      if (strcasecmp(fld->getName(), "updsp") == 0 || strcasecmp(fld->getName(), "inssp") == 0)
         setValue(fld, time(0));
   }

   if (stmtInsert->execute())
      return fail;

   lastInsertId = stmtInsert->getLastInsertId();

   return stmtInsert->getAffected() == 1 ? success : fail;
}

//***************************************************************************
// Update
//***************************************************************************

int cDbTable::update()
{
   std::map<std::string, cDbFieldDef*>::iterator f;

   if (!stmtUpdate)
   {
      tell(0, "Fatal missing update statement\n");
      return fail;
   }

   for (f = tableDef->dfields.begin(); f != tableDef->dfields.end(); f++)
   {
      cDbFieldDef* fld = f->second;

      if (strcasecmp(fld->getName(), "updsp") == 0)
      {
         setValue(fld, time(0));
         break;
      }
   }

   if (stmtUpdate->execute())
      return fail;

   return stmtUpdate->getAffected() == 1 ? success : fail;
}

//***************************************************************************
// Find
//***************************************************************************

int cDbTable::find()
{
   if (!stmtSelect)
      return no;

   if (stmtSelect->execute() != success)
   {
      connection->errorSql(connection, "find()");
      return no;
   }

   return stmtSelect->getAffected() == 1 ? yes : no;
}

//***************************************************************************
// Find via Statement
//***************************************************************************

int cDbTable::find(cDbStatement* stmt)
{
   if (!stmt)
      return no;

   if (stmt->execute() != success)
   {
      connection->errorSql(connection, "find(stmt)");
      return no;
   }

   return stmt->getAffected() > 0 ? yes : no;
}

//***************************************************************************
// Fetch
//***************************************************************************

int cDbTable::fetch(cDbStatement* stmt)
{
   if (!stmt)
      return no;

   return stmt->fetch();
}

//***************************************************************************
// Reset Fetch
//***************************************************************************

void cDbTable::reset(cDbStatement* stmt)
{
   if (stmt)
      stmt->freeResult();
}
