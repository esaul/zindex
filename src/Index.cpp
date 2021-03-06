#include "Index.h"

#include "LineFinder.h"
#include "LineSink.h"
#include "LineIndexer.h"
#include "Sqlite.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <unordered_map>
#include "IndexSink.h"
#include "Log.h"
#include "StringView.h"
#include "PrettyBytes.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr auto DefaultIndexEvery = 32 * 1024 * 1024u;
constexpr auto WindowSize = 32768u;
constexpr auto ChunkSize = 16384u;
constexpr auto LogProgressEverySecs = 20;
constexpr auto Version = 1;

void seek(File &f, uint64_t pos) {
    auto err = ::fseek(f.get(), pos, SEEK_SET);
    if (err != 0)
        throw std::runtime_error("Error seeking in file"); // todo errno
}

struct ZlibError : std::runtime_error {
    ZlibError(int result) :
            std::runtime_error(
                    std::string("Error from zlib : ") + zError(result)) { }
};

void X(int zlibErr) {
    if (zlibErr != Z_OK) throw ZlibError(zlibErr);
}

size_t makeWindow(uint8_t *out, size_t outSize, const uint8_t *in,
                  uint64_t left) {
    uint8_t temp[WindowSize];
    // Could compress directly into out if I wasn't so lazy.
    if (left)
        memcpy(temp, in + WindowSize - left, left);
    if (left < WindowSize)
        memcpy(temp + left, in, WindowSize - left);
    uLongf destLen = outSize;
    X(compress2(out, &destLen, temp, WindowSize, 9));
    return destLen;
}

void uncompress(const std::vector<uint8_t> &compressed, uint8_t *to,
                size_t len) {
    uLongf destLen = len;
    X(::uncompress(to, &len, &compressed[0], compressed.size()));
    if (destLen != len)
        throw std::runtime_error("Unable to decompress a full window");
}

struct ZStream {
    z_stream stream;
    enum class Type : int {
        ZlibOrGzip = 47, Raw = -15,
    };

    explicit ZStream(Type type) {
        memset(&stream, 0, sizeof(stream));
        X(inflateInit2(&stream, (int)type));
    }

    ~ZStream() {
        (void)inflateEnd(&stream);
    }

    ZStream(ZStream &) = delete;

    ZStream &operator=(ZStream &) = delete;
};

struct IndexHandler : IndexSink {
    Log &log;
    std::unique_ptr<LineIndexer> indexer;
    uint64_t currentLine;

    IndexHandler(Log &log, std::unique_ptr<LineIndexer> indexer) :
            log(log), indexer(std::move(indexer)), currentLine(0) { }

    virtual ~IndexHandler() { }

    void onLine(uint64_t lineNumber, const char *line, size_t length) {
        try {
            currentLine = lineNumber;
            StringView stringView(line, length);
            log.debug("Indexing line '", stringView, "'");
            indexer->index(*this, stringView);
        } catch (const std::exception &e) {
            throw std::runtime_error(
                    "Failed to index line " + std::to_string(currentLine)
                    + ": '" + std::string(line, length) +
                    "' - " + e.what());
        }
    }
};

struct AlphaHandler : IndexHandler {
    Sqlite::Statement insert;

    AlphaHandler(Log &log, std::unique_ptr<LineIndexer> indexer,
                 Sqlite::Statement &&insert)
            : IndexHandler(log, std::move(indexer)),
              insert(std::move(insert)) { }

    void add(const char *index, size_t indexLength, size_t offset) override {
        auto key = std::string(index, indexLength);
        log.debug("Found key '", key, "'");
        insert
                .reset()
                .bindString(":key", key)
                .bindInt64(":line", currentLine)
                .bindInt64(":offset", offset)
                .step();
    }
};

struct NumericHandler : IndexHandler {
    Sqlite::Statement insert;

    NumericHandler(Log &log, std::unique_ptr<LineIndexer> indexer,
                   Sqlite::Statement &&insert)
            : IndexHandler(log, std::move(indexer)),
              insert(std::move(insert)) { }

    void add(const char *index, size_t indexLength, size_t offset) override {
        auto initIndex = index;
        auto initLen = indexLength;
        int64_t val = 0;
        bool negative = false;
        if (indexLength > 0 && *index == '-') {
            negative = true;
            indexLength--;
            index++;
        }
        if (indexLength == 0)
            throw std::invalid_argument("Non-numeric: empty string");
        while (indexLength) {
            val *= 10;
            if (*index < '0' || *index > '9')
                throw std::invalid_argument("Non-numeric: '"
                                            + std::string(initIndex, initLen) +
                                            "'");
            val += *index - '0';
            index++;
            indexLength--;
        }
        if (negative) val = -val;
        log.debug("Found key ", val);
        insert
                .reset()
                .bindInt64(":key", val)
                .bindInt64(":line", currentLine)
                .bindInt64(":offset", offset)
                .step();
    }
};

}

struct Index::Impl {
    Log &log_;
    File compressed_;
    Sqlite db_;
    Sqlite::Statement lineQuery_;
    Index::Metadata metadata_;

    Impl(Log &log, File &&fromCompressed, Sqlite &&db)
            : log_(log), compressed_(std::move(fromCompressed)),
              db_(std::move(db)),
              lineQuery_(db_.prepare(R"(
SELECT line, offset, compressedOffset, uncompressedOffset, length, bitOffset, window
FROM LineOffsets, AccessPoints
WHERE offset >= uncompressedOffset AND offset <= uncompressedEndOffset
AND line = :line
LIMIT 1)")) {
        try {
            auto queryMeta = db_.prepare("SELECT key, value FROM Metadata");
            for (; ;) {
                if (queryMeta.step()) break;
                auto key = queryMeta.columnString(0);
                auto value = queryMeta.columnString(1);
                log_.debug("Metadata: ", key, " = ", value);
                metadata_.emplace(key, value);
            }
        } catch (const std::exception &e) {
            log.warn("Caught exception reading metadata: ", e.what());
        }
    }

    void init(bool force) {
        struct stat stats;
        if (fstat(fileno(compressed_.get()), &stats) != 0) {
            throw std::runtime_error("Unable to get file stats"); // todo errno
        }
        auto sizeStr = std::to_string(stats.st_size);
        auto timeStr = std::to_string(stats.st_mtime);
        log_.debug("Opened compressde file of size ", sizeStr, " mtime ",
                   timeStr);
        if (metadata_.find("compressedSize") != metadata_.end()
            && sizeStr != metadata_.at("compressedSize")) {
            if (force) {
                log_.warn("Expected compressed size mismatched, "
                                  "continuing anyway (", stats.st_size,
                          " vs expected ",
                          metadata_.at("compressedSize"), ")");
            } else {
                throw std::runtime_error(
                        "Compressed size changed since index was built");
            }
        }
        if (metadata_.find("compressedModTime") != metadata_.end()
            && timeStr != metadata_.at("compressedModTime")) {
            if (force) {
                log_.warn("Expected compressed timestamp, continuing anyway");
            } else {
                throw std::runtime_error(
                        "Compressed file has been modified since index was built");
            }
        }
    }

    void getLine(uint64_t line, LineSink &sink) {
        lineQuery_.reset();
        lineQuery_.bindInt64(":line", line);
        if (lineQuery_.step()) return;
        print(lineQuery_, sink);
    }

    void queryIndex(const std::string &index, const std::string &query,
                    LineFunction lineFunc) {
        auto stmt = db_.prepare(R"(
SELECT line FROM index_)" + index + R"(
WHERE key = :query
)");
        stmt.bindString(":query", query);
        for (; ;) {
            if (stmt.step()) return;
            lineFunc(stmt.columnInt64(0));
        }
    }

    size_t indexSize(const std::string &index) const {
        auto stmt = db_.prepare("SELECT COUNT(*) FROM index_" + index);
        if (stmt.step()) return 0;
        return stmt.columnInt64(0);
    }

    void print(Sqlite::Statement &q, LineSink &sink) {
        auto line = q.columnInt64(0);
        auto offset = q.columnInt64(1);
        auto compressedOffset = q.columnInt64(2);
        auto uncompressedOffset = q.columnInt64(3);
        auto length = q.columnInt64(4);
        auto bitOffset = q.columnInt64(5);
        uint8_t window[WindowSize];
        uncompress(q.columnBlob(6), window, WindowSize);

        ZStream zs(ZStream::Type::Raw);
        uint8_t input[ChunkSize];
        uint8_t discardBuffer[WindowSize];

        seek(compressed_, bitOffset ? compressedOffset - 1 : compressedOffset);
        if (bitOffset) {
            auto c = fgetc(compressed_.get());
            if (c == -1)
                throw ZlibError(ferror(compressed_.get()) ?
                                Z_ERRNO : Z_DATA_ERROR);
            X(inflatePrime(&zs.stream, bitOffset, c >> (8 - bitOffset)));
        }
        X(inflateSetDictionary(&zs.stream, &window[0], WindowSize));

        uint8_t lineBuf[length];
        auto numToSkip = offset - uncompressedOffset;
        bool skipping = true;
        zs.stream.avail_in = 0;
        do {
            if (numToSkip == 0 && skipping) {
                zs.stream.avail_out = length;
                zs.stream.next_out = lineBuf;
                skipping = false;
            }
            if (numToSkip > WindowSize) {
                zs.stream.avail_out = WindowSize;
                zs.stream.next_out = discardBuffer;
                numToSkip -= WindowSize;
            } else if (numToSkip) {
                zs.stream.avail_out = (uInt)numToSkip;
                zs.stream.next_out = discardBuffer;
                numToSkip = 0;
            }
            do {
                if (zs.stream.avail_in == 0) {
                    zs.stream.avail_in = ::fread(input, 1, sizeof(input),
                                                 compressed_.get());
                    if (ferror(compressed_.get())) throw ZlibError(Z_ERRNO);
                    if (zs.stream.avail_in == 0) throw ZlibError(Z_DATA_ERROR);
                    zs.stream.next_in = input;
                }
                auto ret = inflate(&zs.stream, Z_NO_FLUSH);
                if (ret == Z_NEED_DICT) throw ZlibError(Z_DATA_ERROR);
                if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                    throw ZlibError(ret);
                if (ret == Z_STREAM_END) break;
            } while (zs.stream.avail_out);
        } while (skipping);
        sink.onLine(line, offset, reinterpret_cast<const char *>(lineBuf),
                    length - 1);
    }
};

Index::Index() { }

Index::~Index() { }

Index::Index(Index &&other) : impl_(std::move(other.impl_)) { }

Index::Index(std::unique_ptr<Impl> &&imp) : impl_(std::move(imp)) { }

struct Index::Builder::Impl : LineSink {
    Log &log;
    File from;
    std::string fromPath;
    std::string indexFilename;
    uint64_t skipFirst;
    Sqlite db;
    Sqlite::Statement addIndexSql;
    Sqlite::Statement addMetaSql;
    uint64_t indexEvery = DefaultIndexEvery;
    std::unordered_map<std::string, std::unique_ptr<IndexHandler>> indexers;

    Impl(Log &log, File &&from, const std::string &fromPath,
         const std::string &indexFilename, uint64_t skipFirst)
            : log(log), from(std::move(from)), fromPath(fromPath),
              indexFilename(indexFilename), skipFirst(skipFirst),
              db(log), addIndexSql(log), addMetaSql(log) { }

    void init() {
        if (unlink(indexFilename.c_str()) == 0) {
            log.warn("Rebuilding existing index ", indexFilename);
        }
        db.open(indexFilename, false);

        db.exec(R"(PRAGMA synchronous = OFF)");
        db.exec(R"(PRAGMA journal_mode = MEMORY)");
        db.exec(R"(PRAGMA application_id = 0x5a494458)");

        db.exec(R"(
CREATE TABLE AccessPoints(
    uncompressedOffset INTEGER PRIMARY KEY,
    uncompressedEndOffset INTEGER,
    compressedOffset INTEGER,
    bitOffset INTEGER,
    window BLOB
))");

        db.exec(R"(
CREATE TABLE Metadata(
    key TEXT PRIMARY KEY,
    value TEXT
))");
        addMetaSql = db.prepare("INSERT INTO Metadata VALUES(:key, :value)");
        addMeta("version", std::to_string(Version));
        addMeta("compressedFile", fromPath);
        struct stat stats;
        if (fstat(fileno(from.get()), &stats) != 0) {
            throw std::runtime_error("Unable to get file stats"); // todo errno
        }
        addMeta("compressedSize", std::to_string(stats.st_size));
        addMeta("compressedModTime", std::to_string(stats.st_mtime));

        db.exec(R"(
CREATE TABLE LineOffsets(
    line INTEGER PRIMARY KEY,
    offset INTEGER,
    length INTEGER
))");

        db.exec(R"(
CREATE TABLE Indexes(
    name TEXT PRIMARY KEY,
    creationString TEXT,
    isNumeric INTEGER
))");
        addIndexSql = db.prepare(R"(
INSERT INTO Indexes VALUES(:name, :creationString, :isNumeric)
)");
    }

    void build() {
        log.info("Building index, generating a checkpoint every ",
                 PrettyBytes(indexEvery));
        struct stat compressedStat;
        if (fstat(fileno(from.get()), &compressedStat) != 0)
            throw ZlibError(Z_DATA_ERROR);

        db.exec(R"(BEGIN TRANSACTION)");

        auto addIndex = db.prepare(R"(
INSERT INTO AccessPoints VALUES(
:uncompressedOffset, :uncompressedEndOffset,
:compressedOffset, :bitOffset, :window))");
        auto addLine = db.prepare(R"(
INSERT INTO LineOffsets VALUES(:line, :offset, :length))");

        ZStream zs(ZStream::Type::ZlibOrGzip);
        uint8_t input[ChunkSize];
        uint8_t window[WindowSize];

        int ret = 0;
        time_t nextProgress = 0;
        uint64_t totalIn = 0;
        uint64_t totalOut = 0;
        uint64_t last = 0;
        bool first = true;
        LineFinder finder(*this);

        log.info("Indexing...");
        do {
            zs.stream.avail_in = fread(input, 1, ChunkSize, from.get());
            if (ferror(from.get()))
                throw ZlibError(Z_ERRNO);
            if (zs.stream.avail_in == 0)
                throw ZlibError(Z_DATA_ERROR);
            zs.stream.next_in = input;
            do {
                if (zs.stream.avail_out == 0) {
                    zs.stream.avail_out = WindowSize;
                    zs.stream.next_out = window;
                    if (!first) {
                        finder.add(window, WindowSize, false);
                    }
                    first = false;
                }
                totalIn += zs.stream.avail_in;
                totalOut += zs.stream.avail_out;
                ret = inflate(&zs.stream, Z_BLOCK);
                totalIn -= zs.stream.avail_in;
                totalOut -= zs.stream.avail_out;
                if (ret == Z_NEED_DICT)
                    throw ZlibError(Z_DATA_ERROR);
                if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                    throw ZlibError(ret);
                if (ret == Z_STREAM_END)
                    break;
                auto sinceLast = totalOut - last;
                bool needsIndex = sinceLast > indexEvery || totalOut == 0;
                bool endOfBlock = zs.stream.data_type & 0x80;
                bool lastBlockInStream = zs.stream.data_type & 0x40;
                if (endOfBlock && !lastBlockInStream && needsIndex) {
                    log.debug("Creating checkpoint at ", PrettyBytes(totalOut),
                              " (compressed offset ", PrettyBytes(totalIn),
                              ")");
                    if (totalOut != 0) {
                        // Flush previous information.
                        addIndex
                                .bindInt64(":uncompressedEndOffset",
                                           totalOut - 1)
                                .step();
                        addIndex.reset();
                    }
                    uint8_t apWindow[compressBound(WindowSize)];
                    auto size = makeWindow(apWindow, sizeof(apWindow), window,
                                           zs.stream.avail_out);
                    addIndex
                            .bindInt64(":uncompressedOffset", totalOut)
                            .bindInt64(":compressedOffset", totalIn)
                            .bindInt64(":bitOffset", zs.stream.data_type & 0x7)
                            .bindBlob(":window", apWindow, size);
                    last = totalOut;
                }
                auto now = time(nullptr);
                if (now >= nextProgress) {
                    char pc[16];
                    snprintf(pc, sizeof(pc) - 1, "%.2f",
                             (totalIn * 100.0) / compressedStat.st_size);
                    log.info("Progress: ", PrettyBytes(totalIn), " of ",
                             PrettyBytes(compressedStat.st_size),
                             " (", pc, "%)");
                    nextProgress = now + LogProgressEverySecs;
                }
            } while (zs.stream.avail_in);
        } while (ret != Z_STREAM_END);

        if (totalOut != 0) {
            // Flush last block.
            addIndex
                    .bindInt64(":uncompressedEndOffset", totalOut - 1)
                    .step();
        }

        log.info("Index reading complete");

        finder.add(window, WindowSize - zs.stream.avail_out, true);
        const auto &lineOffsets = finder.lineOffsets();
        for (size_t line = 0; line < lineOffsets.size() - 1; ++line) {
            addLine
                    .reset()
                    .bindInt64(":line", line + 1)
                    .bindInt64(":offset", lineOffsets[line])
                    .bindInt64(":length",
                               lineOffsets[line + 1] - lineOffsets[line])
                    .step();
        }

        log.info("Flushing");
        db.exec(R"(END TRANSACTION)");
        log.info("Done");
    }

    void addMeta(const std::string &key, const std::string &value) {
        log.debug("Adding metadata ", key, " = ", value);
        addMetaSql
                .reset()
                .bindString(":key", key)
                .bindString(":value", value)
                .step();
    }

    void addIndexer(const std::string &name, const std::string &creation,
                    bool numeric, bool unique,
                    std::unique_ptr<LineIndexer> indexer) {
        auto table = "index_" + name;
        std::string type = numeric ? "INTEGER" : "TEXT";
        if (unique) type += " PRIMARY KEY";
        db.exec(R"(
CREATE TABLE )" + table + R"((
    key )" + type + R"(,
    line INTEGER,
    offset INTEGER
))");
        addIndexSql
                .reset()
                .bindString(":name", name)
                .bindString(":creationString", creation)
                .bindInt64(":isNumeric", numeric ? 1 : 0)
                .step();

        auto inserter = db.prepare(R"(
INSERT INTO )" + table + R"( VALUES(:key, :line, :offset)
)");
        if (numeric) {
            indexers.emplace(name, std::unique_ptr<IndexHandler>(
                    new NumericHandler(log, std::move(indexer),
                                       std::move(inserter))));
        } else {
            indexers.emplace(name, std::unique_ptr<IndexHandler>(
                    new AlphaHandler(log, std::move(indexer),
                                     std::move(inserter))));
        }
    }

    void onLine(
            size_t lineNumber,
            size_t /*fileOffset*/,
            const char *line, size_t length) override {
        if (lineNumber <= skipFirst) return;
        for (auto &&pair : indexers) {
            pair.second->onLine(lineNumber, line, length);
        }
    }
};

Index::Builder::Builder(Log &log, File &&from, const std::string &fromPath,
                        const std::string &indexFilename, uint64_t skipFirst)
        : impl_(new Impl(log, std::move(from), fromPath, indexFilename,
                         skipFirst)) {
    impl_->init();
}

Index::Builder &Index::Builder::addIndexer(
        const std::string &name,
        const std::string &creation,
        bool numeric,
        bool unique,
        std::unique_ptr<LineIndexer> indexer) {
    impl_->addIndexer(name, creation, numeric, unique, std::move(indexer));
    return *this;
}

Index::Builder &Index::Builder::indexEvery(uint64_t bytes) {
    impl_->indexEvery = bytes;
    return *this;
}

void Index::Builder::build() {
    impl_->build();
}

Index Index::load(Log &log, File &&fromCompressed,
                  const std::string &indexFilename,
                  bool forceLoad) {
    Sqlite db(log);
    db.open(indexFilename.c_str(), true);

    std::unique_ptr<Impl> impl(new Impl(log,
                                        std::move(fromCompressed),
                                        std::move(db)));
    impl->init(forceLoad);
    return Index(std::move(impl));
}

void Index::getLine(uint64_t line, LineSink &sink) {
    impl_->getLine(line, sink);
}

void Index::getLines(const std::vector<uint64_t> &lines, LineSink &sink) {
    for (auto line : lines) impl_->getLine(line, sink);
}

void Index::queryIndex(const std::string &index, const std::string &query,
                       LineFunction lineFunction) {
    impl_->queryIndex(index, query, lineFunction);
}

void Index::queryIndexMulti(const std::string &index,
                            const std::vector<std::string> &queries,
                            LineFunction lineFunction) {
    // TODO be a little smarter about this.
    for (auto query : queries) impl_->queryIndex(index, query, lineFunction);
}

Index::Builder::~Builder() {
}

size_t Index::indexSize(const std::string &index) const {
    return impl_->indexSize(index);
}

const Index::Metadata &Index::getMetadata() const {
    return impl_->metadata_;
}

Index::LineFunction Index::sinkFetch(LineSink &sink) {
    return std::function<void(size_t)>([ this, &sink ](size_t line) {
        this->getLine(line, sink);
    });
}
