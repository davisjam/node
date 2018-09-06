#include "node_platform.h"
#include "node_internals.h"

#include "env-inl.h"
#include "debug_utils.h"
#include "util.h"
#include <algorithm>

namespace node {

using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::Platform;
using v8::Task;
using v8::TracingController;

// Wrapper for delivery to threadpool::Threadpool.
class V8Task : public threadpool::Task {
 public:
  explicit V8Task(std::unique_ptr<v8::Task> task) {
    task_ = std::move(task);
    details_.type = threadpool::TaskDetails::V8;
    details_.priority = -1;
    details_.cancelable = -1;
  }
  ~V8Task() {}

  void Run() {
    task_->Run();
  }

 private:
  std::unique_ptr<v8::Task> task_;
};

class WorkerThreadsTaskRunner::DelayedTaskScheduler {
 public:
  explicit DelayedTaskScheduler(std::shared_ptr<threadpool::Threadpool> tp)
    : tp_(tp) {}

  std::unique_ptr<uv_thread_t> Start() {
    auto start_thread = [](void* data) {
      static_cast<DelayedTaskScheduler*>(data)->Run();
    };
    std::unique_ptr<uv_thread_t> t { new uv_thread_t() };
    uv_sem_init(&ready_, 0);
    CHECK_EQ(0, uv_thread_create(t.get(), start_thread, this));
    uv_sem_wait(&ready_);
    uv_sem_destroy(&ready_);
    return t;
  }

  void PostDelayedTask(std::unique_ptr<Task> task, double delay_in_seconds) {
    tasks_.Push(std::unique_ptr<Task>(new ScheduleTask(this, std::move(task),
                                                       delay_in_seconds)));
    uv_async_send(&flush_tasks_);
  }

  void Stop() {
    tasks_.Push(std::unique_ptr<Task>(new StopTask(this)));
    uv_async_send(&flush_tasks_);
  }

 private:
  void Run() {
    TRACE_EVENT_METADATA1("__metadata", "thread_name", "name",
                          "WorkerThreadsTaskRunner::DelayedTaskScheduler");
    loop_.data = this;
    CHECK_EQ(0, uv_loop_init(&loop_));
    flush_tasks_.data = this;
    CHECK_EQ(0, uv_async_init(&loop_, &flush_tasks_, FlushTasks));
    uv_sem_post(&ready_);

    uv_run(&loop_, UV_RUN_DEFAULT);
    CheckedUvLoopClose(&loop_);
  }

  static void FlushTasks(uv_async_t* flush_tasks) {
    DelayedTaskScheduler* scheduler =
        ContainerOf(&DelayedTaskScheduler::loop_, flush_tasks->loop);
    while (std::unique_ptr<Task> task = scheduler->tasks_.Pop())
      task->Run();
  }

  class StopTask : public Task {
   public:
    explicit StopTask(DelayedTaskScheduler* scheduler): scheduler_(scheduler) {}

    void Run() override {
      std::vector<uv_timer_t*> timers;
      for (uv_timer_t* timer : scheduler_->timers_)
        timers.push_back(timer);
      for (uv_timer_t* timer : timers)
        scheduler_->TakeTimerTask(timer);
      uv_close(reinterpret_cast<uv_handle_t*>(&scheduler_->flush_tasks_),
               [](uv_handle_t* handle) {});
    }

   private:
     DelayedTaskScheduler* scheduler_;
  };

  class ScheduleTask : public Task {
   public:
    ScheduleTask(DelayedTaskScheduler* scheduler,
                 std::unique_ptr<Task> task,
                 double delay_in_seconds)
      : scheduler_(scheduler),
        task_(std::move(task)),
        delay_in_seconds_(delay_in_seconds) {}

    void Run() override {
      uint64_t delay_millis =
          static_cast<uint64_t>(delay_in_seconds_ + 0.5) * 1000;
      std::unique_ptr<uv_timer_t> timer(new uv_timer_t());
      CHECK_EQ(0, uv_timer_init(&scheduler_->loop_, timer.get()));
      timer->data = task_.release();
      CHECK_EQ(0, uv_timer_start(timer.get(), RunTask, delay_millis, 0));
      scheduler_->timers_.insert(timer.release());
    }

   private:
    DelayedTaskScheduler* scheduler_;
    std::unique_ptr<Task> task_;
    double delay_in_seconds_;
  };

  static void RunTask(uv_timer_t* timer) {
    // This DelayedTask is ready. Post it to the threadpool.
    DelayedTaskScheduler* scheduler =
        ContainerOf(&DelayedTaskScheduler::loop_, timer->loop);
    scheduler->tp_->Post(scheduler->TakeTimerTask(timer));
  }

  std::unique_ptr<threadpool::Task> TakeTimerTask(uv_timer_t* timer) {
    std::unique_ptr<Task> task(static_cast<Task*>(timer->data));
    uv_timer_stop(timer);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), [](uv_handle_t* handle) {
      delete reinterpret_cast<uv_timer_t*>(handle);
    });
    timers_.erase(timer);
    return std::unique_ptr<threadpool::Task>(
      new V8Task(std::move(task)));
  }

  uv_sem_t ready_;
  std::shared_ptr<threadpool::Threadpool> tp_;

  TaskQueue<v8::Task> tasks_;
  uv_loop_t loop_;
  uv_async_t flush_tasks_;
  std::unordered_set<uv_timer_t*> timers_;
};

WorkerThreadsTaskRunner::WorkerThreadsTaskRunner(
  std::shared_ptr<threadpool::Threadpool> tp) {
  tp_ = tp;
  delayed_task_scheduler_.reset(
      new DelayedTaskScheduler(tp_));
}

void WorkerThreadsTaskRunner::PostTask(std::unique_ptr<Task> task) {
  fprintf(stderr, "Posting to threadpool!\n");
  tp_->Post(std::unique_ptr<V8Task>(
    new V8Task(std::move(task))));
}

void WorkerThreadsTaskRunner::PostDelayedTask(std::unique_ptr<v8::Task> task,
                                              double delay_in_seconds) {
  delayed_task_scheduler_->PostDelayedTask(std::move(task), delay_in_seconds);
}

void WorkerThreadsTaskRunner::BlockingDrain() {
  // TODO(davisjam): No support for this in threadpool::Threadpool
  // at the moment.
  // I believe this is the cause of the segfaults at the end of running 'node'.
  // pending_worker_tasks_.BlockingDrain();
}

void WorkerThreadsTaskRunner::Shutdown() {
  // TODO(davisjam): More cleanup?
  delayed_task_scheduler_->Stop();
}

int WorkerThreadsTaskRunner::NumberOfWorkerThreads() const {
  return tp_->NWorkers();
}

PerIsolatePlatformData::PerIsolatePlatformData(
    v8::Isolate* isolate, uv_loop_t* loop)
  : loop_(loop) {
  flush_tasks_ = new uv_async_t();
  CHECK_EQ(0, uv_async_init(loop, flush_tasks_, FlushTasks));
  flush_tasks_->data = static_cast<void*>(this);
  uv_unref(reinterpret_cast<uv_handle_t*>(flush_tasks_));
}

void PerIsolatePlatformData::FlushTasks(uv_async_t* handle) {
  auto platform_data = static_cast<PerIsolatePlatformData*>(handle->data);
  platform_data->FlushForegroundTasksInternal();
}

void PerIsolatePlatformData::PostIdleTask(std::unique_ptr<v8::IdleTask> task) {
  UNREACHABLE();
}

void PerIsolatePlatformData::PostTask(std::unique_ptr<Task> task) {
  CHECK_NE(flush_tasks_, nullptr);
  foreground_tasks_.Push(std::move(task));
  uv_async_send(flush_tasks_);
}

void PerIsolatePlatformData::PostDelayedTask(
    std::unique_ptr<Task> task, double delay_in_seconds) {
  CHECK_NE(flush_tasks_, nullptr);
  std::unique_ptr<DelayedTask> delayed(new DelayedTask());
  delayed->task = std::move(task);
  delayed->platform_data = shared_from_this();
  delayed->timeout = delay_in_seconds;
  foreground_delayed_tasks_.Push(std::move(delayed));
  uv_async_send(flush_tasks_);
}

PerIsolatePlatformData::~PerIsolatePlatformData() {
  Shutdown();
}

void PerIsolatePlatformData::Shutdown() {
  if (flush_tasks_ == nullptr)
    return;

  while (FlushForegroundTasksInternal()) {}
  CancelPendingDelayedTasks();

  uv_close(reinterpret_cast<uv_handle_t*>(flush_tasks_),
           [](uv_handle_t* handle) {
    delete reinterpret_cast<uv_async_t*>(handle);
  });
  flush_tasks_ = nullptr;
}

void PerIsolatePlatformData::ref() {
  ref_count_++;
}

int PerIsolatePlatformData::unref() {
  return --ref_count_;
}

NodePlatform::NodePlatform(std::shared_ptr<threadpool::Threadpool> tp,
                           TracingController* tracing_controller) {
  if (tracing_controller) {
    tracing_controller_.reset(tracing_controller);
  } else {
    TracingController* controller = new TracingController();
    tracing_controller_.reset(controller);
  }
  worker_thread_task_runner_ =
      std::make_shared<WorkerThreadsTaskRunner>(tp);
}

void NodePlatform::RegisterIsolate(IsolateData* isolate_data, uv_loop_t* loop) {
  Isolate* isolate = isolate_data->isolate();
  Mutex::ScopedLock lock(per_isolate_mutex_);
  std::shared_ptr<PerIsolatePlatformData> existing = per_isolate_[isolate];
  if (existing) {
    CHECK_EQ(loop, existing->event_loop());
    existing->ref();
  } else {
    per_isolate_[isolate] =
        std::make_shared<PerIsolatePlatformData>(isolate, loop);
  }
}

void NodePlatform::UnregisterIsolate(IsolateData* isolate_data) {
  Isolate* isolate = isolate_data->isolate();
  Mutex::ScopedLock lock(per_isolate_mutex_);
  std::shared_ptr<PerIsolatePlatformData> existing = per_isolate_[isolate];
  CHECK(existing);
  if (existing->unref() == 0) {
    existing->Shutdown();
    per_isolate_.erase(isolate);
  }
}

void NodePlatform::Shutdown() {
  worker_thread_task_runner_->Shutdown();

  {
    Mutex::ScopedLock lock(per_isolate_mutex_);
    per_isolate_.clear();
  }
}

int NodePlatform::NumberOfWorkerThreads() {
  return worker_thread_task_runner_->NumberOfWorkerThreads();
}

void PerIsolatePlatformData::RunForegroundTask(std::unique_ptr<Task> task) {
  Isolate* isolate = Isolate::GetCurrent();
  HandleScope scope(isolate);
  Environment* env = Environment::GetCurrent(isolate);
  InternalCallbackScope cb_scope(env, Local<Object>(), { 0, 0 },
                                 InternalCallbackScope::kAllowEmptyResource);
  task->Run();
}

void PerIsolatePlatformData::DeleteFromScheduledTasks(DelayedTask* task) {
  auto it = std::find_if(scheduled_delayed_tasks_.begin(),
                         scheduled_delayed_tasks_.end(),
                         [task](const DelayedTaskPointer& delayed) -> bool {
          return delayed.get() == task;
      });
  CHECK_NE(it, scheduled_delayed_tasks_.end());
  scheduled_delayed_tasks_.erase(it);
}

void PerIsolatePlatformData::RunForegroundTask(uv_timer_t* handle) {
  DelayedTask* delayed = static_cast<DelayedTask*>(handle->data);
  RunForegroundTask(std::move(delayed->task));
  delayed->platform_data->DeleteFromScheduledTasks(delayed);
}

void PerIsolatePlatformData::CancelPendingDelayedTasks() {
  scheduled_delayed_tasks_.clear();
}

void NodePlatform::DrainTasks(Isolate* isolate) {
  std::shared_ptr<PerIsolatePlatformData> per_isolate = ForIsolate(isolate);

  do {
    // Worker tasks aren't associated with any particular Isolate.
    worker_thread_task_runner_->BlockingDrain();
  } while (per_isolate->FlushForegroundTasksInternal());
}

bool PerIsolatePlatformData::FlushForegroundTasksInternal() {
  bool did_work = false;

  while (std::unique_ptr<DelayedTask> delayed =
      foreground_delayed_tasks_.Pop()) {
    did_work = true;
    uint64_t delay_millis =
        static_cast<uint64_t>(delayed->timeout + 0.5) * 1000;
    delayed->timer.data = static_cast<void*>(delayed.get());
    uv_timer_init(loop_, &delayed->timer);
    // Timers may not guarantee queue ordering of events with the same delay if
    // the delay is non-zero. This should not be a problem in practice.
    uv_timer_start(&delayed->timer, RunForegroundTask, delay_millis, 0);
    uv_unref(reinterpret_cast<uv_handle_t*>(&delayed->timer));

    scheduled_delayed_tasks_.emplace_back(delayed.release(),
                                          [](DelayedTask* delayed) {
      uv_close(reinterpret_cast<uv_handle_t*>(&delayed->timer),
               [](uv_handle_t* handle) {
        delete static_cast<DelayedTask*>(handle->data);
      });
    });
  }
  // Move all foreground tasks into a separate queue and flush that queue.
  // This way tasks that are posted while flushing the queue will be run on the
  // next call of FlushForegroundTasksInternal.
  std::queue<std::unique_ptr<Task>> tasks = foreground_tasks_.PopAll();
  while (!tasks.empty()) {
    std::unique_ptr<Task> task = std::move(tasks.front());
    tasks.pop();
    did_work = true;
    RunForegroundTask(std::move(task));
  }
  return did_work;
}

void NodePlatform::CallOnWorkerThread(std::unique_ptr<v8::Task> task) {
  worker_thread_task_runner_->PostTask(std::move(task));
}

void NodePlatform::CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> task,
                                             double delay_in_seconds) {
  worker_thread_task_runner_->PostDelayedTask(std::move(task),
                                              delay_in_seconds);
}


std::shared_ptr<PerIsolatePlatformData>
NodePlatform::ForIsolate(Isolate* isolate) {
  Mutex::ScopedLock lock(per_isolate_mutex_);
  std::shared_ptr<PerIsolatePlatformData> data = per_isolate_[isolate];
  CHECK(data);
  return data;
}

void NodePlatform::CallOnForegroundThread(Isolate* isolate, Task* task) {
  ForIsolate(isolate)->PostTask(std::unique_ptr<Task>(task));
}

void NodePlatform::CallDelayedOnForegroundThread(Isolate* isolate,
                                                 Task* task,
                                                 double delay_in_seconds) {
  ForIsolate(isolate)->PostDelayedTask(
    std::unique_ptr<Task>(task), delay_in_seconds);
}

bool NodePlatform::FlushForegroundTasks(v8::Isolate* isolate) {
  return ForIsolate(isolate)->FlushForegroundTasksInternal();
}

void NodePlatform::CancelPendingDelayedTasks(v8::Isolate* isolate) {
  ForIsolate(isolate)->CancelPendingDelayedTasks();
}

bool NodePlatform::IdleTasksEnabled(Isolate* isolate) { return false; }

std::shared_ptr<v8::TaskRunner>
NodePlatform::GetForegroundTaskRunner(Isolate* isolate) {
  return ForIsolate(isolate);
}

double NodePlatform::MonotonicallyIncreasingTime() {
  // Convert nanos to seconds.
  return uv_hrtime() / 1e9;
}

double NodePlatform::CurrentClockTimeMillis() {
  return SystemClockTimeMillis();
}

TracingController* NodePlatform::GetTracingController() {
  return tracing_controller_.get();
}

template <class T>
TaskQueue<T>::TaskQueue()
    : lock_(), tasks_available_(), tasks_drained_(),
      outstanding_tasks_(0), stopped_(false), task_queue_() { }

template <class T>
void TaskQueue<T>::Push(std::unique_ptr<T> task) {
  Mutex::ScopedLock scoped_lock(lock_);
  outstanding_tasks_++;
  task_queue_.push(std::move(task));
  tasks_available_.Signal(scoped_lock);
}

template <class T>
std::unique_ptr<T> TaskQueue<T>::Pop() {
  Mutex::ScopedLock scoped_lock(lock_);
  if (task_queue_.empty()) {
    return std::unique_ptr<T>(nullptr);
  }
  std::unique_ptr<T> result = std::move(task_queue_.front());
  task_queue_.pop();
  return result;
}

template <class T>
std::unique_ptr<T> TaskQueue<T>::BlockingPop() {
  Mutex::ScopedLock scoped_lock(lock_);
  while (task_queue_.empty() && !stopped_) {
    tasks_available_.Wait(scoped_lock);
  }
  if (stopped_) {
    return std::unique_ptr<T>(nullptr);
  }
  std::unique_ptr<T> result = std::move(task_queue_.front());
  task_queue_.pop();
  return result;
}

template <class T>
void TaskQueue<T>::NotifyOfCompletion() {
  Mutex::ScopedLock scoped_lock(lock_);
  if (--outstanding_tasks_ == 0) {
    tasks_drained_.Broadcast(scoped_lock);
  }
}

template <class T>
void TaskQueue<T>::BlockingDrain() {
  Mutex::ScopedLock scoped_lock(lock_);
  while (outstanding_tasks_ > 0) {
    tasks_drained_.Wait(scoped_lock);
  }
}

template <class T>
void TaskQueue<T>::Stop() {
  Mutex::ScopedLock scoped_lock(lock_);
  stopped_ = true;
  tasks_available_.Broadcast(scoped_lock);
}

template <class T>
std::queue<std::unique_ptr<T>> TaskQueue<T>::PopAll() {
  Mutex::ScopedLock scoped_lock(lock_);
  std::queue<std::unique_ptr<T>> result;
  result.swap(task_queue_);
  return result;
}

}  // namespace node
