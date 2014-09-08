#ifndef __POSIX_LOGGER_H_
#define __POSIX_LOGGER_H_

#include <algorithm>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include "env.h"

namespace leveldb{
//POSIXģʽ�µ���־ʵ��
class PosixLogger : public Logger
{
public:
	PosixLogger(FILE* f, uint64_t (*gettid)()) : file_(f), gettid_(gettid){};
	virtual ~PosixLogger()
	{
		fclose(file_);
	};

	virtual void Logv(const char* format, va_list ap)
	{
		const uint64_t thread_id = (*gettid_)();
		char buffer[500];
		for(int iter = 0; iter < 2; iter ++){
			char* base;
			int bufsize;
			if(iter == 0){
				bufsize = sizeof(buffer);
				base = buffer;
			}
			else{
				bufsize = 30000;
				base = new char[bufsize];
			}

			char* p = base;
			char* limit = base + bufsize;

			struct timeval now_tv;
			gettimeofday(&now_tv, NULL);
			const time_t seconds = now_tv.tv_sec;
			struct tm t;
			localtime_r(&seconds, &t);
			//�����־ʱ��
			p += snprintf(p, limit - p,
                    "%04d/%02d/%02d-%02d:%02d:%02d.%06d %llx ",
                    t.tm_year + 1900,
                    t.tm_mon + 1,
                    t.tm_mday,
                    t.tm_hour,
                    t.tm_min,
                    t.tm_sec,
                    static_cast<int>(now_tv.tv_usec),
                    static_cast<long long unsigned int>(thread_id));

			if(p < limit){
				if(iter == 0){ //����û����ȫƴ����ɣ�����һ������Ļ�����
					continue;
				}
				else {
					p = limit - 1;
				}
			}

			//�жϻ���
			if(p == base || p[-1] != '\n'){
				*p++ = '\n';
			}
			//�ļ�д��
			assert(p < limit);
			fwrite(base, 1, p - base, file_);
			fflush(file_);
			if(base != buffer){ //���¿������µĻ�����������־д��
				delete []base;
			}
			break;
		}
	}

private:
	FILE* file_;
	uint64_t (*gettid_)();
};

#endif

