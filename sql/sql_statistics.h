/* Copyright 2006-2008 MySQL AB, 2008 Sun Microsystems, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_STATISTICS_H
#define SQL_STATISTICS_H

/*
  For COMPLEMENTARY_FOR_QUERIES and PREFERABLY_FOR_QUERIES they are
  similar to the COMPLEMENTARY and PREFERABLY respectively except that
  with these values we would not be collecting EITS for queries like
    ANALYZE TABLE t1;
  To collect EITS with these values, we have to use PERSISITENT FOR
  analyze table t1 persistent for
     columns (col1,col2...) index (idx1, idx2...)
     or
  analyze table t1 persistent for all
*/

typedef
enum enum_use_stat_tables_mode
{
  NEVER,
  COMPLEMENTARY,
  PREFERABLY,
  COMPLEMENTARY_FOR_QUERIES,
  PREFERABLY_FOR_QUERIES
} Use_stat_tables_mode;

typedef
enum enum_histogram_type
{
  SINGLE_PREC_HB,
  DOUBLE_PREC_HB,
  JSON,
  INVALID_HISTOGRAM
} Histogram_type;

enum enum_stat_tables
{
  TABLE_STAT,
  COLUMN_STAT,
  INDEX_STAT,
};


/* 
  These enumeration types comprise the dictionary of three
  statistical tables table_stat, column_stat and index_stat
  as they defined in ../scripts/mysql_system_tables.sql.

  It would be nice if the declarations of these types were
  generated automatically by the table definitions.   
*/

enum enum_table_stat_col
{
  TABLE_STAT_DB_NAME,
  TABLE_STAT_TABLE_NAME,
  TABLE_STAT_CARDINALITY,
  TABLE_STAT_N_FIELDS
};

enum enum_column_stat_col
{
  COLUMN_STAT_DB_NAME,
  COLUMN_STAT_TABLE_NAME,
  COLUMN_STAT_COLUMN_NAME,
  COLUMN_STAT_MIN_VALUE,
  COLUMN_STAT_MAX_VALUE,
  COLUMN_STAT_NULLS_RATIO,
  COLUMN_STAT_AVG_LENGTH,
  COLUMN_STAT_AVG_FREQUENCY,
  COLUMN_STAT_HIST_SIZE,
  COLUMN_STAT_HIST_TYPE,
  COLUMN_STAT_HISTOGRAM,
  COLUMN_STAT_N_FIELDS
};

enum enum_index_stat_col
{
  INDEX_STAT_DB_NAME,
  INDEX_STAT_TABLE_NAME,
  INDEX_STAT_INDEX_NAME,
  INDEX_STAT_PREFIX_ARITY,
  INDEX_STAT_AVG_FREQUENCY,
  INDEX_STAT_N_FIELDS
};

inline
Use_stat_tables_mode get_use_stat_tables_mode(THD *thd)
{ 
  return (Use_stat_tables_mode) (thd->variables.use_stat_tables);
}
inline
bool check_eits_collection_allowed(THD *thd)
{
  return (get_use_stat_tables_mode(thd) == COMPLEMENTARY ||
          get_use_stat_tables_mode(thd) == PREFERABLY);
}

inline
bool check_eits_preferred(THD *thd)
{
  return (get_use_stat_tables_mode(thd) == PREFERABLY ||
          get_use_stat_tables_mode(thd) == PREFERABLY_FOR_QUERIES);
}

int read_statistics_for_tables_if_needed(THD *thd, TABLE_LIST *tables);
int read_statistics_for_tables(THD *thd, TABLE_LIST *tables);
int collect_statistics_for_table(THD *thd, TABLE *table);
void delete_stat_values_for_table_share(TABLE_SHARE *table_share);
int alloc_statistics_for_table(THD *thd, TABLE *table);
int update_statistics_for_table(THD *thd, TABLE *table);
int delete_statistics_for_table(THD *thd, const LEX_CSTRING *db, const LEX_CSTRING *tab);
int delete_statistics_for_column(THD *thd, TABLE *tab, Field *col);
int delete_statistics_for_index(THD *thd, TABLE *tab, KEY *key_info,
                                bool ext_prefixes_only);
int rename_table_in_stat_tables(THD *thd, const LEX_CSTRING *db, const LEX_CSTRING *tab,
                                const LEX_CSTRING *new_db, const LEX_CSTRING *new_tab);
int rename_column_in_stat_tables(THD *thd, TABLE *tab, Field *col,
                                  const char *new_name);
void set_statistics_for_table(THD *thd, TABLE *table);

double get_column_avg_frequency(Field * field);

double get_column_range_cardinality(Field *field,
                                    key_range *min_endp,
                                    key_range *max_endp,
                                    uint range_flag);
bool is_stat_table(const LEX_CSTRING *db, LEX_CSTRING *table);
bool is_eits_usable(Field* field);

/*
  Common base for all histograms
*/
class Histogram_base : public Sql_alloc
{
public:
  virtual bool parse(MEM_ROOT *mem_root, Histogram_type type_arg, 
                     const uchar *ptr, uint size)= 0;
  virtual void serialize(Field *to_field)= 0;

  virtual Histogram_type get_type()=0;

  virtual uint get_width()=0;

  virtual void init_for_collection(MEM_ROOT *mem_root, Histogram_type htype_arg, ulonglong size)=0;

  virtual bool is_available()=0;

  virtual bool is_usable(THD *thd)=0;

  virtual void set_values(uchar * values)=0;

  virtual uchar *get_values()=0;

  virtual void set_size(ulonglong sz)=0;

  virtual double range_selectivity(double min_pos, double max_pos)=0;

  virtual double point_selectivity(double pos, double avg_selection)=0;
  
  // Legacy: return the size of the histogram on disk.
  // This will be stored in mysql.column_stats.hist_size column.
  // Newer, JSON-based histograms may return 0.
  virtual uint get_size()=0;

  virtual ~Histogram_base(){}
};

class Histogram_binary : public Histogram_base
{
public:
  bool parse(MEM_ROOT *mem_root, Histogram_type type_arg, 
             const uchar *ptr_arg, uint size_arg) override;
  void serialize(Field *to_field) override;

  Histogram_type get_type() override { return type; }

  uint get_size() override { return (uint) size; }

  uint get_width() override
  {
    switch (type) {
    case SINGLE_PREC_HB:
      return size;
    case DOUBLE_PREC_HB:
      return size / 2;
    default:
      DBUG_ASSERT(0);
    }
    return 0;
  }

private:
  Histogram_type type;
  uint8 size; /* Size of values array, in bytes */
  uchar *values;

  uint prec_factor()
  {
    switch (type) {
    case SINGLE_PREC_HB:
      return ((uint) (1 << 8) - 1);
    case DOUBLE_PREC_HB:
      return ((uint) (1 << 16) - 1);
    default:
      DBUG_ASSERT(0);
    }
    return 1;
  }

  uint get_value(uint i)
  {
    DBUG_ASSERT(i < get_width());
    switch (type) {
    case SINGLE_PREC_HB:
      return (uint) (((uint8 *) values)[i]);
    case DOUBLE_PREC_HB:
      return (uint) uint2korr(values + i * 2);
    default:
      DBUG_ASSERT(0);
    }
    return 0;
  }

  /* Find the bucket which value 'pos' falls into. */
  uint find_bucket(double pos, bool first)
  {
    uint val= (uint) (pos * prec_factor());
    int lp= 0;
    int rp= get_width() - 1;
    int d= get_width() / 2;
    uint i= lp + d;
    for ( ; d;  d= (rp - lp) / 2, i= lp + d)
    {
      if (val == get_value(i))
	break; 
      if (val < get_value(i))
        rp= i;
      else if (val > get_value(i + 1))
        lp= i + 1;
      else
        break;
    }

    if (val > get_value(i) && i < (get_width() - 1))
      i++;

    if (val == get_value(i))
    {
      if (first)
      {
        while(i && val == get_value(i - 1))
          i--;
      }
      else
      {
        while(i + 1 < get_width() && val == get_value(i + 1))
          i++;
      }
    }
    return i;
  }

  uchar *get_values() override { return (uchar *) values; }
public:
  void init_for_collection(MEM_ROOT *mem_root, Histogram_type htype_arg, ulonglong size) override;

  // Note: these two are used only for saving the JSON text:
  void set_values (uchar *vals) override { values= (uchar *) vals; }
  void set_size (ulonglong sz) override { size= (uint8) sz; }

  bool is_available() override { return get_size() > 0 && get_values(); }

  /*
    This function checks that histograms should be usable only when
      1) the level of optimizer_use_condition_selectivity > 3
      2) histograms have been collected
  */
  bool is_usable(THD *thd) override
  {
    return thd->variables.optimizer_use_condition_selectivity > 3 &&
           is_available();
  }

  void set_value(uint i, double val)
  {
    switch (type) {
    case SINGLE_PREC_HB:
      ((uint8 *) values)[i]= (uint8) (val * prec_factor());
      return;
    case DOUBLE_PREC_HB:
      int2store(values + i * 2, val * prec_factor());
      return;
    default:
      DBUG_ASSERT(0);
      return;
    }
  }

  void set_prev_value(uint i)
  {
    switch (type) {
    case SINGLE_PREC_HB:
      ((uint8 *) values)[i]= ((uint8 *) values)[i-1];
      return;
    case DOUBLE_PREC_HB:
      int2store(values + i * 2, uint2korr(values + i * 2 - 2));
      return;
    default:
      DBUG_ASSERT(0);
      return;
    }
  }

  double range_selectivity(double min_pos, double max_pos) override
  {
    double sel;
    double bucket_sel= 1.0/(get_width() + 1);  
    uint min= find_bucket(min_pos, TRUE);
    uint max= find_bucket(max_pos, FALSE);
    sel= bucket_sel * (max - min + 1);
    return sel;
  } 
  
  /*
    Estimate selectivity of "col=const" using a histogram
  */
  double point_selectivity(double pos, double avg_sel) override;
};

class Histogram_json : public Histogram_base
{
private:
  Histogram_type type;
  uint8 size; /* Number of elements in the histogram*/
  uchar *values;

public:
  bool parse(MEM_ROOT *mem_root, Histogram_type type_arg, const uchar *ptr, uint size) override {return false;}

  void serialize(Field *to_field) override{}

  uint get_size() override {return (uint) size;}

  // returns number of buckets in the histogram
  uint get_width() override
  {
      return size;
  };

  Histogram_type get_type() override
  {
    return JSON;
  }

  void set_size (ulonglong sz) override {size = (uint8) sz; }

  void init_for_collection(MEM_ROOT *mem_root, Histogram_type htype_arg, ulonglong size) override;

  bool is_available() override {return get_size() > 0 && get_values(); }

  bool is_usable(THD *thd) override
  {
    return thd->variables.optimizer_use_condition_selectivity > 3 &&
           is_available();
  }

  void set_values (uchar *vals) override { values= (uchar *) vals; }

  uchar *get_values() override { return (uchar *) values; }

  double range_selectivity(double min_pos, double max_pos) override {return 0.1;}

  double point_selectivity(double pos, double avg_selection) override {return 0.5;}
};

class Columns_statistics;
class Index_statistics;

/* Statistical data on a table */

class Table_statistics
{

public:
  my_bool cardinality_is_null;      /* TRUE if the cardinality is unknown */
  ha_rows cardinality;              /* Number of rows in the table        */
  uchar *min_max_record_buffers;    /* Record buffers for min/max values  */
  Column_statistics *column_stats;  /* Array of statistical data for columns */
  Index_statistics *index_stats;    /* Array of statistical data for indexes */

  /* Array of records per key for index prefixes */
  ulonglong *idx_avg_frequency;
  //uchar *histograms;                /* Sequence of histograms       */
};


/* 
  Statistical data on a column 

  Note: objects of this class may be "empty", where they have almost all fields
  as zeros, for example, get_avg_frequency() will return 0.

  objects are allocated in alloc_statistics_for_table[_share].
*/

class Column_statistics
{

private:
  static const uint Scale_factor_nulls_ratio= 100000;
  static const uint Scale_factor_avg_length= 100000;
  static const uint Scale_factor_avg_frequency= 100000;

public:
  /* 
    Bitmap indicating  what statistical characteristics
    are available for the column
  */
  uint32 column_stat_nulls;
  
  /* For the below two, see comments in get_column_range_cardinality() */
  /* Minimum value for the column */
  Field *min_value; 
  /* Maximum value for the column */   
  Field *max_value;

private:

  /* 
    The ratio Z/N multiplied by the scale factor Scale_factor_nulls_ratio,
    where 
      N is the total number of rows,
      Z is the number of nulls in the column
  */
  ulong nulls_ratio;
 
  /*
    Average number of bytes occupied by the representation of a
    value of the column in memory buffers such as join buffer
    multiplied by the scale factor Scale_factor_avg_length.
    CHAR values are stripped of trailing spaces.
    Flexible values are stripped of their length prefixes.
  */
  ulonglong avg_length;

  /*
    The ratio N/D multiplied by the scale factor Scale_factor_avg_frequency,
    where
       N is the number of rows with not null value in the column,
       D the number of distinct values among them
  */
  ulonglong avg_frequency;

public:

  Histogram_type histogram_type_on_disk;
  Histogram_base *histogram_;

  uint32 no_values_provided_bitmap()
  {
    return
     ((1 << (COLUMN_STAT_HISTOGRAM-COLUMN_STAT_COLUMN_NAME))-1) <<
      (COLUMN_STAT_COLUMN_NAME+1);
  }
 
  void set_all_nulls()
  {
    column_stat_nulls= no_values_provided_bitmap();
  }

  void set_not_null(uint stat_field_no)
  {
    column_stat_nulls&= ~(1 << stat_field_no);
  }

  bool is_null(uint stat_field_no)
  {
    return MY_TEST(column_stat_nulls & (1 << stat_field_no));
  }

  double get_nulls_ratio()
  {
    return (double) nulls_ratio /  Scale_factor_nulls_ratio;
  }

  double get_avg_length()
  {
    return (double) avg_length / Scale_factor_avg_length;
  }

  double get_avg_frequency()
  {
    return (double) avg_frequency / Scale_factor_avg_frequency;
  }

  void set_nulls_ratio (double val)
  {
    nulls_ratio= (ulong) (val * Scale_factor_nulls_ratio);
  }

  void set_avg_length (double val)
  {
    avg_length= (ulonglong) (val * Scale_factor_avg_length);
  }

  void set_avg_frequency (double val)
  {
    avg_frequency= (ulonglong) (val * Scale_factor_avg_frequency);
  }

  bool min_max_values_are_provided()
  {
    return !is_null(COLUMN_STAT_MIN_VALUE) && 
      !is_null(COLUMN_STAT_MAX_VALUE);
  }
  /*
    This function checks whether the values for the fields of the statistical
    tables that were NULL by DEFAULT for a column have changed or not.

    @retval
    TRUE: Statistics are not present for a column
    FALSE: Statisitics are present for a column
  */
  bool no_stat_values_provided()
  {
    if (column_stat_nulls == no_values_provided_bitmap())
      return true;
    return false;
  }
};


/* Statistical data on an index prefixes */

class Index_statistics
{

private:
  static const uint Scale_factor_avg_frequency= 100000;
  /*
    The k-th element of this array contains the ratio N/D
    multiplied by the scale factor Scale_factor_avg_frequency, 
    where N is the number of index entries without nulls 
    in the first k components, and D is the number of distinct
    k-component prefixes among them 
  */
  ulonglong *avg_frequency;

public:

  void init_avg_frequency(ulonglong *ptr) { avg_frequency= ptr; }

  bool avg_frequency_is_inited() { return avg_frequency != NULL; }

  double get_avg_frequency(uint i)
  {
    return (double) avg_frequency[i] / Scale_factor_avg_frequency;
  }

  void set_avg_frequency(uint i, double val)
  {
    avg_frequency[i]= (ulonglong) (val * Scale_factor_avg_frequency);
  }

};

#endif /* SQL_STATISTICS_H */
