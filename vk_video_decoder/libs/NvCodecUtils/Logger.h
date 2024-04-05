/*
* Copyright 2017-2018 NVIDIA Corporation.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <mutex>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#elif defined(ERROR)
#undef ERROR // Windows NT has defined ERROR
#endif


namespace simplelogger{

enum LogLevel {
    TRACE,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class Logger {
public:
    Logger(LogLevel level, bool bPrintTimeStamp) : m_level(level), m_bPrintTimeStamp(bPrintTimeStamp) {}
    virtual ~Logger() {}
    virtual std::ostream& GetStream() = 0;
    virtual void FlushStream() {}
    bool ShouldLogFor(LogLevel l) {
        return l >= m_level;
    }
    char* GetLead(LogLevel l, const char*, int, const char*) {
        if (l < TRACE || l > FATAL) {
            sprintf(m_szLead, "[?????] ");
            return m_szLead;
        }
        const char *szLevels[] = {"TRACE", "INFO", "WARN", "ERROR", "FATAL"};
        if (m_bPrintTimeStamp) {
            time_t t = time(NULL);
            struct tm *ptm = localtime(&t);
            sprintf(m_szLead, "[%-5s][%02d:%02d:%02d] ",
                szLevels[l], ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
        } else {
            sprintf(m_szLead, "[%-5s] ", szLevels[l]);
        }
        return m_szLead;
    }
    void EnterCriticalSection() {
        m_mtx.lock();
    }
    void LeaveCriticalSection() {
        m_mtx.unlock();
    }
private:
    LogLevel m_level;
    char m_szLead[80];
    bool m_bPrintTimeStamp;
    std::mutex m_mtx;
};

class LoggerFactory {
public:
    static Logger* CreateFileLogger(std::string strFilePath,
            LogLevel level = ERROR, bool bPrintTimeStamp = true) {
        return new FileLogger(strFilePath, level, bPrintTimeStamp);
    }
    static Logger* CreateConsoleLogger(LogLevel level = ERROR,
            bool bPrintTimeStamp = true) {
        return new ConsoleLogger(level, bPrintTimeStamp);
    }

private:
    LoggerFactory() {}

    class FileLogger : public Logger {
    public:
        FileLogger(std::string strFilePath, LogLevel level, bool bPrintTimeStamp)
        : Logger(level, bPrintTimeStamp) {
            pFileOut = new std::ofstream();
            pFileOut->open(strFilePath.c_str());
        }
        ~FileLogger() {
            pFileOut->close();
        }
        std::ostream& GetStream() {
            return *pFileOut;
        }
    private:
        std::ofstream *pFileOut;
    };

    class ConsoleLogger : public Logger {
    public:
        ConsoleLogger(LogLevel level, bool bPrintTimeStamp)
        : Logger(level, bPrintTimeStamp) {}
        std::ostream& GetStream() {
            return std::cout;
        }
    };

};

class LogTransaction {
public:
    LogTransaction(Logger *pLogger, LogLevel level, const char *szFile, const int nLine, const char *szFunc) : m_pLogger(pLogger), m_level(level) {
        if (!pLogger) {
            std::cout << "[-----] ";
            return;
        }
        if (!pLogger->ShouldLogFor(level)) {
            return;
        }
        pLogger->EnterCriticalSection();
        pLogger->GetStream() << pLogger->GetLead(level, szFile, nLine, szFunc);
    }
    ~LogTransaction() {
        if (!m_pLogger) {
            std::cout << std::endl;
            return;
        }
        if (!m_pLogger->ShouldLogFor(m_level)) {
            return;
        }
        m_pLogger->GetStream() << std::endl;
        m_pLogger->FlushStream();
        m_pLogger->LeaveCriticalSection();
        if (m_level == FATAL) {
            exit(1);
        }
    }
    std::ostream& GetStream() {
        if (!m_pLogger) {
            return std::cout;
        }
        if (!m_pLogger->ShouldLogFor(m_level)) {
            return ossNull;
        }
        return m_pLogger->GetStream();
    }
private:
    Logger *m_pLogger;
    LogLevel m_level;
    std::ostringstream ossNull;
};

}

extern simplelogger::Logger *logger;
#define LOG(level) simplelogger::LogTransaction(logger, simplelogger::level, __FILE__, __LINE__, __FUNCTION__).GetStream()
