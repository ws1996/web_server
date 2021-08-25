#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

template< typename T >
class threadpool
{
public:
    threadpool( int thread_number = 8, int max_requests = 10000 );
    ~threadpool();
    bool append( T* request );

private:
    static void* worker( void* arg );
    void run();

private:
    int m_thread_number;
    int m_max_requests;
    pthread_t* m_threads;
    std::list< T* > m_workqueue;
    locker m_queuelocker;
    sem m_queuestat;
    bool m_stop;
};
// 线程池的构造函数
template< typename T >
threadpool< T >::threadpool( int thread_number, int max_requests ) : 
        m_thread_number( thread_number ), m_max_requests( max_requests ), m_stop( false ), m_threads( NULL )
{
	// 首先检查输入参数
    if( ( thread_number <= 0 ) || ( max_requests <= 0 ) )
    {
        throw std::exception();
    }
	// 存放线程的数组
    m_threads = new pthread_t[ m_thread_number ];
    if( ! m_threads )
    {
        throw std::exception();
    }

    for ( int i = 0; i < thread_number; ++i )
    {
        printf( "create the %dth thread\n", i );
		// 创建线程
        if( pthread_create( m_threads + i, NULL, worker, this ) != 0 )
        {
            delete [] m_threads;
            throw std::exception();
        }
		// 线程脱离
        if( pthread_detach( m_threads[i] ) )
        {
            delete [] m_threads;
            throw std::exception();
        }
    }
}
// 线程池的析构函数
template< typename T >
threadpool< T >::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}
// 向工作队列添加任务
template< typename T >
bool threadpool< T >::append( T* request )
{
    m_queuelocker.lock();
	// 首先检查工作队列中的任务数是否超过最大限制
    if ( m_workqueue.size() > m_max_requests )
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back( request );
    m_queuelocker.unlock();
	// 信号量解锁,工作线程将会从任务队列中取出任务并进行响应
    m_queuestat.post();
    return true;
}
// 线程的工作函数
template< typename T >
void* threadpool< T >::worker( void* arg )
{
	// 传入参数为该线程池本身
    threadpool* pool = ( threadpool* )arg;
	// 运行线程池的函数
    pool->run();
    return pool;
}
// 线程池的运行函数
template< typename T >
void threadpool< T >::run()
{
	// 如果停止变量为false则一直运行
    while ( ! m_stop )
    {
		// 对任务标志变量加锁,并阻塞在这里,直到有任务需要处理
        m_queuestat.wait();
		// 对保护请求队列的互斥变量加锁
        m_queuelocker.lock();
		// 首先检查请求队列是否为空
        if ( m_workqueue.empty() )
        {
            m_queuelocker.unlock();
            continue;
        }
		// 取出请求
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
		// 如果请求为空
        if ( ! request )
        {
            continue;
        }
		// 响应请求,调用任务类的接口函数
        request->process();
    }
}

#endif
