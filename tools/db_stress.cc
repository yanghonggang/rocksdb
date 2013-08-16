// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The test uses an array to compare against values written to the database.
// Keys written to the array are in 1:1 correspondence to the actual values in
// the database according to the formula in the function GenerateValue.

// Space is reserved in the array from 0 to FLAGS_max_key and values are
// randomly written/deleted/read from those positions. During verification we
// compare all the positions in the array. To shorten/elongate the running
// time, you could change the settings: FLAGS_max_key, FLAGS_ops_per_thread,
// (sometimes also FLAGS_threads).
//
// NOTE that if FLAGS_test_batches_snapshots is set, the test will have
// different behavior. See comment of the flag for details.

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include "db/db_impl.h"
#include "db/version_set.h"
#include "db/db_statistics.h"
#include "leveldb/cache.h"
#include "utilities/utility_db.h"
#include "leveldb/env.h"
#include "leveldb/write_batch.h"
#include "leveldb/statistics.h"
#include "port/port.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"
#include "util/logging.h"
#include "utilities/ttl/db_ttl.h"
#include "hdfs/env_hdfs.h"
#include "utilities/merge_operators.h"

static const long KB = 1024;

// Seed for PRNG
static uint32_t FLAGS_seed = 2341234;

// Max number of key/values to place in database
static long FLAGS_max_key = 1 * KB * KB * KB;

// If set, the test uses MultiGet(), MultiPut() and MultiDelete() which
// read/write/delete multiple keys in a batch. In this mode, we do not verify
// db content by comparing the content with the pre-allocated array. Instead,
// we do partial verification inside MultiGet() by checking various values in
// a batch. Benefit of this mode:
// (a) No need to acquire mutexes during writes (less cache flushes
//     in multi-core leading to speed up)
// (b) No long validation at the end (more speed up)
// (c) Test snapshot and atomicity of batch writes
static bool FLAGS_test_batches_snapshots = false;

// Number of concurrent threads to run.
static int FLAGS_threads = 32;

// Opens the db with this ttl value if this is not -1
// Carefully specify a large value such that verifications on deleted
// values don't fail
static int FLAGS_ttl = -1;

// Size of each value will be this number times rand_int(1,3) bytes
static int FLAGS_value_size_mult = 8;

static bool FLAGS_verify_before_write = false;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Destroys the database dir before start if this is true
static bool FLAGS_destroy_db_initially = true;

static bool FLAGS_verbose = false;

// Number of bytes to buffer in memtable before compacting
// (initialized to default value by "main")
static int FLAGS_write_buffer_size = 0;

// The number of in-memory memtables.
// Each memtable is of size FLAGS_write_buffer_size.
// This is initialized to default value of 2 in "main" function.
static int FLAGS_max_write_buffer_number = 0;

// The maximum number of concurrent background compactions
// that can occur in parallel.
// This is initialized to default value of 1 in "main" function.
static int FLAGS_max_background_compactions = 0;

// This is initialized to default value of false
static leveldb::CompactionStyle FLAGS_compaction_style = leveldb::kCompactionStyleLevel;

// Number of bytes to use as a cache of uncompressed data.
static long FLAGS_cache_size = 2 * KB * KB * KB;

// Number of bytes in a block.
static int FLAGS_block_size = 4 * KB;

// Number of times database reopens
static int FLAGS_reopen = 10;

// Maximum number of files to keep open at the same time (use default if == 0)
static int FLAGS_open_files = 0;

// Bloom filter bits per key.
// Negative means use default settings.
static int FLAGS_bloom_bits = 10;

// Use the db with the following name.
static const char* FLAGS_db = nullptr;

// Verify checksum for every block read from storage
static bool FLAGS_verify_checksum = false;

// Allow reads to occur via mmap-ing files
static bool FLAGS_use_mmap_reads = leveldb::EnvOptions().use_mmap_reads;

// Database statistics
static std::shared_ptr<leveldb::Statistics> dbstats;

// Sync all writes to disk
static bool FLAGS_sync = false;

// If true, do not wait until data is synced to disk.
static bool FLAGS_disable_data_sync = false;

// If true, issue fsync instead of fdatasync
static bool FLAGS_use_fsync = false;

// If non-zero, kill at various points in source code with probability 1/this
static int FLAGS_kill_random_test = 0;
extern int leveldb_kill_odds;

// If true, do not write WAL for write.
static bool FLAGS_disable_wal = false;

// Target level-1 file size for compaction
static int FLAGS_target_file_size_base = 64 * KB;

// A multiplier to compute targe level-N file size (N >= 2)
static int FLAGS_target_file_size_multiplier = 1;

// Max bytes for level-1
static uint64_t FLAGS_max_bytes_for_level_base = 256 * KB;

// A multiplier to compute max bytes for level-N (N >= 2)
static int FLAGS_max_bytes_for_level_multiplier = 2;

// Number of files in level-0 that will trigger put stop.
static int FLAGS_level0_stop_writes_trigger = 12;

// Number of files in level-0 that will slow down writes.
static int FLAGS_level0_slowdown_writes_trigger = 8;

// Ratio of reads to total workload (expressed as a percentage)
static unsigned int FLAGS_readpercent = 10;

// Ratio of prefix iterators to total workload (expressed as a percentage)
static unsigned int FLAGS_prefixpercent = 25;

// Ratio of deletes to total workload (expressed as a percentage)
static unsigned int FLAGS_writepercent = 50;

// Ratio of deletes to total workload (expressed as a percentage)
static unsigned int FLAGS_delpercent = 15;

// Option to disable compation triggered by read.
static int FLAGS_disable_seek_compaction = false;

// Option to delete obsolete files periodically
// Default: 0 which means that obsolete files are
// deleted after every compaction run.
 static uint64_t FLAGS_delete_obsolete_files_period_micros = 0;

// Algorithm to use to compress the database
static enum leveldb::CompressionType FLAGS_compression_type =
    leveldb::kSnappyCompression;

// posix or hdfs environment
static leveldb::Env* FLAGS_env = leveldb::Env::Default();

// Number of operations per thread.
static uint32_t FLAGS_ops_per_thread = 600000;

// Log2 of number of keys per lock
static uint32_t FLAGS_log2_keys_per_lock = 2; // implies 2^2 keys per lock

// Percentage of times we want to purge redundant keys in memory before flushing
static uint32_t FLAGS_purge_redundant_percent = 50;

// On true, deletes use KeyMayExist to drop the delete if key not present
static bool FLAGS_filter_deletes = false;

// Level0 compaction start trigger
static int FLAGS_level0_file_num_compaction_trigger = 0;

// On true, replaces all writes with a Merge that behaves like a Put
static bool FLAGS_use_merge_put = false;

namespace leveldb {

// convert long to a big-endian slice key
static std::string Key(long val) {
  std::string little_endian_key;
  std::string big_endian_key;
  PutFixed64(&little_endian_key, val);
  assert(little_endian_key.size() == sizeof(val));
  big_endian_key.resize(sizeof(val));
  for (int i=0; i<(int)sizeof(val); i++) {
    big_endian_key[i] = little_endian_key[sizeof(val) - 1 - i];
  }
  return big_endian_key;
}

class StressTest;
namespace {

class Stats {
 private:
  double start_;
  double finish_;
  double seconds_;
  long done_;
  long gets_;
  long prefixes_;
  long writes_;
  long deletes_;
  long iterator_size_sums_;
  long founds_;
  long errors_;
  int next_report_;
  size_t bytes_;
  double last_op_finish_;
  HistogramImpl hist_;

 public:
  Stats() { }

  void Start() {
    next_report_ = 100;
    hist_.Clear();
    done_ = 0;
    gets_ = 0;
    prefixes_ = 0;
    writes_ = 0;
    deletes_ = 0;
    iterator_size_sums_ = 0;
    founds_ = 0;
    errors_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = FLAGS_env->NowMicros();
    last_op_finish_ = start_;
    finish_ = start_;
  }

  void Merge(const Stats& other) {
    hist_.Merge(other.hist_);
    done_ += other.done_;
    gets_ += other.gets_;
    prefixes_ += other.prefixes_;
    writes_ += other.writes_;
    deletes_ += other.deletes_;
    iterator_size_sums_ += other.iterator_size_sums_;
    founds_ += other.founds_;
    errors_ += other.errors_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;
  }

  void Stop() {
    finish_ = FLAGS_env->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = FLAGS_env->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000) {
        fprintf(stdout, "long op: %.1f micros%30s\r", micros, "");
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if      (next_report_ < 1000)   next_report_ += 100;
      else if (next_report_ < 5000)   next_report_ += 500;
      else if (next_report_ < 10000)  next_report_ += 1000;
      else if (next_report_ < 50000)  next_report_ += 5000;
      else if (next_report_ < 100000) next_report_ += 10000;
      else if (next_report_ < 500000) next_report_ += 50000;
      else                            next_report_ += 100000;
      fprintf(stdout, "... finished %ld ops%30s\r", done_, "");
    }
  }

  void AddBytesForWrites(int nwrites, size_t nbytes) {
    writes_ += nwrites;
    bytes_ += nbytes;
  }

  void AddGets(int ngets, int nfounds) {
    founds_ += nfounds;
    gets_ += ngets;
  }

  void AddPrefixes(int nprefixes, int count) {
    prefixes_ += nprefixes;
    iterator_size_sums_ += count;
  }

  void AddDeletes(int n) {
    deletes_ += n;
  }

  void AddErrors(int n) {
    errors_ += n;
  }

  void Report(const char* name) {
    std::string extra;
    if (bytes_ < 1 || done_ < 1) {
      fprintf(stderr, "No writes or ops?\n");
      return;
    }

    double elapsed = (finish_ - start_) * 1e-6;
    double bytes_mb = bytes_ / 1048576.0;
    double rate = bytes_mb / elapsed;
    double throughput = (double)done_/elapsed;

    fprintf(stdout, "%-12s: ", name);
    fprintf(stdout, "%.3f micros/op %ld ops/sec\n",
            seconds_ * 1e6 / done_, (long)throughput);
    fprintf(stdout, "%-12s: Wrote %.2f MB (%.2f MB/sec) (%ld%% of %ld ops)\n",
            "", bytes_mb, rate, (100*writes_)/done_, done_);
    fprintf(stdout, "%-12s: Wrote %ld times\n", "", writes_);
    fprintf(stdout, "%-12s: Deleted %ld times\n", "", deletes_);
    fprintf(stdout, "%-12s: %ld read and %ld found the key\n", "",
            gets_, founds_);
    fprintf(stdout, "%-12s: Prefix scanned %ld times\n", "", prefixes_);
    fprintf(stdout, "%-12s: Iterator size sum is %ld\n", "",
            iterator_size_sums_);
    fprintf(stdout, "%-12s: Got errors %ld times\n", "", errors_);

    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
class SharedState {
 public:
  static const uint32_t SENTINEL = 0xffffffff;

  explicit SharedState(StressTest* stress_test) :
      cv_(&mu_),
      seed_(FLAGS_seed),
      max_key_(FLAGS_max_key),
      log2_keys_per_lock_(FLAGS_log2_keys_per_lock),
      num_threads_(FLAGS_threads),
      num_initialized_(0),
      num_populated_(0),
      vote_reopen_(0),
      num_done_(0),
      start_(false),
      start_verify_(false),
      stress_test_(stress_test) {
    if (FLAGS_test_batches_snapshots) {
      key_locks_ = nullptr;
      values_ = nullptr;
      fprintf(stdout, "No lock creation because test_batches_snapshots set\n");
      return;
    }
    values_ = new uint32_t[max_key_];
    for (long i = 0; i < max_key_; i++) {
      values_[i] = SENTINEL;
    }

    long num_locks = (max_key_ >> log2_keys_per_lock_);
    if (max_key_ & ((1 << log2_keys_per_lock_) - 1)) {
      num_locks ++;
    }
    fprintf(stdout, "Creating %ld locks\n", num_locks);
    key_locks_ = new port::Mutex[num_locks];
  }

  ~SharedState() {
    delete[] values_;
    delete[] key_locks_;
  }

  port::Mutex* GetMutex() {
    return &mu_;
  }

  port::CondVar* GetCondVar() {
    return &cv_;
  }

  StressTest* GetStressTest() const {
    return stress_test_;
  }

  long GetMaxKey() const {
    return max_key_;
  }

  uint32_t GetNumThreads() const {
    return num_threads_;
  }

  void IncInitialized() {
    num_initialized_++;
  }

  void IncOperated() {
    num_populated_++;
  }

  void IncDone() {
    num_done_++;
  }

  void IncVotedReopen() {
    vote_reopen_ = (vote_reopen_ + 1) % num_threads_;
  }

  bool AllInitialized() const {
    return num_initialized_ >= num_threads_;
  }

  bool AllOperated() const {
    return num_populated_ >= num_threads_;
  }

  bool AllDone() const {
    return num_done_ >= num_threads_;
  }

  bool AllVotedReopen() {
    return (vote_reopen_ == 0);
  }

  void SetStart() {
    start_ = true;
  }

  void SetStartVerify() {
    start_verify_ = true;
  }

  bool Started() const {
    return start_;
  }

  bool VerifyStarted() const {
    return start_verify_;
  }

  port::Mutex* GetMutexForKey(long key) {
    return &key_locks_[key >> log2_keys_per_lock_];
  }

  void Put(long key, uint32_t value_base) {
    values_[key] = value_base;
  }

  uint32_t Get(long key) const {
    return values_[key];
  }

  void Delete(long key) const {
    values_[key] = SENTINEL;
  }

  uint32_t GetSeed() const {
    return seed_;
  }

 private:
  port::Mutex mu_;
  port::CondVar cv_;
  const uint32_t seed_;
  const long max_key_;
  const uint32_t log2_keys_per_lock_;
  const int num_threads_;
  long num_initialized_;
  long num_populated_;
  long vote_reopen_;
  long num_done_;
  bool start_;
  bool start_verify_;
  StressTest* stress_test_;

  uint32_t *values_;
  port::Mutex *key_locks_;

};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  uint32_t tid; // 0..n-1
  Random rand;  // Has different seeds for different threads
  SharedState* shared;
  Stats stats;

  ThreadState(uint32_t index, SharedState *shared)
      : tid(index),
        rand(1000 + index + shared->GetSeed()),
        shared(shared) {
  }
};

}  // namespace

class StressTest {
 public:
  StressTest()
      : cache_(NewLRUCache(FLAGS_cache_size)),
        filter_policy_(FLAGS_bloom_bits >= 0
                       ? NewBloomFilterPolicy(FLAGS_bloom_bits)
                       : nullptr),
        prefix_extractor_(NewFixedPrefixTransform(
                          FLAGS_test_batches_snapshots ?
                          sizeof(long) : sizeof(long)-1)),
        db_(nullptr),
        merge_operator_(MergeOperators::CreatePutOperator()),
        num_times_reopened_(0) {
    if (FLAGS_destroy_db_initially) {
      std::vector<std::string> files;
      FLAGS_env->GetChildren(FLAGS_db, &files);
      for (unsigned int i = 0; i < files.size(); i++) {
        if (Slice(files[i]).starts_with("heap-")) {
          FLAGS_env->DeleteFile(std::string(FLAGS_db) + "/" + files[i]);
        }
      }
      DestroyDB(FLAGS_db, Options());
    }
  }

  ~StressTest() {
    delete db_;
    merge_operator_ = nullptr;
    delete filter_policy_;
    delete prefix_extractor_;
  }

  void Run() {
    PrintEnv();
    Open();
    SharedState shared(this);
    uint32_t n = shared.GetNumThreads();

    std::vector<ThreadState*> threads(n);
    for (uint32_t i = 0; i < n; i++) {
      threads[i] = new ThreadState(i, &shared);
      FLAGS_env->StartThread(ThreadBody, threads[i]);
    }
    // Each thread goes through the following states:
    // initializing -> wait for others to init -> read/populate/depopulate
    // wait for others to operate -> verify -> done

    {
      MutexLock l(shared.GetMutex());
      while (!shared.AllInitialized()) {
        shared.GetCondVar()->Wait();
      }

      double now = FLAGS_env->NowMicros();
      fprintf(stdout, "%s Starting database operations\n",
              FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());

      shared.SetStart();
      shared.GetCondVar()->SignalAll();
      while (!shared.AllOperated()) {
        shared.GetCondVar()->Wait();
      }

      now = FLAGS_env->NowMicros();
      if (FLAGS_test_batches_snapshots) {
        fprintf(stdout, "%s Limited verification already done during gets\n",
                FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());
      } else {
        fprintf(stdout, "%s Starting verification\n",
                FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());
      }

      shared.SetStartVerify();
      shared.GetCondVar()->SignalAll();
      while (!shared.AllDone()) {
        shared.GetCondVar()->Wait();
      }
    }

    for (unsigned int i = 1; i < n; i++) {
      threads[0]->stats.Merge(threads[i]->stats);
    }
    threads[0]->stats.Report("Stress Test");

    for (unsigned int i = 0; i < n; i++) {
      delete threads[i];
      threads[i] = nullptr;
    }
    double now = FLAGS_env->NowMicros();
    if (!FLAGS_test_batches_snapshots) {
      fprintf(stdout, "%s Verification successful\n",
              FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());
    }
    PrintStatistics();
  }

 private:

  static void ThreadBody(void* v) {
    ThreadState* thread = reinterpret_cast<ThreadState*>(v);
    SharedState* shared = thread->shared;

    {
      MutexLock l(shared->GetMutex());
      shared->IncInitialized();
      if (shared->AllInitialized()) {
        shared->GetCondVar()->SignalAll();
      }
      while (!shared->Started()) {
        shared->GetCondVar()->Wait();
      }
    }
    thread->shared->GetStressTest()->OperateDb(thread);

    {
      MutexLock l(shared->GetMutex());
      shared->IncOperated();
      if (shared->AllOperated()) {
        shared->GetCondVar()->SignalAll();
      }
      while (!shared->VerifyStarted()) {
        shared->GetCondVar()->Wait();
      }
    }

    if (!FLAGS_test_batches_snapshots) {
      thread->shared->GetStressTest()->VerifyDb(*(thread->shared),
                                                 thread->tid);
    }

    {
      MutexLock l(shared->GetMutex());
      shared->IncDone();
      if (shared->AllDone()) {
        shared->GetCondVar()->SignalAll();
      }
    }

  }

  // Given a key K and value V, this puts ("0"+K, "0"+V), ("1"+K, "1"+V), ...
  // ("9"+K, "9"+V) in DB atomically i.e in a single batch.
  // Also refer MultiGet.
  Status MultiPut(ThreadState* thread,
                  const WriteOptions& writeoptions,
                  const Slice& key, const Slice& value, size_t sz) {
    std::string keys[10] = {"9", "8", "7", "6", "5",
                            "4", "3", "2", "1", "0"};
    std::string values[10] = {"9", "8", "7", "6", "5",
                              "4", "3", "2", "1", "0"};
    Slice value_slices[10];
    WriteBatch batch;
    Status s;
    for (int i = 0; i < 10; i++) {
      keys[i] += key.ToString();
      values[i] += value.ToString();
      value_slices[i] = values[i];
      if (FLAGS_use_merge_put) {
        batch.Merge(keys[i], value_slices[i]);
      } else {
        batch.Put(keys[i], value_slices[i]);
      }
    }

    s = db_->Write(writeoptions, &batch);
    if (!s.ok()) {
      fprintf(stderr, "multiput error: %s\n", s.ToString().c_str());
      thread->stats.AddErrors(1);
    } else {
      // we did 10 writes each of size sz + 1
      thread->stats.AddBytesForWrites(10, (sz + 1) * 10);
    }

    return s;
  }

  // Given a key K, this deletes ("0"+K), ("1"+K),... ("9"+K)
  // in DB atomically i.e in a single batch. Also refer MultiGet.
  Status MultiDelete(ThreadState* thread,
                     const WriteOptions& writeoptions,
                     const Slice& key) {
    std::string keys[10] = {"9", "7", "5", "3", "1",
                            "8", "6", "4", "2", "0"};

    WriteBatch batch;
    Status s;
    for (int i = 0; i < 10; i++) {
      keys[i] += key.ToString();
      batch.Delete(keys[i]);
    }

    s = db_->Write(writeoptions, &batch);
    if (!s.ok()) {
      fprintf(stderr, "multidelete error: %s\n", s.ToString().c_str());
      thread->stats.AddErrors(1);
    } else {
      thread->stats.AddDeletes(10);
    }

    return s;
  }

  // Given a key K, this gets values for "0"+K, "1"+K,..."9"+K
  // in the same snapshot, and verifies that all the values are of the form
  // "0"+V, "1"+V,..."9"+V.
  // ASSUMES that MultiPut was used to put (K, V) into the DB.
  Status MultiGet(ThreadState* thread,
                  const ReadOptions& readoptions,
                  const Slice& key, std::string* value) {
    std::string keys[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    Slice key_slices[10];
    std::string values[10];
    ReadOptions readoptionscopy = readoptions;
    readoptionscopy.snapshot = db_->GetSnapshot();
    Status s;
    for (int i = 0; i < 10; i++) {
      keys[i] += key.ToString();
      key_slices[i] = keys[i];
      s = db_->Get(readoptionscopy, key_slices[i], value);
      if (!s.ok() && !s.IsNotFound()) {
        fprintf(stderr, "get error: %s\n", s.ToString().c_str());
        values[i] = "";
        thread->stats.AddErrors(1);
        // we continue after error rather than exiting so that we can
        // find more errors if any
      } else if (s.IsNotFound()) {
        values[i] = "";
        thread->stats.AddGets(1, 0);
      } else {
        values[i] = *value;

        char expected_prefix = (keys[i])[0];
        char actual_prefix = (values[i])[0];
        if (actual_prefix != expected_prefix) {
          fprintf(stderr, "error expected prefix = %c actual = %c\n",
                  expected_prefix, actual_prefix);
        }
        (values[i])[0] = ' '; // blank out the differing character
        thread->stats.AddGets(1, 1);
      }
    }
    db_->ReleaseSnapshot(readoptionscopy.snapshot);

    // Now that we retrieved all values, check that they all match
    for (int i = 1; i < 10; i++) {
      if (values[i] != values[0]) {
        fprintf(stderr, "error : inconsistent values for key %s: %s, %s\n",
                key.ToString().c_str(), values[0].c_str(),
                values[i].c_str());
      // we continue after error rather than exiting so that we can
      // find more errors if any
      }
    }

    return s;
  }

  // Given a prefix P, this does prefix scans for "0"+P, "1"+P,..."9"+P
  // in the same snapshot.  Each of these 10 scans returns a series of
  // values; each series should be the same length, and it is verified
  // for each index i that all the i'th values are of the form "0"+V,
  // "1"+V,..."9"+V.
  // ASSUMES that MultiPut was used to put (K, V)
  Status MultiPrefixScan(ThreadState* thread,
                         const ReadOptions& readoptions,
                         const Slice& prefix) {
    std::string prefixes[10] = {"0", "1", "2", "3", "4",
                                "5", "6", "7", "8", "9"};
    Slice prefix_slices[10];
    ReadOptions readoptionscopy[10];
    const Snapshot* snapshot = db_->GetSnapshot();
    Iterator* iters[10];
    Status s = Status::OK();
    for (int i = 0; i < 10; i++) {
      prefixes[i] += prefix.ToString();
      prefix_slices[i] = prefixes[i];
      readoptionscopy[i] = readoptions;
      readoptionscopy[i].prefix = &prefix_slices[i];
      readoptionscopy[i].snapshot = snapshot;
      iters[i] = db_->NewIterator(readoptionscopy[i]);
      iters[i]->SeekToFirst();
    }

    int count = 0;
    while (iters[0]->Valid()) {
      count++;
      std::string values[10];
      // get list of all values for this iteration
      for (int i = 0; i < 10; i++) {
        // no iterator should finish before the first one
        assert(iters[i]->Valid());
        values[i] = iters[i]->value().ToString();

        char expected_first = (prefixes[i])[0];
        char actual_first = (values[i])[0];

        if (actual_first != expected_first) {
          fprintf(stderr, "error expected first = %c actual = %c\n",
                  expected_first, actual_first);
        }
        (values[i])[0] = ' '; // blank out the differing character
      }
      // make sure all values are equivalent
      for (int i = 0; i < 10; i++) {
        if (values[i] != values[0]) {
          fprintf(stderr, "error : inconsistent values for prefix %s: %s, %s\n",
                  prefix.ToString().c_str(), values[0].c_str(),
                  values[i].c_str());
          // we continue after error rather than exiting so that we can
          // find more errors if any
        }
        iters[i]->Next();
      }
    }

    // cleanup iterators and snapshot
    for (int i = 0; i < 10; i++) {
      // if the first iterator finished, they should have all finished
      assert(!iters[i]->Valid());
      assert(iters[i]->status().ok());
      delete iters[i];
    }
    db_->ReleaseSnapshot(snapshot);

    if (s.ok()) {
      thread->stats.AddPrefixes(1, count);
    } else {
      thread->stats.AddErrors(1);
    }

    return s;
  }

  void OperateDb(ThreadState* thread) {
    ReadOptions read_opts(FLAGS_verify_checksum, true);
    WriteOptions write_opts;
    char value[100];
    long max_key = thread->shared->GetMaxKey();
    std::string from_db;
    if (FLAGS_sync) {
      write_opts.sync = true;
    }
    write_opts.disableWAL = FLAGS_disable_wal;

    thread->stats.Start();
    for (long i = 0; i < FLAGS_ops_per_thread; i++) {
      if(i != 0 && (i % (FLAGS_ops_per_thread / (FLAGS_reopen + 1))) == 0) {
        {
          thread->stats.FinishedSingleOp();
          MutexLock l(thread->shared->GetMutex());
          thread->shared->IncVotedReopen();
          if (thread->shared->AllVotedReopen()) {
            thread->shared->GetStressTest()->Reopen();
            thread->shared->GetCondVar()->SignalAll();
          }
          else {
            thread->shared->GetCondVar()->Wait();
          }
          // Commenting this out as we don't want to reset stats on each open.
          // thread->stats.Start();
        }
      }

      long rand_key = thread->rand.Next() % max_key;
      std::string keystr = Key(rand_key);
      Slice key = keystr;
      int prob_op = thread->rand.Uniform(100);

      // OPERATION read?
      if (prob_op >= 0 && prob_op < (int)FLAGS_readpercent) {
        if (!FLAGS_test_batches_snapshots) {
          Status s = db_->Get(read_opts, key, &from_db);
          if (s.ok()) {
            // found case
            thread->stats.AddGets(1, 1);
          } else if (s.IsNotFound()) {
            // not found case
            thread->stats.AddGets(1, 0);
          } else {
            // errors case
            thread->stats.AddErrors(1);
          }
        } else {
          MultiGet(thread, read_opts, key, &from_db);
        }
      }
      prob_op -= FLAGS_readpercent;

      // OPERATION prefix scan?
      if (prob_op >= 0 && prob_op < (int)FLAGS_prefixpercent) {
        // keys are longs (e.g., 8 bytes), so we let prefixes be
        // everything except the last byte.  So there will be 2^8=256
        // keys per prefix.
        Slice prefix = Slice(key.data(), key.size() - 1);
        if (!FLAGS_test_batches_snapshots) {
          read_opts.prefix = &prefix;
          Iterator* iter = db_->NewIterator(read_opts);
          int count = 0;
          for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
            assert(iter->key().starts_with(prefix));
            count++;
          }
          assert(count <= 256);
          if (iter->status().ok()) {
            thread->stats.AddPrefixes(1, count);
          } else {
            thread->stats.AddErrors(1);
          }
          delete iter;
        } else {
          MultiPrefixScan(thread, read_opts, prefix);
        }
      }
      prob_op -= FLAGS_prefixpercent;

      // OPERATION write?
      if (prob_op >= 0 && prob_op < (int)FLAGS_writepercent) {
        uint32_t value_base = thread->rand.Next();
        size_t sz = GenerateValue(value_base, value, sizeof(value));
        Slice v(value, sz);
        if (!FLAGS_test_batches_snapshots) {
          MutexLock l(thread->shared->GetMutexForKey(rand_key));
          if (FLAGS_verify_before_write) {
            VerifyValue(rand_key, read_opts, *(thread->shared), &from_db, true);
          }
          thread->shared->Put(rand_key, value_base);
          if (FLAGS_use_merge_put) {
            db_->Merge(write_opts, key, v);
          } else {
            db_->Put(write_opts, key, v);
          }
          thread->stats.AddBytesForWrites(1, sz);
        } else {
          MultiPut(thread, write_opts, key, v, sz);
        }
        PrintKeyValue(rand_key, value, sz);
      }
      prob_op -= FLAGS_writepercent;

      // OPERATION delete?
      if (prob_op >= 0 && prob_op < (int)FLAGS_delpercent) {
        if (!FLAGS_test_batches_snapshots) {
          MutexLock l(thread->shared->GetMutexForKey(rand_key));
          thread->shared->Delete(rand_key);
          db_->Delete(write_opts, key);
          thread->stats.AddDeletes(1);
        } else {
          MultiDelete(thread, write_opts, key);
        }
      }
      prob_op -= FLAGS_delpercent;

      thread->stats.FinishedSingleOp();
    }
    thread->stats.Stop();
  }

  void VerifyDb(const SharedState &shared, long start) const {
    ReadOptions options(FLAGS_verify_checksum, true);
    long max_key = shared.GetMaxKey();
    long step = shared.GetNumThreads();
    for (long i = start; i < max_key; i+= step) {
      std::string from_db;
      VerifyValue(i, options, shared, &from_db, true);
      if (from_db.length()) {
        PrintKeyValue(i, from_db.data(), from_db.length());
      }
    }
  }

  void VerificationAbort(std::string msg, long key) const {
    fprintf(stderr, "Verification failed for key %ld: %s\n",
            key, msg.c_str());
    exit(1);
  }

  void VerifyValue(long key, const ReadOptions &opts, const SharedState &shared,
                   std::string *value_from_db, bool strict=false) const {
    std::string keystr = Key(key);
    Slice k = keystr;
    char value[100];
    uint32_t value_base = shared.Get(key);
    if (value_base == SharedState::SENTINEL && !strict) {
      return;
    }

    if (db_->Get(opts, k, value_from_db).ok()) {
      if (value_base == SharedState::SENTINEL) {
        VerificationAbort("Unexpected value found", key);
      }
      size_t sz = GenerateValue(value_base, value, sizeof(value));
      if (value_from_db->length() != sz) {
        VerificationAbort("Length of value read is not equal", key);
      }
      if (memcmp(value_from_db->data(), value, sz) != 0) {
        VerificationAbort("Contents of value read don't match", key);
      }
    } else {
      if (value_base != SharedState::SENTINEL) {
        VerificationAbort("Value not found", key);
      }
    }
  }

  static void PrintKeyValue(uint32_t key, const char *value, size_t sz) {
    if (!FLAGS_verbose) return;
    fprintf(stdout, "%u ==> (%u) ", key, (unsigned int)sz);
    for (size_t i=0; i<sz; i++) {
      fprintf(stdout, "%X", value[i]);
    }
    fprintf(stdout, "\n");
  }

  static size_t GenerateValue(uint32_t rand, char *v, size_t max_sz) {
    size_t value_sz = ((rand % 3) + 1) * FLAGS_value_size_mult;
    assert(value_sz <= max_sz && value_sz >= sizeof(uint32_t));
    *((uint32_t*)v) = rand;
    for (size_t i=sizeof(uint32_t); i < value_sz; i++) {
      v[i] = (char)(rand ^ i);
    }
    v[value_sz] = '\0';
    return value_sz; // the size of the value set.
  }

  void PrintEnv() const {
    fprintf(stdout, "LevelDB version     : %d.%d\n",
            kMajorVersion, kMinorVersion);
    fprintf(stdout, "Number of threads   : %d\n", FLAGS_threads);
    fprintf(stdout, "Ops per thread      : %d\n", FLAGS_ops_per_thread);
    std::string ttl_state("unused");
    if (FLAGS_ttl > 0) {
      ttl_state = NumberToString(FLAGS_ttl);
    }
    fprintf(stdout, "Time to live(sec)   : %s\n", ttl_state.c_str());
    fprintf(stdout, "Read percentage     : %d\n", FLAGS_readpercent);
    fprintf(stdout, "Prefix percentage   : %d\n", FLAGS_prefixpercent);
    fprintf(stdout, "Write percentage    : %d\n", FLAGS_writepercent);
    fprintf(stdout, "Delete percentage   : %d\n", FLAGS_delpercent);
    fprintf(stdout, "Write-buffer-size   : %d\n", FLAGS_write_buffer_size);
    fprintf(stdout, "Delete percentage   : %d\n", FLAGS_delpercent);
    fprintf(stdout, "Max key             : %ld\n", FLAGS_max_key);
    fprintf(stdout, "Ratio #ops/#keys    : %f\n",
            (1.0 * FLAGS_ops_per_thread * FLAGS_threads)/FLAGS_max_key);
    fprintf(stdout, "Num times DB reopens: %d\n", FLAGS_reopen);
    fprintf(stdout, "Batches/snapshots   : %d\n",
            FLAGS_test_batches_snapshots);
    fprintf(stdout, "Purge redundant %%   : %d\n",
            FLAGS_purge_redundant_percent);
    fprintf(stdout, "Deletes use filter  : %d\n",
            FLAGS_filter_deletes);
    fprintf(stdout, "Num keys per lock   : %d\n",
            1 << FLAGS_log2_keys_per_lock);

    const char* compression = "";
    switch (FLAGS_compression_type) {
      case leveldb::kNoCompression:
        compression = "none";
        break;
      case leveldb::kSnappyCompression:
        compression = "snappy";
        break;
      case leveldb::kZlibCompression:
        compression = "zlib";
        break;
      case leveldb::kBZip2Compression:
        compression = "bzip2";
        break;
    }

    fprintf(stdout, "Compression         : %s\n", compression);
    fprintf(stdout, "------------------------------------------------\n");
  }

  void Open() {
    assert(db_ == nullptr);
    Options options;
    options.block_cache = cache_;
    options.write_buffer_size = FLAGS_write_buffer_size;
    options.max_write_buffer_number = FLAGS_max_write_buffer_number;
    options.max_background_compactions = FLAGS_max_background_compactions;
    options.compaction_style = FLAGS_compaction_style;
    options.block_size = FLAGS_block_size;
    options.filter_policy = filter_policy_;
    options.prefix_extractor = prefix_extractor_;
    options.max_open_files = FLAGS_open_files;
    options.statistics = dbstats;
    options.env = FLAGS_env;
    options.disableDataSync = FLAGS_disable_data_sync;
    options.use_fsync = FLAGS_use_fsync;
    options.allow_mmap_reads = FLAGS_use_mmap_reads;
    leveldb_kill_odds = FLAGS_kill_random_test;
    options.target_file_size_base = FLAGS_target_file_size_base;
    options.target_file_size_multiplier = FLAGS_target_file_size_multiplier;
    options.max_bytes_for_level_base = FLAGS_max_bytes_for_level_base;
    options.max_bytes_for_level_multiplier =
        FLAGS_max_bytes_for_level_multiplier;
    options.level0_stop_writes_trigger = FLAGS_level0_stop_writes_trigger;
    options.level0_slowdown_writes_trigger =
      FLAGS_level0_slowdown_writes_trigger;
    options.level0_file_num_compaction_trigger =
      FLAGS_level0_file_num_compaction_trigger;
    options.compression = FLAGS_compression_type;
    options.create_if_missing = true;
    options.disable_seek_compaction = FLAGS_disable_seek_compaction;
    options.delete_obsolete_files_period_micros =
      FLAGS_delete_obsolete_files_period_micros;
    options.max_manifest_file_size = 1024;
    options.filter_deletes = FLAGS_filter_deletes;
    static Random purge_percent(1000); // no benefit from non-determinism here
    if (purge_percent.Uniform(100) < FLAGS_purge_redundant_percent - 1) {
      options.purge_redundant_kvs_while_flush = false;
    }

    if (FLAGS_use_merge_put) {
      options.merge_operator = merge_operator_.get();
    }

    fprintf(stdout, "DB path: [%s]\n", FLAGS_db);

    Status s;
    if (FLAGS_ttl == -1) {
      s = DB::Open(options, FLAGS_db, &db_);
    } else {
      s = UtilityDB::OpenTtlDB(options, FLAGS_db, &sdb_, FLAGS_ttl);
      db_ = sdb_;
    }
    if (!s.ok()) {
      fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  void Reopen() {
    // do not close the db. Just delete the lock file. This
    // simulates a crash-recovery kind of situation.
    if (FLAGS_ttl != -1) {
      ((DBWithTTL*) db_)->TEST_Destroy_DBWithTtl();
    } else {
      ((DBImpl*) db_)->TEST_Destroy_DBImpl();
    }
    db_ = nullptr;

    num_times_reopened_++;
    double now = FLAGS_env->NowMicros();
    fprintf(stdout, "%s Reopening database for the %dth time\n",
            FLAGS_env->TimeToString((uint64_t) now/1000000).c_str(),
            num_times_reopened_);
    Open();
  }

  void PrintStatistics() {
    if (dbstats) {
      fprintf(stdout, "STATISTICS:\n%s\n", dbstats->ToString().c_str());
    }
  }

 private:
  shared_ptr<Cache> cache_;
  const FilterPolicy* filter_policy_;
  const SliceTransform* prefix_extractor_;
  DB* db_;
  StackableDB* sdb_;
  std::shared_ptr<MergeOperator> merge_operator_;
  int num_times_reopened_;
};

}  // namespace leveldb

int main(int argc, char** argv) {
  FLAGS_write_buffer_size = leveldb::Options().write_buffer_size;
  FLAGS_max_write_buffer_number = leveldb::Options().max_write_buffer_number;
  FLAGS_open_files = leveldb::Options().max_open_files;
  FLAGS_max_background_compactions =
    leveldb::Options().max_background_compactions;
  FLAGS_compaction_style =
    leveldb::Options().compaction_style;
  FLAGS_level0_file_num_compaction_trigger =
    leveldb::Options().level0_file_num_compaction_trigger;
  FLAGS_level0_slowdown_writes_trigger =
    leveldb::Options().level0_slowdown_writes_trigger;
  FLAGS_level0_stop_writes_trigger =
    leveldb::Options().level0_stop_writes_trigger;
  // Compression test code above refers to FLAGS_block_size
  FLAGS_block_size = leveldb::Options().block_size;
  std::string default_db_path;

  for (int i = 1; i < argc; i++) {
    int n;
    uint32_t u;
    long l;
    char junk;
    char hdfsname[2048];

    if (sscanf(argv[i], "--seed=%uf%c", &u, &junk) == 1) {
      FLAGS_seed = u;
    } else if (sscanf(argv[i], "--max_key=%ld%c", &l, &junk) == 1) {
      FLAGS_max_key = l;
    } else if (sscanf(argv[i], "--log2_keys_per_lock=%u%c", &u, &junk) == 1) {
      FLAGS_log2_keys_per_lock = u;
    } else if (sscanf(argv[i], "--ops_per_thread=%u%c", &u, &junk) == 1) {
      FLAGS_ops_per_thread = u;
    } else if (sscanf(argv[i], "--verbose=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_verbose = n;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--destroy_db_initially=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_destroy_db_initially = n;
    } else if (sscanf(argv[i], "--verify_before_write=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_verify_before_write = n;
    } else if (sscanf(argv[i], "--test_batches_snapshots=%d%c", &n, &junk) == 1
               && (n == 0 || n == 1)) {
      FLAGS_test_batches_snapshots = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--ttl=%d%c", &n, &junk) == 1) {
      FLAGS_ttl = n;
    } else if (sscanf(argv[i], "--value_size_mult=%d%c", &n, &junk) == 1) {
      FLAGS_value_size_mult = n;
    } else if (sscanf(argv[i], "--write_buffer_size=%d%c", &n, &junk) == 1) {
      FLAGS_write_buffer_size = n;
    } else if (sscanf(argv[i], "--max_write_buffer_number=%d%c", &n, &junk) == 1) {
      FLAGS_max_write_buffer_number = n;
    } else if (sscanf(argv[i], "--max_background_compactions=%d%c", &n, &junk) == 1) {
      FLAGS_max_background_compactions = n;
    } else if (sscanf(argv[i], "--compaction_style=%d%c", &n, &junk) == 1) {
      FLAGS_compaction_style = (leveldb::CompactionStyle)n;
    } else if (sscanf(argv[i], "--cache_size=%ld%c", &l, &junk) == 1) {
      FLAGS_cache_size = l;
    } else if (sscanf(argv[i], "--block_size=%d%c", &n, &junk) == 1) {
      FLAGS_block_size = n;
    } else if (sscanf(argv[i], "--reopen=%d%c", &n, &junk) == 1 && n >= 0) {
      FLAGS_reopen = n;
    } else if (sscanf(argv[i], "--bloom_bits=%d%c", &n, &junk) == 1) {
      FLAGS_bloom_bits = n;
    } else if (sscanf(argv[i], "--open_files=%d%c", &n, &junk) == 1) {
      FLAGS_open_files = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (sscanf(argv[i], "--verify_checksum=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_verify_checksum = n;
    } else if (sscanf(argv[i], "--mmap_read=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_mmap_reads = n;
    } else if (sscanf(argv[i], "--statistics=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      if (n == 1) {
        dbstats = leveldb::CreateDBStatistics();
      }
    } else if (sscanf(argv[i], "--sync=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_sync = n;
    } else if (sscanf(argv[i], "--readpercent=%d%c", &n, &junk) == 1 &&
               (n >= 0 && n <= 100)) {
      FLAGS_readpercent = n;
    } else if (sscanf(argv[i], "--prefixpercent=%d%c", &n, &junk) == 1 &&
               (n >= 0 && n <= 100)) {
      FLAGS_prefixpercent = n;
    } else if (sscanf(argv[i], "--writepercent=%d%c", &n, &junk) == 1 &&
               (n >= 0 && n <= 100)) {
      FLAGS_writepercent = n;
    } else if (sscanf(argv[i], "--delpercent=%d%c", &n, &junk) == 1 &&
               (n >= 0 && n <= 100)) {
      FLAGS_delpercent = n;
    } else if (sscanf(argv[i], "--disable_data_sync=%d%c", &n, &junk) == 1 &&
        (n == 0 || n == 1)) {
      FLAGS_disable_data_sync = n;
    } else if (sscanf(argv[i], "--use_fsync=%d%c", &n, &junk) == 1 &&
        (n == 0 || n == 1)) {
      FLAGS_use_fsync = n;
    } else if (sscanf(argv[i], "--kill_random_test=%d%c", &n, &junk) == 1 &&
               (n >= 0)) {
      FLAGS_kill_random_test = n;
    } else if (sscanf(argv[i], "--disable_wal=%d%c", &n, &junk) == 1 &&
        (n == 0 || n == 1)) {
      FLAGS_disable_wal = n;
    } else if (sscanf(argv[i], "--hdfs=%s", hdfsname) == 1) {
      FLAGS_env  = new leveldb::HdfsEnv(hdfsname);
    } else if (sscanf(argv[i], "--target_file_size_base=%d%c",
        &n, &junk) == 1) {
      FLAGS_target_file_size_base = n;
    } else if ( sscanf(argv[i], "--target_file_size_multiplier=%d%c",
        &n, &junk) == 1) {
      FLAGS_target_file_size_multiplier = n;
    } else if (
        sscanf(argv[i], "--max_bytes_for_level_base=%ld%c", &l, &junk) == 1) {
      FLAGS_max_bytes_for_level_base = l;
    } else if (sscanf(argv[i], "--max_bytes_for_level_multiplier=%d%c",
        &n, &junk) == 1) {
      FLAGS_max_bytes_for_level_multiplier = n;
    } else if (sscanf(argv[i],"--level0_stop_writes_trigger=%d%c",
        &n, &junk) == 1) {
      FLAGS_level0_stop_writes_trigger = n;
    } else if (sscanf(argv[i],"--level0_slowdown_writes_trigger=%d%c",
        &n, &junk) == 1) {
      FLAGS_level0_slowdown_writes_trigger = n;
    } else if (sscanf(argv[i],"--level0_file_num_compaction_trigger=%d%c",
        &n, &junk) == 1) {
      FLAGS_level0_file_num_compaction_trigger = n;
    } else if (strncmp(argv[i], "--compression_type=", 19) == 0) {
      const char* ctype = argv[i] + 19;
      if (!strcasecmp(ctype, "none"))
        FLAGS_compression_type = leveldb::kNoCompression;
      else if (!strcasecmp(ctype, "snappy"))
        FLAGS_compression_type = leveldb::kSnappyCompression;
      else if (!strcasecmp(ctype, "zlib"))
        FLAGS_compression_type = leveldb::kZlibCompression;
      else if (!strcasecmp(ctype, "bzip2"))
        FLAGS_compression_type = leveldb::kBZip2Compression;
      else {
        fprintf(stdout, "Cannot parse %s\n", argv[i]);
      }
    } else if (sscanf(argv[i], "--disable_seek_compaction=%d%c", &n, &junk) == 1
        && (n == 0 || n == 1)) {
      FLAGS_disable_seek_compaction = n;
    } else if (sscanf(argv[i], "--delete_obsolete_files_period_micros=%ld%c",
                      &l, &junk) == 1) {
      FLAGS_delete_obsolete_files_period_micros = n;
    } else if (sscanf(argv[i], "--purge_redundant_percent=%d%c", &n, &junk) == 1
        && (n >= 0 && n <= 100)) {
      FLAGS_purge_redundant_percent = n;
    } else if (sscanf(argv[i], "--filter_deletes=%d%c", &n, &junk)
        == 1 && (n == 0 || n == 1)) {
      FLAGS_filter_deletes = n;
    } else if (sscanf(argv[i], "--use_merge=%d%c", &n, &junk)
        == 1 && (n == 0 || n == 1)) {
      FLAGS_use_merge_put = n;
    } else {
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  // The number of background threads should be at least as much the
  // max number of concurrent compactions.
  FLAGS_env->SetBackgroundThreads(FLAGS_max_background_compactions);

  if ((FLAGS_readpercent + FLAGS_prefixpercent +
       FLAGS_writepercent + FLAGS_delpercent) != 100) {
      fprintf(stderr, "Error: Read+Prefix+Write+Delete percents != 100!\n");
      exit(1);
  }
  if (FLAGS_disable_wal == 1 && FLAGS_reopen > 0) {
      fprintf(stderr, "Error: Db cannot reopen safely with disable_wal set!\n");
      exit(1);
  }
  if ((unsigned)FLAGS_reopen >= FLAGS_ops_per_thread) {
      fprintf(stderr, "Error: #DB-reopens should be < ops_per_thread\n"
        "Provided reopens = %d and ops_per_thread = %u\n", FLAGS_reopen,
        FLAGS_ops_per_thread);
      exit(1);
  }

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == nullptr) {
      leveldb::Env::Default()->GetTestDirectory(&default_db_path);
      default_db_path += "/dbstress";
      FLAGS_db = default_db_path.c_str();
  }

  leveldb::StressTest stress;
  stress.Run();
  return 0;
}
