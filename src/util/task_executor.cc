// Copyright (c) 2013, Cloudera, inc.

#include <boost/foreach.hpp>
#include <boost/thread/locks.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <string>

#include "gutil/stl_util.h"
#include "util/countdown_latch.h"
#include "util/task_executor.h"

namespace kudu {

//////////////////////////////////////////////////
// FutureTask
//////////////////////////////////////////////////

FutureTask::FutureTask(const std::tr1::shared_ptr<Task>& task)
: state_(kTaskPendingState),
  task_(task),
  latch_(1) {
}

void FutureTask::Run() {
  if (!set_state(kTaskRunningState)) {
    Status s = Status::Aborted("Task was aborted before it ran");
    BOOST_FOREACH(ListenerCallback callback, listeners_) {
      callback->OnFailure(s);
    }
    latch_.CountDown();
    return;
  }

  status_ = task_->Run();
  set_state(kTaskFinishedState);

  {
    boost::lock_guard<LockType> l(lock_);
    if (status_.ok()) {
      BOOST_FOREACH(ListenerCallback callback, listeners_) {
        callback->OnSuccess();
      }
    } else {
      BOOST_FOREACH(ListenerCallback callback, listeners_) {
        callback->OnFailure(status_);
      }
    }
  }

  latch_.CountDown();
}

bool FutureTask::Abort() {
  boost::lock_guard<LockType> l(lock_);
  if (state_ != kTaskFinishedState && task_->Abort()) {
    state_ = kTaskAbortedState;
    return true;
  }
  return false;
}

// TODO: Consider making it so that all callbacks are invoked on the executor thread.
void FutureTask::AddListener(
    std::tr1::shared_ptr<FutureCallback> callback) {
  boost::lock_guard<LockType> l(lock_);
  if (state_ != kTaskFinishedState && state_ != kTaskAbortedState) {
    listeners_.push_back(callback);
  } else if (status_.ok()) {
    callback->OnSuccess();
  } else {
    callback->OnFailure(status_);
  }
}

bool FutureTask::is_aborted() const {
  boost::lock_guard<LockType> l(lock_);
  return state_ == kTaskAbortedState;
}

bool FutureTask::is_done() const {
  boost::lock_guard<LockType> l(lock_);
  return state_ == kTaskFinishedState || state_ == kTaskAbortedState;
}

bool FutureTask::is_running() const {
  boost::lock_guard<LockType> l(lock_);
  return state_ == kTaskRunningState;
}

void FutureTask::Wait() {
  latch_.Wait();
}

bool FutureTask::TimedWait(const boost::system_time& time_until) {
  return latch_.TimedWait(time_until);
}

bool FutureTask::set_state(TaskState state) {
  boost::lock_guard<LockType> l(lock_);
  if (state_ == kTaskAbortedState) {
    return false;
  }
  state_ = state;
  return true;
}

//////////////////////////////////////////////////
// TaskExecutor
//////////////////////////////////////////////////

TaskExecutor::TaskExecutor(gscoped_ptr<ThreadPool> thread_pool)
  : thread_pool_(thread_pool.Pass()) {
}

Status TaskExecutor::Submit(const std::tr1::shared_ptr<Task>& task,
                          std::tr1::shared_ptr<Future> *future) {
  std::tr1::shared_ptr<FutureTask> future_task(new FutureTask(task));
  if (future != NULL) {
    DCHECK(future->get() == NULL);
    *future = future_task;
  }
  return thread_pool_->Submit(future_task);
}


Status TaskExecutor::Submit(const boost::function<Status()>& run_method,
                            std::tr1::shared_ptr<Future>* future) {
  return Submit(std::tr1::shared_ptr<Task>(new BoundTask(run_method)), future);
}

Status TaskExecutor::Submit(const boost::function<Status()>& run_method,
                            const boost::function<bool()>& abort_method,
                            std::tr1::shared_ptr<Future>* future) {
  return Submit(std::tr1::shared_ptr<Task>(new BoundTask(run_method, abort_method)), future);
}

Status TaskExecutor::SubmitFutureTask(const std::tr1::shared_ptr<FutureTask>* future_task) {
  CHECK(future_task != NULL);
  return thread_pool_->Submit(*future_task);
}

void TaskExecutor::Wait() {
  thread_pool_->Wait();
}

bool TaskExecutor::TimedWait(const boost::system_time& time_until) {
  return thread_pool_->TimedWait(time_until);
}

TaskExecutor::~TaskExecutor() {
  Shutdown();
}

void TaskExecutor::Shutdown() {
  thread_pool_->Shutdown();
}

TaskExecutor* TaskExecutor::CreateNew(const string& name,
                                      size_t max_threads) {
  return CreateNew(name, 0, max_threads);
}

TaskExecutor* TaskExecutor::CreateNew(const string& name,
                                      size_t min_threads,
                                      size_t max_threads) {
  gscoped_ptr<ThreadPool> thread_pool;
  Status s = ThreadPoolBuilder(name).set_min_threads(min_threads)
                                    .set_max_threads(max_threads)
                                    .Build(&thread_pool);
  if (!s.ok()) {
    LOG(ERROR) << "Unable to initialize the TaskExecutor ThreadPool for "
               << name << ": " << s.ToString();
    return(NULL);
  }

  return new TaskExecutor(thread_pool.Pass());
}

} // namespace kudu
