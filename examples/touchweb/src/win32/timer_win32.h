/* timer_win32.h - Agave application
 *
 * Copyright (C) 2010 Zhang Ji Peng
 * Contact : onecoolx@gmail.com
 */

#ifndef _TIMER_WIN32_H_
#define _TIMER_WIN32_H_

#include <windows.h>

class Timer;

class TimerImpl
{
public:
    TimerImpl(Timer* t);
    ~TimerImpl();

    void start(unsigned int m, void* d);
    void stop(void);

    virtual void OnTimer(void);
private:
    Timer* timer;
    UINT_PTR id;
    void* data;
};

#endif/*_TIMER_WIN32_H_*/
