// Copyright (c) 2013, Cloudera, inc.

#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/thread/locks.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <limits>
#include <string>

#include "gutil/stl_util.h"
#include "gutil/strings/substitute.h"
#include "gutil/sysinfo.h"
#include "util/thread.h"
#include "util/threadpool.h"
#include "util/trace.h"

namespace kudu {

using strings::Substitute;

////////////////////////////////////////////////////////
// FunctionRunnable
////////////////////////////////////////////////////////

class FunctionRunnable : public Runnable {
 public:
  FunctionRunnable(const boost::function<void()>& func)
    : func_(func) {
  }

  void Run() OVERRIDE {
    func_();
  }

 private:
  boost::function<void()> func_;
};

////////////////////////////////////////////////////////
// ThreadPoolBuilder
////////////////////////////////////////////////////////

ThreadPoolBuilder::ThreadPoolBuilder(const std::string& name)
  : name_(name),
    min_threads_(0),
    max_threads_(base::NumCPUs()),
    max_queue_size_(std::numeric_limits<int>::max()),
    timeout_(MonoDelta::FromMilliseconds(500)) {
}

ThreadPoolBuilder& ThreadPoolBuilder::set_min_threads(int min_threads) {
  CHECK_GE(min_threads, 0);
  min_threads_ = min_threads;
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_max_threads(int max_threads) {
  CHECK_GT(max_threads, 0);
  max_threads_ = max_threads;
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_max_queue_size(int max_queue_size) {
  CHECK_GT(max_queue_size, 0);
  max_queue_size_ = max_queue_size;
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_timeout(const MonoDelta& timeout) {
  timeout_ = timeout;
  return *this;
}

Status ThreadPoolBuilder::Build(gscoped_ptr<ThreadPool>* pool) const {
  pool->reset(new ThreadPool(*this));
  RETURN_NOT_OK((*pool)->Init());
  return Status::OK();
}

////////////////////////////////////////////////////////
// ThreadPool
////////////////////////////////////////////////////////

ThreadPool::ThreadPool(const ThreadPoolBuilder& builder)
  : name_(builder.name_),
    min_threads_(builder.min_threads_),
    max_threads_(builder.max_threads_),
    max_queue_size_(builder.max_queue_size_),
    timeout_(builder.timeout_),
    pool_status_(Status::Uninitialized("The pool was not initialized.")),
    num_threads_(0),
    active_threads_(0),
    queue_size_(0) {
}

ThreadPool::~ThreadPool() {
  Shutdown();
}

Status ThreadPool::Init() {
  boost::unique_lock<boost::mutex> unique_lock(lock_);
  if (!pool_status_.IsUninitialized()) {
    return Status::NotSupported("The thread pool is already initialized");
  }
  pool_status_ = Status::OK();
  for (int i = 0; i < min_threads_; i++) {
    Status status = CreateThreadUnlocked();
    if (!status.ok()) {
      Shutdown();
      return status;
    }
  }
  return Status::OK();
}

void ThreadPool::ClearQueue() {
  BOOST_FOREACH(QueueEntry& e, queue_) {
    if (e.trace) {
      e.trace->Release();
    }
  }
  queue_.clear();
  queue_size_ = 0;
}

void ThreadPool::Shutdown() {
  boost::unique_lock<boost::mutex> unique_lock(lock_);
  pool_status_ = Status::ServiceUnavailable("The pool has been shut down.");
  ClearQueue();
  not_empty_.notify_all();

  // The Runnable doesn't have Abort() so we must wait
  // and hopefully the abort is done outside before calling Shutdown().
  while (num_threads_ > 0) {
    no_threads_cond_.wait(unique_lock);
  }
}

Status ThreadPool::SubmitFunc(const boost::function<void()>& func) {
  return Submit(std::tr1::shared_ptr<Runnable>(new FunctionRunnable(func)));
}

Status ThreadPool::Submit(const std::tr1::shared_ptr<Runnable>& task) {
  boost::lock_guard<boost::mutex> guard(lock_);
  if (PREDICT_FALSE(!pool_status_.ok())) {
    return pool_status_;
  }

  // Size limit check.
  if (queue_size_ == max_queue_size_) {
    return Status::ServiceUnavailable(Substitute("Thread pool queue is full ($0 items)",
                                                 queue_size_));
  }

  // Should we create another thread?
  // We assume that each current inactive thread will grab one item from the
  // queue.  If it seems like we'll need another thread, we create one.
  // In theory, a currently active thread could finish immediately after this
  // calculation.  This would mean we created a thread we didn't really need.
  // However, this race is unavoidable, since we don't do the work under a lock.
  // It's also harmless.
  //
  // Of course, we never create more than max_threads_ threads no matter what.
  int inactive_threads = num_threads_ - active_threads_;
  int additional_threads = (queue_size_ + 1) - inactive_threads;
  if (additional_threads > 0 && num_threads_ < max_threads_) {
    Status status = CreateThreadUnlocked();
    if (!status.ok()) {
      if (num_threads_ == 0) {
        // If we have no threads, we can't do any work.
        return status;
      } else {
        // If we failed to create a thread, but there are still some other
        // worker threads, log a warning message and continue.
        LOG(WARNING) << "Thread pool failed to create thread: "
                     << status.ToString();
      }
    }
  }

  QueueEntry e;
  e.runnable = task;
  e.trace = Trace::CurrentTrace();
  // Need to AddRef, since the thread which submitted the task may go away,
  // and we don't want the trace to be destructed while waiting in the queue.
  if (e.trace) {
    e.trace->AddRef();
  }
  queue_.push_back(e);
  queue_size_++;

  not_empty_.notify_one();
  return Status::OK();
}

void ThreadPool::Wait() {
  boost::unique_lock<boost::mutex> unique_lock(lock_);
  while ((!queue_.empty()) || (active_threads_ > 0)) {
    idle_cond_.wait(unique_lock);
  }
}

bool ThreadPool::TimedWait(const boost::system_time& time_until) {
  boost::unique_lock<boost::mutex> unique_lock(lock_);
  while ((!queue_.empty()) || (active_threads_ > 0)) {
    if (!idle_cond_.timed_wait(unique_lock, time_until)) {
      return false;
    }
  }
  return true;
}

void ThreadPool::DispatchThread(bool permanent) {
  boost::unique_lock<boost::mutex> unique_lock(lock_);
  while (true) {
    // Note: Status::Aborted() is used to indicate normal shutdown.
    if (!pool_status_.ok()) {
      VLOG(2) << "DispatchThread exiting: " << pool_status_.ToString();
      break;
    }

    if (queue_.empty()) {
      if (permanent) {
        not_empty_.wait(unique_lock);
      } else {
        boost::posix_time::time_duration timeout =
          boost::posix_time::microseconds(timeout_.ToMicroseconds());
        if (!not_empty_.timed_wait(unique_lock, timeout)) {
          // After much investigation, it appears that boost's condition variables have
          // a weird behavior in which they can return 'false' from timed_wait even if
          // another thread did in fact notify. This doesn't happen with pthread cond
          // vars, but apparently after a timeout there is some brief period during
          // which another thread may actually grab the mutex, notify, and release again
          // before we get the mutex. So, we'll recheck the empty queue case regardless.
          if (queue_.empty()) {
            VLOG(1) << "Timed out worker for pool " << name_ << " after "
                    << timeout_.ToMilliseconds() << " ms.";
            break;
          }
        }
      }
      continue;
    }

    // Fetch a pending task
    QueueEntry entry = queue_.front();
    queue_.pop_front();
    queue_size_--;
    ++active_threads_;

    unique_lock.unlock();
    ADOPT_TRACE(entry.trace);
    // Release the reference which was held by the queued item.
    if (entry.trace) {
      entry.trace->Release();
    }
    // Execute the task
    entry.runnable->Run();
    unique_lock.lock();

    if (--active_threads_ == 0) {
      idle_cond_.notify_all();
    }
  }

  // It's important that we hold the lock between exiting the loop and dropping
  // num_threads_. Otherwise it's possible someone else could come along here
  // and add a new task just as the last running thread is about to exit.
  CHECK(unique_lock.owns_lock());

  if (--num_threads_ == 0) {
    no_threads_cond_.notify_all();

    // Sanity check: if we're the last thread exiting, the queue ought to be
    // empty. Otherwise it will never get processed.
    CHECK(queue_.empty());
    DCHECK_EQ(0, queue_size_);
  }
}

Status ThreadPool::CreateThreadUnlocked() {
  // The first few threads are permanent, and do not time out.
  bool permanent = (num_threads_ < min_threads_);
  Status s = kudu::Thread::Create("thread pool", strings::Substitute("$0 [worker]", name_),
                                  &ThreadPool::DispatchThread, this, permanent, NULL);
  if (s.ok()) {
    num_threads_++;
  }
  return s;
}

} // namespace kudu
