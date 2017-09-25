//------------------------------------------------------------------------------
//  Copyright 2017 H2O.ai
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//------------------------------------------------------------------------------
#include <new>          // placement new
#include <stdexcept>    // std::runtime_error
#include <errno.h>      // errno
#include <fcntl.h>      // open
#include <stdint.h>     // int32_t, etc
#include <string.h>     // strerror
#include <sys/mman.h>   // mmap
#include "column.h"
#include "csv.h"
#include "datatable.h"
#include "memorybuf.h"
#include "myomp.h"
#include "types.h"
#include "utils.h"


class CsvColumn;
inline static void write_int32(char **pch, int32_t value);
static void write_string(char **pch, const char *value);

typedef void (*writer_fn)(char **pch, CsvColumn* col, int64_t row);

static const int64_t max_chunk_size = 1024 * 1024;
static const int64_t min_chunk_size = 1024;

static int64_t bytes_per_stype[DT_STYPES_COUNT];
static writer_fn writers_per_stype[DT_STYPES_COUNT];

// Helper lookup table for writing integers
static const int32_t DIVS32[10] = {
  1,
  10,
  100,
  1000,
  10000,
  100000,
  1000000,
  10000000,
  100000000,
  1000000000,
};

static const int64_t DIVS64[19] = {
  1L,
  10L,
  100L,
  1000L,
  10000L,
  100000L,
  1000000L,
  10000000L,
  100000000L,
  1000000000L,
  10000000000L,
  100000000000L,
  1000000000000L,
  10000000000000L,
  100000000000000L,
  1000000000000000L,
  10000000000000000L,
  100000000000000000L,
  1000000000000000000L,
};

// TODO: replace with classes that derive from CsvColumn and implement write()
class CsvColumn {
public:
  void *data;
  char *strbuf;
  writer_fn writer;

  CsvColumn(Column *col) {
    data = col->data;
    strbuf = NULL;
    writer = writers_per_stype[col->stype];
    if (!writer) throw std::runtime_error("Cannot write this type");
    if (col->stype == ST_STRING_I4_VCHAR) {
      strbuf = reinterpret_cast<char*>(data) - 1;
      data = strbuf + 1 + ((VarcharMeta*)col->meta)->offoff;
    }
  }

  void write(char **pch, int64_t row) {
    writer(pch, this, row);
  }
};

#define VLOG(...)  do { if (args->verbose) log_message(args->logger, __VA_ARGS__); } while (0)


//=================================================================================================
// Field writers
//
// Note: we attempt to optimize these functions for speed. See /microbench/writecsv for various
// experiments and benchmarks.
//=================================================================================================

static void write_b1(char **pch, CsvColumn *col, int64_t row)
{
  int8_t value = reinterpret_cast<int8_t*>(col->data)[row];
  **pch = static_cast<char>(value) + '0';
  *pch += (value != NA_I1);
}


static void write_i1(char **pch, CsvColumn *col, int64_t row)
{
  int value = static_cast<int>(reinterpret_cast<int8_t*>(col->data)[row]);
  char *ch = *pch;
  if (value == NA_I1) return;
  if (value < 0) {
    *ch++ = '-';
    value = -value;
  }
  if (value >= 100) {  // the range of `value` is up to 127
    *ch++ = '1';
    int d = value/10;
    *ch++ = static_cast<char>(d) - 10 + '0';
    value -= d*10;
  } else if (value >= 10) {
    int d = value/10;
    *ch++ = static_cast<char>(d) + '0';
    value -= d*10;
  }
  *ch++ = static_cast<char>(value) + '0';
  *pch = ch;
}


static void write_i2(char **pch, CsvColumn *col, int64_t row)
{
  int value = ((int16_t*) col->data)[row];
  if (value == 0) {
    *((*pch)++) = '0';
    return;
  }
  if (value == NA_I2) return;
  char *ch = *pch;
  if (value < 0) {
    *ch++ = '-';
    value = -value;
  }
  int r = (value < 1000)? 2 : 4;
  for (; value < DIVS32[r]; r--);
  for (; r; r--) {
    int d = value / DIVS32[r];
    *ch++ = static_cast<char>(d) + '0';
    value -= d * DIVS32[r];
  }
  *ch = static_cast<char>(value) + '0';
  *pch = ch + 1;
}


static void write_i4(char **pch, CsvColumn *col, int64_t row)
{
  int32_t value = ((int32_t*) col->data)[row];
  if (value == 0) {
    *((*pch)++) = '0';
    return;
  }
  if (value == NA_I4) return;
  char *ch = *pch;
  if (value < 0) {
    *ch++ = '-';
    value = -value;
  }
  int r = (value < 100000)? 4 : 9;
  for (; value < DIVS32[r]; r--);
  for (; r; r--) {
    int d = value / DIVS32[r];
    *ch++ = static_cast<char>(d) + '0';
    value -= d * DIVS32[r];
  }
  *ch = static_cast<char>(value) + '0';
  *pch = ch + 1;
}


static void write_i8(char **pch, CsvColumn *col, int64_t row)
{
  int64_t value = ((int64_t*) col->data)[row];
  if (value == 0) {
    *((*pch)++) = '0';
    return;
  }
  if (value == NA_I8) return;
  char *ch = *pch;
  if (value < 0) {
    *ch++ = '-';
    value = -value;
  }
  int r = (value < 10000000)? 6 : 18;
  for (; value < DIVS64[r]; r--);
  for (; r; r--) {
    int64_t d = value / DIVS64[r];
    *ch++ = static_cast<char>(d) + '0';
    value -= d * DIVS64[r];
  }
  *ch = static_cast<char>(value) + '0';
  *pch = ch + 1;
}


static void write_s4(char **pch, CsvColumn *col, int64_t row)
{
  int32_t offset1 = ((int32_t*) col->data)[row];
  int32_t offset0 = abs(((int32_t*) col->data)[row - 1]);
  char *ch = *pch;

  if (offset1 < 0) return;
  if (offset0 == offset1) {
    ch[0] = '"';
    ch[1] = '"';
    *pch = ch + 2;
    return;
  }
  const char *strstart = col->strbuf + offset0;
  const char *strend = col->strbuf + offset1;
  const char *sch = strstart;
  while (sch < strend) {  // ',' is 44, '"' is 34
    char c = *sch;
    if ((uint8_t)c <= (uint8_t)',' && (c == ',' || c == '"' || (uint8_t)c < 32)) break;
    *ch++ = c;
    sch++;
  }
  if (sch < strend) {
    ch = *pch;
    memcpy(ch+1, strstart, static_cast<size_t>(sch - strstart));
    *ch = '"';
    ch += sch - strstart + 1;
    while (sch < strend) {
      if (*sch == '"') *ch++ = '"';  // double the quote
      *ch++ = *sch++;
    }
    *ch++ = '"';
  }
  *pch = ch;
}


static char hexdigits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
static void write_f8_hex(char **pch, CsvColumn *col, int64_t row)
{
  // Read the value as if it was uint64_t
  uint64_t value = ((uint64_t*) col->data)[row];
  char *ch = *pch;

  int exp = (int)(value >> 52);
  uint64_t sig = (value & 0xFFFFFFFFFFFFF);
  if (exp & 0x800) {
    *ch++ = '-';
    exp ^= 0x800;
  }
  if (exp == 0x7FF) {  // nan & inf
    if (sig == 0) {  // - sign was already printed, if any
      ch[0] = 'i'; ch[1] = 'n'; ch[2] = 'f';
    } else {
      ch[0] = 'n'; ch[1] = 'a'; ch[2] = 'n';
    }
    *pch = ch + 3;
    return;
  }
  ch[0] = '0';
  ch[1] = 'x';
  ch[2] = '0' + (exp != 0x000);
  ch[3] = '.';
  ch += 3 + (sig != 0);
  while (sig) {
    uint64_t r = sig & 0xF000000000000;
    *ch++ = hexdigits[r >> 48];
    sig = (sig ^ r) << 4;
  }
  if (exp) exp -= 0x3FF;
  *ch++ = 'p';
  *ch++ = '+' + (exp < 0)*('-' - '+');
  write_int32(&ch, abs(exp));
  *pch = ch;
}


static void write_f4_hex(char **pch, CsvColumn *col, int64_t row)
{
  // Read the value as if it was uint32_t
  uint32_t value = static_cast<uint32_t*>(col->data)[row];
  char *ch = *pch;

  int exp = static_cast<int>(value >> 23);
  uint32_t sig = (value & 0x7FFFFF);
  if (exp & 0x100) {
    *ch++ = '-';
    exp ^= 0x100;
  }
  if (exp == 0xFF) {  // nan & inf
    if (sig == 0) {  // - sign was already printed, if any
      ch[0] = 'i'; ch[1] = 'n'; ch[2] = 'f';
    } else {
      ch[0] = 'n'; ch[1] = 'a'; ch[2] = 'n';
    }
    *pch = ch + 3;
    return;
  }
  ch[0] = '0';
  ch[1] = 'x';
  ch[2] = '0' + (exp != 0x00);
  ch[3] = '.';
  ch += 3 + (sig != 0);
  while (sig) {
    uint32_t r = sig & 0x780000;
    *ch++ = hexdigits[r >> 19];
    sig = (sig ^ r) << 4;
  }
  if (exp) exp -= 0x7F;
  *ch++ = 'p';
  *ch++ = '+' + (exp < 0)*('-' - '+');
  write_int32(&ch, abs(exp));
  *pch = ch;
}



/**
 * This function writes a plain 0-terminated C string. This is not a regular
 * "writer" -- instead it may be used to write extra data to the file, such
 * as headers/footers.
 */
static void write_string(char **pch, const char *value)
{
  char *ch = *pch;
  const char *sch = value;
  for (;;) {
    char c = *sch++;
    if (!c) { *pch = ch; return; }
    if (c == '"' || c == ',' || static_cast<uint8_t>(c) < 32) break;
    *ch++ = c;
  }
  // If we broke out of the loop above, it means we need to quote the field.
  // So, first rewind to the beginning
  ch = *pch;
  sch = value;
  *ch++ = '"';
  for (;;) {
    char c = *sch++;
    if (!c) break;
    if (c == '"') *ch++ = '"';  // double the quote
    *ch++ = c;
  }
  *ch++ = '"';
  *pch = ch;
}

inline static void write_int32(char **pch, int32_t value)
{
  if (value == 0) {
    *((*pch)++) = '0';
    return;
  }
  char *ch = *pch;
  if (value < 0) {
    if (value == NA_I4) return;
    *ch++ = '-';
    value = -value;
  }
  int r = (value < 100000)? 4 : 9;
  for (; value < DIVS32[r]; r--);
  for (; r; r--) {
    int d = value / DIVS32[r];
    *ch++ = static_cast<char>(d) + '0';
    value -= d * DIVS32[r];
  }
  *ch = static_cast<char>(value) + '0';
  *pch = ch + 1;
}



//=================================================================================================
//
// Main CSV-writing function
//
//=================================================================================================
MemoryBuffer* csv_write(CsvWriteParameters *args)
{
  // Fetch arguments
  DataTable *dt = args->dt;
  int nthreads = args->nthreads;

  // First, estimate the size of the output CSV file
  // The size of string columns is estimated liberally, assuming it may
  // get inflated by no more than 20% (+2 chars for the quotes). If the data
  // contains many quotes, they may inflate more than this.
  // The size of numeric columns is estimated conservatively: we compute the
  // maximum amount of space that is theoretically required.
  // Overall, we will probably overestimate the final size of the CSV by a big
  // margin.
  double t0 = wallclock();
  int64_t nrows = dt->nrows;
  int64_t ncols = dt->ncols;
  int64_t bytes_total = 0;
  for (int64_t i = 0; i < ncols; i++) {
    Column *col = dt->columns[i];
    SType stype = col->stype;
    if (stype == ST_STRING_I4_VCHAR) {
      bytes_total += (int64_t)(1.2 * column_i4s_datasize(col)) + 2 * nrows;
    } else if (stype == ST_STRING_I8_VCHAR) {
      bytes_total += (int64_t)(1.2 * column_i8s_datasize(col)) + 2 * nrows;
    } else {
      bytes_total += bytes_per_stype[stype] * nrows;
    }
  }
  bytes_total += ncols * nrows;  // Account for separators / newlines
  double bytes_per_row = nrows? static_cast<double>(bytes_total / nrows) : 0;
  VLOG("Estimated file size to be no more than %lldB\n", bytes_total);
  double t1 = wallclock();

  // Create the target memory region
  MemoryBuffer *mb = NULL;
  size_t allocsize = static_cast<size_t>(bytes_total);
  if (args->path) {
    VLOG("Creating destination file of size %.3fGB\n", 1e-9*allocsize);
    mb = new MmapMemoryBuffer(args->path, allocsize, MB_CREATE|MB_EXTERNAL);
  } else {
    mb = new RamMemoryBuffer(allocsize);
  }
  size_t bytes_written = 0;
  double t2 = wallclock();

  // Write the column names
  char **colnames = args->column_names;
  if (colnames) {
    char *ch, *ch0, *colname;
    size_t maxsize = 0;
    while ((colname = *colnames++)) {
      // A string may expand up to twice in size (if all its characters need
      // to be escaped) + add 2 surrounding quotes + add a comma in the end.
      maxsize += strlen(colname)*2 + 2 + 1;
    }
    mb->ensuresize(maxsize + allocsize);
    ch = ch0 = static_cast<char*>(mb->get());
    colnames = args->column_names;
    while ((colname = *colnames++)) {
      write_string(&ch, colname);
      *ch++ = ',';
    }
    // Replace the last ',' with a newline
    ch[-1] = '\n';
    bytes_written += static_cast<size_t>(ch - ch0);
  }
  double t3 = wallclock();

  // Calculate the best chunking strategy for this file
  double rows_per_chunk;
  size_t bytes_per_chunk;
  int min_nchunks = nthreads == 1 ? 1 : nthreads*2;
  int64_t nchunks = bytes_total / max_chunk_size;
  if (nchunks < min_nchunks) nchunks = min_nchunks;
  while (1) {
    rows_per_chunk = 1.0 * (nrows + 1) / nchunks;
    bytes_per_chunk = static_cast<size_t>(bytes_per_row * rows_per_chunk);
    if (rows_per_chunk < 1.0) {
      // If each row's size is too large, then parse 1 row at a time.
      nchunks = nrows;
    } else if (bytes_per_chunk < min_chunk_size && nchunks > 1) {
      // The data is too small, and number of available threads too large --
      // reduce the number of chunks so that we don't waste resources on
      // needless thread manipulation.
      // This formula guarantees that new bytes_per_chunk will be no less
      // than min_chunk_size (or nchunks will be 1).
      nchunks = bytes_total / min_chunk_size;
      if (nchunks < 1) nchunks = 1;
    } else {
      break;
    }
  }

  // Prepare columns for writing
  CsvColumn *columns = reinterpret_cast<CsvColumn*>(malloc((size_t)ncols * sizeof(CsvColumn)));
  for (int64_t i = 0; i < ncols; i++) {
    new (columns + i) CsvColumn(dt->columns[i]);
  }
  double t4 = wallclock();

  // Start writing the CSV
  bool stopTeam = false;
  #pragma omp parallel num_threads(nthreads)
  {
    #pragma omp single
    {
      VLOG("Writing file using %lld chunks, with %.1f rows per chunk\n",
           nchunks, rows_per_chunk);
      VLOG("Using nthreads = %d\n", omp_get_num_threads());
      VLOG("Initial buffer size in each thread: %zuB\n", bytes_per_chunk);
    }
    size_t bufsize = bytes_per_chunk;
    char *thbuf = new char[bufsize];
    char *tch = thbuf;
    size_t th_write_at = 0;
    size_t th_write_size = 0;

    #pragma omp for ordered schedule(dynamic)
    for (int64_t i = 0; i < nchunks; i++) {
      if (stopTeam) continue;
      int64_t row0 = static_cast<int64_t>(i * rows_per_chunk);
      int64_t row1 = static_cast<int64_t>((i + 1) * rows_per_chunk);
      if (i == nchunks-1) row1 = nrows;  // always go to the last row for last chunk

      // write the thread-local buffer into the output
      if (th_write_size) {
        char *target = static_cast<char*>(mb->get()) + th_write_at;
        memcpy(target, thbuf, th_write_size);
        tch = thbuf;
        th_write_size = 0;
      }

      for (int64_t row = row0; row < row1; row++) {
        for (int64_t col = 0; col < ncols; col++) {
          columns[col].write(&tch, row);
          *tch++ = ',';
        }
        tch[-1] = '\n';
      }

      #pragma omp ordered
      {
        th_write_at = bytes_written;
        th_write_size = static_cast<size_t>(tch - thbuf);
        bytes_written += th_write_size;
        // TODO: Add check that the output buffer has enough space
      }
    }
    if (th_write_size) {
      char *target = static_cast<char*>(mb->get()) + th_write_at;
      memcpy(target, thbuf, th_write_size);
    }
    delete[] thbuf;
  }
  double t5 = wallclock();

  // Done writing; if writing to stdout then append '\0' to make it a regular
  // C string; otherwise truncate MemoryBuffer to the final size.
  if (args->path) {
    VLOG("Reducing destination file to size %.3fGB\n", 1e-9*bytes_written);
    mb->resize(bytes_written);
  } else {
    char *target = static_cast<char*>(mb->get());
    target[bytes_written] = '\0';
    mb->resize(bytes_written + 1);
  }
  free(columns);
  double t6 = wallclock();

  VLOG("Timing report:\n");
  VLOG("   %6.3fs  Calculate expected file size\n", t1 - t0);
  VLOG(" + %6.3fs  Allocate file\n",                t2 - t1);
  VLOG(" + %6.3fs  Write column names\n",           t3 - t2);
  VLOG(" + %6.3fs  Prepare for writing\n",          t4 - t3);
  VLOG(" + %6.3fs  Write the data\n",               t5 - t4);
  VLOG(" + %6.3fs  Finalize the file\n",            t6 - t5);
  VLOG(" = %6.3fs  Overall time taken\n",           t6 - t0);
  return mb;
}



//=================================================================================================
// Helper functions
//=================================================================================================

void init_csvwrite_constants() {

  for (int i = 0; i < DT_STYPES_COUNT; i++) {
    bytes_per_stype[i] = 0;
    writers_per_stype[i] = NULL;
  }
  bytes_per_stype[ST_BOOLEAN_I1] = 1;  // 1
  bytes_per_stype[ST_INTEGER_I1] = 4;  // -100
  bytes_per_stype[ST_INTEGER_I2] = 6;  // -32000
  bytes_per_stype[ST_INTEGER_I4] = 11; // -2000000000
  bytes_per_stype[ST_INTEGER_I8] = 20; // -9223372036854775800
  bytes_per_stype[ST_REAL_F8]    = 24; // -0x1.23456789ABCDEp+1000

  writers_per_stype[ST_BOOLEAN_I1] = (writer_fn) write_b1;
  writers_per_stype[ST_INTEGER_I1] = (writer_fn) write_i1;
  writers_per_stype[ST_INTEGER_I2] = (writer_fn) write_i2;
  writers_per_stype[ST_INTEGER_I4] = (writer_fn) write_i4;
  writers_per_stype[ST_INTEGER_I8] = (writer_fn) write_i8;
  writers_per_stype[ST_REAL_F4]    = (writer_fn) write_f4_hex;
  writers_per_stype[ST_REAL_F8]    = (writer_fn) write_f8_hex;
  writers_per_stype[ST_STRING_I4_VCHAR] = (writer_fn) write_s4;
}