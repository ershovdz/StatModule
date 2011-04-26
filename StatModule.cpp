#include "StatModule.h"


#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/interprocess/sync/named_semaphore.hpp>
#include "boost/date_time/posix_time/posix_time.hpp"
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/interprocess/exceptions.hpp>

using namespace boost::posix_time;
using namespace boost::interprocess;

/***********************************************
* class StatElement
*
* Stores statistic for period = interval/bufSize
************************************************/

class StatElement
{
	friend class StatBlock;

protected:
	StatElement()
	: _totalDuration(0)
	, _minDuration(-1)
	, _maxDuration(0)
	, _callsCounter(0)
	{
	};
	StatElement(uint64_t totalDuration, uint64_t minDuration, uint64_t maxDuration, uint64_t callsCounter)
	: _totalDuration(totalDuration)
	, _minDuration(minDuration)
	, _maxDuration(maxDuration)
	, _callsCounter(callsCounter)
	{
	};

protected:
	uint64_t _totalDuration;
	uint64_t _minDuration;
	uint64_t _maxDuration;
	uint64_t _callsCounter;
};


typedef allocator<StatElement, managed_shared_memory::segment_manager>  ShmStatElementAllocator;
typedef boost::circular_buffer<StatElement, ShmStatElementAllocator>    ShmStatElementsBuffer;
typedef scoped_lock<interprocess_mutex>									ScopedLocker;
typedef scoped_lock<named_mutex>										NamedMutexLocker;

/****************************************************
* class StatBlock
*
* Stores StatElement objects for given block
* and provides functionality for API of StatModule
****************************************************/
class StatBlock
{
public:
	/*
	We must achieve the following precision: interval +- 10%.
	So bufSize has to be 100% / 10% = 10 elements
	 */
	StatBlock(uint64_t interval, const ShmStatElementAllocator& ac, uint64_t bufSize = 10);

	void AddCallInfo(uint64_t callDuration);
	uint64_t GetCallCount();
	uint64_t GetAvgDuration();
	uint64_t GetMaxDuration();
	uint64_t GetMinDuration();

private:
	StatBlock(const StatBlock&);
	uint64_t UpdateBuffer();

private:
	ShmStatElementsBuffer _statBuf;
	ptime _startTime;
	interprocess_mutex _blockMutex; //may be spinlock is more suitable here
	const uint64_t _interval;
	const uint64_t _bufSize;
};

/*********************************************
* class StatBlockStorage
*
* Stores globals StatBlock objects
* and provides wrapper for shared memory
*********************************************/

class StatModule::StatBlockStorage
{
public:
	static StatBlockStorage* CreateStorage();
	static void RemoveStorage();
	StatBlock* AddStat(const char* szName, uint64_t interval);

private:
	StatBlockStorage(void);
	StatBlockStorage(const StatBlockStorage&) {};

private:
	managed_shared_memory 		_shmSegment;
	uint64_t 					_localRefCounter;
	uint64_t volatile* 			_pInterprocessRefCounter; //pointer to counter, stored in shared memory

	static StatBlockStorage* 	_pStorageInstance;
	static const uint64_t 		SEGMENT_SIZE;
};



/*******************************
class StatBlock
 ********************************/

StatBlock::StatBlock(uint64_t interval, const ShmStatElementAllocator& ac, uint64_t bufSize)
: _statBuf(bufSize, ac)
, _startTime(boost::posix_time::microsec_clock::local_time())
, _interval(interval*1000000) //interval is in seconds, but we use microseconds
, _bufSize(bufSize)
{
	printf("\n CREATE NEW StatBlock\n");

	for(uint64_t i = 0; i < bufSize; ++i)
	{
		_statBuf.push_back(StatElement());
	}
};

uint64_t StatBlock::UpdateBuffer()
{
	ptime callTime(boost::posix_time::microsec_clock::local_time());
	uint64_t cur_interval = (callTime - _startTime).total_microseconds();
	uint64_t shift = ((double)cur_interval/_interval)*_bufSize;

	/**********************************************
	 * CASE 1:
	 *	     ____ <== _startTime
	 *	    |____|
	 *_statBuf  |____|<== cur_interval is here now
	 *	    |____|
	 *	    |____|<== _startTime + _interval
	 ***********************************************/
	if(shift < _bufSize)
		return shift;

	/**********************************************
	 * CASE 2:
	 *	     _____ <== _startTime
	 *	    |stale|
	 *	    |stale|
	 *	    |_____|
	 *_statBuf  |_____|<== _startTime + _interval
	 *	    |_____|
	 *	    |_____|<== cur_interval is here now
	 *	    |_____|
	 *	    |_____|<== _startTime + 2*_interval
	 ***********************************************/
	if(cur_interval - _interval  < _interval)
	{
		//printf("\n\n CASE 2 \n\n");

		shift -= _bufSize - 1;
		for(uint64_t i = 0; i < shift; ++i)
		{
			_statBuf.push_back(StatElement()); //remove stale StatElement and insert new
		}

		_startTime += microseconds(shift*(_interval/_bufSize));
		return _bufSize;
	}
	//printf("\n\n CASE 3 \n\n");
	/******************************************************************************
	 * CASE 3:
	 *	      _____ <== _startTime                                    ____ <== _startTime,
	 *	     |stale|					             |____|<== cur_interval is here now
	 *	     |stale|						     |____|
	 *	     |stale|						     |____|
	 *_statBuf   |stale|<== _startTime + _interv                         |____|
	 *	     |stale|						     |____|
	 *	     |stale|                              Clear _statBuf     |____|<== _startTime + _interval
	 *	     |stale|				  ==============>
	 *	     |stale|<== _startTime + 2*_interval
	 *	     |_____|
	 *	     |_____|<== cur_interval is here now
	 *	     |_____|
	 *	     |_____|<== _startTime + 3*_interval
	 ********************************************************************************/
	for(uint64_t i = 0; i < _bufSize; ++i)
	{
		_statBuf[i]._callsCounter	= 0;
		_statBuf[i]._totalDuration	= 0;
		_statBuf[i]._minDuration	= -1;
		_statBuf[i]._maxDuration	= 0;
	}

	_startTime += microseconds(shift*(_interval/_bufSize));
	return shift;
}

void StatBlock::AddCallInfo(uint64_t callDuration)
{
	ScopedLocker lock(_blockMutex);

	uint64_t shift = UpdateBuffer();

	if(shift < _bufSize) //case 1
	{
		_statBuf[shift]._callsCounter++;
		_statBuf[shift]._totalDuration += callDuration;
		if(_statBuf[shift]._minDuration > callDuration)
			_statBuf[shift]._minDuration = callDuration;
		else if(_statBuf[shift]._maxDuration < callDuration)
			_statBuf[shift]._maxDuration = callDuration;
	}
	else if(shift  == _bufSize) //case 2
	{
		_statBuf[_bufSize - 1]._callsCounter  = 1;
		_statBuf[_bufSize - 1]._totalDuration = callDuration;
		_statBuf[_bufSize - 1]._minDuration   = callDuration;
		_statBuf[_bufSize - 1]._maxDuration   = callDuration;
	}
	else if(shift  > _bufSize) //case 3, buffer is clear now
	{
		_statBuf[0]._callsCounter  = 1;
		_statBuf[0]._totalDuration = callDuration;
		_statBuf[0]._minDuration   = callDuration;
		_statBuf[0]._maxDuration   = callDuration;
	}
}

uint64_t StatBlock::GetAvgDuration()
{
	ScopedLocker lock(_blockMutex);

	UpdateBuffer();

	uint64_t totalTime = 0;
	uint64_t totalCallCount = 0;

	for(uint64_t i = 0; i < _bufSize; ++i)
	{
		totalTime  	+= _statBuf[i]._totalDuration;
		totalCallCount 	+= _statBuf[i]._callsCounter;
	}

	if(totalCallCount)
	{
		return totalTime/totalCallCount;
	}
	return 0;
}

uint64_t StatBlock::GetMaxDuration()
{
	ScopedLocker lock(_blockMutex);

	UpdateBuffer();

	uint64_t maxDuration = 0;

	for(uint64_t i = 0; i < _bufSize; ++i)
	{
		if(maxDuration <  _statBuf[i]._maxDuration)
			maxDuration =  _statBuf[i]._maxDuration;
	}
	return maxDuration;
}

uint64_t StatBlock::GetMinDuration()
{
	ScopedLocker lock(_blockMutex);

	UpdateBuffer();

	uint64_t minDuration = _statBuf[0]._minDuration;

	for(uint64_t i = 1; i < _bufSize; ++i)
	{
		if(minDuration >  _statBuf[i]._minDuration)
			minDuration =  _statBuf[i]._minDuration;
	}
	if(minDuration == uint64_t(-1))
		return 0;

	return minDuration;
}

uint64_t StatBlock::GetCallCount()
{
	ScopedLocker lock(_blockMutex);

	UpdateBuffer();

	uint64_t totalCallCounter = 0;

	for(uint64_t i = 0; i < _bufSize; i++)
	{
		totalCallCounter += _statBuf[i]._callsCounter;
	}
	return totalCallCounter;
}

/*******************************
class StatBlockStorage
 ********************************/
StatModule::StatBlockStorage* StatModule::StatBlockStorage::_pStorageInstance = 0;

//we are going to store up to 1000 stat block. Size of each one is ~600 bytes
//Let's set segment size to 2 MB, just in case
const uint64_t StatModule::StatBlockStorage::SEGMENT_SIZE = 2091008;

StatModule::StatBlockStorage::StatBlockStorage()
:_shmSegment(open_or_create, "STATBLOCK_STORAGE", SEGMENT_SIZE) // can throw
, _localRefCounter(1)
, _pInterprocessRefCounter(0)
{
}

StatModule::StatBlockStorage* StatModule::StatBlockStorage::CreateStorage()
{
	named_mutex storageMutex(open_or_create, "STATBLOCK_STORAGE_MUTEX");
	NamedMutexLocker lock(storageMutex);

	try
	{
		if(_pStorageInstance)
		{
			_pStorageInstance->_localRefCounter++;
			printf("\nCurrent thread counter = %llu\n", _pStorageInstance->_localRefCounter);
			return _pStorageInstance;
		}

		_pStorageInstance = new StatBlockStorage();

		// increment interprocess ref counter
		_pStorageInstance->_pInterprocessRefCounter = _pStorageInstance->_shmSegment.find_or_construct<uint64_t>("STATBLOCK_STORAGE_REF_COUNTER")(0);
		(*(_pStorageInstance->_pInterprocessRefCounter))++;

		printf("Current proc counter = %llu\n", *(_pStorageInstance->_pInterprocessRefCounter));
	}
	catch(...) // could not create storage or obtain ref counter
	{
		if(_pStorageInstance) //we have created a first instance, so we can delete it
		{
			try
			{
				delete _pStorageInstance;
			}
			catch(...) //shit happens
			{
			}
			_pStorageInstance = 0;
		}
	}

	printf("\n STORAGE IS CREATED \n");
	return _pStorageInstance;
}

void StatModule::StatBlockStorage::RemoveStorage()
{
	named_mutex storageMutex(open_or_create, "STATBLOCK_STORAGE_MUTEX");
	NamedMutexLocker lock(storageMutex);

	try
	{
		if(_pStorageInstance)
		{
			_pStorageInstance->_localRefCounter--;
			printf("\nCurrent thread counter = %llu\n\n", _pStorageInstance->_localRefCounter);

			if(0 == _pStorageInstance->_localRefCounter) // last thread
			{
				(*(_pStorageInstance->_pInterprocessRefCounter))--;
				printf("\nCurrent proc counter = %llu\n", *(_pStorageInstance->_pInterprocessRefCounter));
				if(0 == *(_pStorageInstance->_pInterprocessRefCounter)) // last process
				{
					shared_memory_object::remove("STATBLOCK_STORAGE");

					printf("\n SHARED MEMORY IS REMOVED \n");
				}
				delete _pStorageInstance;
				_pStorageInstance = 0;

				printf("\n\n STORAGE IS REMOVED \n\n");
			}
		}
	}
	catch(...) //
	{
	}
}

StatBlock* StatModule::StatBlockStorage::AddStat(const char* szName, uint64_t interval)
{
	named_mutex storageMutex(open_or_create, "STATBLOCK_STORAGE_MUTEX");
	NamedMutexLocker lock(storageMutex);

	const ShmStatElementAllocator allocator(_shmSegment.get_segment_manager());
	return _shmSegment.find_or_construct<StatBlock>(szName)(interval, allocator, 10);
};

/*******************************
class StatModule
 ********************************/
StatModule::StatModule(): _interval(600) // 10 minutes, by default
{
	pStorage = StatModule::StatBlockStorage::CreateStorage();
}

StatModule::~StatModule()
{
	StatModule::StatBlockStorage::RemoveStorage();
	pStorage = 0;
}

void StatModule::SetInterval(uint64_t interval)
{
	_interval = interval;
}

STAT_HANDLE StatModule::AddStat(const char* szName)
{
	try
	{
		return pStorage->AddStat(szName, _interval);
	}
	catch(...)
	{
	}
	return 0;
}
void StatModule::AddCallInfo(STAT_HANDLE h, uint64_t callDuration)
{
	h->AddCallInfo(callDuration);
}
uint64_t StatModule::GetCallCount(STAT_HANDLE h)
{
	return h->GetCallCount();
}
uint64_t StatModule::GetAvgDuration(STAT_HANDLE h)
{
	return h->GetAvgDuration();
}
uint64_t StatModule::GetMaxDuration(STAT_HANDLE h)
{
	return h->GetMaxDuration();
}
uint64_t StatModule::GetMinDuration(STAT_HANDLE h)
{
	return h->GetMinDuration();
}
