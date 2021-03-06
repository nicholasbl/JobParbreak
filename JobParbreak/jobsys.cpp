#include "jobsys.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QFile>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cassert>
#include <iostream>
#include <variant>

// classic visitor helper
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

auto const JSON_ASSIGNMENT = QStringLiteral("assignment");
auto const JSON_SUCCESS    = QStringLiteral("success");
auto const JSON_FAILED     = QStringLiteral("failed");

MessageType decode_message(QByteArray data) {
    auto doc = QJsonDocument::fromJson(data);

    auto obj = doc.object();

    qDebug() << Q_FUNC_INFO << obj;

    if (obj.contains(JSON_ASSIGNMENT)) {
        auto mobj = obj[JSON_ASSIGNMENT].toObject();

        MessageAssignment ret;
        ret.id      = QUuid(mobj["id"].toString());
        ret.command = mobj["command"].toString();

        return ret;

    } else if (obj.contains(JSON_SUCCESS)) {
        auto mobj = obj[JSON_SUCCESS].toObject();

        MessageSuccess ret;
        ret.completed = QUuid(mobj["id"].toString());
        ret.std_out   = mobj["std_out"].toString();
        ret.std_err   = mobj["std_err"].toString();

        return ret;

    } else if (obj.contains(JSON_FAILED)) {
        auto mobj = obj[JSON_FAILED].toObject();

        MessageFailed ret;
        ret.failed  = QUuid(mobj["id"].toString());
        ret.std_out = mobj["std_out"].toString();
        ret.std_err = mobj["std_err"].toString();

        return ret;
    }

    return {};
}


QByteArray encode_message(MessageAssignment const& m) {
    QJsonObject obj;

    obj["id"]      = m.id.toString();
    obj["command"] = m.command;

    QJsonObject msg;
    msg[JSON_ASSIGNMENT] = obj;

    return QJsonDocument(msg).toJson();
}

QByteArray encode_message(MessageSuccess const& m) {
    QJsonObject obj;

    obj["id"]      = m.completed.toString();
    obj["std_out"] = m.std_out;
    obj["std_err"] = m.std_err;

    QJsonObject msg;
    msg[JSON_SUCCESS] = obj;

    return QJsonDocument(msg).toJson();
}

QByteArray encode_message(MessageFailed const& m) {
    QJsonObject obj;

    obj["id"]      = m.failed.toString();
    obj["std_out"] = m.std_out;
    obj["std_err"] = m.std_err;

    QJsonObject msg;
    msg[JSON_FAILED] = obj;

    return QJsonDocument(msg).toJson();
}

// =============================================================================


QDataStream& operator<<(QDataStream& stream, JobStatus const& r) {
    stream << std::underlying_type_t<JobStatus>(r);
    return stream;
}
QDataStream& operator>>(QDataStream& stream, JobStatus& r) {
    std::underlying_type_t<JobStatus> v;
    stream >> v;
    r = (JobStatus)v;
    return stream;
}


QDataStream& operator<<(QDataStream& stream, JobRecord const& r) {
    stream << r.id << r.command << r.status;
    return stream;
}
QDataStream& operator>>(QDataStream& stream, JobRecord& r) {
    stream >> r.id;
    stream >> r.command;
    stream >> r.status;
    return stream;
}

// =============================================================================

// Following https://github.com/juangburgos/QConsoleListener

AsyncPrompt::AsyncPrompt(QObject* parent) : QObject(parent) {
    connect(this,
            &AsyncPrompt::_get_new_line,
            this,
            &AsyncPrompt::on_get_new_line,
            Qt::QueuedConnection);

    m_notifier = new QSocketNotifier(fileno(stdin), QSocketNotifier::Read);

    m_notifier->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_notifier, &QObject::deleteLater);

    connect(m_notifier, &QSocketNotifier::activated, [this]() {
        std::string line;
        std::getline(std::cin, line);
        auto qline = QString::fromStdString(line);
        emit _get_new_line(qline);
    });

    m_thread.start();
}
AsyncPrompt::~AsyncPrompt() {
    m_thread.quit();
    m_thread.wait();
}

void AsyncPrompt::on_get_new_line(QString line) {
    emit new_text(line);
}

// =============================================================================

RemoteCommand::RemoteCommand(QString  remote_host,
                             QString  exe_path,
                             uint16_t port,
                             QObject* parent)
    : QObject(parent),
      m_remote_host(remote_host),
      m_exe_path(exe_path),
      m_port(port) { }

void RemoteCommand::start() {
    QProcess* p = new QProcess(this);

    // in the future, we should probe for this

    p->setProgram("/usr/bin/ssh");

    QStringList args;

    args << "-o"
         << "PasswordAuthentication=no"
         << "-f" << m_remote_host;

    QString host =
        QString("ws://%1:%2").arg(QHostInfo::localHostName()).arg(m_port);

    qDebug() << Q_FUNC_INFO << host;

    auto sarglist = QStringList()
                    << "nohup" << m_exe_path << "-c" << host << "&";

    args << sarglist.join(" ");

    p->setArguments(args);

    qInfo() << "Launching:" << args.join(" ");

    connect(p,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &RemoteCommand::on_finished);

    p->start();
}

void RemoteCommand::on_finished(int exit_code, QProcess::ExitStatus status) {
    deleteLater();

    auto* p = qobject_cast<QProcess*>(sender());

    if (!p) return;

    qDebug() << Q_FUNC_INFO;

    auto all_out = p->readAllStandardOutput();
    auto all_err = p->readAllStandardError();

    if (status == QProcess::CrashExit) {
        qCritical() << "Unable to launch remote job, ssh crashed.";
        qCritical() << all_err;
        return;
    }


    if (exit_code != 0) {
        qCritical() << "Unable to launch remote job, job failed.";
        qCritical() << all_err;
        return;
    }

    qInfo() << "Job launched:" << all_out;
}

// =============================================================================

void Worker::on_message(MessageSuccess const& m) {
    assert(m_assignment);
    assert(m.completed == m_assignment->id);
    m_assignment.reset();
    auto duration = m_start_time.secsTo(QDateTime::currentDateTime());
    emit success(m, duration);
    emit want_new_work();
}
void Worker::on_message(MessageFailed const& m) {
    assert(m_assignment);
    assert(m.failed == m_assignment->id);
    m_assignment.reset();
    emit failed(m);
    emit want_new_work();
}

Worker::Worker(QPointer<QWebSocket> s, size_t wid, QObject* parent)
    : QObject(parent), m_socket(s), m_worker_id(wid) {

    qInfo() << "Connection from" << s->origin();

    connect(s, &QWebSocket::textMessageReceived, this, &Worker::on_text);
    connect(s, &QWebSocket::binaryMessageReceived, this, &Worker::on_data);
    connect(s, &QWebSocket::disconnected, this, &Worker::on_conn_closed);
}
Worker::~Worker() = default;

QString Worker::name() const {
    return m_socket ? m_socket->origin() : QString("<zombie>");
}

bool Worker::has_assignment() const {
    return m_assignment.has_value();
}

QUuid Worker::assignment_id() const {
    return m_assignment ? m_assignment->id : QUuid();
}

QString Worker::status_string() const {
    QString st = has_assignment() ? assignment_id().toString() : "idle";

    return QString("- %1 %2 : %3").arg(m_worker_id).arg(name()).arg(st);
}

void Worker::kill() const {
    m_socket->close();
}

void Worker::on_conn_closed() {
    qInfo() << "Worker" << m_worker_id << "disconnected.";
    qDebug() << Q_FUNC_INFO;
    if (m_assignment) {
        MessageFailed f;
        f.failed  = m_assignment->id;
        f.std_out = "Connection closed";
        emit failed(f);
    }
    auto* client = qobject_cast<QWebSocket*>(sender());
    client->deleteLater();
    emit disconnected();
    deleteLater();
}

void Worker::on_text(QString) {
    qWarning() << "Text data? We don't handle that!";
}

void Worker::on_data(QByteArray data) {

    auto message = decode_message(data);

    std::visit(overloaded {
                   [](std::monostate) {}, // do nothing
                   [](MessageAssignment const&) {
                       // ok, we shouldnt be getting this from a client...
                       qCritical() << "Confusing message from client!";
                   },
                   [this](auto const& m) { on_message(m); },
               },
               message);
}

void Worker::on_new_work_available() {
    qDebug() << Q_FUNC_INFO;
    // if we have a job, skip
    if (m_assignment) return;

    emit want_new_work();
}

void Worker::on_new_work_assigned(JobAssignment record) {
    qDebug() << Q_FUNC_INFO;
    assert(!m_assignment);

    m_assignment.emplace(record);
    m_start_time = QDateTime::currentDateTime();

    MessageAssignment message;
    message.id      = record.id;
    message.command = record.command;

    m_socket->sendBinaryMessage(encode_message(message));
}


// =============================================================================

void Server::assign_work_to(Worker* w) {
    qDebug() << Q_FUNC_INFO << w;
    if (!w) return;

    if (m_pending_jobs.isEmpty()) return;

    if (w->has_assignment()) return;

    auto next = m_pending_jobs.dequeue();

    assert(m_jobs.count(next));

    JobRecord& record = m_jobs[next];

    record.status = JobStatus::IN_WORK;

    JobAssignment assignment { record.id, record.command };

    w->on_new_work_assigned(assignment);
}

void Server::enqueue(QVector<QUuid> new_items) {
    qDebug() << Q_FUNC_INFO;
    // old QT makes this annoying...
    for (auto const& id : new_items) {
        m_pending_jobs.enqueue(id);
    }

    emit work_available();
}

void Server::c_exit(QStringList const&) {
    // nothing special to do
    qInfo() << "Closing down server...";
    QCoreApplication::quit();
}

void Server::c_haltsave(QStringList const& args) {
    if (args.empty()) {
        qInfo() << "Need a filename";
        return;
    }

    if (m_pending_jobs.size()) {
        qInfo()
            << "Please clear pending jobs and wait for workers to complete.";
        return;
    }

    for (auto const& v : m_jobs) {
        if (v.status == JobStatus::IN_WORK) {
            qInfo() << "Please wait for workers to complete.";
            return;
        }
    }

    QFile file(args.value(0));

    bool ok = file.open(QFile::WriteOnly);

    if (!ok) {
        qInfo() << "Unable to open file for writing.";
        return;
    }

    {
        QDataStream outf(&file);

        outf << m_jobs;
    }

    file.close();

    qInfo() << "State written. You can stop the server when clients are done.";
}
void Server::c_restore(QStringList const& args) {
    if (args.empty()) {
        qInfo() << "Need a filename";
        return;
    }

    QFile file(args.value(0));

    bool ok = file.open(QFile::ReadOnly);

    if (!ok) {
        qInfo() << "Unable to open file for reading.";
        return;
    }

    QVector<QUuid> to_add;

    QHash<QUuid, JobRecord> tmp;

    {
        QDataStream inf(&file);

        inf >> tmp;
    }

    auto iter = tmp.begin();

    while (iter != tmp.end()) {

        auto state = iter.value().status;

        if (state == JobStatus::PENDING) {
            ++iter;
            to_add << iter.value().id;
            continue;
        }

        // delete others

        iter = tmp.erase(iter);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_jobs.insert(tmp);
#else
    for (auto iter = tmp.begin(); iter != tmp.end(); iter++) {
        m_jobs[iter.key()] = iter.value();
    }
#endif

    qInfo() << "State loaded...";

    enqueue(to_add);
}

void Server::c_status(QStringList const&) {
    qInfo() << m_pending_jobs.size() << "jobs in queue";
    if (m_failed_jobs.size()) {
        qInfo() << m_failed_jobs.size() << "jobs failed";
    }

    qInfo() << "Workers:";

    for (auto w : m_clients) {
        auto info = qInfo();
        info.noquote() << w->status_string();
    }
}
void Server::c_clear(QStringList const& args) {
    using FType = std::function<void()>;

    const QHash<QString, FType> commands = {
        { "pending", [this]() { m_pending_jobs.clear(); } },
    };

    if (args.empty() or !commands.contains(args.value(0))) {
        qInfo() << "Clear what?";
        for (auto const& k : commands.keys()) {
            qInfo() << "-" << k;
        }
        return;
    }

    commands[args.value(0)]();
}
void Server::c_add(QStringList const& args) {
    auto source = args.value(0);
    qInfo() << "Sourcing new jobs from" << source;

    add_file(source);
}

void Server::c_worker(QStringList const& args) {
    using FType = std::function<void()>;

    auto subcommand = args.value(0);

    auto subcommand_args = args;
    if (subcommand_args.size()) { subcommand_args.pop_front(); }

    auto list_workers = [this]() {
        qInfo() << "Workers:";

        for (auto w : m_clients) {
            auto info = qInfo();
            info.noquote() << w->status_string();
        }
    };

    auto add_worker = [this, &subcommand_args]() {
        auto host     = subcommand_args.value(0);
        auto exe_path = subcommand_args.value(1);

        if (host.isEmpty()) return;

        if (exe_path.isEmpty()) {
            exe_path = QCoreApplication::applicationFilePath();
        }

        auto* p = new RemoteCommand(
            host, exe_path, m_socket_server->serverPort(), this);
        p->start();
    };


    auto drop_worker = [this, &subcommand_args]() {
        auto s_id = subcommand_args.value(0);

        if (s_id.isEmpty()) return;

        bool ok = false;

        auto int_id = s_id.toUInt(&ok);

        if (!ok) return;

        for (auto w : m_clients) {
            if (w->worker_id() != int_id) continue;

            w->kill();
            break;
        }
    };

    const QHash<QString, FType> sub_commands = { { "list", list_workers },
                                                 { "add", add_worker },
                                                 { "drop", drop_worker } };

    auto iter = sub_commands.find(subcommand);

    if (iter == sub_commands.end()) {
        qInfo() << "Unknown worker subcommand";
        return;
    }

    iter.value()();
}

Server::Server(uint16_t port) {
    m_socket_server = new QWebSocketServer(
        "Job Server", QWebSocketServer::NonSecureMode, this);

    if (!m_socket_server->listen(QHostAddress::Any, port)) {
        qCritical() << "Unable to listen on port" << port;
        return;
    }

    qInfo() << "Listening on" << m_socket_server->serverUrl();

    connect(m_socket_server,
            &QWebSocketServer::newConnection,
            this,
            &Server::on_new_connection);
    //    connect(m_socket_server,
    //            &QWebSocketServer::closed,
    //            this,
    //            &Server::on_conn_closed);

    m_prompt = new AsyncPrompt(this);
    connect(m_prompt, &AsyncPrompt::new_text, this, &Server::on_console_text);
}

Server::~Server() { }

void Server::add_file(QString filename) {
    qDebug() << Q_FUNC_INFO << filename;

    QFile file(filename);

    if (!file.open(QFile::ReadOnly)) {
        qCritical() << "Unable to open file" << filename;
        return;
    }

    QTextStream stream(&file);

    size_t counter = 0;

    while (!stream.atEnd()) {
        auto      line = stream.readLine();
        JobRecord rec;
        rec.id      = QUuid::createUuid();
        rec.command = line;
        rec.status  = JobStatus::PENDING;

        m_jobs[rec.id] = rec;

        m_pending_jobs.enqueue(rec.id);
        counter++;
    }

    qInfo() << "Added" << counter << "jobs," << m_pending_jobs.size()
            << "now pending";

    emit work_available();
}

void Server::add_clients(QString filename) {
    qDebug() << Q_FUNC_INFO << filename;

    QFile file(filename);

    if (!file.open(QFile::ReadOnly)) {
        qCritical() << "Unable to open file" << filename;
        return;
    }

    QTextStream stream(&file);

    while (!stream.atEnd()) {
        auto line = stream.readLine();

        // read out hostname and path to exe
        auto parts = line.split(" ", Qt::SkipEmptyParts);

        if (parts.empty()) continue;

        auto host     = parts.value(0);
        auto exe_path = parts.value(1);

        if (exe_path.isEmpty()) {
            exe_path = QCoreApplication::applicationFilePath();
        }

        auto* p = new RemoteCommand(
            host, exe_path, m_socket_server->serverPort(), this);
        p->start();
    }
}

void Server::on_new_connection() {
    qDebug() << Q_FUNC_INFO;
    auto* socket = m_socket_server->nextPendingConnection();


    auto* worker = new Worker(socket, m_next_worker_id, this);

    m_next_worker_id++;

    connect(worker, &Worker::disconnected, this, &Server::on_client_lost);

    connect(worker, &Worker::failed, this, &Server::on_worker_failed);
    connect(worker, &Worker::success, this, &Server::on_worker_success);
    connect(
        worker, &Worker::want_new_work, this, &Server::on_worker_wants_work);

    connect(
        this, &Server::work_available, worker, &Worker::on_new_work_available);

    m_clients << worker;

    assign_work_to(worker);
}

void Server::on_client_lost() {
    qDebug() << Q_FUNC_INFO;
    auto* client = qobject_cast<Worker*>(sender());
    m_clients.remove(client);
}

void Server::on_worker_failed(MessageFailed m) {
    qDebug() << Q_FUNC_INFO;
    m_jobs[m.failed].status = JobStatus::FAILED;
    m_failed_jobs.push_back(m.failed);
}
void Server::on_worker_success(MessageSuccess m, double seconds) {
    qDebug() << Q_FUNC_INFO;
    m_jobs[m.completed].status = JobStatus::DONE;

    auto time = QTime::fromMSecsSinceStartOfDay(seconds * 1000);

    qInfo() << "Job" << m.completed.toString() << "done in:" << time.toString();
}

void Server::on_worker_wants_work() {
    qDebug() << Q_FUNC_INFO;
    auto* worker = qobject_cast<Worker*>(sender());
    assign_work_to(worker);
}

void Server::on_console_text(QString text) {
    using FType = void (Server::*)(QStringList const&);
    static const QHash<QString, FType> commands = {
        { "exit", &Server::c_exit },       { "haltsave", &Server::c_haltsave },
        { "restore", &Server::c_restore }, { "status", &Server::c_status },
        { "clear", &Server::c_clear },     { "add", &Server::c_add },
        { "worker", &Server::c_worker },
    };

    // regex gets hairy with qt6 and qt5...
#if QT_VERSION > QT_VERSION_CHECK(5, 14, 0)
    auto parts = text.trimmed().split(' ', Qt::SkipEmptyParts);
#else
    auto parts = text.trimmed().split(' ');
#endif

    if (parts.isEmpty()) return;

    auto iter = commands.find(parts[0]);

    if (iter == commands.end()) {
        qInfo() << "Unknown command" << text;
        return;
    }

    parts.removeFirst();

    FType to_call = iter.value();

    (this->*to_call)(parts);
}

// =============================================================================

void Client::on_assignment(MessageAssignment const& m) {
    qDebug() << Q_FUNC_INFO;
    if (m_assignment) {
        // uh oh. this shouldnt happen
        MessageFailed failed;
        failed.failed  = m.id;
        failed.std_out = "Already have assignment!";

        m_socket->sendBinaryMessage(encode_message(failed));
        return;
    }

    m_assignment.emplace(m);

    qInfo() << "New job" << m.id;

    auto* p = new QProcess(this);

    m_process = p;

    p->setProgram("/bin/sh");

    QStringList arglist;
    arglist << "-c";
    arglist << m.command;

    p->setArguments(arglist);

    qInfo() << "Launching" << p->program() << arglist.join(" ");

    p->start();

    connect(p,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &Client::on_process_finished);
}

Client::Client(QUrl url) {
    qInfo() << "Connecting to" << url;
    auto name = QHostInfo::localHostName();

    m_socket = new QWebSocket(name, QWebSocketProtocol::VersionLatest, this);

    connect(m_socket, &QWebSocket::connected, this, &Client::on_connected);
    connect(m_socket, &QWebSocket::disconnected, this, &Client::on_closed);

    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this,
            &Client::socket_error);

    m_socket->open(url);
}

Client::~Client() = default;

void Client::on_connected() {
    qDebug() << Q_FUNC_INFO;
    qInfo() << "Connected to" << m_socket->peerName();

    connect(
        m_socket, &QWebSocket::binaryMessageReceived, this, &Client::on_data);
}

void Client::on_closed() {
    qDebug() << Q_FUNC_INFO;
    qInfo() << "Closed.";
    QCoreApplication::quit();
}

void Client::on_data(QByteArray data) {
    qDebug() << Q_FUNC_INFO;
    auto message = decode_message(data);

    bool h = std::visit(overloaded {
                            [](std::monostate) {
                                qCritical() << "Message was empty?";
                                return false;
                            },
                            [this](MessageAssignment const& m) {
                                on_assignment(m);
                                return true;
                            },
                            [](auto const&) {
                                // we dont get these
                                qCritical() << "Confusing message from server!";
                                return true;
                            },
                        },
                        message);

    if (!h) { qFatal("The server is being confusing. Bailing."); }
}

void Client::on_process_finished(int exit_code, QProcess::ExitStatus status) {
    qDebug() << Q_FUNC_INFO << exit_code << status;
    QProcess* p = qobject_cast<QProcess*>(sender());

    assert(m_assignment);

    auto job_id  = m_assignment->id;
    auto std_out = p->readAllStandardOutput();
    auto std_err = p->readAllStandardError();

    m_assignment.reset();

    if (exit_code == 0 and status == QProcess::NormalExit) {
        // success
        MessageSuccess m;
        m.completed = job_id;
        m.std_out   = std_out;
        m.std_err   = std_err;

        m_socket->sendBinaryMessage(encode_message(m));
    } else {
        MessageFailed m;
        m.failed  = job_id;
        m.std_out = std_out;
        m.std_err = std_err;

        m_socket->sendBinaryMessage(encode_message(m));
    }
}

void Client::socket_error(QAbstractSocket::SocketError error) {
    qCritical() << error;
}
