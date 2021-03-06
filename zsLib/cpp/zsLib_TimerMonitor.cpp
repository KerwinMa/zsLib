/*
 *  Created by Robin Raymond.
 *  Copyright 2009-2013. Robin Raymond. All rights reserved.
 *
 * This file is part of zsLib.
 *
 * zsLib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (LGPL) as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * zsLib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with zsLib; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include <zsLib/internal/zsLib_TimerMonitor.h>

#include <zsLib/Timer.h>
#include <zsLib/XML.h>
#include <zsLib/helpers.h>

#include <boost/thread.hpp>

#include <pthread.h>

#ifdef __QNX__
#include <sys/time.h>
#endif //

namespace zsLib {ZS_DECLARE_SUBSYSTEM(zsLib)}

namespace zsLib
{
  namespace internal
  {
    //-------------------------------------------------------------------------
    static MonitorPriorityHelper &getTimerMonitorPrioritySingleton()
    {
      static Singleton<MonitorPriorityHelper, false> singleton;
      return singleton.singleton();
    }

    //-------------------------------------------------------------------------
    TimerMonitor::TimerMonitor() :
      mShouldShutdown(false)
    {
#ifdef __QNX__
      static pthread_cond_t defaultCondition = PTHREAD_COND_INITIALIZER;
      static pthread_mutex_t defaultMutex = PTHREAD_MUTEX_INITIALIZER;

      memcpy(&mCondition, &defaultCondition, sizeof(mCondition));
      memcpy(&mMutex, &defaultMutex, sizeof(mMutex));
#endif //__QNX__
      ZS_LOG_DETAIL(log("created"))
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::init()
    {
    }

    //-------------------------------------------------------------------------
    TimerMonitor::~TimerMonitor()
    {
      mThisWeak.reset();
      ZS_LOG_DETAIL(log("destroyed"))
      cancel();

#ifdef __QNX__
      pthread_cond_destroy(&mCondition);
      pthread_mutex_destroy(&mMutex);
#endif //__QNX__
    }

    //-------------------------------------------------------------------------
    TimerMonitorPtr TimerMonitor::singleton()
    {
      static SingletonLazySharedPtr<TimerMonitor> singleton(TimerMonitor::create());
      class Once {
      public: Once() {getTimerMonitorPrioritySingleton().notify();}
      };
      static Once once;

      TimerMonitorPtr result = singleton.singleton();
      if (!result) {
        ZS_LOG_WARNING(Detail, slog("singleton gone"))
      }
      return result;
    }

    //-------------------------------------------------------------------------
    TimerMonitorPtr TimerMonitor::create()
    {
      TimerMonitorPtr pThis(new TimerMonitor);
      pThis->mThisWeak = pThis;
      pThis->init();
      return pThis;
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::setPriority(ThreadPriorities priority)
    {
      MonitorPriorityHelper &prioritySingleton = getTimerMonitorPrioritySingleton();

      bool changed = prioritySingleton.setPriority(priority);
      if (!changed) return;

      if (!prioritySingleton.wasNotified()) return;

      TimerMonitorPtr singleton = TimerMonitor::singleton();
      if (!singleton) return;

      AutoRecursiveLock lock(singleton->mLock);
      if (!singleton->mThread) return;

      setThreadPriority(*singleton->mThread, priority);
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::monitorBegin(TimerPtr timer)
    {
      AutoRecursiveLock lock(mLock);

      if (!mThread) {
        mThread = ThreadPtr(new boost::thread(boost::ref(*this)));
        setThreadPriority(mThread->native_handle(), getTimerMonitorPrioritySingleton().getPriority());
      }

      PUID timerID = timer->getID();
      mMonitoredTimers[timerID] = TimerWeakPtr(timer);
      wakeUp();
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::monitorEnd(zsLib::Timer &timer)
    {
      AutoRecursiveLock lock(mLock);

      PUID timerID = timer.getID();

      TimerMap::iterator result = mMonitoredTimers.find(timerID);   // if the timer was scheduled to be monitored, then need to remove it from being monitored
      if (mMonitoredTimers.end() == result)
        return;

      mMonitoredTimers.erase(result);
      wakeUp();
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::operator()()
    {
      bool shouldShutdown = false;

#ifdef __QNX__
      pthread_setname_np(pthread_self(), "com.zslib.timer");
#else
#ifndef _LINUX
#ifndef _ANDROID
      pthread_setname_np("com.zslib.timer");
#endif // _ANDROID
#endif // _LINUX
#endif // __QNX__


      do
      {
        Duration duration;
        // wait completed, do notifications from select
        {
          AutoRecursiveLock lock(mLock);
          shouldShutdown = mShouldShutdown;

          duration = fireTimers();
        }

#ifdef __QNX__
        struct timeval tp;
        struct timespec ts;
        memset(&tp, 0, sizeof(tp));
        memset(&ts, 0, sizeof(ts));

        int rc =  gettimeofday(&tp, NULL);
        ZS_THROW_BAD_STATE_IF(0 != rc)

        // Convert from timeval to timespec
        ts.tv_sec  = tp.tv_sec;
        ts.tv_nsec = tp.tv_usec * 1000;

        // add the time to expire from now
        ts.tv_sec += duration.seconds();
        ts.tv_nsec += ((duration - Seconds(duration.total_seconds()))).total_nanoseconds();

        // this could have caused tv_nsec to wrapped above second mark since it started in absolute time since epoch
        if (ts.tv_nsec >= (Seconds(1).total_nanoseconds())) {
          Duration wrapSeconds = Seconds(ts.tv_nsec / (Seconds(1).total_nanoseconds()));
          ts.tv_sec += wrapSeconds.total_seconds();
          ts.tv_nsec -= wrapSeconds.total_nanoseconds();
        }

        rc = pthread_mutex_lock(&mMutex);
        ZS_THROW_BAD_STATE_IF(0 != rc)

        rc = pthread_cond_timedwait(&mCondition, &mMutex, &ts);
        ZS_THROW_BAD_STATE_IF((0 != rc) && (ETIMEDOUT != rc))

        rc = pthread_mutex_unlock(&mMutex);
        ZS_THROW_BAD_STATE_IF(0 != rc)
#else
        boost::unique_lock<boost::mutex> flagLock(mFlagLock);
        mFlagNotify.timed_wait<Duration>(flagLock, duration);
#endif //__QNX__

        // notify all those timers needing to be notified
      } while (!shouldShutdown);

      TimerMonitorPtr gracefulReference;

      {
        AutoRecursiveLock lock(mLock);

        // transfer graceful shutdown reference to thread
        gracefulReference = mGracefulShutdownReference;
        mGracefulShutdownReference.reset();

        // go through timers and cancel them completely
        for (TimerMap::iterator monIter = mMonitoredTimers.begin(); monIter != mMonitoredTimers.end(); ) {
          TimerMap::iterator current = monIter;
          ++monIter;

          TimerPtr timer = current->second.lock();
          if (timer)
            timer->background(false);
        }
        mMonitoredTimers.clear();
      }
    }

    //-----------------------------------------------------------------------
    static void debugAppend(zsLib::XML::ElementPtr &parentEl, const char *name, const char *value)
    {
      ZS_DECLARE_TYPEDEF_PTR(zsLib::XML::Element, Element)
      ZS_DECLARE_TYPEDEF_PTR(zsLib::XML::Text, Text)

      ZS_THROW_INVALID_ARGUMENT_IF(!parentEl)
      ZS_THROW_INVALID_ARGUMENT_IF(!name)

      if (!value) return;
      if ('\0' == *value) return;

      ElementPtr element = Element::create(name);

      TextPtr tmpTxt = Text::create();
      tmpTxt->setValueAndJSONEncode(value);
      element->adoptAsFirstChild(tmpTxt);

      parentEl->adoptAsLastChild(element);
    }

    //-----------------------------------------------------------------------
    zsLib::Log::Params TimerMonitor::log(const char *message) const
    {
      ElementPtr objectEl = Element::create("TimerMonitor");

      ElementPtr element = Element::create("id");

      TextPtr tmpTxt = Text::create();
      tmpTxt->setValueAndJSONEncode(string(mID));
      element->adoptAsFirstChild(tmpTxt);

      objectEl->adoptAsLastChild(element);

      return zsLib::Log::Params(message, objectEl);
    }

    //-----------------------------------------------------------------------
    zsLib::Log::Params TimerMonitor::slog(const char *message)
    {
      return zsLib::Log::Params(message, "TimerMonitor");
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::cancel()
    {
      ThreadPtr thread;
      {
        AutoRecursiveLock lock(mLock);
        mGracefulShutdownReference = mThisWeak.lock();
        thread = mThread;

        mShouldShutdown = true;
        wakeUp();
      }

      if (!thread)
        return;

      thread->join();

      {
        AutoRecursiveLock lock(mLock);
        mThread.reset();
      }
    }

    //-------------------------------------------------------------------------
    Duration TimerMonitor::fireTimers()
    {
      AutoRecursiveLock lock(mLock);

      Time time = boost::posix_time::microsec_clock::universal_time();

      Duration duration = boost::posix_time::seconds(1);

      for (TimerMap::iterator monIter = mMonitoredTimers.begin(); monIter != mMonitoredTimers.end(); )
      {
        TimerMap::iterator current = monIter;
        ++monIter;

        TimerPtr timer = (current->second).lock();
        bool done = true;

        if (timer)
          done = timer->tick(time, duration);

        if (done)
          mMonitoredTimers.erase(current);
      }
      return duration;
    }

    //-------------------------------------------------------------------------
    void TimerMonitor::wakeUp()
    {
#ifdef __QNX__
      int rc = pthread_mutex_lock(&mMutex);
      ZS_THROW_BAD_STATE_IF(0 != rc)

      rc = pthread_cond_signal(&mCondition);
      ZS_THROW_BAD_STATE_IF(0 != rc)

      rc = pthread_mutex_unlock(&mMutex);
      ZS_THROW_BAD_STATE_IF(0 != rc)

#else
      mFlagNotify.notify_one();
#endif //__QNX__
    }
  }
}
