/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // needed for getopt_long
#endif

#include <sys/time.h>
#include <getopt.h>
#include <boost/test/unit_test.hpp>

#include <transport/TFileTransport.h>

using namespace apache::thrift::transport;

/**************************************************************************
 * Global state
 **************************************************************************/

static const char* tmp_dir = "/tmp";

class FsyncLog;
FsyncLog* fsync_log;


/**************************************************************************
 * Helper code
 **************************************************************************/

// Provide BOOST_CHECK_LT() and BOOST_CHECK_GT(), in case we're compiled
// with an older version of boost
#ifndef BOOST_CHECK_LT
#define BOOST_CHECK_CMP(a, b, op) \
  BOOST_CHECK_MESSAGE((a) op (b), \
                      "check " BOOST_STRINGIZE(a) " " BOOST_STRINGIZE(op) " " \
                      BOOST_STRINGIZE(b) " failed: " BOOST_STRINGIZE(a) "=" <<\
                      (a) << " " BOOST_STRINGIZE(b) "=" << (b))

#define BOOST_CHECK_LT(a, b) BOOST_CHECK_CMP(a, b, <)
#define BOOST_CHECK_GT(a, b) BOOST_CHECK_CMP(a, b, >)
#endif // BOOST_CHECK_LT

/**
 * Class to record calls to fsync
 */
class FsyncLog {
 public:
  struct FsyncCall {
    struct timeval time;
    int fd;
  };
  typedef std::list<FsyncCall> CallList;

  FsyncLog() {}

  void fsync(int fd) {
    FsyncCall call;
    gettimeofday(&call.time, NULL);
    calls_.push_back(call);
  }

  const CallList* getCalls() const {
    return &calls_;
  }

 private:
  CallList calls_;
};

/**
 * Helper class to clean up temporary files
 */
class TempFile {
 public:
  TempFile(const char* directory, const char* prefix) {
    size_t path_len = strlen(directory) + strlen(prefix) + 8;
    path_ = new char[path_len];
    snprintf(path_, path_len, "%s/%sXXXXXX", directory, prefix);

    fd_ = mkstemp(path_);
    if (fd_ < 0) {
      throw apache::thrift::TException("mkstemp() failed");
    }
  }

  ~TempFile() {
    unlink();
    close();
  }

  const char* getPath() const {
    return path_;
  }

  int getFD() const {
    return fd_;
  }

  void unlink() {
    if (path_) {
      ::unlink(path_);
      delete[] path_;
      path_ = NULL;
    }
  }

  void close() {
    if (fd_ < 0) {
      return;
    }

    ::close(fd_);
    fd_ = -1;
  }

 private:
  char* path_;
  int fd_;
};

// Use our own version of fsync() for testing.
// This returns immediately, so timing in test_destructor() isn't affected by
// waiting on the actual filesystem.
extern "C"
int fsync(int fd) {
  if (fsync_log) {
    fsync_log->fsync(fd);
  }
  return 0;
}


int time_diff(const struct timeval* t1, const struct timeval* t2) {
  return (t2->tv_usec - t1->tv_usec) + (t2->tv_sec - t1->tv_sec) * 1000000;
}


/**************************************************************************
 * Test cases
 **************************************************************************/

/**
 * Make sure the TFileTransport destructor exits "quickly".
 *
 * Previous versions had a bug causing the writer thread not to exit
 * right away.
 *
 * It's kind of lame that we just check to see how long the destructor takes in
 * wall-clock time.  This could result in false failures on slower systems, or
 * on heavily loaded machines.
 */
void test_destructor() {
  TempFile f(tmp_dir, "thrift.TFileTransportTest.");

  unsigned int const NUM_ITERATIONS = 1000;
  int const MAX_DESTRUCTOR_USEC = 500;

  for (unsigned int n = 0; n < NUM_ITERATIONS; ++n) {
    ftruncate(f.getFD(), 0);

    TFileTransport* transport = new TFileTransport(f.getPath());

    // write something so that the writer thread gets started
    transport->write(reinterpret_cast<const uint8_t*>("foo"), 3);

    // Every other iteration, also call flush(), just in case that potentially
    // has any effect on how the writer thread wakes up.
    if (n & 0x1) {
      transport->flush();
    }

    /*
     * Time the call to the destructor
     */
    struct timeval start;
    struct timeval end;

    gettimeofday(&start, NULL);
    delete transport;
    gettimeofday(&end, NULL);

    int delta = time_diff(&start, &end);
    BOOST_CHECK_LT(delta, MAX_DESTRUCTOR_USEC);
  }
}

/**
 * Make sure setFlushMaxUs() is honored.
 */
void test_flush_max_us_impl(uint32_t flush_us, uint32_t write_us,
                            uint32_t test_us) {
  // TFileTransport only calls fsync() if data has been written,
  // so make sure the write interval is smaller than the flush interval.
  BOOST_REQUIRE(write_us < flush_us);

  TempFile f(tmp_dir, "thrift.TFileTransportTest.");

  // Record calls to fsync()
  FsyncLog log;
  fsync_log = &log;

  TFileTransport* transport = new TFileTransport(f.getPath());
  // Don't flush because of # of bytes written
  transport->setFlushMaxBytes(0xffffffff);
  uint8_t buf[] = "a";
  uint32_t buflen = sizeof(buf);

  // Flush every 500us
  transport->setFlushMaxUs(flush_us);

  // Add one entry to the fsync log, just to mark the start time
  log.fsync(-1);

  // Loop doing write(), sleep(), ...
  uint32_t total_time = 0;
  while (true) {
    transport->write(buf, buflen);
    if (total_time > test_us) {
      break;
    }
    usleep(write_us);
    total_time += write_us;
  }

  delete transport;

  // Stop logging new fsync() calls
  fsync_log = NULL;

  // Examine the fsync() log
  //
  // TFileTransport uses pthread_cond_timedwait(), which only has millisecond
  // resolution.  In my testing, it normally wakes up about 1 millisecond late.
  // However, sometimes it takes a bit longer.  Allow 5ms leeway.
  int max_allowed_delta = flush_us + 5000;

  const FsyncLog::CallList* calls = log.getCalls();
  // We added 1 fsync call above.
  // Make sure TFileTransport called fsync at least once
  BOOST_CHECK_GT(calls->size(), 1);

  const struct timeval* prev_time = NULL;
  for (FsyncLog::CallList::const_iterator it = calls->begin();
       it != calls->end();
       ++it) {
    if (prev_time) {
      int delta = time_diff(prev_time, &it->time);
      BOOST_CHECK_LT(delta, max_allowed_delta);
    }
    prev_time = &it->time;
  }
}

void test_flush_max_us1() {
  // fsync every 10ms, write every 5ms, for 500ms
  test_flush_max_us_impl(10000, 5000, 500000);
}

void test_flush_max_us2() {
  // fsync every 50ms, write every 20ms, for 500ms
  test_flush_max_us_impl(50000, 20000, 500000);
}

void test_flush_max_us3() {
  // fsync every 400ms, write every 300ms, for 1s
  test_flush_max_us_impl(400000, 300000, 1000000);
}

/**************************************************************************
 * General Initialization
 **************************************************************************/

void print_usage(FILE* f, const char* argv0) {
  fprintf(f, "Usage: %s [boost_options] [options]\n", argv0);
  fprintf(f, "Options:\n");
  fprintf(f, "  --tmp-dir=DIR, -t DIR\n");
  fprintf(f, "  --help\n");
}

void parse_args(int argc, char* argv[]) {
  int seed;
  int *seedptr = NULL;

  struct option long_opts[] = {
    { "help", false, NULL, 'h' },
    { "tmp-dir", true, NULL, 't' },
    { NULL, 0, NULL, 0 }
  };

  while (true) {
    optopt = 1;
    int optchar = getopt_long(argc, argv, "ht:", long_opts, NULL);
    if (optchar == -1) {
      break;
    }

    switch (optchar) {
      case 't':
        tmp_dir = optarg;
        break;
      case 'h':
        print_usage(stdout, argv[0]);
        exit(0);
      case '?':
        exit(1);
      default:
        // Only happens if someone adds another option to the optarg string,
        // but doesn't update the switch statement to handle it.
        fprintf(stderr, "unknown option \"-%c\"\n", optchar);
        exit(1);
    }
  }
}

boost::unit_test::test_suite* init_unit_test_suite(int argc, char* argv[]) {
  // Parse arguments
  parse_args(argc, argv);

  boost::unit_test::test_suite* suite =
    BOOST_TEST_SUITE("TFileTransportTests");

  suite->add(BOOST_TEST_CASE(test_destructor));
  suite->add(BOOST_TEST_CASE(test_flush_max_us1));
  suite->add(BOOST_TEST_CASE(test_flush_max_us2));
  suite->add(BOOST_TEST_CASE(test_flush_max_us3));

  return suite;
}