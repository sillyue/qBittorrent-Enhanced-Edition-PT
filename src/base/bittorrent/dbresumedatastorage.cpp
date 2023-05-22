/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2021-2023  Vladimir Golovnev <glassez@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "dbresumedatastorage.h"

#include <memory>
#include <queue>
#include <utility>

#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QMutex>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include "base/exceptions.h"
#include "base/global.h"
#include "base/logger.h"
#include "base/path.h"
#include "base/profile.h"
#include "base/utils/fs.h"
#include "base/utils/string.h"
#include "infohash.h"
#include "loadtorrentparams.h"

namespace
{
    const QString DB_CONNECTION_NAME = u"ResumeDataStorage"_qs;

    const int DB_VERSION = 4;

    const QString DB_TABLE_META = u"meta"_qs;
    const QString DB_TABLE_TORRENTS = u"torrents"_qs;

    const QString META_VERSION = u"version"_qs;

    using namespace BitTorrent;

    class Job
    {
    public:
        virtual ~Job() = default;
        virtual void perform(QSqlDatabase db) = 0;
    };

    class StoreJob final : public Job
    {
    public:
        StoreJob(const TorrentID &torrentID, const LoadTorrentParams &resumeData);
        void perform(QSqlDatabase db) override;

    private:
        const TorrentID m_torrentID;
        const LoadTorrentParams m_resumeData;
    };

    class RemoveJob final : public Job
    {
    public:
        explicit RemoveJob(const TorrentID &torrentID);
        void perform(QSqlDatabase db) override;

    private:
        const TorrentID m_torrentID;
    };

    class StoreQueueJob final : public Job
    {
    public:
        explicit StoreQueueJob(const QVector<TorrentID> &queue);
        void perform(QSqlDatabase db) override;

    private:
        const QVector<TorrentID> m_queue;
    };

    struct Column
    {
        QString name;
        QString placeholder;
    };

    Column makeColumn(const char *columnName)
    {
        return {QString::fromLatin1(columnName), (u':' + QString::fromLatin1(columnName))};
    }

    const Column DB_COLUMN_ID = makeColumn("id");
    const Column DB_COLUMN_TORRENT_ID = makeColumn("torrent_id");
    const Column DB_COLUMN_QUEUE_POSITION = makeColumn("queue_position");
    const Column DB_COLUMN_NAME = makeColumn("name");
    const Column DB_COLUMN_CATEGORY = makeColumn("category");
    const Column DB_COLUMN_TAGS = makeColumn("tags");
    const Column DB_COLUMN_TARGET_SAVE_PATH = makeColumn("target_save_path");
    const Column DB_COLUMN_DOWNLOAD_PATH = makeColumn("download_path");
    const Column DB_COLUMN_CONTENT_LAYOUT = makeColumn("content_layout");
    const Column DB_COLUMN_RATIO_LIMIT = makeColumn("ratio_limit");
    const Column DB_COLUMN_SEEDING_TIME_LIMIT = makeColumn("seeding_time_limit");
    const Column DB_COLUMN_HAS_OUTER_PIECES_PRIORITY = makeColumn("has_outer_pieces_priority");
    const Column DB_COLUMN_HAS_SEED_STATUS = makeColumn("has_seed_status");
    const Column DB_COLUMN_OPERATING_MODE = makeColumn("operating_mode");
    const Column DB_COLUMN_STOPPED = makeColumn("stopped");
    const Column DB_COLUMN_STOP_CONDITION = makeColumn("stop_condition");
    const Column DB_COLUMN_RESUMEDATA = makeColumn("libtorrent_resume_data");
    const Column DB_COLUMN_METADATA = makeColumn("metadata");
    const Column DB_COLUMN_VALUE = makeColumn("value");

    template <typename LTStr>
    QString fromLTString(const LTStr &str)
    {
        return QString::fromUtf8(str.data(), static_cast<int>(str.size()));
    }

    QString quoted(const QString &name)
    {
        const QChar quote = u'`';

        return (quote + name + quote);
    }

    QString makeCreateTableStatement(const QString &tableName, const QStringList &items)
    {
        return u"CREATE TABLE %1 (%2)"_qs.arg(quoted(tableName), items.join(u','));
    }

    std::pair<QString, QString> joinColumns(const QVector<Column> &columns)
    {
        int namesSize = columns.size();
        int valuesSize = columns.size();
        for (const Column &column : columns)
        {
            namesSize += column.name.size() + 2;
            valuesSize += column.placeholder.size();
        }

        QString names;
        names.reserve(namesSize);
        QString values;
        values.reserve(valuesSize);
        for (const Column &column : columns)
        {
            names.append(quoted(column.name) + u',');
            values.append(column.placeholder + u',');
        }
        names.chop(1);
        values.chop(1);

        return std::make_pair(names, values);
    }

    QString makeInsertStatement(const QString &tableName, const QVector<Column> &columns)
    {
        const auto [names, values] = joinColumns(columns);
        return u"INSERT INTO %1 (%2) VALUES (%3)"_qs
                .arg(quoted(tableName), names, values);
    }

    QString makeUpdateStatement(const QString &tableName, const QVector<Column> &columns)
    {
        const auto [names, values] = joinColumns(columns);
        return u"UPDATE %1 SET (%2) = (%3)"_qs
                .arg(quoted(tableName), names, values);
    }

    QString makeOnConflictUpdateStatement(const Column &constraint, const QVector<Column> &columns)
    {
        const auto [names, values] = joinColumns(columns);
        return u" ON CONFLICT (%1) DO UPDATE SET (%2) = (%3)"_qs
                .arg(quoted(constraint.name), names, values);
    }

    QString makeColumnDefinition(const Column &column, const char *definition)
    {
        return u"%1 %2"_qs.arg(quoted(column.name), QString::fromLatin1(definition));
    }

    LoadTorrentParams parseQueryResultRow(const QSqlQuery &query)
    {
        LoadTorrentParams resumeData;
        resumeData.name = query.value(DB_COLUMN_NAME.name).toString();
        resumeData.category = query.value(DB_COLUMN_CATEGORY.name).toString();
        const QString tagsData = query.value(DB_COLUMN_TAGS.name).toString();
        if (!tagsData.isEmpty())
        {
            const QStringList tagList = tagsData.split(u',');
            resumeData.tags.insert(tagList.cbegin(), tagList.cend());
        }
        resumeData.hasFinishedStatus = query.value(DB_COLUMN_HAS_SEED_STATUS.name).toBool();
        resumeData.firstLastPiecePriority = query.value(DB_COLUMN_HAS_OUTER_PIECES_PRIORITY.name).toBool();
        resumeData.ratioLimit = query.value(DB_COLUMN_RATIO_LIMIT.name).toInt() / 1000.0;
        resumeData.seedingTimeLimit = query.value(DB_COLUMN_SEEDING_TIME_LIMIT.name).toInt();
        resumeData.contentLayout = Utils::String::toEnum<TorrentContentLayout>(
                    query.value(DB_COLUMN_CONTENT_LAYOUT.name).toString(), TorrentContentLayout::Original);
        resumeData.operatingMode = Utils::String::toEnum<TorrentOperatingMode>(
                    query.value(DB_COLUMN_OPERATING_MODE.name).toString(), TorrentOperatingMode::AutoManaged);
        resumeData.stopped = query.value(DB_COLUMN_STOPPED.name).toBool();
        resumeData.stopCondition = Utils::String::toEnum(
                    query.value(DB_COLUMN_STOP_CONDITION.name).toString(), Torrent::StopCondition::None);

        resumeData.savePath = Profile::instance()->fromPortablePath(
                    Path(query.value(DB_COLUMN_TARGET_SAVE_PATH.name).toString()));
        resumeData.useAutoTMM = resumeData.savePath.isEmpty();
        if (!resumeData.useAutoTMM)
        {
            resumeData.downloadPath = Profile::instance()->fromPortablePath(
                        Path(query.value(DB_COLUMN_DOWNLOAD_PATH.name).toString()));
        }

        const QByteArray bencodedResumeData = query.value(DB_COLUMN_RESUMEDATA.name).toByteArray();

        lt::error_code ec;
        const lt::bdecode_node resumeDataRoot = lt::bdecode(bencodedResumeData, ec);

        lt::add_torrent_params &p = resumeData.ltAddTorrentParams;

        p = lt::read_resume_data(resumeDataRoot, ec);

        if (const QByteArray bencodedMetadata = query.value(DB_COLUMN_METADATA.name).toByteArray()
                ; !bencodedMetadata.isEmpty())
        {
            const lt::bdecode_node torentInfoRoot = lt::bdecode(bencodedMetadata, ec);
            p.ti = std::make_shared<lt::torrent_info>(torentInfoRoot, ec);
        }

        p.save_path = Profile::instance()->fromPortablePath(Path(fromLTString(p.save_path)))
                .toString().toStdString();

        if (p.flags & lt::torrent_flags::stop_when_ready)
        {
            p.flags &= ~lt::torrent_flags::stop_when_ready;
            resumeData.stopCondition = Torrent::StopCondition::FilesChecked;
        }

        return resumeData;
    }
}

namespace BitTorrent
{
    class DBResumeDataStorage::Worker final : public QThread
    {
        Q_DISABLE_COPY_MOVE(Worker)

    public:
        Worker(const Path &dbPath, QReadWriteLock &dbLock);

        void run() override;
        void requestInterruption();

        void store(const TorrentID &id, const LoadTorrentParams &resumeData);
        void remove(const TorrentID &id);
        void storeQueue(const QVector<TorrentID> &queue);

    private:
        void addJob(std::unique_ptr<Job> job);

        const QString m_connectionName = u"ResumeDataStorageWorker"_qs;
        const Path m_path;
        QReadWriteLock &m_dbLock;

        std::queue<std::unique_ptr<Job>> m_jobs;
        QMutex m_jobsMutex;
        QWaitCondition m_waitCondition;
    };
}

BitTorrent::DBResumeDataStorage::DBResumeDataStorage(const Path &dbPath, QObject *parent)
    : ResumeDataStorage(dbPath, parent)
    , m_ioThread {new QThread}
{
    const bool needCreateDB = !dbPath.exists();

    auto db = QSqlDatabase::addDatabase(u"QSQLITE"_qs, DB_CONNECTION_NAME);
    db.setDatabaseName(dbPath.data());
    if (!db.open())
        throw RuntimeError(db.lastError().text());

    if (needCreateDB)
    {
        createDB();
    }
    else
    {
        const int dbVersion = (!db.record(DB_TABLE_TORRENTS).contains(DB_COLUMN_DOWNLOAD_PATH.name) ? 1 : currentDBVersion());
        if (dbVersion < DB_VERSION)
            updateDB(dbVersion);
    }

    m_asyncWorker = new Worker(dbPath, m_dbLock);
    m_asyncWorker->start();
}

BitTorrent::DBResumeDataStorage::~DBResumeDataStorage()
{
    m_asyncWorker->requestInterruption();
    m_asyncWorker->wait();
    QSqlDatabase::removeDatabase(DB_CONNECTION_NAME);
}

QVector<BitTorrent::TorrentID> BitTorrent::DBResumeDataStorage::registeredTorrents() const
{
    const auto selectTorrentIDStatement = u"SELECT %1 FROM %2 ORDER BY %3;"_qs
            .arg(quoted(DB_COLUMN_TORRENT_ID.name), quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_QUEUE_POSITION.name));

    auto db = QSqlDatabase::database(DB_CONNECTION_NAME);
    QSqlQuery query {db};

    if (!query.exec(selectTorrentIDStatement))
        throw RuntimeError(query.lastError().text());

    QVector<TorrentID> registeredTorrents;
    registeredTorrents.reserve(query.size());
    while (query.next())
        registeredTorrents.append(BitTorrent::TorrentID::fromString(query.value(0).toString()));

    return registeredTorrents;
}

BitTorrent::LoadResumeDataResult BitTorrent::DBResumeDataStorage::load(const TorrentID &id) const
{
    const QString selectTorrentStatement = u"SELECT * FROM %1 WHERE %2 = %3;"_qs
        .arg(quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_TORRENT_ID.name), DB_COLUMN_TORRENT_ID.placeholder);

    auto db = QSqlDatabase::database(DB_CONNECTION_NAME);
    QSqlQuery query {db};
    try
    {
        if (!query.prepare(selectTorrentStatement))
            throw RuntimeError(query.lastError().text());

        query.bindValue(DB_COLUMN_TORRENT_ID.placeholder, id.toString());
        if (!query.exec())
            throw RuntimeError(query.lastError().text());

        if (!query.next())
            throw RuntimeError(tr("Not found."));
    }
    catch (const RuntimeError &err)
    {
        return nonstd::make_unexpected(tr("Couldn't load resume data of torrent '%1'. Error: %2")
            .arg(id.toString(), err.message()));
    }

    return parseQueryResultRow(query);
}

void BitTorrent::DBResumeDataStorage::store(const TorrentID &id, const LoadTorrentParams &resumeData) const
{
    m_asyncWorker->store(id, resumeData);
}

void BitTorrent::DBResumeDataStorage::remove(const BitTorrent::TorrentID &id) const
{
    m_asyncWorker->remove(id);
}

void BitTorrent::DBResumeDataStorage::storeQueue(const QVector<TorrentID> &queue) const
{
    m_asyncWorker->storeQueue(queue);
}

void BitTorrent::DBResumeDataStorage::doLoadAll() const
{
    const QString connectionName = u"ResumeDataStorageLoadAll"_qs;

    {
        auto db = QSqlDatabase::addDatabase(u"QSQLITE"_qs, connectionName);
        db.setDatabaseName(path().data());
        if (!db.open())
            throw RuntimeError(db.lastError().text());

        QSqlQuery query {db};

        const auto selectTorrentIDStatement = u"SELECT %1 FROM %2 ORDER BY %3;"_qs
                .arg(quoted(DB_COLUMN_TORRENT_ID.name), quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_QUEUE_POSITION.name));

        const QReadLocker locker {&m_dbLock};

        if (!query.exec(selectTorrentIDStatement))
            throw RuntimeError(query.lastError().text());

        QVector<TorrentID> registeredTorrents;
        registeredTorrents.reserve(query.size());
        while (query.next())
            registeredTorrents.append(TorrentID::fromString(query.value(0).toString()));

        emit const_cast<DBResumeDataStorage *>(this)->loadStarted(registeredTorrents);

        const auto selectStatement = u"SELECT * FROM %1 ORDER BY %2;"_qs.arg(quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_QUEUE_POSITION.name));
        if (!query.exec(selectStatement))
            throw RuntimeError(query.lastError().text());

        while (query.next())
        {
            const auto torrentID = TorrentID::fromString(query.value(DB_COLUMN_TORRENT_ID.name).toString());
            onResumeDataLoaded(torrentID, parseQueryResultRow(query));
        }
    }

    emit const_cast<DBResumeDataStorage *>(this)->loadFinished();

    QSqlDatabase::removeDatabase(connectionName);
}

int BitTorrent::DBResumeDataStorage::currentDBVersion() const
{
    const auto selectDBVersionStatement = u"SELECT %1 FROM %2 WHERE %3 = %4;"_qs
            .arg(quoted(DB_COLUMN_VALUE.name), quoted(DB_TABLE_META), quoted(DB_COLUMN_NAME.name), DB_COLUMN_NAME.placeholder);

    auto db = QSqlDatabase::database(DB_CONNECTION_NAME);
    QSqlQuery query {db};

    if (!query.prepare(selectDBVersionStatement))
        throw RuntimeError(query.lastError().text());

    query.bindValue(DB_COLUMN_NAME.placeholder, META_VERSION);

    const QReadLocker locker {&m_dbLock};

    if (!query.exec())
        throw RuntimeError(query.lastError().text());

    if (!query.next())
        throw RuntimeError(tr("Database is corrupted."));

    bool ok;
    const int dbVersion = query.value(0).toInt(&ok);
    if (!ok)
        throw RuntimeError(tr("Database is corrupted."));

    return dbVersion;
}

void BitTorrent::DBResumeDataStorage::createDB() const
{
    try
    {
        enableWALMode();
    }
    catch (const RuntimeError &err)
    {
        LogMsg(tr("Couldn't enable Write-Ahead Logging (WAL) journaling mode. Error: %1.")
               .arg(err.message()), Log::WARNING);
    }

    auto db = QSqlDatabase::database(DB_CONNECTION_NAME);

    if (!db.transaction())
        throw RuntimeError(db.lastError().text());

    QSqlQuery query {db};

    try
    {
        const QStringList tableMetaItems = {
            makeColumnDefinition(DB_COLUMN_ID, "INTEGER PRIMARY KEY"),
            makeColumnDefinition(DB_COLUMN_NAME, "TEXT NOT NULL UNIQUE"),
            makeColumnDefinition(DB_COLUMN_VALUE, "BLOB")
        };
        const QString createTableMetaQuery = makeCreateTableStatement(DB_TABLE_META, tableMetaItems);
        if (!query.exec(createTableMetaQuery))
            throw RuntimeError(query.lastError().text());

        const QString insertMetaVersionQuery = makeInsertStatement(DB_TABLE_META, {DB_COLUMN_NAME, DB_COLUMN_VALUE});
        if (!query.prepare(insertMetaVersionQuery))
            throw RuntimeError(query.lastError().text());

        query.bindValue(DB_COLUMN_NAME.placeholder, META_VERSION);
        query.bindValue(DB_COLUMN_VALUE.placeholder, DB_VERSION);

        if (!query.exec())
            throw RuntimeError(query.lastError().text());

        const QStringList tableTorrentsItems = {
            makeColumnDefinition(DB_COLUMN_ID, "INTEGER PRIMARY KEY"),
            makeColumnDefinition(DB_COLUMN_TORRENT_ID, "BLOB NOT NULL UNIQUE"),
            makeColumnDefinition(DB_COLUMN_QUEUE_POSITION, "INTEGER NOT NULL DEFAULT -1"),
            makeColumnDefinition(DB_COLUMN_NAME, "TEXT"),
            makeColumnDefinition(DB_COLUMN_CATEGORY, "TEXT"),
            makeColumnDefinition(DB_COLUMN_TAGS, "TEXT"),
            makeColumnDefinition(DB_COLUMN_TARGET_SAVE_PATH, "TEXT"),
            makeColumnDefinition(DB_COLUMN_DOWNLOAD_PATH, "TEXT"),
            makeColumnDefinition(DB_COLUMN_CONTENT_LAYOUT, "TEXT NOT NULL"),
            makeColumnDefinition(DB_COLUMN_RATIO_LIMIT, "INTEGER NOT NULL"),
            makeColumnDefinition(DB_COLUMN_SEEDING_TIME_LIMIT, "INTEGER NOT NULL"),
            makeColumnDefinition(DB_COLUMN_HAS_OUTER_PIECES_PRIORITY, "INTEGER NOT NULL"),
            makeColumnDefinition(DB_COLUMN_HAS_SEED_STATUS, "INTEGER NOT NULL"),
            makeColumnDefinition(DB_COLUMN_OPERATING_MODE, "TEXT NOT NULL"),
            makeColumnDefinition(DB_COLUMN_STOPPED, "INTEGER NOT NULL"),
            makeColumnDefinition(DB_COLUMN_STOP_CONDITION, "TEXT NOT NULL DEFAULT `None`"),
            makeColumnDefinition(DB_COLUMN_RESUMEDATA, "BLOB NOT NULL"),
            makeColumnDefinition(DB_COLUMN_METADATA, "BLOB")
        };
        const QString createTableTorrentsQuery = makeCreateTableStatement(DB_TABLE_TORRENTS, tableTorrentsItems);
        if (!query.exec(createTableTorrentsQuery))
            throw RuntimeError(query.lastError().text());

        const QString torrentsQueuePositionIndexName = u"%1_%2_INDEX"_qs.arg(DB_TABLE_TORRENTS, DB_COLUMN_QUEUE_POSITION.name);
        const QString createTorrentsQueuePositionIndexQuery = u"CREATE INDEX %1 ON %2 (%3)"_qs
                .arg(quoted(torrentsQueuePositionIndexName), quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_QUEUE_POSITION.name));
        if (!query.exec(createTorrentsQueuePositionIndexQuery))
            throw RuntimeError(query.lastError().text());

        if (!db.commit())
            throw RuntimeError(db.lastError().text());
    }
    catch (const RuntimeError &)
    {
        db.rollback();
        throw;
    }
}

void BitTorrent::DBResumeDataStorage::updateDB(const int fromVersion) const
{
    Q_ASSERT(fromVersion > 0);
    Q_ASSERT(fromVersion != DB_VERSION);

    auto db = QSqlDatabase::database(DB_CONNECTION_NAME);

    const QWriteLocker locker {&m_dbLock};

    if (!db.transaction())
        throw RuntimeError(db.lastError().text());

    QSqlQuery query {db};

    try
    {
        if (fromVersion == 1)
        {
            const auto testQuery = u"SELECT COUNT(%1) FROM %2;"_qs
                    .arg(quoted(DB_COLUMN_DOWNLOAD_PATH.name), quoted(DB_TABLE_TORRENTS));
            if (!query.exec(testQuery))
            {
                const auto alterTableTorrentsQuery = u"ALTER TABLE %1 ADD %2"_qs
                        .arg(quoted(DB_TABLE_TORRENTS), makeColumnDefinition(DB_COLUMN_DOWNLOAD_PATH, "TEXT"));
                if (!query.exec(alterTableTorrentsQuery))
                    throw RuntimeError(query.lastError().text());
            }
        }

        if (fromVersion <= 2)
        {
            const auto testQuery = u"SELECT COUNT(%1) FROM %2;"_qs
                    .arg(quoted(DB_COLUMN_STOP_CONDITION.name), quoted(DB_TABLE_TORRENTS));
            if (!query.exec(testQuery))
            {
                const auto alterTableTorrentsQuery = u"ALTER TABLE %1 ADD %2"_qs
                        .arg(quoted(DB_TABLE_TORRENTS), makeColumnDefinition(DB_COLUMN_STOP_CONDITION, "TEXT NOT NULL DEFAULT `None`"));
                if (!query.exec(alterTableTorrentsQuery))
                    throw RuntimeError(query.lastError().text());
            }
        }

        if (fromVersion <= 3)
        {
            const QString torrentsQueuePositionIndexName = u"%1_%2_INDEX"_qs.arg(DB_TABLE_TORRENTS, DB_COLUMN_QUEUE_POSITION.name);
            const QString createTorrentsQueuePositionIndexQuery = u"CREATE INDEX IF NOT EXISTS %1 ON %2 (%3)"_qs
                    .arg(quoted(torrentsQueuePositionIndexName), quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_QUEUE_POSITION.name));
            if (!query.exec(createTorrentsQueuePositionIndexQuery))
                throw RuntimeError(query.lastError().text());
        }

        const QString updateMetaVersionQuery = makeUpdateStatement(DB_TABLE_META, {DB_COLUMN_NAME, DB_COLUMN_VALUE});
        if (!query.prepare(updateMetaVersionQuery))
            throw RuntimeError(query.lastError().text());

        query.bindValue(DB_COLUMN_NAME.placeholder, META_VERSION);
        query.bindValue(DB_COLUMN_VALUE.placeholder, DB_VERSION);

        if (!query.exec())
            throw RuntimeError(query.lastError().text());

        if (!db.commit())
            throw RuntimeError(db.lastError().text());
    }
    catch (const RuntimeError &)
    {
        db.rollback();
        throw;
    }
}

void BitTorrent::DBResumeDataStorage::enableWALMode() const
{
    auto db = QSqlDatabase::database(DB_CONNECTION_NAME);
    QSqlQuery query {db};

    if (!query.exec(u"PRAGMA journal_mode = WAL;"_qs))
        throw RuntimeError(query.lastError().text());

    if (!query.next())
        throw RuntimeError(tr("Couldn't obtain query result."));

    const QString result = query.value(0).toString();
    if (result.compare(u"WAL"_qs, Qt::CaseInsensitive) != 0)
        throw RuntimeError(tr("WAL mode is probably unsupported due to filesystem limitations."));
}

BitTorrent::DBResumeDataStorage::Worker::Worker(const Path &dbPath, QReadWriteLock &dbLock)
    : m_path {dbPath}
    , m_dbLock {dbLock}
{
}

void BitTorrent::DBResumeDataStorage::Worker::run()
{
    {
        auto db = QSqlDatabase::addDatabase(u"QSQLITE"_qs, m_connectionName);
        db.setDatabaseName(m_path.data());
        if (!db.open())
            throw RuntimeError(db.lastError().text());

        int64_t transactedJobsCount = 0;
        while (true)
        {
            m_jobsMutex.lock();
            if (m_jobs.empty())
            {
                if (transactedJobsCount > 0)
                {
                    db.commit();
                    m_dbLock.unlock();

                    qDebug() << "Resume data changes are commited. Transacted jobs:" << transactedJobsCount;
                    transactedJobsCount = 0;
                }

                if (isInterruptionRequested())
                {
                    m_jobsMutex.unlock();
                    break;
                }

                m_waitCondition.wait(&m_jobsMutex);
                if (isInterruptionRequested())
                {
                    m_jobsMutex.unlock();
                    break;
                }

                m_dbLock.lockForWrite();
                if (!db.transaction())
                {
                    LogMsg(tr("Couldn't begin transaction. Error: %1").arg(db.lastError().text()), Log::WARNING);
                    m_dbLock.unlock();
                    break;
                }
            }
            std::unique_ptr<Job> job = std::move(m_jobs.front());
            m_jobs.pop();
            m_jobsMutex.unlock();

            job->perform(db);
            ++transactedJobsCount;
        }

        db.close();
    }

    QSqlDatabase::removeDatabase(m_connectionName);
}

void DBResumeDataStorage::Worker::requestInterruption()
{
    QThread::requestInterruption();
    m_waitCondition.wakeAll();
}

void BitTorrent::DBResumeDataStorage::Worker::store(const TorrentID &id, const LoadTorrentParams &resumeData)
{
    addJob(std::make_unique<StoreJob>(id, resumeData));
}

void BitTorrent::DBResumeDataStorage::Worker::remove(const TorrentID &id)
{
    addJob(std::make_unique<RemoveJob>(id));
}

void BitTorrent::DBResumeDataStorage::Worker::storeQueue(const QVector<TorrentID> &queue)
{
    addJob(std::make_unique<StoreQueueJob>(queue));
}

void BitTorrent::DBResumeDataStorage::Worker::addJob(std::unique_ptr<Job> job)
{
    m_jobsMutex.lock();
    m_jobs.push(std::move(job));
    m_jobsMutex.unlock();

    m_waitCondition.wakeAll();
}

namespace
{
    using namespace BitTorrent;

    StoreJob::StoreJob(const TorrentID &torrentID, const LoadTorrentParams &resumeData)
        : m_torrentID {torrentID}
        , m_resumeData {resumeData}
    {
    }

    void StoreJob::perform(QSqlDatabase db)
    {
        // We need to adjust native libtorrent resume data
        lt::add_torrent_params p = m_resumeData.ltAddTorrentParams;
        p.save_path = Profile::instance()->toPortablePath(Path(p.save_path))
                .toString().toStdString();
        if (m_resumeData.stopped)
        {
            p.flags |= lt::torrent_flags::paused;
            p.flags &= ~lt::torrent_flags::auto_managed;
        }
        else
        {
            // Torrent can be actually "running" but temporarily "paused" to perform some
            // service jobs behind the scenes so we need to restore it as "running"
            if (m_resumeData.operatingMode == BitTorrent::TorrentOperatingMode::AutoManaged)
            {
                p.flags |= lt::torrent_flags::auto_managed;
            }
            else
            {
                p.flags &= ~lt::torrent_flags::paused;
                p.flags &= ~lt::torrent_flags::auto_managed;
            }
        }

        QVector<Column> columns {
            DB_COLUMN_TORRENT_ID,
            DB_COLUMN_NAME,
            DB_COLUMN_CATEGORY,
            DB_COLUMN_TAGS,
            DB_COLUMN_TARGET_SAVE_PATH,
            DB_COLUMN_DOWNLOAD_PATH,
            DB_COLUMN_CONTENT_LAYOUT,
            DB_COLUMN_RATIO_LIMIT,
            DB_COLUMN_SEEDING_TIME_LIMIT,
            DB_COLUMN_HAS_OUTER_PIECES_PRIORITY,
            DB_COLUMN_HAS_SEED_STATUS,
            DB_COLUMN_OPERATING_MODE,
            DB_COLUMN_STOPPED,
            DB_COLUMN_STOP_CONDITION,
            DB_COLUMN_RESUMEDATA
        };

        lt::entry data = lt::write_resume_data(p);

        // metadata is stored in separate column
        QByteArray bencodedMetadata;
        if (p.ti)
        {
            lt::entry::dictionary_type &dataDict = data.dict();
            lt::entry metadata {lt::entry::dictionary_t};
            lt::entry::dictionary_type &metadataDict = metadata.dict();
            metadataDict.insert(dataDict.extract("info"));
            metadataDict.insert(dataDict.extract("creation date"));
            metadataDict.insert(dataDict.extract("created by"));
            metadataDict.insert(dataDict.extract("comment"));

            try
            {
                bencodedMetadata.reserve(512 * 1024);
                lt::bencode(std::back_inserter(bencodedMetadata), metadata);
            }
            catch (const std::exception &err)
            {
                LogMsg(ResumeDataStorage::tr("Couldn't save torrent metadata. Error: %1.")
                        .arg(QString::fromLocal8Bit(err.what())), Log::CRITICAL);
                return;
            }

            columns.append(DB_COLUMN_METADATA);
        }

        QByteArray bencodedResumeData;
        bencodedResumeData.reserve(256 * 1024);
        lt::bencode(std::back_inserter(bencodedResumeData), data);

        const QString insertTorrentStatement = makeInsertStatement(DB_TABLE_TORRENTS, columns)
                + makeOnConflictUpdateStatement(DB_COLUMN_TORRENT_ID, columns);
        QSqlQuery query {db};

        try
        {
            if (!query.prepare(insertTorrentStatement))
                throw RuntimeError(query.lastError().text());

            query.bindValue(DB_COLUMN_TORRENT_ID.placeholder, m_torrentID.toString());
            query.bindValue(DB_COLUMN_NAME.placeholder, m_resumeData.name);
            query.bindValue(DB_COLUMN_CATEGORY.placeholder, m_resumeData.category);
            query.bindValue(DB_COLUMN_TAGS.placeholder, (m_resumeData.tags.isEmpty()
                    ? QVariant(QVariant::String) : m_resumeData.tags.join(u","_qs)));
            query.bindValue(DB_COLUMN_CONTENT_LAYOUT.placeholder, Utils::String::fromEnum(m_resumeData.contentLayout));
            query.bindValue(DB_COLUMN_RATIO_LIMIT.placeholder, static_cast<int>(m_resumeData.ratioLimit * 1000));
            query.bindValue(DB_COLUMN_SEEDING_TIME_LIMIT.placeholder, m_resumeData.seedingTimeLimit);
            query.bindValue(DB_COLUMN_HAS_OUTER_PIECES_PRIORITY.placeholder, m_resumeData.firstLastPiecePriority);
            query.bindValue(DB_COLUMN_HAS_SEED_STATUS.placeholder, m_resumeData.hasFinishedStatus);
            query.bindValue(DB_COLUMN_OPERATING_MODE.placeholder, Utils::String::fromEnum(m_resumeData.operatingMode));
            query.bindValue(DB_COLUMN_STOPPED.placeholder, m_resumeData.stopped);
            query.bindValue(DB_COLUMN_STOP_CONDITION.placeholder, Utils::String::fromEnum(m_resumeData.stopCondition));

            if (!m_resumeData.useAutoTMM)
            {
                query.bindValue(DB_COLUMN_TARGET_SAVE_PATH.placeholder, Profile::instance()->toPortablePath(m_resumeData.savePath).data());
                query.bindValue(DB_COLUMN_DOWNLOAD_PATH.placeholder, Profile::instance()->toPortablePath(m_resumeData.downloadPath).data());
            }

            query.bindValue(DB_COLUMN_RESUMEDATA.placeholder, bencodedResumeData);
            if (!bencodedMetadata.isEmpty())
                query.bindValue(DB_COLUMN_METADATA.placeholder, bencodedMetadata);

            if (!query.exec())
                throw RuntimeError(query.lastError().text());
        }
        catch (const RuntimeError &err)
        {
            LogMsg(ResumeDataStorage::tr("Couldn't store resume data for torrent '%1'. Error: %2")
                    .arg(m_torrentID.toString(), err.message()), Log::CRITICAL);
        }
    }

    RemoveJob::RemoveJob(const TorrentID &torrentID)
        : m_torrentID {torrentID}
    {
    }

    void RemoveJob::perform(QSqlDatabase db)
    {
        const auto deleteTorrentStatement = u"DELETE FROM %1 WHERE %2 = %3;"_qs
                .arg(quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_TORRENT_ID.name), DB_COLUMN_TORRENT_ID.placeholder);

        QSqlQuery query {db};
        try
        {
            if (!query.prepare(deleteTorrentStatement))
                throw RuntimeError(query.lastError().text());

            query.bindValue(DB_COLUMN_TORRENT_ID.placeholder, m_torrentID.toString());

            if (!query.exec())
                throw RuntimeError(query.lastError().text());
        }
        catch (const RuntimeError &err)
        {
            LogMsg(ResumeDataStorage::tr("Couldn't delete resume data of torrent '%1'. Error: %2")
                    .arg(m_torrentID.toString(), err.message()), Log::CRITICAL);
        }
    }

    StoreQueueJob::StoreQueueJob(const QVector<TorrentID> &queue)
        : m_queue {queue}
    {
    }

    void StoreQueueJob::perform(QSqlDatabase db)
    {
        const auto updateQueuePosStatement = u"UPDATE %1 SET %2 = %3 WHERE %4 = %5;"_qs
                .arg(quoted(DB_TABLE_TORRENTS), quoted(DB_COLUMN_QUEUE_POSITION.name), DB_COLUMN_QUEUE_POSITION.placeholder
                        , quoted(DB_COLUMN_TORRENT_ID.name), DB_COLUMN_TORRENT_ID.placeholder);

        try
        {
            QSqlQuery query {db};

            if (!query.prepare(updateQueuePosStatement))
                throw RuntimeError(query.lastError().text());

            int pos = 0;
            for (const TorrentID &torrentID : m_queue)
            {
                query.bindValue(DB_COLUMN_TORRENT_ID.placeholder, torrentID.toString());
                query.bindValue(DB_COLUMN_QUEUE_POSITION.placeholder, pos++);
                if (!query.exec())
                    throw RuntimeError(query.lastError().text());
            }
        }
        catch (const RuntimeError &err)
        {
            LogMsg(ResumeDataStorage::tr("Couldn't store torrents queue positions. Error: %1")
                    .arg(err.message()), Log::CRITICAL);
        }
    }
}
