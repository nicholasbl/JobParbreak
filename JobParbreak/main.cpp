#include "jobsys.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QLoggingCategory>

#include <memory>

int main(int argc, char* argv[]) {
    QCoreApplication a(argc, argv);

    QCoreApplication::setApplicationName("Job Scheduler");
    QCoreApplication::setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("A simple job distribution system");
    parser.addHelpOption();
    parser.addVersionOption();

    auto tl = [](const char* key) {
        return QCoreApplication::translate("main", key);
    };

    QCommandLineOption as_server_option({ "s", "server" },
                                        tl("Run as a server"));
    parser.addOption(as_server_option);

    QCommandLineOption as_client_option(
        { "c", "client" }, tl("Run as a client"), tl("host"));
    parser.addOption(as_client_option);

    QCommandLineOption port_option(
        { "p", "port" }, tl("Port to use"), tl("port"), "55000");
    parser.addOption(port_option);

    QCommandLineOption text_option({ "t", "txtfile" },
                                   tl("Serve jobs from the given text file"),
                                   tl("file"));
    parser.addOption(text_option);


    QCommandLineOption debug_option({ "d", "debug" },
                                    tl("Enable debug output"));
    parser.addOption(debug_option);


    // Process the actual command line arguments given by the user
    parser.process(a);

    if (parser.isSet(as_server_option) and parser.isSet(as_client_option)) {
        qCritical() << "Cannot run as both server and client!";
        return EXIT_FAILURE;
    }

    bool ok;
    auto port = parser.value(port_option).toUInt(&ok);

    if (!ok) {
        qCritical() << "Confusing port given!";
        return EXIT_FAILURE;
    }


    if (!parser.isSet(debug_option)) {
        QLoggingCategory::setFilterRules("*.debug=false");
    }

    std::unique_ptr<Server> server;
    std::unique_ptr<Client> client;

    if (parser.isSet(as_server_option)) {
        server = std::make_unique<Server>(port);

        if (parser.isSet(text_option)) {
            server->add_file(parser.value(text_option));
        }
    }

    if (parser.isSet(as_client_option)) {
        auto h   = parser.value(as_client_option);
        auto url = QUrl(h);
        qDebug() << url;
        if (!url.isValid()) {
            qCritical() << "Bad host!";
            return EXIT_FAILURE;
        }

        client = std::make_unique<Client>(url);
    }

    return a.exec();
}
