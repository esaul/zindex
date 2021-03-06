#pragma once

#include "LineIndexer.h"
#include "Log.h"
#include "Pipe.h"

#include <unistd.h>
#include <string>

class ExternalIndexer : public LineIndexer {
    Log &log_;
    pid_t childPid_;
    Pipe sendPipe_;
    Pipe receivePipe_;
    char separator_;

public:
    ExternalIndexer(Log &log, const std::string &command, char separator);
    ~ExternalIndexer();

    ExternalIndexer(const ExternalIndexer &) = delete;
    ExternalIndexer &operator=(const ExternalIndexer &) = delete;

    void index(IndexSink &sink, StringView line) override;

private:
    void runChild(const std::string &command);
};


