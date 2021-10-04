#ifndef JOBSYS_H
#define JOBSYS_H

#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QSocketNotifier>
#include <QString>
#include <QThread>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

#include <optional>
#include <variant>

enum class JobStatus { PENDING, IN_WORK, DONE, FAILED };

struct JobAssignment {
    QUuid   id;
    QString command;
};

struct JobRecord {
    QUuid     id;
    QString   command;
    JobStatus status;
};

struct MessageAssignment : JobAssignment { };

struct MessageSuccess {
    QUuid   completed;
    QString std_out;
    QString std_err;
};

struct MessageFailed {
    QUuid   failed;
    QString std_out;
    QString std_err;
};

using MessageType = std::
    variant<std::monostate, MessageAssignment, MessageSuccess, MessageFailed>;

// =============================================================================

class AsyncPrompt : public QObject {
    Q_OBJECT

    QSocketNotifier* m_notifier;
    QThread          m_thread;

public:
    explicit AsyncPrompt(QObject* parent = nullptr);
    virtual ~AsyncPrompt();

signals:
    void new_text(QString);


    void _get_new_line(QString);

private slots:
    void on_get_new_line(QString);
};

// =============================================================================

class RemoteCommand : public QObject {
    Q_OBJECT
    QString  m_remote_host;
    QString  m_exe_path;
    uint16_t m_port;

public:
    RemoteCommand(QString remote_host, QString exe_path, uint16_t, QObject*);

    void start();

public slots:
    void on_finished(int exit_code, QProcess::ExitStatus);
};

// =============================================================================

class Worker : public QObject {
    Q_OBJECT
    QPointer<QWebSocket> m_socket;
    size_t               m_worker_id;

    std::optional<JobAssignment> m_assignment;
    QDateTime                    m_start_time;

    void on_message(MessageSuccess const&);
    void on_message(MessageFailed const&);

public:
    Worker(QPointer<QWebSocket>, size_t wid, QObject* parent);
    virtual ~Worker();

    QString name() const;
    bool    has_assignment() const;
    QUuid   assignment_id() const;

    size_t worker_id() const { return m_worker_id; }

    QString status_string() const;

    void kill() const;

private slots:
    void on_conn_closed();

    void on_text(QString);
    void on_data(QByteArray);

public slots:
    void on_new_work_available();

    void on_new_work_assigned(JobAssignment);

signals:
    void disconnected();
    void failed(MessageFailed);
    void success(MessageSuccess, double seconds);
    void want_new_work();
};

// =============================================================================

class Server : public QObject {
    Q_OBJECT

    QWebSocketServer* m_socket_server;

    size_t m_next_worker_id = 0;

    QSet<Worker*> m_clients;

    AsyncPrompt* m_prompt;

    QQueue<QUuid>           m_pending_jobs;
    QHash<QUuid, JobRecord> m_jobs;
    QVector<QUuid>          m_failed_jobs;

    void assign_work_to(Worker*);

    void enqueue(QVector<QUuid>);

    void c_exit(QStringList const&);
    void c_haltsave(QStringList const&);
    void c_restore(QStringList const&);
    void c_status(QStringList const&);
    void c_clear(QStringList const&);
    void c_add(QStringList const&);
    void c_worker(QStringList const&);

public:
    Server(uint16_t port);
    virtual ~Server();

    void add_file(QString);

signals:
    void work_available();

private slots:
    void on_new_connection();
    void on_client_lost();

    void on_worker_failed(MessageFailed);
    void on_worker_success(MessageSuccess, double seconds);

    void on_worker_wants_work();

    void on_console_text(QString);
};

// =============================================================================

class Client : public QObject {
    Q_OBJECT

    QWebSocket* m_socket;

    std::optional<MessageAssignment> m_assignment;
    QPointer<QProcess>               m_process;

    void on_assignment(MessageAssignment const&);

public:
    Client(QUrl);
    virtual ~Client();

private slots:
    void on_connected();
    void on_closed();
    void on_data(QByteArray);
    void on_process_finished(int exit_code, QProcess::ExitStatus);

    void socket_error(QAbstractSocket::SocketError);
};


#endif // JOBSYS_H
