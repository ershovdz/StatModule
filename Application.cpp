#include "StatModule.h"

#include "boost/date_time/posix_time/posix_time.hpp"
using namespace boost::posix_time;
#include <boost/thread/thread.hpp>
#include <stdio.h>
#include <cstdlib>
#include <pthread.h>

void thread_func(int timeout, int dur, const char* szName)
{
	sleep(timeout);
	

	StatModule statModule;
	statModule.SetInterval(4000);

	STAT_HANDLE h = statModule.AddStat(szName);

	boost::posix_time::ptime t1(microsec_clock::local_time());

	uint64_t i = 0;
	for(i = 0; i < 700000; i++)
	{
	//	//boost::posix_time::ptime t2(microsec_clock::local_time());
		statModule.AddCallInfo(h, dur);
	}

	printf("\n Call name = %s\n\n", szName);
	printf("Call number = %llu\n", statModule.GetCallCount(h));
	printf("Call avg duration = %llu\n", statModule.GetAvgDuration(h));
	printf("Call min duration = %llu\n", statModule.GetMinDuration(h));
	printf("Call max duration = %llu\n", statModule.GetMaxDuration(h));
	boost::posix_time::ptime t2(microsec_clock::local_time());
	time_duration td = t2 - t1;
	std::cout << "Time duration: " << td << std::endl;
}

int main()
{

	boost::thread_group th_group;

	th_group.create_thread(boost::bind(thread_func, 1, 10, "test func1 call"));
//	th_group.create_thread(boost::bind(thread_func, 2, 10, "test func1 call"));
//	th_group.create_thread(boost::bind(thread_func, 3, 10, "test func1 call"));
//	th_group.create_thread(boost::bind(thread_func, 4, 10, "test func3 call"));
//	th_group.create_thread(boost::bind(thread_func, 5, 10, "test func2 call"));
//	th_group.create_thread(boost::bind(thread_func, 6, 10, "test func4 call"));
//	th_group.create_thread(boost::bind(thread_func, 7, 10, "test func3 call"));
//	th_group.create_thread(boost::bind(thread_func, 8, 10, "test func3 call"));
//	th_group.create_thread(boost::bind(thread_func, 9, 10, "test func4 call"));

	th_group.join_all();
	printf("\n TIME OUT \n\n");
	sleep(10);

	return 0;
}
