#include "RelayRendezvous.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTextStream>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("relay-rendezvous");
    QCommandLineParser options; options.addHelpOption();
    options.addOption({"listen", "Listen address; use loopback unless running on a private container network", "address", "127.0.0.1"});
    options.addOption({"port", "Loopback port behind an HTTPS/WebSocket proxy", "port", "39872"}); options.process(app);
    bool valid = false; const auto port = options.value("port").toUInt(&valid);
    if (!valid || port > 65535 || port == 0) return 2;
    RelayRendezvous server;
    const QHostAddress address(options.value("listen"));
    if (address.isNull()) return 2;
    if (!server.listen(address, quint16(port))) { QTextStream(stderr) << "Could not start Relay setup service.\n"; return 1; }
    return app.exec();
}
