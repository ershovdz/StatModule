class  StatBlock;
typedef StatBlock* STAT_HANDLE;

#include <stdint.h>

class StatModule
{
public:
	StatModule();
	~StatModule();

	void SetInterval(uint64_t secs);
	STAT_HANDLE AddStat(const char* szName);
	void AddCallInfo(STAT_HANDLE h, uint64_t callDuration);
	uint64_t GetCallCount(STAT_HANDLE h);
	uint64_t GetAvgDuration(STAT_HANDLE h);
	uint64_t GetMaxDuration(STAT_HANDLE h);
	uint64_t GetMinDuration(STAT_HANDLE h);

private:
	uint64_t _interval;
	class StatBlockStorage;
	StatBlockStorage* pStorage;
};

